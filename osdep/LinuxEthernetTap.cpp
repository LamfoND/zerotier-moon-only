/*
 * Copyright (c)2019 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include "../node/Constants.hpp"

#ifdef __LINUX__

#include "../node/Utils.hpp"
#include "../node/Mutex.hpp"
#include "../node/Dictionary.hpp"
#include "OSUtils.hpp"
#include "LinuxEthernetTap.hpp"
#include "LinuxNetLink.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <net/if_arp.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_addr.h>
#include <linux/if_ether.h>
#include <ifaddrs.h>

#include <algorithm>
#include <utility>
#include <string>

// ff:ff:ff:ff:ff:ff with no ADI
static const ZeroTier::MulticastGroup _blindWildcardMulticastGroup(ZeroTier::MAC(0xff),0);

namespace ZeroTier {

static Mutex __tapCreateLock;

static const char _base32_chars[32] = { 'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z','2','3','4','5','6','7' };
static void _base32_5_to_8(const uint8_t *in,char *out)
{
	out[0] = _base32_chars[(in[0]) >> 3];
	out[1] = _base32_chars[(in[0] & 0x07) << 2 | (in[1] & 0xc0) >> 6];
	out[2] = _base32_chars[(in[1] & 0x3e) >> 1];
	out[3] = _base32_chars[(in[1] & 0x01) << 4 | (in[2] & 0xf0) >> 4];
	out[4] = _base32_chars[(in[2] & 0x0f) << 1 | (in[3] & 0x80) >> 7];
	out[5] = _base32_chars[(in[3] & 0x7c) >> 2];
	out[6] = _base32_chars[(in[3] & 0x03) << 3 | (in[4] & 0xe0) >> 5];
	out[7] = _base32_chars[(in[4] & 0x1f)];
}

LinuxEthernetTap::LinuxEthernetTap(
	const char *homePath,
	const MAC &mac,
	unsigned int mtu,
	unsigned int metric,
	uint64_t nwid,
	const char *friendlyName,
	void (*handler)(void *,void *,uint64_t,const MAC &,const MAC &,unsigned int,unsigned int,const void *,unsigned int),
	void *arg) :
	_handler(handler),
	_arg(arg),
	_nwid(nwid),
	_homePath(homePath),
	_mtu(mtu),
	_fd(0),
	_enabled(true)
{
	char procpath[128],nwids[32];
	struct stat sbuf;

	// ensure netlink connection is started
	(void)LinuxNetLink::getInstance();

	OSUtils::ztsnprintf(nwids,sizeof(nwids),"%.16llx",nwid);

	Mutex::Lock _l(__tapCreateLock); // create only one tap at a time, globally

	_fd = ::open("/dev/net/tun",O_RDWR);
	if (_fd <= 0) {
		_fd = ::open("/dev/tun",O_RDWR);
		if (_fd <= 0)
			throw std::runtime_error(std::string("could not open TUN/TAP device: ") + strerror(errno));
	}

	struct ifreq ifr;
	memset(&ifr,0,sizeof(ifr));

	// Restore device names from legacy devicemap, but for new devices we use a base32-based canonical naming
	std::map<std::string,std::string> globalDeviceMap;
	FILE *devmapf = fopen((_homePath + ZT_PATH_SEPARATOR_S + "devicemap").c_str(),"r");
	if (devmapf) {
		char buf[256];
		while (fgets(buf,sizeof(buf),devmapf)) {
			char *x = (char *)0;
			char *y = (char *)0;
			char *saveptr = (char *)0;
			for(char *f=Utils::stok(buf,"\r\n=",&saveptr);(f);f=Utils::stok((char *)0,"\r\n=",&saveptr)) {
				if (!x) x = f;
				else if (!y) y = f;
				else break;
			}
			if ((x)&&(y)&&(x[0])&&(y[0]))
				globalDeviceMap[x] = y;
		}
		fclose(devmapf);
	}
	bool recalledDevice = false;
	std::map<std::string,std::string>::const_iterator gdmEntry = globalDeviceMap.find(nwids);
	if (gdmEntry != globalDeviceMap.end()) {
		Utils::scopy(ifr.ifr_name,sizeof(ifr.ifr_name),gdmEntry->second.c_str());
		OSUtils::ztsnprintf(procpath,sizeof(procpath),"/proc/sys/net/ipv4/conf/%s",ifr.ifr_name);
		recalledDevice = (stat(procpath,&sbuf) != 0);
	}

	if (!recalledDevice) {
#ifdef __SYNOLOGY__
		int devno = 50;
		do {
			OSUtils::ztsnprintf(ifr.ifr_name,sizeof(ifr.ifr_name),"eth%d",devno++);
			OSUtils::ztsnprintf(procpath,sizeof(procpath),"/proc/sys/net/ipv4/conf/%s",ifr.ifr_name);
		} while (stat(procpath,&sbuf) == 0); // try zt#++ until we find one that does not exist
#else
		uint64_t trial = 0; // incremented in the very unlikely event of a name collision with another network
		do {
			const uint64_t nwid40 = (nwid ^ (nwid >> 24)) + trial++;
			uint8_t tmp2[5];
			char tmp3[11];
			tmp2[0] = (uint8_t)((nwid40 >> 32) & 0xff);
			tmp2[1] = (uint8_t)((nwid40 >> 24) & 0xff);
			tmp2[2] = (uint8_t)((nwid40 >> 16) & 0xff);
			tmp2[3] = (uint8_t)((nwid40 >> 8) & 0xff);
			tmp2[4] = (uint8_t)(nwid40 & 0xff);
			tmp3[0] = 'z';
			tmp3[1] = 't';
			_base32_5_to_8(tmp2,tmp3 + 2);
			tmp3[10] = (char)0;
			memcpy(ifr.ifr_name,tmp3,11);
			OSUtils::ztsnprintf(procpath,sizeof(procpath),"/proc/sys/net/ipv4/conf/%s",ifr.ifr_name);
		} while (stat(procpath,&sbuf) == 0);
#endif
	}

	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	if (ioctl(_fd,TUNSETIFF,(void *)&ifr) < 0) {
		::close(_fd);
		throw std::runtime_error("unable to configure TUN/TAP device for TAP operation");
	}

	_dev = ifr.ifr_name;

	::ioctl(_fd,TUNSETPERSIST,0); // valgrind may generate a false alarm here

	// Open an arbitrary socket to talk to netlink
	int sock = socket(AF_INET,SOCK_DGRAM,0);
	if (sock <= 0) {
		::close(_fd);
		throw std::runtime_error("unable to open netlink socket");
	}

	// Set MAC address
	ifr.ifr_ifru.ifru_hwaddr.sa_family = ARPHRD_ETHER;
	mac.copyTo(ifr.ifr_ifru.ifru_hwaddr.sa_data,6);
	if (ioctl(sock,SIOCSIFHWADDR,(void *)&ifr) < 0) {
		::close(_fd);
		::close(sock);
		throw std::runtime_error("unable to configure TAP hardware (MAC) address");
		return;
	}

	// Set MTU
	ifr.ifr_ifru.ifru_mtu = (int)mtu;
	if (ioctl(sock,SIOCSIFMTU,(void *)&ifr) < 0) {
		::close(_fd);
		::close(sock);
		throw std::runtime_error("unable to configure TAP MTU");
	}

	if (fcntl(_fd,F_SETFL,fcntl(_fd,F_GETFL) & ~O_NONBLOCK) == -1) {
		::close(_fd);
		throw std::runtime_error("unable to set flags on file descriptor for TAP device");
	}

	/* Bring interface up */
	if (ioctl(sock,SIOCGIFFLAGS,(void *)&ifr) < 0) {
		::close(_fd);
		::close(sock);
		throw std::runtime_error("unable to get TAP interface flags");
	}
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(sock,SIOCSIFFLAGS,(void *)&ifr) < 0) {
		::close(_fd);
		::close(sock);
		throw std::runtime_error("unable to set TAP interface flags");
	}

	::close(sock);

	// Set close-on-exec so that devices cannot persist if we fork/exec for update
	::fcntl(_fd,F_SETFD,fcntl(_fd,F_GETFD) | FD_CLOEXEC);

	(void)::pipe(_shutdownSignalPipe);

	/*
	globalDeviceMap[nwids] = _dev;
	devmapf = fopen((_homePath + ZT_PATH_SEPARATOR_S + "devicemap").c_str(),"w");
	if (devmapf) {
		gdmEntry = globalDeviceMap.begin();
		while (gdmEntry != globalDeviceMap.end()) {
			fprintf(devmapf,"%s=%s\n",gdmEntry->first.c_str(),gdmEntry->second.c_str());
			++gdmEntry;
		}
		fclose(devmapf);
	}
	*/

	_thread = Thread::start(this);
}

LinuxEthernetTap::~LinuxEthernetTap()
{
	(void)::write(_shutdownSignalPipe[1],"\0",1); // causes thread to exit
	Thread::join(_thread);
	::close(_fd);
	::close(_shutdownSignalPipe[0]);
	::close(_shutdownSignalPipe[1]);
}

void LinuxEthernetTap::setEnabled(bool en)
{
	_enabled = en;
}

bool LinuxEthernetTap::enabled() const
{
	return _enabled;
}

static bool ___removeIp(const std::string &_dev,const InetAddress &ip)
{
	LinuxNetLink::getInstance().removeAddress(ip, _dev.c_str());
	return true;
}

bool LinuxEthernetTap::addIps(std::vector<InetAddress> ips)
{
#ifdef __SYNOLOGY__
	std::string filepath = "/etc/sysconfig/network-scripts/ifcfg-"+_dev;
	std::string cfg_contents = "DEVICE="+_dev+"\nBOOTPROTO=static";
	int ip4=0,ip6=0,ip4_tot=0,ip6_tot=0;

	for(int i=0; i<(int)ips.size(); i++) {
		if (ips[i].isV4())
			ip4_tot++;
		else
			ip6_tot++;
	}
	// Assemble and write contents of ifcfg-dev file
	for(int i=0; i<(int)ips.size(); i++) {
		if (ips[i].isV4()) {
			char iptmp[64],iptmp2[64];
			std::string numstr4 = ip4_tot > 1 ? std::to_string(ip4) : "";
			cfg_contents += "\nIPADDR"+numstr4+"="+ips[i].toIpString(iptmp)
				+ "\nNETMASK"+numstr4+"="+ips[i].netmask().toIpString(iptmp2)+"\n";
			ip4++;
		} else {
			char iptmp[64],iptmp2[64];
			std::string numstr6 = ip6_tot > 1 ? std::to_string(ip6) : "";
			cfg_contents += "\nIPV6ADDR"+numstr6+"="+ips[i].toIpString(iptmp)
				+ "\nNETMASK"+numstr6+"="+ips[i].netmask().toIpString(iptmp2)+"\n";
			ip6++;
		}
	}
	OSUtils::writeFile(filepath.c_str(), cfg_contents.c_str(), cfg_contents.length());
	// Finally, add IPs
	for(int i=0; i<(int)ips.size(); i++){
		LinuxNetLink::getInstance().addAddress(ips[i], _dev.c_str());
	}
	return true;
#endif // __SYNOLOGY__
	return false;
}

bool LinuxEthernetTap::addIp(const InetAddress &ip)
{
	if (!ip)
		return false;

	std::vector<InetAddress> allIps(ips());
	if (std::binary_search(allIps.begin(),allIps.end(),ip))
		return true;

	// Remove and reconfigure if address is the same but netmask is different
	for(std::vector<InetAddress>::iterator i(allIps.begin());i!=allIps.end();++i) {
		if (i->ipsEqual(ip))
			___removeIp(_dev,*i);
	}

	LinuxNetLink::getInstance().addAddress(ip, _dev.c_str());

	return true;
}

bool LinuxEthernetTap::removeIp(const InetAddress &ip)
{
	if (!ip)
		return true;
	std::vector<InetAddress> allIps(ips());
	if (std::find(allIps.begin(),allIps.end(),ip) != allIps.end()) {
		if (___removeIp(_dev,ip))
			return true;
	}
	return false;
}

std::vector<InetAddress> LinuxEthernetTap::ips() const
{
	struct ifaddrs *ifa = (struct ifaddrs *)0;
	if (getifaddrs(&ifa))
		return std::vector<InetAddress>();

	std::vector<InetAddress> r;

	struct ifaddrs *p = ifa;
	while (p) {
		if ((!strcmp(p->ifa_name,_dev.c_str()))&&(p->ifa_addr)&&(p->ifa_netmask)&&(p->ifa_addr->sa_family == p->ifa_netmask->sa_family)) {
			switch(p->ifa_addr->sa_family) {
				case AF_INET: {
					struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
					struct sockaddr_in *nm = (struct sockaddr_in *)p->ifa_netmask;
					r.push_back(InetAddress(&(sin->sin_addr.s_addr),4,Utils::countBits((uint32_t)nm->sin_addr.s_addr)));
				}	break;
				case AF_INET6: {
					struct sockaddr_in6 *sin = (struct sockaddr_in6 *)p->ifa_addr;
					struct sockaddr_in6 *nm = (struct sockaddr_in6 *)p->ifa_netmask;
					uint32_t b[4];
					memcpy(b,nm->sin6_addr.s6_addr,sizeof(b));
					r.push_back(InetAddress(sin->sin6_addr.s6_addr,16,Utils::countBits(b[0]) + Utils::countBits(b[1]) + Utils::countBits(b[2]) + Utils::countBits(b[3])));
				}	break;
			}
		}
		p = p->ifa_next;
	}

	if (ifa)
		freeifaddrs(ifa);

	std::sort(r.begin(),r.end());
	r.erase(std::unique(r.begin(),r.end()),r.end());

	return r;
}

void LinuxEthernetTap::put(const MAC &from,const MAC &to,unsigned int etherType,const void *data,unsigned int len)
{
	char putBuf[ZT_MAX_MTU + 64];
	if ((_fd > 0)&&(len <= _mtu)&&(_enabled)) {
		to.copyTo(putBuf,6);
		from.copyTo(putBuf + 6,6);
		*((uint16_t *)(putBuf + 12)) = htons((uint16_t)etherType);
		memcpy(putBuf + 14,data,len);
		len += 14;
		(void)::write(_fd,putBuf,len);
	}
}

std::string LinuxEthernetTap::deviceName() const
{
	return _dev;
}

void LinuxEthernetTap::setFriendlyName(const char *friendlyName)
{
}

void LinuxEthernetTap::scanMulticastGroups(std::vector<MulticastGroup> &added,std::vector<MulticastGroup> &removed)
{
	char *ptr,*ptr2;
	unsigned char mac[6];
	std::vector<MulticastGroup> newGroups;

	int fd = ::open("/proc/net/dev_mcast",O_RDONLY);
	if (fd > 0) {
		char buf[131072];
		int n = (int)::read(fd,buf,sizeof(buf));
		if ((n > 0)&&(n < (int)sizeof(buf))) {
			buf[n] = (char)0;
			for(char *l=strtok_r(buf,"\r\n",&ptr);(l);l=strtok_r((char *)0,"\r\n",&ptr)) {
				int fno = 0;
				char *devname = (char *)0;
				char *mcastmac = (char *)0;
				for(char *f=strtok_r(l," \t",&ptr2);(f);f=strtok_r((char *)0," \t",&ptr2)) {
					if (fno == 1)
						devname = f;
					else if (fno == 4)
						mcastmac = f;
					++fno;
				}
				if ((devname)&&(!strcmp(devname,_dev.c_str()))&&(mcastmac)&&(Utils::unhex(mcastmac,mac,6) == 6))
					newGroups.push_back(MulticastGroup(MAC(mac,6),0));
			}
		}
		::close(fd);
	}

	std::vector<InetAddress> allIps(ips());
	for(std::vector<InetAddress>::iterator ip(allIps.begin());ip!=allIps.end();++ip)
		newGroups.push_back(MulticastGroup::deriveMulticastGroupForAddressResolution(*ip));

	std::sort(newGroups.begin(),newGroups.end());
	newGroups.erase(std::unique(newGroups.begin(),newGroups.end()),newGroups.end());

	for(std::vector<MulticastGroup>::iterator m(newGroups.begin());m!=newGroups.end();++m) {
		if (!std::binary_search(_multicastGroups.begin(),_multicastGroups.end(),*m))
			added.push_back(*m);
	}
	for(std::vector<MulticastGroup>::iterator m(_multicastGroups.begin());m!=_multicastGroups.end();++m) {
		if (!std::binary_search(newGroups.begin(),newGroups.end(),*m))
			removed.push_back(*m);
	}

	_multicastGroups.swap(newGroups);
}

void LinuxEthernetTap::setMtu(unsigned int mtu)
{
	if (_mtu != mtu) {
		_mtu = mtu;
		int sock = socket(AF_INET,SOCK_DGRAM,0);
		if (sock > 0) {
			struct ifreq ifr;
			memset(&ifr,0,sizeof(ifr));
			ifr.ifr_ifru.ifru_mtu = (int)mtu;
			ioctl(sock,SIOCSIFMTU,(void *)&ifr);
			close(sock);
		}
	}
}

void LinuxEthernetTap::threadMain()
	throw()
{
	fd_set readfds,nullfds;
	MAC to,from;
	int n,nfds,r;
	char getBuf[ZT_MAX_MTU + 64];

	Thread::sleep(500);

	FD_ZERO(&readfds);
	FD_ZERO(&nullfds);
	nfds = (int)std::max(_shutdownSignalPipe[0],_fd) + 1;

	r = 0;
	for(;;) {
		FD_SET(_shutdownSignalPipe[0],&readfds);
		FD_SET(_fd,&readfds);
		select(nfds,&readfds,&nullfds,&nullfds,(struct timeval *)0);

		if (FD_ISSET(_shutdownSignalPipe[0],&readfds)) // writes to shutdown pipe terminate thread
			break;

		if (FD_ISSET(_fd,&readfds)) {
			n = (int)::read(_fd,getBuf + r,sizeof(getBuf) - r);
			if (n < 0) {
				if ((errno != EINTR)&&(errno != ETIMEDOUT))
					break;
			} else {
				// Some tap drivers like to send the ethernet frame and the
				// payload in two chunks, so handle that by accumulating
				// data until we have at least a frame.
				r += n;
				if (r > 14) {
					if (r > ((int)_mtu + 14)) // sanity check for weird TAP behavior on some platforms
						r = _mtu + 14;

					if (_enabled) {
						to.setTo(getBuf,6);
						from.setTo(getBuf + 6,6);
						unsigned int etherType = ntohs(((const uint16_t *)getBuf)[6]);
						// TODO: VLAN support
						_handler(_arg,(void *)0,_nwid,from,to,etherType,0,(const void *)(getBuf + 14),r - 14);
					}

					r = 0;
				}
			}
		}
	}
}

} // namespace ZeroTier

#endif // __LINUX__

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

#include "PostgreSQL.hpp"

#ifdef ZT_CONTROLLER_USE_LIBPQ

#include "../node/Constants.hpp"
#include "EmbeddedNetworkController.hpp"
#include "RabbitMQ.hpp"
#include "../version.h"

#include <libpq-fe.h>
#include <sstream>
#include <amqp.h>
#include <amqp_tcp_socket.h>

using json = nlohmann::json;

namespace {

static const int DB_MINIMUM_VERSION = 5;

static const char *_timestr()
{
	time_t t = time(0);
	char *ts = ctime(&t);
	char *p = ts;
	if (!p)
		return "";
	while (*p) {
		if (*p == '\n') {
			*p = (char)0;
			break;
		}
		++p;
	}
	return ts;
}

/*
std::string join(const std::vector<std::string> &elements, const char * const separator)
{
	switch(elements.size()) {
	case 0:
		return "";
	case 1:
		return elements[0];
	default:
		std::ostringstream os;
		std::copy(elements.begin(), elements.end()-1, std::ostream_iterator<std::string>(os, separator));
		os << *elements.rbegin();
		return os.str();
	}
}
*/

} // anonymous namespace

using namespace ZeroTier;

PostgreSQL::PostgreSQL(const Identity &myId, const char *path, int listenPort, MQConfig *mqc)
	: DB()
	, _myId(myId)
	, _myAddress(myId.address())
	, _ready(0)
	, _connected(1)
	, _run(1)
	, _waitNoticePrinted(false)
	, _listenPort(listenPort)
	, _mqc(mqc)
{
	char myAddress[64];
	_myAddressStr = myId.address().toString(myAddress);
	_connString = std::string(path) + " application_name=controller_" + _myAddressStr;

	// Database Schema Version Check
	PGconn *conn = getPgConn();
	if (PQstatus(conn) != CONNECTION_OK) {
		fprintf(stderr, "Bad Database Connection: %s", PQerrorMessage(conn));
		exit(1);
	}

	PGresult *res = PQexec(conn, "SELECT version FROM ztc_database");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		fprintf(stderr, "Error determining database version");
		exit(1);
	}

	if (PQntuples(res) != 1) {
		fprintf(stderr, "Invalid number of db version tuples returned.");
		exit(1);
	}

	int dbVersion = std::stoi(PQgetvalue(res, 0, 0));

	if (dbVersion < DB_MINIMUM_VERSION) {
		fprintf(stderr, "Central database schema version too low.  This controller version requires a minimum schema version of %d. Please upgrade your Central instance", DB_MINIMUM_VERSION);
		exit(1);
	}

	PQclear(res);
	res = NULL;
	PQfinish(conn);
	conn = NULL;

	_readyLock.lock();
	_heartbeatThread = std::thread(&PostgreSQL::heartbeat, this);
	_membersDbWatcher = std::thread(&PostgreSQL::membersDbWatcher, this);
	_networksDbWatcher = std::thread(&PostgreSQL::networksDbWatcher, this);
	for (int i = 0; i < ZT_CENTRAL_CONTROLLER_COMMIT_THREADS; ++i) {
		_commitThread[i] = std::thread(&PostgreSQL::commitThread, this);
	}
	_onlineNotificationThread = std::thread(&PostgreSQL::onlineNotificationThread, this);
}

PostgreSQL::~PostgreSQL()
{
	_run = 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	_heartbeatThread.join();
	_membersDbWatcher.join();
	_networksDbWatcher.join();
	for (int i = 0; i < ZT_CENTRAL_CONTROLLER_COMMIT_THREADS; ++i) {
		_commitThread[i].join();
	}
	_onlineNotificationThread.join();

}


bool PostgreSQL::waitForReady()
{
	while (_ready < 2) {
		if (!_waitNoticePrinted) {
			_waitNoticePrinted = true;
			fprintf(stderr, "[%s] NOTICE: %.10llx controller PostgreSQL waiting for initial data download..." ZT_EOL_S, ::_timestr(), (unsigned long long)_myAddress.toInt());
		}
		_readyLock.lock();
		_readyLock.unlock();
	}
	return true;
}

bool PostgreSQL::isReady()
{
	return ((_ready == 2)&&(_connected));
}

bool PostgreSQL::save(nlohmann::json &record,bool notifyListeners)
{
	bool modified = false;
	try {
		if (!record.is_object())
			return false;
		const std::string objtype = record["objtype"];
		if (objtype == "network") {
			const uint64_t nwid = OSUtils::jsonIntHex(record["id"],0ULL);
			if (nwid) {
				nlohmann::json old;
				get(nwid,old);
				if ((!old.is_object())||(!_compareRecords(old,record))) {
					record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1ULL;
					_commitQueue.post(std::pair<nlohmann::json,bool>(record,notifyListeners));
					modified = true;
				}
			}
		} else if (objtype == "member") {
			const uint64_t nwid = OSUtils::jsonIntHex(record["nwid"],0ULL);
			const uint64_t id = OSUtils::jsonIntHex(record["id"],0ULL);
			if ((id)&&(nwid)) {
				nlohmann::json network,old;
				get(nwid,network,id,old);
				if ((!old.is_object())||(!_compareRecords(old,record))) {
					record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1ULL;
					_commitQueue.post(std::pair<nlohmann::json,bool>(record,notifyListeners));
					modified = true;
				}
			}
		}
	} catch (std::exception &e) {
		fprintf(stderr, "Error on PostgreSQL::save: %s\n", e.what());
	} catch (...) {
		fprintf(stderr, "Unknown error on PostgreSQL::save\n");
	}
	return modified;
}

void PostgreSQL::eraseNetwork(const uint64_t networkId)
{
	char tmp2[24];
	waitForReady();
	Utils::hex(networkId, tmp2);
	std::pair<nlohmann::json,bool> tmp;
	tmp.first["id"] = tmp2;
	tmp.first["objtype"] = "_delete_network";
	tmp.second = true;
	_commitQueue.post(tmp);
}

void PostgreSQL::eraseMember(const uint64_t networkId, const uint64_t memberId)
{
	char tmp2[24];
	std::pair<nlohmann::json,bool> tmp;
	Utils::hex(networkId, tmp2);
	tmp.first["nwid"] = tmp2;
	Utils::hex(memberId, tmp2);
	tmp.first["id"] = tmp2;
	tmp.first["objtype"] = "_delete_member";
	tmp.second = true;
	_commitQueue.post(tmp);
}

void PostgreSQL::nodeIsOnline(const uint64_t networkId, const uint64_t memberId, const InetAddress &physicalAddress)
{
	std::lock_guard<std::mutex> l(_lastOnline_l);
	std::pair<int64_t, InetAddress> &i = _lastOnline[std::pair<uint64_t,uint64_t>(networkId, memberId)];
	i.first = OSUtils::now();
	if (physicalAddress) {
		i.second = physicalAddress;
	}
}

void PostgreSQL::initializeNetworks(PGconn *conn)
{
	try {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "Bad Database Connection: %s", PQerrorMessage(conn));
			exit(1);
		}

		const char *params[1] = {
			_myAddressStr.c_str()
		};

		PGresult *res = PQexecParams(conn, "SELECT id, EXTRACT(EPOCH FROM creation_time AT TIME ZONE 'UTC')*1000, capabilities, "
			"enable_broadcast, EXTRACT(EPOCH FROM last_modified AT TIME ZONE 'UTC')*1000, mtu, multicast_limit, name, private, remote_trace_level, "
			"remote_trace_target, revision, rules, tags, v4_assign_mode, v6_assign_mode FROM ztc_network "
			"WHERE deleted = false AND controller_id = $1",
			1,
			NULL,
			params,
			NULL,
			NULL,
			0);

		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			fprintf(stderr, "Networks Initialization Failed: %s", PQerrorMessage(conn));
			PQclear(res);
			exit(1);
		}

		int numRows = PQntuples(res);
		for (int i = 0; i < numRows; ++i) {
			json empty;
			json config;

			const char *nwidparam[1] = {
				PQgetvalue(res, i, 0)
			};

			config["id"] = PQgetvalue(res, i, 0);
			config["nwid"] = PQgetvalue(res, i, 0);
			try {
				config["creationTime"] = std::stoull(PQgetvalue(res, i, 1));
			} catch (std::exception &e) {
				config["creationTime"] = 0ULL;
				//fprintf(stderr, "Error converting creation time: %s\n", PQgetvalue(res, i, 1));
			}
			config["capabilities"] = json::parse(PQgetvalue(res, i, 2));
			config["enableBroadcast"] = (strcmp(PQgetvalue(res, i, 3),"t")==0);
			try {
				config["lastModified"] = std::stoull(PQgetvalue(res, i, 4));
			} catch (std::exception &e) {
				config["lastModified"] = 0ULL;
				//fprintf(stderr, "Error converting last modified: %s\n", PQgetvalue(res, i, 4));
			}
			try {
				config["mtu"] = std::stoi(PQgetvalue(res, i, 5));
			} catch (std::exception &e) {
				config["mtu"] = 2800;
			}
			try {
				config["multicastLimit"] = std::stoi(PQgetvalue(res, i, 6));
			} catch (std::exception &e) {
				config["multicastLimit"] = 64;
			}
			config["name"] = PQgetvalue(res, i, 7);
			config["private"] = (strcmp(PQgetvalue(res, i, 8),"t")==0);
			try {
				config["remoteTraceLevel"] = std::stoi(PQgetvalue(res, i, 9));
			} catch (std::exception &e) {
				config["remoteTraceLevel"] = 0;
			}
			config["remoteTraceTarget"] = PQgetvalue(res, i, 10);
			try {
				config["revision"] = std::stoull(PQgetvalue(res, i, 11));
			} catch (std::exception &e) {
				config["revision"] = 0ULL;
				//fprintf(stderr, "Error converting revision: %s\n", PQgetvalue(res, i, 11));
			}
			config["rules"] = json::parse(PQgetvalue(res, i, 12));
			config["tags"] = json::parse(PQgetvalue(res, i, 13));
			config["v4AssignMode"] = json::parse(PQgetvalue(res, i, 14));
			config["v6AssignMode"] = json::parse(PQgetvalue(res, i, 15));
			config["objtype"] = "network";
			config["ipAssignmentPools"] = json::array();
			config["routes"] = json::array();

			PGresult *r2 = PQexecParams(conn,
				"SELECT host(ip_range_start), host(ip_range_end) FROM ztc_network_assignment_pool WHERE network_id = $1",
				1,
				NULL,
				nwidparam,
				NULL,
				NULL,
				0);

			if (PQresultStatus(r2) != PGRES_TUPLES_OK) {
				fprintf(stderr, "ERROR: Error retreiving IP pools for network: %s\n", PQresultErrorMessage(r2));
				PQclear(r2);
				PQclear(res);
				exit(1);
			}

			int n = PQntuples(r2);
			for (int j = 0; j < n; ++j) {
				json ip;
				ip["ipRangeStart"] = PQgetvalue(r2, j, 0);
				ip["ipRangeEnd"] = PQgetvalue(r2, j, 1);

				config["ipAssignmentPools"].push_back(ip);
			}

			PQclear(r2);

			r2 = PQexecParams(conn,
				"SELECT host(address), bits, host(via) FROM ztc_network_route WHERE network_id = $1",
				1,
				NULL,
				nwidparam,
				NULL,
				NULL,
				0);

			if (PQresultStatus(r2) != PGRES_TUPLES_OK) {
				fprintf(stderr, "ERROR: Error retreiving routes for network: %s\n", PQresultErrorMessage(r2));
				PQclear(r2);
				PQclear(res);
				exit(1);
			}

			n = PQntuples(r2);
			for (int j = 0; j < n; ++j) {
				std::string addr = PQgetvalue(r2, j, 0);
				std::string bits = PQgetvalue(r2, j, 1);
				std::string via = PQgetvalue(r2, j, 2);
				json route;
				route["target"] = addr + "/" + bits;

				if (via == "NULL") {
					route["via"] = nullptr;
				} else {
					route["via"] = via;
				}
				config["routes"].push_back(route);
			}

			PQclear(r2);

			_networkChanged(empty, config, false);
		}

		PQclear(res);

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(stderr,"[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S,_timestr(),(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
	} catch (std::exception &e) {
		fprintf(stderr, "ERROR: Error initializing networks: %s", e.what());
		exit(-1);
	}
}

void PostgreSQL::initializeMembers(PGconn *conn)
{
	try {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "Bad Database Connection: %s", PQerrorMessage(conn));
			exit(1);
		}

		const char *params[1] = {
			_myAddressStr.c_str()
		};

		PGresult *res = PQexecParams(conn,
			"SELECT m.id, m.network_id, m.active_bridge, m.authorized, m.capabilities, EXTRACT(EPOCH FROM m.creation_time AT TIME ZONE 'UTC')*1000, m.identity, "
			"	EXTRACT(EPOCH FROM m.last_authorized_time AT TIME ZONE 'UTC')*1000, "
			"	EXTRACT(EPOCH FROM m.last_deauthorized_time AT TIME ZONE 'UTC')*1000, "
			"	m.remote_trace_level, m.remote_trace_target, m.tags, m.v_major, m.v_minor, m.v_rev, m.v_proto, "
			"	m.no_auto_assign_ips, m.revision "
			"FROM ztc_member m "
			"INNER JOIN ztc_network n "
			"	ON n.id = m.network_id "
			"WHERE n.controller_id = $1 AND m.deleted = false",
			1,
			NULL,
			params,
			NULL,
			NULL,
			0);

		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			fprintf(stderr, "Member Initialization Failed: %s", PQerrorMessage(conn));
			PQclear(res);
			exit(1);
		}

		int numRows = PQntuples(res);
		for (int i = 0; i < numRows; ++i) {
			json empty;
			json config;

			std::string memberId(PQgetvalue(res, i, 0));
			std::string networkId(PQgetvalue(res, i, 1));
			std::string ctime = PQgetvalue(res, i, 5);
			config["id"] = memberId;
			config["nwid"] = networkId;
			config["activeBridge"] = (strcmp(PQgetvalue(res, i, 2), "t") == 0);
			config["authorized"] = (strcmp(PQgetvalue(res, i, 3), "t") == 0);
			try {
				config["capabilities"] = json::parse(PQgetvalue(res, i, 4));
			} catch (std::exception &e) {
				config["capabilities"] = json::array();
			}
			try {
				config["creationTime"] = std::stoull(PQgetvalue(res, i, 5));
			} catch (std::exception &e) {
				config["creationTime"] = 0ULL;
				//fprintf(stderr, "Error upding creation time (member): %s\n", PQgetvalue(res, i, 5));
			}
			config["identity"] = PQgetvalue(res, i, 6);
			try {
				config["lastAuthorizedTime"] = std::stoull(PQgetvalue(res, i, 7));
			} catch(std::exception &e) {
				config["lastAuthorizedTime"] = 0ULL;
				//fprintf(stderr, "Error updating last auth time (member): %s\n", PQgetvalue(res, i, 7));
			}
			try {
				config["lastDeauthorizedTime"] = std::stoull(PQgetvalue(res, i, 8));
			} catch( std::exception &e) {
				config["lastDeauthorizedTime"] = 0ULL;
				//fprintf(stderr, "Error updating last deauth time (member): %s\n", PQgetvalue(res, i, 8));
			}
			try {
				config["remoteTraceLevel"] = std::stoi(PQgetvalue(res, i, 9));
			} catch (std::exception &e) {
				config["remoteTraceLevel"] = 0;
			}
			config["remoteTraceTarget"] = PQgetvalue(res, i, 10);
			try {
				config["tags"] = json::parse(PQgetvalue(res, i, 11));
			} catch (std::exception &e) {
				config["tags"] = json::array();
			}
			try {
				config["vMajor"] = std::stoi(PQgetvalue(res, i, 12));
			} catch(std::exception &e) {
				config["vMajor"] = -1;
			}
			try {
				config["vMinor"] = std::stoi(PQgetvalue(res, i, 13));
			} catch (std::exception &e) {
				config["vMinor"] = -1;
			}
			try {
				config["vRev"] = std::stoi(PQgetvalue(res, i, 14));
			} catch (std::exception &e) {
				config["vRev"] = -1;
			}
			try {
				config["vProto"] = std::stoi(PQgetvalue(res, i, 15));
			} catch (std::exception &e) {
				config["vProto"] = -1;
			}
			config["noAutoAssignIps"] = (strcmp(PQgetvalue(res, i, 16), "t") == 0);
			try {
				config["revision"] = std::stoull(PQgetvalue(res, i, 17));
			} catch (std::exception &e) {
				config["revision"] = 0ULL;
				//fprintf(stderr, "Error updating revision (member): %s\n", PQgetvalue(res, i, 17));
			}
			config["objtype"] = "member";
			config["ipAssignments"] = json::array();
			const char *p2[2] = {
				memberId.c_str(),
				networkId.c_str()
			};

			PGresult *r2 = PQexecParams(conn,
				"SELECT DISTINCT address FROM ztc_member_ip_assignment WHERE member_id = $1 AND network_id = $2",
				2,
				NULL,
				p2,
				NULL,
				NULL,
				0);

			if (PQresultStatus(r2) != PGRES_TUPLES_OK) {
				fprintf(stderr, "Member Initialization Failed: %s", PQerrorMessage(conn));
				PQclear(r2);
				PQclear(res);
				exit(1);
			}

			int n = PQntuples(r2);
			for (int j = 0; j < n; ++j) {
				config["ipAssignments"].push_back(PQgetvalue(r2, j, 0));
			}

			_memberChanged(empty, config, false);
		}

		PQclear(res);

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(stderr,"[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S,_timestr(),(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
	} catch (std::exception &e) {
		fprintf(stderr, "ERROR: Error initializing members: %s\n", e.what());
		exit(-1);
	}
}

void PostgreSQL::heartbeat()
{
	char publicId[1024];
	char hostnameTmp[1024];
	_myId.toString(false,publicId);
	if (gethostname(hostnameTmp, sizeof(hostnameTmp))!= 0) {
		hostnameTmp[0] = (char)0;
	} else {
		for (int i = 0; i < (int)sizeof(hostnameTmp); ++i) {
			if ((hostnameTmp[i] == '.')||(hostnameTmp[i] == 0)) {
				hostnameTmp[i] = (char)0;
				break;
			}
		}
	}
	const char *controllerId = _myAddressStr.c_str();
	const char *publicIdentity = publicId;
	const char *hostname = hostnameTmp;

	PGconn *conn = getPgConn();
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	while (_run == 1) {
		if(PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "%s heartbeat thread lost connection to Database\n", _myAddressStr.c_str());
			PQfinish(conn);
			exit(6);
		}
		if (conn) {
			std::string major = std::to_string(ZEROTIER_ONE_VERSION_MAJOR);
			std::string minor = std::to_string(ZEROTIER_ONE_VERSION_MINOR);
			std::string rev = std::to_string(ZEROTIER_ONE_VERSION_REVISION);
			std::string build = std::to_string(ZEROTIER_ONE_VERSION_BUILD);
			std::string now = std::to_string(OSUtils::now());
			std::string host_port = std::to_string(_listenPort);
			std::string use_rabbitmq = (_mqc != NULL) ? "true" : "false";
			const char *values[10] = {
				controllerId,
				hostname,
				now.c_str(),
				publicIdentity,
				major.c_str(),
				minor.c_str(),
				rev.c_str(),
				build.c_str(),
				host_port.c_str(),
				use_rabbitmq.c_str()
			};

			PGresult *res = PQexecParams(conn,
				"INSERT INTO ztc_controller (id, cluster_host, last_alive, public_identity, v_major, v_minor, v_rev, v_build, host_port, use_rabbitmq) "
				"VALUES ($1, $2, TO_TIMESTAMP($3::double precision/1000), $4, $5, $6, $7, $8, $9, $10) "
				"ON CONFLICT (id) DO UPDATE SET cluster_host = EXCLUDED.cluster_host, last_alive = EXCLUDED.last_alive, "
				"public_identity = EXCLUDED.public_identity, v_major = EXCLUDED.v_major, v_minor = EXCLUDED.v_minor, "
				"v_rev = EXCLUDED.v_rev, v_build = EXCLUDED.v_rev, host_port = EXCLUDED.host_port, "
				"use_rabbitmq = EXCLUDED.use_rabbitmq",
				10,	   // number of parameters
				NULL,	// oid field.   ignore
				values,  // values for substitution
				NULL, // lengths in bytes of each value
				NULL,  // binary?
				0);

			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				fprintf(stderr, "Heartbeat Update Failed: %s\n", PQresultErrorMessage(res));
			}
			PQclear(res);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	PQfinish(conn);
	conn = NULL;
}

void PostgreSQL::membersDbWatcher()
{
	PGconn *conn = getPgConn(NO_OVERRIDE);
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	initializeMembers(conn);

	if (this->_mqc != NULL) {
		PQfinish(conn);
		conn = NULL;
		_membersWatcher_RabbitMQ();
	} else {
		_membersWatcher_Postgres(conn);
		PQfinish(conn);
		conn = NULL;
	}

	if (_run == 1) {
		fprintf(stderr, "ERROR: %s membersDbWatcher should still be running! Exiting Controller.\n", _myAddressStr.c_str());
		exit(9);
	}
	fprintf(stderr, "Exited membersDbWatcher\n");
}

void PostgreSQL::_membersWatcher_Postgres(PGconn *conn) {
	char buf[11] = {0};
	std::string cmd = "LISTEN member_" + std::string(_myAddress.toString(buf));
	PGresult *res = PQexec(conn, cmd.c_str());
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
		fprintf(stderr, "LISTEN command failed: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res); res = NULL;

	while(_run == 1) {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "ERROR: Member Watcher lost connection to Postgres.");
			exit(-1);
		}
		PGnotify *notify = NULL;
		PQconsumeInput(conn);
		while ((notify = PQnotifies(conn)) != NULL) {
			//fprintf(stderr, "ASYNC NOTIFY of '%s' id:%s received\n", notify->relname, notify->extra);

			try {
				json tmp(json::parse(notify->extra));
				json &ov = tmp["old_val"];
				json &nv = tmp["new_val"];
				json oldConfig, newConfig;
				if (ov.is_object()) oldConfig = ov;
				if (nv.is_object()) newConfig = nv;
				if (oldConfig.is_object() || newConfig.is_object()) {
					_memberChanged(oldConfig,newConfig,(this->_ready>=2));
				}
			} catch (...) {} // ignore bad records

			free(notify);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void PostgreSQL::_membersWatcher_RabbitMQ() {
	char buf[11] = {0};
	std::string qname = "member_"+ std::string(_myAddress.toString(buf));
	RabbitMQ rmq(_mqc, qname.c_str());
	try {
		rmq.init();
	} catch (std::runtime_error &e) {
		fprintf(stderr, "RABBITMQ ERROR: %s\n", e.what());
		exit(11);
	}
	while (_run == 1) {
		try {
			std::string msg = rmq.consume();
			// fprintf(stderr, "Got Member Update: %s\n", msg.c_str());
			if (msg.empty()) {
				continue;
			}
			json tmp(json::parse(msg));
			json &ov = tmp["old_val"];
			json &nv = tmp["new_val"];
			json oldConfig, newConfig;
			if (ov.is_object()) oldConfig = ov;
			if (nv.is_object()) newConfig = nv;
			if (oldConfig.is_object() || newConfig.is_object()) {
				_memberChanged(oldConfig,newConfig,(this->_ready>=2));
			}
		} catch (std::runtime_error &e) {
			fprintf(stderr, "RABBITMQ ERROR member change: %s\n", e.what());
			break;
		} catch(std::exception &e ) {
			fprintf(stderr, "RABBITMQ ERROR member change: %s\n", e.what());
		} catch(...) {
			fprintf(stderr, "RABBITMQ ERROR member change: unknown error\n");
		}
	}
}

void PostgreSQL::networksDbWatcher()
{
	PGconn *conn = getPgConn(NO_OVERRIDE);
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	initializeNetworks(conn);

	if (this->_mqc != NULL) {
		PQfinish(conn);
		conn = NULL;
		_networksWatcher_RabbitMQ();
	} else {
		_networksWatcher_Postgres(conn);
		PQfinish(conn);
		conn = NULL;
	}

	if (_run == 1) {
		fprintf(stderr, "ERROR: %s networksDbWatcher should still be running! Exiting Controller.\n", _myAddressStr.c_str());
		exit(8);
	}
	fprintf(stderr, "Exited membersDbWatcher\n");
}

void PostgreSQL::_networksWatcher_Postgres(PGconn *conn) {
	char buf[11] = {0};
	std::string cmd = "LISTEN network_" + std::string(_myAddress.toString(buf));
	PGresult *res = PQexec(conn, cmd.c_str());
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
		fprintf(stderr, "LISTEN command failed: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res); res = NULL;

	while(_run == 1) {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "ERROR: Network Watcher lost connection to Postgres.");
			exit(-1);
		}
		PGnotify *notify = NULL;
		PQconsumeInput(conn);
		while ((notify = PQnotifies(conn)) != NULL) {
			//fprintf(stderr, "ASYNC NOTIFY of '%s' id:%s received\n", notify->relname, notify->extra);
			try {
				json tmp(json::parse(notify->extra));
				json &ov = tmp["old_val"];
				json &nv = tmp["new_val"];
				json oldConfig, newConfig;
				if (ov.is_object()) oldConfig = ov;
				if (nv.is_object()) newConfig = nv;
				if (oldConfig.is_object()||newConfig.is_object()) {
					_networkChanged(oldConfig,newConfig,(this->_ready >= 2));
				}
			} catch (...) {} // ignore bad records
			free(notify);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void PostgreSQL::_networksWatcher_RabbitMQ() {
	char buf[11] = {0};
	std::string qname = "network_"+ std::string(_myAddress.toString(buf));
	RabbitMQ rmq(_mqc, qname.c_str());
	try {
		rmq.init();
	} catch (std::runtime_error &e) {
		fprintf(stderr, "RABBITMQ ERROR: %s\n", e.what());
		exit(11);
	}
	while (_run == 1) {
		try {
			std::string msg = rmq.consume();
			if (msg.empty()) {
				continue;
			}
			// fprintf(stderr, "Got network update: %s\n", msg.c_str());
			json tmp(json::parse(msg));
			json &ov = tmp["old_val"];
			json &nv = tmp["new_val"];
			json oldConfig, newConfig;
			if (ov.is_object()) oldConfig = ov;
			if (nv.is_object()) newConfig = nv;
			if (oldConfig.is_object()||newConfig.is_object()) {
				_networkChanged(oldConfig,newConfig,(this->_ready >= 2));
			}
		} catch (std::runtime_error &e) {
			fprintf(stderr, "RABBITMQ ERROR: %s\n", e.what());
			break;
		} catch (std::exception &e) {
			fprintf(stderr, "RABBITMQ ERROR network watcher: %s\n", e.what());
		} catch(...) {
			fprintf(stderr, "RABBITMQ ERROR network watcher: unknown error\n");
		}
	}
}

void PostgreSQL::commitThread()
{
	PGconn *conn = getPgConn();
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "ERROR: Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	std::pair<nlohmann::json,bool> qitem;
	while(_commitQueue.get(qitem)&(_run == 1)) {
		if (!qitem.first.is_object()) {
			continue;
		}
		if (PQstatus(conn) == CONNECTION_BAD) {
			fprintf(stderr, "ERROR: Connection to database failed: %s\n", PQerrorMessage(conn));
			PQfinish(conn);
			exit(1);
		}
		try {
			nlohmann::json *config = &(qitem.first);
			const std::string objtype = (*config)["objtype"];
			if (objtype == "member") {
				try {
					std::string memberId = (*config)["id"];
					std::string networkId = (*config)["nwid"];
					std::string identity = (*config)["identity"];
					std::string target = "NULL";

					if (!(*config)["remoteTraceTarget"].is_null()) {
						target = (*config)["remoteTraceTarget"];
					}

					std::string caps = OSUtils::jsonDump((*config)["capabilities"], -1);
					std::string lastAuthTime = std::to_string((long long)(*config)["lastAuthorizedTime"]);
					std::string lastDeauthTime = std::to_string((long long)(*config)["lastDeauthorizedTime"]);
					std::string rtraceLevel = std::to_string((int)(*config)["remoteTraceLevel"]);
					std::string rev = std::to_string((unsigned long long)(*config)["revision"]);
					std::string tags = OSUtils::jsonDump((*config)["tags"], -1);
					std::string vmajor = std::to_string((int)(*config)["vMajor"]);
					std::string vminor = std::to_string((int)(*config)["vMinor"]);
					std::string vrev = std::to_string((int)(*config)["vRev"]);
					std::string vproto = std::to_string((int)(*config)["vProto"]);
					const char *values[19] = {
						memberId.c_str(),
						networkId.c_str(),
						((*config)["activeBridge"] ? "true" : "false"),
						((*config)["authorized"] ? "true" : "false"),
						caps.c_str(),
						identity.c_str(),
						lastAuthTime.c_str(),
						lastDeauthTime.c_str(),
						((*config)["noAutoAssignIps"] ? "true" : "false"),
						rtraceLevel.c_str(),
						(target == "NULL") ? NULL : target.c_str(),
						rev.c_str(),
						tags.c_str(),
						vmajor.c_str(),
						vminor.c_str(),
						vrev.c_str(),
						vproto.c_str()
					};

					PGresult *res = PQexecParams(conn,
						"INSERT INTO ztc_member (id, network_id, active_bridge, authorized, capabilities, "
						"identity, last_authorized_time, last_deauthorized_time, no_auto_assign_ips, "
						"remote_trace_level, remote_trace_target, revision, tags, v_major, v_minor, v_rev, v_proto) "
						"VALUES ($1, $2, $3, $4, $5, $6, "
						"TO_TIMESTAMP($7::double precision/1000), TO_TIMESTAMP($8::double precision/1000), "
						"$9, $10, $11, $12, $13, $14, $15, $16, $17) ON CONFLICT (network_id, id) DO UPDATE SET "
						"active_bridge = EXCLUDED.active_bridge, authorized = EXCLUDED.authorized, capabilities = EXCLUDED.capabilities, "
						"identity = EXCLUDED.identity, last_authorized_time = EXCLUDED.last_authorized_time, "
						"last_deauthorized_time = EXCLUDED.last_deauthorized_time, no_auto_assign_ips = EXCLUDED.no_auto_assign_ips, "
						"remote_trace_level = EXCLUDED.remote_trace_level, remote_trace_target = EXCLUDED.remote_trace_target, "
						"revision = EXCLUDED.revision+1, tags = EXCLUDED.tags, v_major = EXCLUDED.v_major, "
						"v_minor = EXCLUDED.v_minor, v_rev = EXCLUDED.v_rev, v_proto = EXCLUDED.v_proto",
						17,
						NULL,
						values,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating member: %s\n", PQresultErrorMessage(res));
						fprintf(stderr, "%s", OSUtils::jsonDump(*config, 2).c_str());
						PQclear(res);
						delete config;
						config = nullptr;
						continue;
					}

					PQclear(res);

					res = PQexec(conn, "BEGIN");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error beginning transaction: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						delete config;
						config = nullptr;
						continue;
					}

					PQclear(res);

					const char *v2[2] = {
						memberId.c_str(),
						networkId.c_str()
					};

					res = PQexecParams(conn,
						"DELETE FROM ztc_member_ip_assignment WHERE member_id = $1 AND network_id = $2",
						2,
						NULL,
						v2,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating IP address assignments: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						PQclear(PQexec(conn, "ROLLBACK"));;
						delete config;
						config = nullptr;
						continue;
					}

					PQclear(res);

					std::vector<std::string> assignments;
					for (auto i = (*config)["ipAssignments"].begin(); i != (*config)["ipAssignments"].end(); ++i) {
						std::string addr = *i;

						if (std::find(assignments.begin(), assignments.end(), addr) != assignments.end()) {
							continue;
						}

						const char *v3[3] = {
							memberId.c_str(),
							networkId.c_str(),
							addr.c_str()
						};

						res = PQexecParams(conn,
							"INSERT INTO ztc_member_ip_assignment (member_id, network_id, address) VALUES ($1, $2, $3)",
							3,
							NULL,
							v3,
							NULL,
							NULL,
							0);

						if (PQresultStatus(res) != PGRES_COMMAND_OK) {
							fprintf(stderr, "ERROR: Error setting IP addresses for member: %s\n", PQresultErrorMessage(res));
							PQclear(res);
							PQclear(PQexec(conn, "ROLLBACK"));
							break;;
						}
					}

					res = PQexec(conn, "COMMIT");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error committing ip address data: %s\n", PQresultErrorMessage(res));
					}

					PQclear(res);

					const uint64_t nwidInt = OSUtils::jsonIntHex((*config)["nwid"], 0ULL);
					const uint64_t memberidInt = OSUtils::jsonIntHex((*config)["id"], 0ULL);
					if (nwidInt && memberidInt) {
						nlohmann::json nwOrig;
						nlohmann::json memOrig;

						nlohmann::json memNew(*config);

						get(nwidInt, nwOrig, memberidInt, memOrig);

						_memberChanged(memOrig, memNew, qitem.second);
					} else {
						fprintf(stderr, "Can't notify of change.  Error parsing nwid or memberid: %llu-%llu\n", (unsigned long long)nwidInt, (unsigned long long)memberidInt);
					}

				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error updating member: %s\n", e.what());
				}
			} else if (objtype == "network") {
				try {
					std::string id = (*config)["id"];
					std::string controllerId = _myAddressStr.c_str();
					std::string name = (*config)["name"];
					std::string remoteTraceTarget("NULL");
					if (!(*config)["remoteTraceTarget"].is_null()) {
						remoteTraceTarget = (*config)["remoteTraceTarget"];
					}
					std::string rulesSource;
					if ((*config)["rulesSource"].is_string()) {
						rulesSource = (*config)["rulesSource"];
					}
					std::string caps = OSUtils::jsonDump((*config)["capabilitles"], -1);
					std::string now = std::to_string(OSUtils::now());
					std::string mtu = std::to_string((int)(*config)["mtu"]);
					std::string mcastLimit = std::to_string((int)(*config)["multicastLimit"]);
					std::string rtraceLevel = std::to_string((int)(*config)["remoteTraceLevel"]);
					std::string rules = OSUtils::jsonDump((*config)["rules"], -1);
					std::string tags = OSUtils::jsonDump((*config)["tags"], -1);
					std::string v4mode = OSUtils::jsonDump((*config)["v4AssignMode"],-1);
					std::string v6mode = OSUtils::jsonDump((*config)["v6AssignMode"], -1);
					bool enableBroadcast = (*config)["enableBroadcast"];
					bool isPrivate = (*config)["private"];

					const char *values[16] = {
						id.c_str(),
						controllerId.c_str(),
						caps.c_str(),
						enableBroadcast ? "true" : "false",
						now.c_str(),
						mtu.c_str(),
						mcastLimit.c_str(),
						name.c_str(),
						isPrivate ? "true" : "false",
						rtraceLevel.c_str(),
						(remoteTraceTarget == "NULL" ? NULL : remoteTraceTarget.c_str()),
						rules.c_str(),
						rulesSource.c_str(),
						tags.c_str(),
						v4mode.c_str(),
						v6mode.c_str(),
					};

					// This ugly query exists because when we want to mirror networks to/from
					// another data store (e.g. FileDB or LFDB) it is possible to get a network
					// that doesn't exist in Central's database. This does an upsert and sets
					// the owner_id to the "first" global admin in the user DB if the record
					// did not previously exist. If the record already exists owner_id is left
					// unchanged, so owner_id should be left out of the update clause.
					PGresult *res = PQexecParams(conn,
						"INSERT INTO ztc_network (id, creation_time, owner_id, controller_id, capabilities, enable_broadcast, "
						"last_modified, mtu, multicast_limit, name, private, "
						"remote_trace_level, remote_trace_target, rules, rules_source, "
						"tags, v4_assign_mode, v6_assign_mode) VALUES ("
						"$1, TO_TIMESTAMP($5::double precision/1000), "
						"(SELECT user_id AS owner_id FROM ztc_global_permissions WHERE authorize = true AND del = true AND modify = true AND read = true LIMIT 1),"
						"$2, $3, $4, TO_TIMESTAMP($5::double precision/1000), "
						"$6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16) "
						"ON CONFLICT (id) DO UPDATE set controller_id = EXCLUDED.controller_id, "
						"capabilities = EXCLUDED.capabilities, enable_broadcast = EXCLUDED.enable_broadcast, "
						"last_modified = EXCLUDED.last_modified, mtu = EXCLUDED.mtu, "
						"multicast_limit = EXCLUDED.multicast_limit, name = EXCLUDED.name, "
						"private = EXCLUDED.private, remote_trace_level = EXCLUDED.remote_trace_level, "
						"remote_trace_target = EXCLUDED.remote_trace_target, rules = EXCLUDED.rules, "
						"rules_source = EXCLUDED.rules_source, tags = EXCLUDED.tags, "
						"v4_assign_mode = EXCLUDED.v4_assign_mode, v6_assign_mode = EXCLUDED.v6_assign_mode",
						16,
						NULL,
						values,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating network record: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						delete config;
						config = nullptr;
						continue;
					}

					PQclear(res);

					res = PQexec(conn, "BEGIN");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error beginnning transaction: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						delete config;
						config = nullptr;
						continue;
					}

					PQclear(res);

					const char *params[1] = {
						id.c_str()
					};
					res = PQexecParams(conn,
						"DELETE FROM ztc_network_assignment_pool WHERE network_id = $1",
						1,
						NULL,
						params,
						NULL,
						NULL,
						0);
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating assignment pool: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						PQclear(PQexec(conn, "ROLLBACK"));
						delete config;
						config = nullptr;
						continue;
					}

					PQclear(res);

					auto pool = (*config)["ipAssignmentPools"];
					bool err = false;
					for (auto i = pool.begin(); i != pool.end(); ++i) {
						std::string start = (*i)["ipRangeStart"];
						std::string end = (*i)["ipRangeEnd"];
						const char *p[3] = {
							id.c_str(),
							start.c_str(),
							end.c_str()
						};

						res = PQexecParams(conn,
							"INSERT INTO ztc_network_assignment_pool (network_id, ip_range_start, ip_range_end) "
							"VALUES ($1, $2, $3)",
							3,
							NULL,
							p,
							NULL,
							NULL,
							0);
						if (PQresultStatus(res) != PGRES_COMMAND_OK) {
							fprintf(stderr, "ERROR: Error updating assignment pool: %s\n", PQresultErrorMessage(res));
							PQclear(res);
							err = true;
							break;
						}
						PQclear(res);
					}
					if (err) {
						PQclear(PQexec(conn, "ROLLBACK"));
						delete config;
						config = nullptr;
						continue;
					}

					res = PQexecParams(conn,
						"DELETE FROM ztc_network_route WHERE network_id = $1",
						1,
						NULL,
						params,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating routes: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						PQclear(PQexec(conn, "ROLLBACK"));
						delete config;
						config = nullptr;
						continue;
					}


					auto routes = (*config)["routes"];
					err = false;
					for (auto i = routes.begin(); i != routes.end(); ++i) {
						std::string t = (*i)["target"];
						std::vector<std::string> target;
						std::istringstream f(t);
						std::string s;
						while(std::getline(f, s, '/')) {
							target.push_back(s);
						}
						if (target.empty() || target.size() != 2) {
							continue;
						}
						std::string targetAddr = target[0];
						std::string targetBits = target[1];
						std::string via = "NULL";
						if (!(*i)["via"].is_null()) {
							via = (*i)["via"];
						}

						const char *p[4] = {
							id.c_str(),
							targetAddr.c_str(),
							targetBits.c_str(),
							(via == "NULL" ? NULL : via.c_str()),
						};

						res = PQexecParams(conn,
							"INSERT INTO ztc_network_route (network_id, address, bits, via) VALUES ($1, $2, $3, $4)",
							4,
							NULL,
							p,
							NULL,
							NULL,
							0);

						if (PQresultStatus(res) != PGRES_COMMAND_OK) {
							fprintf(stderr, "ERROR: Error updating routes: %s\n", PQresultErrorMessage(res));
							PQclear(res);
							err = true;
							break;
						}
						PQclear(res);
					}
					if (err) {
						PQclear(PQexec(conn, "ROLLBACK"));
						delete config;
						config = nullptr;
						continue;
					}

					res = PQexec(conn, "COMMIT");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error committing network update: %s\n", PQresultErrorMessage(res));
					}
					PQclear(res);

					const uint64_t nwidInt = OSUtils::jsonIntHex((*config)["nwid"], 0ULL);
					if (nwidInt) {
						nlohmann::json nwOrig;
						nlohmann::json nwNew(*config);

						get(nwidInt, nwOrig);

						_networkChanged(nwOrig, nwNew, qitem.second);
					} else {
						fprintf(stderr, "Can't notify network changed: %llu\n", (unsigned long long)nwidInt);
					}

				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error updating member: %s\n", e.what());
				}
			} else if (objtype == "_delete_network") {
				try {
					std::string networkId = (*config)["nwid"];
					const char *values[1] = {
						networkId.c_str()
					};
					PGresult * res = PQexecParams(conn,
						"UPDATE ztc_network SET deleted = true WHERE id = $1",
						1,
						NULL,
						values,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error deleting network: %s\n", PQresultErrorMessage(res));
					}

					PQclear(res);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error deleting network: %s\n", e.what());
				}
			} else if (objtype == "_delete_member") {
				try {
					std::string memberId = (*config)["id"];
					std::string networkId = (*config)["nwid"];

					const char *values[2] = {
						memberId.c_str(),
						networkId.c_str()
					};

					PGresult *res = PQexecParams(conn,
						"UPDATE ztc_member SET hidden = true, deleted = true WHERE id = $1 AND network_id = $2",
						2,
						NULL,
						values,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error deleting member: %s\n", PQresultErrorMessage(res));
					}

					PQclear(res);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error deleting member: %s\n", e.what());
				}
			} else {
				fprintf(stderr, "ERROR: unknown objtype");
			}
		} catch (std::exception &e) {
			fprintf(stderr, "ERROR: Error getting objtype: %s\n", e.what());
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	PQfinish(conn);
	if (_run == 1) {
		fprintf(stderr, "ERROR: %s commitThread should still be running! Exiting Controller.\n", _myAddressStr.c_str());
		exit(7);
	}
}

void PostgreSQL::onlineNotificationThread()
{
	PGconn *conn = getPgConn();
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	_connected = 1;

	//int64_t	lastUpdatedNetworkStatus = 0;
	std::unordered_map< std::pair<uint64_t,uint64_t>,int64_t,_PairHasher > lastOnlineCumulative;

	while (_run == 1) {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "ERROR: Online Notification thread lost connection to Postgres.");
			PQfinish(conn);
			exit(5);
		}

		// map used to send notifications to front end
		std::unordered_map<std::string, std::vector<std::string>> updateMap;

		std::unordered_map< std::pair<uint64_t,uint64_t>,std::pair<int64_t,InetAddress>,_PairHasher > lastOnline;
		{
			std::lock_guard<std::mutex> l(_lastOnline_l);
			lastOnline.swap(_lastOnline);
		}

		PGresult *res = NULL;

		std::stringstream memberUpdate;
		memberUpdate << "INSERT INTO ztc_member_status (network_id, member_id, address, last_updated) VALUES ";
		bool firstRun = true;
		bool memberAdded = false;
		for (auto i=lastOnline.begin(); i != lastOnline.end(); ++i) {
			uint64_t nwid_i = i->first.first;
			char nwidTmp[64];
			char memTmp[64];
			char ipTmp[64];
			OSUtils::ztsnprintf(nwidTmp,sizeof(nwidTmp), "%.16llx", nwid_i);
			OSUtils::ztsnprintf(memTmp,sizeof(memTmp), "%.10llx", i->first.second);

			auto found = _networks.find(nwid_i);
			if (found == _networks.end()) {
				continue; // skip members trying to join non-existant networks
			}

			std::string networkId(nwidTmp);
			std::string memberId(memTmp);

			std::vector<std::string> &members = updateMap[networkId];
			members.push_back(memberId);

			lastOnlineCumulative[i->first] = i->second.first;


			const char *qvals[2] = {
				networkId.c_str(),
				memberId.c_str()
			};

			res = PQexecParams(conn,
				"SELECT id, network_id FROM ztc_member WHERE network_id = $1 AND id = $2",
				2,
				NULL,
				qvals,
				NULL,
				NULL,
				0);

			if (PQresultStatus(res) != PGRES_TUPLES_OK) {
				fprintf(stderr, "Member count failed: %s", PQerrorMessage(conn));
				PQclear(res);
				continue;
			}

			int nrows = PQntuples(res);
			PQclear(res);

			if (nrows == 1) {
				int64_t ts = i->second.first;
				std::string ipAddr = i->second.second.toIpString(ipTmp);
				std::string timestamp = std::to_string(ts);

				if (firstRun) {
					firstRun = false;
				} else {
					memberUpdate << ", ";
				}

				memberUpdate << "('" << networkId << "', '" << memberId << "', ";
				if (ipAddr.empty()) {
					memberUpdate << "NULL, ";
				} else {
					memberUpdate << "'" << ipAddr << "', ";
				}
				memberUpdate << "TO_TIMESTAMP(" << timestamp << "::double precision/1000))";
				memberAdded = true;
			} else if (nrows > 1) {
				fprintf(stderr, "nrows > 1?!?");
				continue;
			} else {
				continue;
			}
		}
		memberUpdate << " ON CONFLICT (network_id, member_id) DO UPDATE SET address = EXCLUDED.address, last_updated = EXCLUDED.last_updated;";

		if (memberAdded) {
			res = PQexec(conn, memberUpdate.str().c_str());
			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				fprintf(stderr, "Multiple insert failed: %s", PQerrorMessage(conn));
			}
			PQclear(res);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	fprintf(stderr, "%s: Fell out of run loop in onlineNotificationThread\n", _myAddressStr.c_str());
	PQfinish(conn);
	if (_run == 1) {
		fprintf(stderr, "ERROR: %s onlineNotificationThread should still be running! Exiting Controller.\n", _myAddressStr.c_str());
		exit(6);
	}
}

PGconn *PostgreSQL::getPgConn(OverrideMode m)
{
	if (m == ALLOW_PGBOUNCER_OVERRIDE) {
		char *connStr = getenv("PGBOUNCER_CONNSTR");
		if (connStr != NULL) {
			fprintf(stderr, "PGBouncer Override\n");
			std::string conn(connStr);
			conn += " application_name=controller-";
			conn += _myAddressStr.c_str();
			return PQconnectdb(conn.c_str());
		}
	}

	return PQconnectdb(_connString.c_str());
}

#endif //ZT_CONTROLLER_USE_LIBPQ

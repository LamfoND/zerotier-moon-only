ZeroTier Release Notes
======

# 2019-08-30 -- Version 1.4.6

 * Update default root list to latest
 * ARM32 platform build and flag fixes
 * Add a clarification line to LICENSE.txt
 * Fix license message in CLI
 * Windows service now looks for service command line arguments
 * Fixed a bug that could cause excessive queued multicasts

# 2019-08-23 -- Version 1.4.4

 * Change license from GPL3 to BSL 1.1, see LICENSE.txt
 * Fix an issue with the "ipauth" rule and auto-generated unforgeable IPv6 addresses
 * Fix socket/bind errors setting IPs and routes on Linux

# 2019-08-12 -- Version 1.4.2

 * Fix high CPU use bug on some platforms
 * Fix issues with PostgreSQL controller DB (only affects Central)
 * Restore backward compatibility with MacOS versions prior to 10.13

# 2019-07-29 -- Version 1.4.0

### Major Changes

 * Mac version no longer requires a kernel extension, instead making use of the [feth interfaces](https://apple.stackexchange.com/questions/337715/fake-ethernet-interfaces-feth-if-fake-anyone-ever-seen-this).
 * Added support for concurrent multipath (multiple paths at once) with traffic weighting by link quality and faster recovery from lost links.
 * Added under-the-hood support for QoS (not yet exposed) that will eventually be configurable via our rules engine.

### Minor Changes and Bug Fixes

 * Experimental controller DB driver for [LF](https://github.com/zerotier/lf) to store network controller data (LFDB.cpp / LFDB.hpp).
 * Modified credential push and direct path push timings and algorithms to somewhat reduce "chattiness" of the protocol when idle. More radical background overhead reductions will have to wait for the 2.x line.
 * Removed our beta/half-baked integration of Central with the Windows UI. We're going to do a whole new UI of some kind in the future at least for Windows and Mac.
 * Fixed stack overflow issues on Linux versions using musl libc.
 * Fixed some alignment problems reported on ARM and ARM64, but some reports we could not reproduce so please report any issues with exact chip, OS/distro, and ZeroTier version in use.
 * Fixed numerous other small issues and bugs such as ARM alignment issues causing crashes on some devices.
 * Windows now sets the adapter name such that it is consistent in both the Windows UI and command line utilities.

# 2018-07-27 -- Version 1.2.12

 * Fixed a bug that caused exits to take a long time on Mac due to huge numbers of redundant attempts to delete managed routes.
 * Fixed a socket limit problem on Windows that caused the ZeroTier service to run out of sockets, causing the UI and CLI to be unable to access the API.
 * Fixed a threading bug in the ZeroTier Core, albeit one that never manifested on the regular ZeroTier One service/client.
 * Fixed a bug that could cause the service to crash if an authorized local client accessed an invalid URL via the control API. (Not exploitable since you needed admin access anyway.)

# 2018-05-08 -- Version 1.2.10

 * Fix bug loading `moons.d/` files for federated root operation.
 * Fix compile problem with ZT_DEBUG on some versions of `clang`
 * Fix slow network startup bug related to loading of `networks.d/` cache files

# 2018-04-27 -- Version 1.2.8

 * Linux version once again builds with PIE (position independent executable) flags
 * Fixed bug in zerotier-idtool file sign and verify
 * Fixed minor OSX app typo
 * Merged alpha NetBSD support (mostly untested, so YMMV)
 * Merged several minor typo and one-liner bug fixes

# 2018-04-17 -- Version 1.2.6

 * Features and Core Improvements
    * Path selection has been overhauled to improve path stability, simplify code, and prepare for multi-path and trunking in the next major release.
    * This version introduces remote tracing for remote diagnostics. Network controllers can set a node (usually the controller itself) to receive remote tracing events from all members of the network or from select members. Events are only sent if they pertain to a given network for security reasons.
    * Multicast replication can now be done by designated multicast replicators on a network (flagged as such at the controller) rather than by the sender. Most users won't want this, but it's useful for specialized use cases on hub-and-spoke networks and for low-power devices.
    * Cryptographic performance improvements on several platforms.
    * Multithreaded performance improvements throughout the code base, including the use of an inline lightweight spinlock for low-contention resources.
 * Bugs fixed
    * Disappearing routes on Mac (GitHub issue #600)
    * Route flapping and path instability in some dual-stack V4/V6 networks
    * Blacklist (in local.conf) doesn't work reliably (GitHub issue #656)
    * Connection instabilities due to unsigned integer overflows in timing comparisons (use int64_t instead of uint64_t)
    * Binaries don't run on some older or lower-end 32-bit ARM chips (build problem)
    * ARM NEON crypto code crashes (build problem)
    * Fixed some lock ordering issues revealed by "valgrind" tool
    * The "zerotier-idtool" command could not be accessed from "zerotier-one" via command line switch
    * Leaking sockets on some platforms when uPnP/NAT-PMP is enabled
    * Fixed two very rare multithreading issues that were only observed on certain systems
 * Platform-Specific Changes
    * MacOS
        * Installer now loads the kernel extension right away so that High Sierra users will see the prompt to authorize it. This is done in the "Security & Privacy" preference pane and must be done directly on the console (not via remote desktop). On High Sierra and newer kexts must be authorized at the console via security settings system preferences pane.
    * Windows
        * The Windows installer should now install the driver without requiring a special prompt in most cases. This should make it easier for our packages to be accepted into and updated in the Chocolatey repository and should make it easier to perform remote installs across groups of machines using IT management and provisioning tools.
        * The Windows official packages are now signed with an EV certificate (with hardware key).
        * The Windows UI can now log into ZeroTier Central and join networks via the Central API.
        * The `zerotier-idtool` command should now work on Windows without ugly hacks.
        * Upgraded the installer version.
        * Made a few changes to hopefully fix sporadic "will not uninstall" problems, though we cannot duplicate these issues ourselves.
    * Linux
        * Device names are now generated deterministically based on network IDs for all newly joined networks.
    * Android
        * Multicast now works on Android in most cases! Android apps can send and receive multicast and subscribe to multicast group IPs. Note that in some cases the app must bind to the specific correct interface for this to work.
        * IPv6 can be disabled in UI for cases where it causes problems.

# 2017-04-20 -- Version 1.2.4

 * Managed routes are now only bifurcated for the default route. This is a change in behavior, though few people will probably notice. Bifurcating all managed routes was causing more trouble than it was worth for most users.
 * Up to 2X crypto speedup on x86-64 (except Windows, which will take some porting) and 32-bit ARM platforms due to integration of fast assembly language implementations of Salsa20/12 from the [supercop](http://bench.cr.yp.to/supercop.html) code base. These were written by Daniel J. Bernstein and are in the public domain. My Macbook Pro (Core i5 2.8ghz) now does almost 1.5GiB/sec Salsa20/12 per core and a Raspberry Pi got a 2X boost. 64-bit ARM support and Windows support will take some work but should not be too hard.
 * Refactored code that manages credentials to greatly reduce memory use in most cases. This may also result in a small performance improvement.
 * Reworked and simplified path selection and priority logic to fix path instability and dead path persistence edge cases. There have been some sporadic reports of persistent path instabilities and dead paths hanging around that take minutes to resolve. These have proven difficult to reproduce in house, but hopefully this will fix them. In any case it seems to speed up path establishment in our tests and it makes the code simpler and more readable.
 * Eliminated some unused cruft from the code around path management and in the peer class.
 * Fixed an issue causing build problems on some MIPS architecture systems.
 * Fixed Windows forgetting routes on sleep/wake or in some other circumstances. (GitHub issue #465)

# 2017-03-17 -- Version 1.2.2

 * A bug causing unreliable multicast propagation (GitHub issue #461).
 * A crash in ARM binaries due to a build chain and flags problem.
 * A bug in the network controller preventing members from being listed (GitHub issue #460).

# 2017-03-14 -- Version 1.2.0

Version 1.2.0 is a major milestone release representing almost nine months of work. It includes our rules engine for distributed network packet filtering and security monitoring, federated roots, and many other architectural and UI improvements and bug fixes.

## New Features in 1.2.0

### The ZeroTier Rules Engine

The largest new feature in 1.2.0, and the product of many months of work, is our advanced network rules engine. With this release we achieve traffic control, security monitoring, and micro-segmentation capability on par with many enterprise SDN solutions designed for use in advanced data centers and corporate networks.

Rules allow you to filter packets on your network and vector traffic to security observers. Security observation can be performed in-band using REDIRECT or out of band using TEE.

Tags and capabilities provide advanced methods for implementing fine grained permission structures and micro-segmentation schemes without bloating the size and complexity of your rules table.

See the [rules engine announcement blog post](https://www.zerotier.com/blog/?p=927) for an in-depth discussion of theory and implementation. The [manual](https://www.zerotier.com/manual.shtml) contains detailed information on rule, tag, and capability use, and the `rule-compiler/` subfolder of the ZeroTier source tree contains a JavaScript function to compile rules in our human-readable rule definition language into rules suitable for import into a network controller. (ZeroTier Central uses this same script to compile rules on [my.zerotier.com](https://my.zerotier.com/).)

### Root Server Federation

It's now possible to create your own root servers and add them to the root server pool on your nodes. This is done by creating what's called a "moon," which is a signed enumeration of root servers and their stable points on the network. Refer to the [manual](https://www.zerotier.com/manual.shtml) for instructions.

Federated roots achieve a number of things:

 * You can deploy your own infrastructure to reduce dependency on ours.
 * You can deploy roots *inside your LAN* to ensure that network connectivity inside your facility still works if the Internet goes down. This is the first step toward making ZeroTier viable as an in-house SDN solution.
 * Roots can be deployed inside national boundaries for countries with data residency laws or "great firewalls." (As of 1.2.0 there is still no way to force all traffic to use these roots, but that will be easy to do in a later version.)
 * Last but not least this makes ZeroTier somewhat less centralized by eliminating any hard dependency on ZeroTier, Inc.'s infrastructure.

Our roots will of course remain and continue to provide zero-configuration instant-on deployment, a secure global authority for identities, and free traffic relaying for those who can't establish peer to peer connections.

### Local Configuration

An element of our design philosophy is "features are bugs." This isn't an absolute dogma but more of a guiding principle. We try as hard as we can to avoid adding features, especially "knobs" that must be tweaked by a user.

As of 1.2.0 we've decided that certain knobs are unavoidable, and so there is now a `local.conf` file that can be used to configure them. See the ZeroTier One documentation for these. They include:

 * Blacklisting interfaces you want to make sure ZeroTier doesn't use for network traffic, such as VPNs, slow links, or backplanes designated for only certain kinds of traffic.
 * Turning uPnP/NAT-PMP on or off.
 * Configuring software updates on Windows and Mac platforms.
 * Defining trusted paths (the old trusted paths file is now deprecated)
 * Setting the ZeroTier main port so it doesn't have to be changed on the command line, which is very inconvenient in many cases.

### Improved In-Band Software Updates

A good software update system for Windows and Mac clients has been a missing feature in previous versions. It does exist but we've been shy about using it so far due to its fragility in some environments.

We've greatly improved this mechanism in 1.2.0. Not only does it now do a better job of actually invoking the update, but it also transfers updates in-band using the ZeroTier protocol. This means it can work in environments that do not allows http/https traffic or that force it through proxies. There's also now an update channel setting: `beta` or `release` (the default).

Software updates are authenticated three ways:

 1. ZeroTier's own signing key is used to sign all updates and this signature is checked prior to installation. ZeroTier, Inc.'s signatures are performed on an air-gapped machine.

 2. Updates for Mac and Windows are signed using Apple and Microsoft (DigiCert EV) keys and will not install unless these signatures are also valid.

 3. The new in-band update mechanism also authenticates the source of the update via ZeroTier's built-in security features. This provides transport security, while 1 and 2 provide security of the update at rest.

Updates are now configurable via `local.conf`. There are three options: `disable`, `download`, and `apply`. The third (apply) is the default for official builds on Windows and Mac, making updates happen silently and automatically as they do for popular browsers like Chrome and Firefox. Updates are disabled by default on Linux and other Unix-type systems as these are typically updated through package managers.

### Path Link Quality Awareness

Version 1.2.0 is now aware of the link quality of direct paths with other 1.2.0 nodes. This information isn't used yet but is visible through the JSON API. (Quality always shows as 100% with pre-1.2.0 nodes.) Quality is measured passively with no additional overhead using a counter based packet loss detection algorithm.

This information is visible from the command line via `listpeers`:

    200 listpeers XXXXXXXXXX 199.XXX.XXX.XXX/9993;10574;15250;1.00 48 1.2.0 LEAF
    200 listpeers XXXXXXXXXX 195.XXX.XXX.XXX/45584;467;7608;0.44 290 1.2.0 LEAF

The first peer's path is at 100% (1.00), while the second peer's path is suffering quite a bit of packet loss (0.44).

Link quality awareness is a precursor to intelligent multi-path and QoS support, which will in future versions bring us to feature parity with SD-WAN products like Cisco iWAN.

### Security Improvements

Version 1.2.0 adds anti-DOS (denial of service) rate limits and other hardening for improved resiliency against a number of denial of service attack scenarios.

It also adds a mechanism for instantaneous credential revocation. This can be used to revoke certificates of membership instantly to kick a node off a network (for private networks) and also to revoke capabilities and tags. The new controller sends revocations by default when a peer is de-authorized.

Revocations propagate using a "rumor mill" peer to peer algorithm. This means that a controller need only successfully send a revocation to at least one member of a network with connections to other active members. At this point the revocation will flood through the network peer to peer very quickly. This helps make revocations more robust in the face of poor connectivity with the controller or attempts to incapacitate the controller with denial of service attacks, as well as making revocations faster on huge networks.

### Windows and Macintosh UI Improvements (ZeroTier One)

The Mac has a whole new UI built natively in Objective-C. It provides a pulldown similar in appearance and operation to the Mac WiFi task bar menu.

The Windows UI has also been improved and now provides a task bar icon that can be right-clicked to manage networks. Both now expose managed route and IP permissions, allowing nodes to easily opt in to full tunnel operation if you have a router configured on your network.

### Ad-Hoc Networks

A special kind of public network called an ad-hoc network may be accessed by joining a network ID with the format:

    ffSSSSEEEE000000
    | |   |   |
    | |   |   Reserved for future use, must be 0
    | |   End of port range (hex)
    | Start of port range (hex)
    Reserved ZeroTier address prefix indicating a controller-less network

Ad-hoc networks are public (no access control) networks that have no network controller. Instead their configuration and other credentials are generated locally. Ad-hoc networks permit only IPv6 UDP and TCP unicast traffic (no multicast or broadcast) using 6plane format NDP-emulated IPv6 addresses. In addition an ad-hoc network ID encodes an IP port range. UDP packets and TCP SYN (connection open) packets are only allowed to destination ports within the encoded range.

For example `ff00160016000000` is an ad-hoc network allowing only SSH, while `ff0000ffff000000` is an ad-hoc network allowing any UDP or TCP port.

Keep in mind that these networks are public and anyone in the entire world can join them. Care must be taken to avoid exposing vulnerable services or sharing unwanted files or other resources.

### Network Controller (Partial) Rewrite

The network controller has been largely rewritten to use a simple in-filesystem JSON data store in place of SQLite, and it is now included by default in all Windows, Mac, Linux, and BSD builds. This means any desktop or server node running ZeroTier One can now be a controller with no recompilation needed.

If you have data in an old SQLite3 controller we've included a NodeJS script in `controller/migrate-sqlite` to migrate data to the new format. If you don't migrate, members will start getting `NOT_FOUND` when they attempt to query for updates.

## Major Bug Fixes in 1.2.0

 * **The Windows HyperV 100% CPU bug is FINALLY DEAD**: This long-running problem turns out to have been an issue with Windows itself, but one we were triggering by placing invalid data into the Windows registry. Microsoft is aware of the issue but we've also fixed the triggering problem on our side. ZeroTier should now co-exist quite well with HyperV and should now be able to be bridged with a HyperV virtual switch.
 * **Segmentation faults on musl-libc based Linux systems**: Alpine Linux and some embedded Linux systems that use musl libc (a minimal libc) experienced segmentation faults. These were due to a smaller default stack size. A work-around that sets the stack size for new threads has been added.
 * **Windows firewall blocks local JSON API**: On some Windows systems the firewall likes to block 127.0.0.1:9993 for mysterious reasons. This is now fixed in the installer via the addition of another firewall exemption rule.
 * **UI crash on embedded Windows due to missing fonts**: The MSI installer now ships fonts and will install them if they are not present, so this should be fixed.

## Other Improvements in 1.2.0

 * **Improved dead path detection**: ZeroTier is now more aggressive about expiring paths that do not seem to be active. If a path seems marginal it is re-confirmed before re-use.
 * **Minor performance improvements**: We've reduced unnecessary memcpy's and made a few other performance improvements in the core.
 * **Linux static binaries**: For our official packages (the ones in the download.zerotier.com apt and yum repositories) we now build Linux binaries with static linking. Hopefully this will stop all the bug reports relating to library inconsistencies, as well as allowing our deb packages to run on a wider variety of Debian-based distributions. (There are far too many of these to support officially!) The overhead for this is very small, especially since we built our static versions against musl-libc. Distribution maintainers are of course free to build dynamically linked versions for inclusion into distributions; this only affects our official binaries.

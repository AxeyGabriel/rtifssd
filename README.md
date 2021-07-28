# rtifssd
## Real-time Interface Statistics Sender Daemon
rtifssd is publisher daemon that currently sends per second interfaces statistics like inbound and outbound current and peak traffic, total data transferred and interface description.\
It was written to use in a server running Multi-link PPP daemon (MPD).

### Currently implemented features
Logging to syslog(8)\
Current traffic\
Traffic peak\
Total traffic

### Supported platforms
FreeBSD (Tested versions: 12.1-RELEASE and 13.0-RELEASE)

### Usage
Inteended to be used with daemon(8) and RC scripts\
Arguments:\
-s for ZeroMQ connection parameters e.g. tcp://10.0.0.254:5555\
-i for interface matching pattern(prefix) e.g. ng (MPD uses netgraph(4) so its like ng123)

### Observations
MPD daemon needs to write username into iface description, as its the way this software identifies which interface is who\
PS: This may change in future

### Dependencies
ZeroMQ version 4 library\
LLVM

### TODO
Extensive testing\
Further development\
Make it more configurable\
Improve interface matching\
Get MPD user uptime somehow(maybe patching MPD or using its HTTP api?)\
Improve code quality

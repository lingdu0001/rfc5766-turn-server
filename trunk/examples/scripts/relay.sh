#!/bin/sh
#
# This is an example how to start a TURN Server in
# non-secure mode (when authentication is not used).
# We start here a TURN Server listening on IPv4 address
# 127.0.0.1 and on IPv6 address ::1. We use 127.0.0.1 as
# IPv4 relay address, and we use ::1 as IPv6 relay address.
# Other options:
# set bandwidth limit on client session 3000000 bytes per second (--max-bps)
# use fingerprints (-f)
# use 3 relay threads (-m 3)
# use min UDP relay port 32355 and max UDP relay port 65535
# --no-tls and --no-dtls mean that we are not trying to 
# start TLS and DTLS services.
# Other parameters (config file name, etc) are default.
  
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib/

PATH="bin:../bin:../../bin:${PATH}" turnserver -L 127.0.0.1 -L ::1 -E 127.0.0.1 -E ::1 --max-bps=3000000 -f -m 3 --min-port=32355 --max-port=65535 --no-tls --no-dtls 	





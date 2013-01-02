#!/bin/sh

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib/

PATH="bin:../bin:${PATH}" turnserver -L 127.0.0.1 -L ::1 -E 127.0.0.1 -E ::1 -f -m 3 --min-port=32355 --max-port=65535 --user=ninefingers:0xbc807ee29df3c9ffa736523fb2c4e8ee --user=gorst:hero -a -r north.gov --cert=example_turn_server_cert.pem --pkey=example_turn_server_pkey.pem 

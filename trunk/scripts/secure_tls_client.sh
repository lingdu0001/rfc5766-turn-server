#!/bin/sh

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib/

PATH=turn.apps.examples/bin/:../turn.apps.examples/bin:${PATH} uclient -t -S -i example_turn_client_cert.pem -k example_turn_client_pkey.pem -n 1000 -m 10 -l 170 -e ::1 -g -u gorst -w hero 127.0.0.1


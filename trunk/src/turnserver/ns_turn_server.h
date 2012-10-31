/*
 * Copyright (C) 2011, 2012 Citrix Systems
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef __TURN_SERVER__
#define __TURN_SERVER__

#include "ns_turn_utils.h"
#include "ns_turn_session.h"

//////////////////////////////////////////

struct socket_message {
	ioa_addr remote_addr;
	ioa_socket_handle s;
	ioa_network_buffer_handle nbh;
	u16bits chnum;
};

struct _turn_turnserver;
typedef struct _turn_turnserver turn_turnserver;

typedef enum {
	DONT_FRAGMENT_UNSUPPORTED=0,
	DONT_FRAGMENT_SUPPORTED,
	DONT_FRAGMENT_SUPPORT_EMULATED
} dont_fragment_option_t;

///////////////////////////////////////////

turn_turnserver* create_turn_server(int verbose,
				    ioa_engine_handle e,
				    u32bits *stats,
				    int stun_port,
				    int fingerprint,
				    dont_fragment_option_t dont_fragment);

void delete_turn_server(turn_turnserver* server);

///////////////////////////////////////////

int open_client_connection_session(turn_turnserver* server, struct socket_message *sm);

void set_disconnect_cb(turn_turnserver* server, int (*disconnect)(ts_ur_super_session*));

///////////////////////////////////////////

#endif //__TURN_SERVER__

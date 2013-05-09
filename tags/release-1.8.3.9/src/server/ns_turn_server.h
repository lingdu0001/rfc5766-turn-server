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

#ifdef __cplusplus
extern "C" {
#endif

//////////////////////////////////////////

struct socket_message {
	ioa_addr remote_addr;
	ioa_socket_handle s;
	ioa_network_buffer_handle nbh;
	u16bits chnum;
};

typedef u08bits turnserver_id;

struct _turn_turnserver;
typedef struct _turn_turnserver turn_turnserver;

typedef enum {
	DONT_FRAGMENT_UNSUPPORTED=0,
	DONT_FRAGMENT_SUPPORTED,
	DONT_FRAGMENT_SUPPORT_EMULATED
} dont_fragment_option_t;

typedef void (*get_username_resume_cb)(int success, hmackey_t hmackey, st_password_t pwd, void *ctx, ioa_net_data *in_buffer);
typedef u08bits *(*get_user_key_cb)(turnserver_id id, u08bits *uname, get_username_resume_cb resume, ioa_net_data *in_buffer, void *ctx, int *postpone_reply);
typedef int (*check_new_allocation_quota_cb)(u08bits *username);
typedef void (*release_allocation_quota_cb)(u08bits *username);

//////////// ALTERNATE-SERVER /////////////

struct _alternate_servers_list {
	ioa_addr *addrs;
	size_t size;
};

typedef struct _alternate_servers_list alternate_servers_list_t;

///////////////////////////////////////////

turn_turnserver* create_turn_server(turnserver_id id, int verbose,
				    ioa_engine_handle e,
				    u32bits *stats,
				    int stun_port,
				    int fingerprint,
				    dont_fragment_option_t dont_fragment,
				    turn_credential_type ct,
				    u08bits *realm,
				    get_user_key_cb userkeycb,
				    check_new_allocation_quota_cb chquotacb,
				    release_allocation_quota_cb raqcb,
				    ioa_addr *external_addr,
				    int no_tcp_relay,
				    int no_udp_relay,
				    int stale_nonce,
				    int stun_only,
				    alternate_servers_list_t *alternate_servers_list,
				    alternate_servers_list_t *tls_alternate_servers_list,
				    int no_multicast_peers,
				    int no_loopback_peers);

void delete_turn_server(turn_turnserver* server);

ioa_engine_handle turn_server_get_engine(turn_turnserver *s);

////////// RFC 5780 ///////////////////////

typedef int (*get_alt_addr_cb)(ioa_addr *addr, ioa_addr *alt_addr);
typedef int (*send_message_cb)(ioa_engine_handle e, ioa_network_buffer_handle nbh, ioa_addr *origin, ioa_addr *destination);

void set_rfc5780(turn_turnserver *server, get_alt_addr_cb cb, send_message_cb smcb);

///////////////////////////////////////////

int open_client_connection_session(turn_turnserver* server, struct socket_message *sm);
int shutdown_client_connection(turn_turnserver *server, ts_ur_super_session *ss);
void set_disconnect_cb(turn_turnserver* server, int (*disconnect)(ts_ur_super_session*));

///////////////////////////////////////////

#ifdef __cplusplus
}
#endif

#endif //__TURN_SERVER__
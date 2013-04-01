/*
 * Copyright (C) 2012 Citrix Systems
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <ifaddrs.h>
#include <getopt.h>
#include <locale.h>
#include <libgen.h>

#if !defined(TURN_NO_THREADS)
#include <pthread.h>
#endif

#include <signal.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>

#include "ns_turn_utils.h"

#include "userdb.h"

#include "udp_listener.h"
#include "tcp_listener.h"
#include "tls_listener.h"
#include "dtls_listener.h"

#include "ns_turn_server.h"
#include "ns_turn_maps.h"

#include "apputils.h"

#include "ns_ioalib_impl.h"

//////////////// OpenSSL Init //////////////////////

static void openssl_setup(void);
static void openssl_cleanup(void);

//////////////// Common params ////////////////////

static int verbose=0;
static int turn_daemon = 0;
static int stale_nonce = 0;

static int do_not_use_config_file = 0;

#define DEFAULT_CONFIG_FILE "turnserver.conf"

////////////////  Listener server /////////////////

static int listener_port = DEFAULT_STUN_PORT;
static int tls_listener_port = DEFAULT_STUN_TLS_PORT;
static int alt_listener_port = DEFAULT_ALT_STUN_PORT;
static int alt_tls_listener_port = DEFAULT_ALT_STUN_TLS_PORT;
static int rfc5780 = 1;

static int no_udp = 0;
static int no_tcp = 0;
static int no_tls = 0;
static int no_dtls = 0;

static int no_tcp_relay = 0;
static int no_udp_relay = 0;

static SSL_CTX *tls_ctx = NULL;
static SSL_CTX *dtls_ctx = NULL;

static char listener_ifname[1025]="\0";

/*
 * openssl genrsa -out pkey 2048
 * openssl req -new -key pkey -out cert.req
 * openssl x509 -req -days 365 -in cert.req -signkey pkey -out cert
 *
*/
static char cert_file[1025]="turn_server_cert.pem\0";
static char pkey_file[1025]="turn_server_pkey.pem\0";

struct message_to_listener_to_client {
	ioa_addr origin;
	ioa_addr destination;
	ioa_network_buffer_handle nbh;
};

enum _MESSAGE_TO_LISTENER_TYPE {
	LMT_UNKNOWN,
	LMT_TO_CLIENT
};
typedef enum _MESSAGE_TO_LISTENER_TYPE MESSAGE_TO_LISTENER_TYPE;

struct message_to_listener {
	MESSAGE_TO_LISTENER_TYPE t;
	union {
		struct message_to_listener_to_client tc;
	} m;
};

struct listener_server {
	rtcp_map* rtcpmap;
	turnipports* tp;
	struct event_base* event_base;
	ioa_engine_handle ioa_eng;
	struct bufferevent *in_buf;
	struct bufferevent *out_buf;
	char **addrs;
	ioa_addr **encaddrs;
	size_t addrs_number;
	size_t services_number;
	udp_listener_relay_server_type **udp_services;
	tcp_listener_relay_server_type **tcp_services;
	tls_listener_relay_server_type **tls_services;
	dtls_listener_relay_server_type **dtls_services;
};

struct listener_server listener = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL};

static uint32_t stats=0;

//////////////// Relay servers //////////////////////////////////

static band_limit_t max_bps = 0;

static u16bits min_port = LOW_DEFAULT_PORTS_BOUNDARY;
static u16bits max_port = HIGH_DEFAULT_PORTS_BOUNDARY;

static char relay_ifname[1025]="\0";

static size_t relays_number = 0;
static char **relay_addrs = NULL;
static ioa_addr *external_ip = NULL;

static int fingerprint = 0;

static size_t relay_servers_number = 0;
#define get_real_relay_servers_number() (relay_servers_number > 1 ? relay_servers_number : 1)

enum _MESSAGE_TO_RELAY_TYPE {
	RMT_UNKNOWN,
	RMT_SOCKET,
	RMT_USER_HMACKEY
};
typedef enum _MESSAGE_TO_RELAY_TYPE MESSAGE_TO_RELAY_TYPE;

struct message_to_relay {
	MESSAGE_TO_RELAY_TYPE t;
	union {
		struct socket_message sm;
		struct auth_message am;
	} m;
};

struct relay_server {
	turnserver_id id;
	struct event_base* event_base;
	struct bufferevent *in_buf;
	struct bufferevent *out_buf;
	ioa_engine_handle ioa_eng;
	turn_turnserver *server;
#if !defined(TURN_NO_THREADS)
	pthread_t thr;
#endif
};
static struct relay_server **relay_servers = NULL;

////////////// Auth server ////////////////////////////////////////////////

struct auth_server {
	struct event_base* event_base;
	struct bufferevent *in_buf;
	struct bufferevent *out_buf;
#if !defined(TURN_NO_THREADS)
	pthread_t thr;
#endif
};

static struct auth_server authserver;

////////////// Configuration functionality ////////////////////////////////

static void read_config_file(int argc, char **argv, int pass);

//////////////////////////////////////////////////

static void add_listener_addr(const char* addr) {
	++listener.addrs_number;
	++listener.services_number;
	listener.addrs = (char**)realloc(listener.addrs, sizeof(char*)*listener.addrs_number);
	listener.addrs[listener.addrs_number-1]=strdup(addr);
	listener.encaddrs = (ioa_addr**)realloc(listener.encaddrs, sizeof(ioa_addr*)*listener.addrs_number);
	listener.encaddrs[listener.addrs_number-1]=(ioa_addr*)malloc(sizeof(ioa_addr));
	make_ioa_addr((const u08bits*)addr,0,listener.encaddrs[listener.addrs_number-1]);
	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Listener address to use: %s\n",addr);
}

static void add_relay_addr(const char* addr) {
	++relays_number;
	relay_addrs = (char**)realloc(relay_addrs, sizeof(char*)*relays_number);
	relay_addrs[relays_number-1]=strdup(addr);
	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Relay address to use: %s\n",addr);
}

//////////////////////////////////////////////////

// communications between listener and relays ==>>

void send_auth_message_to_auth_server(struct auth_message *am)
{
	struct evbuffer *output = bufferevent_get_output(authserver.out_buf);
	if(evbuffer_add(output,am,sizeof(*am))<0) {
		fprintf(stderr,"%s: Weird buffer error\n",__FUNCTION__);
	}
	bufferevent_flush(authserver.out_buf, EV_WRITE, BEV_FLUSH);
}

static void auth_server_receive_message(struct bufferevent *bev, void *ptr)
{
	UNUSED_ARG(ptr);

	struct auth_message am;
	int n = 0;
	struct evbuffer *input = bufferevent_get_input(bev);
	while ((n = evbuffer_remove(input, &am, sizeof(am))) > 0) {
		if (n != sizeof(am)) {
			fprintf(stderr,"%s: Weird buffer error: size=%d\n",__FUNCTION__,n);
			continue;
		}

		hmackey_t key;
		if(get_user_key(am.username,key)<0) {
			am.success = 0;
		} else {
			ns_bcopy(key,am.key,sizeof(key));
			am.success = 1;
		}

		struct message_to_relay sm;
		sm.t = RMT_USER_HMACKEY;

		size_t dest = am.id;

		ns_bcopy(&am,&(sm.m.am),sizeof(am));

		struct evbuffer *output = bufferevent_get_output(relay_servers[dest]->out_buf);
		evbuffer_add(output,&sm,sizeof(sm));
		bufferevent_flush(relay_servers[dest]->out_buf, EV_WRITE, BEV_FLUSH);
	}
}

static int send_socket_to_relay(ioa_engine_handle e, ioa_socket_handle s, ioa_net_data *nd)
{
	static size_t current_relay_server = 0;

	int is_tcp = ((get_ioa_socket_type(s) == TCP_SOCKET)||(get_ioa_socket_type(s) == TLS_SOCKET));

	UNUSED_ARG(e);

	current_relay_server = current_relay_server % get_real_relay_servers_number();

	struct message_to_relay sm;
	sm.t = RMT_SOCKET;
	addr_cpy(&(sm.m.sm.remote_addr),&(nd->src_addr));
	sm.m.sm.nbh = nd->nbh;
	nd->nbh = NULL;
	sm.m.sm.s = s;

	/* To support RFC 6062, we have to run all TCP connections
	 * within the same relay server: */
	size_t dest = 0;

	if((no_tcp && no_tls) || no_tcp_relay || (get_real_relay_servers_number()<2)) {
		dest = current_relay_server++;
	} else if(is_tcp) {
		dest = 0;
	} else {
		if(!current_relay_server) ++current_relay_server;
		dest = current_relay_server++;
	}

	sm.m.sm.chnum = nd->chnum;

	struct evbuffer *output = bufferevent_get_output(relay_servers[dest]->out_buf);
	evbuffer_add(output,&sm,sizeof(sm));
	bufferevent_flush(relay_servers[dest]->out_buf, EV_WRITE, BEV_FLUSH);

	return 0;
}

static void relay_receive_message(struct bufferevent *bev, void *ptr)
{
	struct message_to_relay sm;
	int n = 0;
	struct evbuffer *input = bufferevent_get_input(bev);
	while ((n = evbuffer_remove(input, &sm, sizeof(sm))) > 0) {
		if (n != sizeof(sm)) {
			perror("Weird buffer error\n");
			continue;
		}
		struct relay_server *rs = (struct relay_server *)ptr;
		switch(sm.t) {
		case RMT_SOCKET: {
			if (sm.m.sm.s->defer_nbh) {
				if (!sm.m.sm.nbh) {
					sm.m.sm.nbh = sm.m.sm.s->defer_nbh;
					sm.m.sm.s->defer_nbh = NULL;
				} else {
					ioa_network_buffer_delete(rs->ioa_eng, sm.m.sm.s->defer_nbh);
					sm.m.sm.s->defer_nbh = NULL;
				}
			}

			ioa_socket_handle s = sm.m.sm.s;

			if (s->read_event || s->bev) {
				TURN_LOG_FUNC(
								TURN_LOG_LEVEL_ERROR,
								"%s: socket wrongly preset: 0x%lx : 0x%lx\n",
								__FUNCTION__, (long) s->read_event,
								(long) s->bev);
				IOA_CLOSE_SOCKET(sm.m.sm.s);
				return;
			}

			s->e = rs->ioa_eng;

			open_client_connection_session(rs->server, &(sm.m.sm));
			ioa_network_buffer_delete(rs->ioa_eng, sm.m.sm.nbh);

			if (ioa_socket_tobeclosed(s)) {
				ts_ur_super_session *ss = (ts_ur_super_session *)s->session;
				if (ss) {
					turn_turnserver *server = (turn_turnserver *)ss->server;
					if (server) {
						shutdown_client_connection(server, ss);
					}
				}
			}
		}
		break;
		case RMT_USER_HMACKEY: {
			sm.m.am.resume_func(sm.m.am.success, sm.m.am.key, sm.m.am.ctx, &(sm.m.am.in_buffer));
			if(sm.m.am.in_buffer.nbh) {
				ioa_network_buffer_delete(rs->ioa_eng, sm.m.am.in_buffer.nbh);
			}
		}
		break;
		default: {
			perror("Weird buffer type\n");
		}
		}
	}
}

static int send_message_from_listener_to_client(ioa_engine_handle e, ioa_network_buffer_handle nbh, ioa_addr *origin, ioa_addr *destination)
{

	struct message_to_listener mm;
	mm.t = LMT_TO_CLIENT;
	addr_cpy(&(mm.m.tc.origin),origin);
	addr_cpy(&(mm.m.tc.destination),destination);
	mm.m.tc.nbh = ioa_network_buffer_allocate(e);
	ioa_network_buffer_header_init(mm.m.tc.nbh);
	ns_bcopy(ioa_network_buffer_data(nbh),ioa_network_buffer_data(mm.m.tc.nbh),ioa_network_buffer_get_size(nbh));
	ioa_network_buffer_set_size(mm.m.tc.nbh,ioa_network_buffer_get_size(nbh));

	struct evbuffer *output = bufferevent_get_output(listener.out_buf);
	evbuffer_add(output,&mm,sizeof(mm));
	bufferevent_flush(listener.out_buf, EV_WRITE, BEV_FLUSH);

	return 0;
}

static void listener_receive_message(struct bufferevent *bev, void *ptr)
{
	UNUSED_ARG(ptr);

	struct message_to_listener mm;
	int n = 0;
	struct evbuffer *input = bufferevent_get_input(bev);
	while ((n = evbuffer_remove(input, &mm, sizeof(mm))) > 0) {
		if (n != sizeof(mm)) {
			perror("Weird buffer error\n");
			continue;
		}
		if (mm.t != LMT_TO_CLIENT) {
			perror("Weird buffer type\n");
			continue;
		}

		size_t i;
		int found = 0;
		for(i=0;i<listener.addrs_number;i++) {
			if(addr_eq_no_port(listener.encaddrs[i],&mm.m.tc.origin)) {
				int o_port = addr_get_port(&mm.m.tc.origin);
				if(listener.addrs_number == listener.services_number) {
					if(o_port == listener_port) {
						if(listener.udp_services && listener.udp_services[i]) {
							found = 1;
							udp_send_message(listener.udp_services[i], mm.m.tc.nbh, &mm.m.tc.destination);
						}
					} else {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Wrong origin port(1): %d\n",__FUNCTION__,o_port);
					}
				} else if((listener.addrs_number * 2) == listener.services_number) {
					if(o_port == listener_port) {
						if(listener.udp_services && listener.udp_services[i*2]) {
							found = 1;
							udp_send_message(listener.udp_services[i*2], mm.m.tc.nbh, &mm.m.tc.destination);
						}
					} else if(o_port == alt_listener_port) {
						if(listener.udp_services && listener.udp_services[i*2+1]) {
							found = 1;
							udp_send_message(listener.udp_services[i*2+1], mm.m.tc.nbh, &mm.m.tc.destination);
						}
					} else {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Wrong origin port(2): %d\n",__FUNCTION__,o_port);
					}
				} else {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Wrong listener setup\n",__FUNCTION__);
				}
				break;
			}
		}

		if(!found) {
			u08bits saddr[129];
			addr_to_string(&mm.m.tc.origin, saddr);
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"%s: Cannot find local source %s\n",__FUNCTION__,saddr);
		}

		ioa_network_buffer_delete(listener.ioa_eng, mm.m.tc.nbh);
	}
}

// <<== communications between listener and relays

static void setup_listener_servers(void)
{
	size_t i = 0;

	listener.tp = turnipports_create(min_port, max_port);

	listener.event_base = event_base_new();

	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method (listener thread): %s\n",event_base_get_method(listener.event_base));

	listener.ioa_eng = create_ioa_engine(listener.event_base, listener.tp, relay_ifname, relays_number, relay_addrs, verbose, max_bps);

	if(!listener.ioa_eng)
		exit(-1);

	set_ssl_ctx(listener.ioa_eng, tls_ctx, dtls_ctx);

	listener.rtcpmap = rtcp_map_create(listener.ioa_eng);

	ioa_engine_set_rtcp_map(listener.ioa_eng, listener.rtcpmap);

	{
		struct bufferevent *pair[2];
		int opts = 0;

#if !defined(TURN_NO_THREADS)
		opts = BEV_OPT_THREADSAFE;
#endif

		bufferevent_pair_new(listener.event_base, opts, pair);
		listener.in_buf = pair[0];
		listener.out_buf = pair[1];
		bufferevent_setcb(listener.in_buf, listener_receive_message, NULL, NULL, &listener);
		bufferevent_enable(listener.in_buf, EV_READ);
	}

	if(alt_listener_port>=0 || alt_tls_listener_port>=0) {
		if(listener.addrs_number<2) {
			rfc5780 = 0;
			TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: I cannot start alternative services of RFC 5780 because only one IP address is provided\n");
		} else {
			listener.services_number = listener.services_number * 2;
		}
	}

	listener.udp_services = (udp_listener_relay_server_type**)realloc(listener.udp_services, sizeof(udp_listener_relay_server_type*)*listener.services_number);
	listener.tcp_services = (tcp_listener_relay_server_type**)realloc(listener.tcp_services, sizeof(tcp_listener_relay_server_type*)*listener.services_number);
	listener.tls_services = (tls_listener_relay_server_type**)realloc(listener.tls_services, sizeof(tls_listener_relay_server_type*)*listener.services_number);
	listener.dtls_services = (dtls_listener_relay_server_type**)realloc(listener.dtls_services, sizeof(dtls_listener_relay_server_type*)*listener.services_number);

	for(i=0; i<listener.addrs_number; i++) {
		int index = rfc5780 ? i*2 : i;
		if(!no_udp) {
			listener.udp_services[index] = create_udp_listener_server(listener_ifname, listener.addrs[i], listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
			if(rfc5780 && alt_listener_port>=0)
				listener.udp_services[index+1] = create_udp_listener_server(listener_ifname, listener.addrs[i], alt_listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
		} else {
			listener.udp_services[index] = NULL;
			if(rfc5780)
				listener.udp_services[index+1] = NULL;
		}
		if(!no_tcp) {
			listener.tcp_services[index] = create_tcp_listener_server(listener_ifname, listener.addrs[i], listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
			if(rfc5780 && alt_listener_port>=0)
				listener.tcp_services[index+1] = create_tcp_listener_server(listener_ifname, listener.addrs[i], alt_listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
		} else {
			listener.tcp_services[index] = NULL;
			if(rfc5780)
				listener.tcp_services[index+1] = NULL;
		}
		if(!no_tls) {
			listener.tls_services[index] = create_tls_listener_server(listener_ifname, listener.addrs[i], tls_listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
			if(rfc5780 && alt_tls_listener_port>=0)
				listener.tls_services[index+1] = create_tls_listener_server(listener_ifname, listener.addrs[i], alt_tls_listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
		} else {
			listener.tls_services[index] = NULL;
			if(rfc5780)
				listener.tls_services[index+1] = NULL;
		}
		if(!no_dtls) {
			listener.dtls_services[index] = create_dtls_listener_server(listener_ifname, listener.addrs[i], tls_listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
			if(rfc5780 && alt_tls_listener_port>=0)
				listener.dtls_services[index+1] = create_dtls_listener_server(listener_ifname, listener.addrs[i], alt_tls_listener_port, verbose, listener.ioa_eng, &stats, send_socket_to_relay);
		} else {
			listener.dtls_services[index] = NULL;
			if(rfc5780)
				listener.dtls_services[index+1] = NULL;
		}
	}
}

static int get_alt_addr(ioa_addr *addr, ioa_addr *alt_addr)
{
	if(!addr || !rfc5780 || (listener.addrs_number<2))
		return -1;
	else {
		size_t index = 0xffff;
		size_t i = 0;
		int alt_port = -1;
		int port = addr_get_port(addr);

		if(port == listener_port)
			alt_port = alt_listener_port;
		else if(port == alt_listener_port)
			alt_port = listener_port;
		else if(port == tls_listener_port)
			alt_port = alt_tls_listener_port;
		else if(port == alt_tls_listener_port)
			alt_port = tls_listener_port;
		else
			return -1;

		for(i=0;i<listener.addrs_number;i++) {
			if(addr_eq_no_port(addr,listener.encaddrs[i])) {
				index=i;
				break;
			}
		}
		if(index!=0xffff) {
			for(i=0;i<listener.addrs_number;i++) {
				size_t ind = (index+i) % listener.addrs_number;
				ioa_addr *caddr = listener.encaddrs[ind];
				if(caddr->ss.ss_family == addr->ss.ss_family) {
					if(addr_eq_no_port(caddr,addr))
						continue;
					addr_cpy(alt_addr,caddr);
					addr_set_port(alt_addr, alt_port);
					return 0;
				}
			}
		}

		return -1;
	}
}

static void run_events(struct event_base *eb)
{

	if (!eb)
		return;

	struct timeval timeout;

	timeout.tv_sec = 5;
	timeout.tv_usec = 0;

	event_base_loopexit(eb, &timeout);

	event_base_dispatch(eb);
}

static void run_listener_server(struct event_base *eb)
{
	unsigned int cycle = 0;
	for (;;) {

		if (verbose) {
			if ((cycle++ & 15) == 0) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s: cycle=%u, stats=%lu\n", __FUNCTION__, cycle,
								(unsigned long) stats);
			}
		}

		run_events(eb);

#if defined(TURN_NO_THREADS)
		/* If there are no threads, then we run it here */
		read_userdb_file();
#endif
	}
}

static void setup_relay_server(struct relay_server *rs, ioa_engine_handle e)
{
	struct bufferevent *pair[2];
	int opts = 0;

	if(e) {
		rs->event_base = e->event_base;
		rs->ioa_eng = e;
	} else {
		rs->event_base = event_base_new();
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method (relay thread): %s\n",event_base_get_method(rs->event_base));
		rs->ioa_eng = create_ioa_engine(rs->event_base, listener.tp, relay_ifname, relays_number, relay_addrs, verbose, max_bps);
		set_ssl_ctx(rs->ioa_eng, tls_ctx, dtls_ctx);
		ioa_engine_set_rtcp_map(rs->ioa_eng, listener.rtcpmap);
	}

#if !defined(TURN_NO_THREADS)
	opts = BEV_OPT_THREADSAFE;
#endif

	bufferevent_pair_new(rs->event_base, opts, pair);
	rs->in_buf = pair[0];
	rs->out_buf = pair[1];
	bufferevent_setcb(rs->in_buf, relay_receive_message, NULL, NULL, rs);
	bufferevent_enable(rs->in_buf, EV_READ);
	rs->server = create_turn_server(rs->id, verbose,
					rs->ioa_eng, &stats, 0, fingerprint, DONT_FRAGMENT_SUPPORTED,
					users->ct,
					users->realm,
					start_user_check,
					check_new_allocation_quota,
					release_allocation_quota,
					external_ip,
					no_tcp_relay,
					no_udp_relay,
					stale_nonce);
	if(rfc5780) {
		set_rfc5780(rs->server, get_alt_addr, send_message_from_listener_to_client);
	}
}

#if !defined(TURN_NO_THREADS)
static void *run_relay_thread(void *arg)
{
  static int always_true = 1;
  struct relay_server *rs = (struct relay_server *)arg;
  
  setup_relay_server(rs, NULL);

  while(always_true)
    run_events(rs->event_base);
  
  return arg;
}
#endif

static void setup_relay_servers(void)
{
	size_t i = 0;

#if defined(TURN_NO_THREADS)
	relay_servers_number = 0;
#endif

	relay_servers = (struct relay_server**)malloc(sizeof(struct relay_server *)*get_real_relay_servers_number());
	ns_bzero(relay_servers,sizeof(struct relay_server *)*get_real_relay_servers_number());

	for(i=0;i<get_real_relay_servers_number();i++) {

		relay_servers[i] = (struct relay_server*)malloc(sizeof(struct relay_server));
		ns_bzero(relay_servers[i], sizeof(struct relay_server));
		relay_servers[i]->id = (turnserver_id)i;

#if defined(TURN_NO_THREADS)
		setup_relay_server(relay_servers[i], listener.ioa_eng);
#else
		if(relay_servers_number == 0) {
			setup_relay_server(relay_servers[i], listener.ioa_eng);
			relay_servers[i]->thr = pthread_self();
		} else {
			if(pthread_create(&(relay_servers[i]->thr), NULL, run_relay_thread, relay_servers[i])<0) {
				perror("Cannot create relay thread\n");
				exit(-1);
			}
			pthread_detach(relay_servers[i]->thr);
		}
#endif
	}
}

#if !defined(TURN_NO_THREADS)

static int run_auth_server_flag = 1;

static void* run_auth_server_thread(void *arg)
{
	struct event_base *eb = (struct event_base*)arg;

	while(run_auth_server_flag) {
		run_events(eb);
		read_userdb_file();
	}

	return arg;
}

#endif

static void setup_auth_server(void)
{
	ns_bzero(&authserver,sizeof(authserver));

#if defined(TURN_NO_THREADS)
	authserver.event_base = listener.event_base;
#else
	authserver.event_base = event_base_new();
	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method (auth thread): %s\n",event_base_get_method(authserver.event_base));
#endif

	struct bufferevent *pair[2];
	int opts = 0;

#if !defined(TURN_NO_THREADS)
	opts = BEV_OPT_THREADSAFE;
#endif

	bufferevent_pair_new(authserver.event_base, opts, pair);
	authserver.in_buf = pair[0];
	authserver.out_buf = pair[1];
	bufferevent_setcb(authserver.in_buf, auth_server_receive_message, NULL, NULL, &authserver);
	bufferevent_enable(authserver.in_buf, EV_READ);

#if !defined(TURN_NO_THREADS)
	if(pthread_create(&(authserver.thr), NULL, run_auth_server_thread, authserver.event_base)<0) {
		perror("Cannot create auth thread\n");
		exit(-1);
	}
	pthread_detach(authserver.thr);
#endif
}

static void setup_server(void)
{
#if !defined(TURN_NO_THREADS)
	evthread_use_pthreads();
#endif

	setup_listener_servers();

	setup_relay_servers();

	setup_auth_server();
}

///////////////////////////////////////////////////////////////

static void clean_server(void)
{
	size_t i = 0;

	if (relay_servers) {
		for(i=0;i<get_real_relay_servers_number();i++) {
			if(relay_servers[i]) {
				delete_turn_server(relay_servers[i]->server);
				if(relay_servers[i]->ioa_eng != listener.ioa_eng)
					close_ioa_engine(relay_servers[i]->ioa_eng);
				if(relay_servers[i]->in_buf) {
					bufferevent_disable(relay_servers[i]->in_buf,EV_READ|EV_WRITE);
					bufferevent_free(relay_servers[i]->in_buf);
				}
				if(relay_servers[i]->out_buf) {
					bufferevent_disable(relay_servers[i]->out_buf,EV_READ|EV_WRITE);
					bufferevent_free(relay_servers[i]->out_buf);
				}
				if(relay_servers[i]->event_base != listener.event_base)
					event_base_free(relay_servers[i]->event_base);
				free(relay_servers[i]);
				relay_servers[i] = NULL;
			}
		}
		free(relay_servers);
	}

	if(listener.udp_services) {
		for(i=0;i<listener.services_number; i++) {
			if (listener.udp_services[i]) {
				delete_udp_listener_server(listener.udp_services[i],0);
				listener.udp_services[i] = NULL;
			}
		}
		free(listener.udp_services);
		listener.udp_services = NULL;
	}

	if(listener.tcp_services) {
		for(i=0;i<listener.services_number; i++) {
			if (listener.tcp_services[i]) {
				delete_tcp_listener_server(listener.tcp_services[i],0);
				listener.tcp_services[i] = NULL;
			}
		}
		free(listener.tcp_services);
		listener.tcp_services = NULL;
	}

	if(listener.tls_services) {
		for(i=0;i<listener.services_number; i++) {
			if (listener.tls_services[i]) {
				delete_tls_listener_server(listener.tls_services[i],0);
				listener.tls_services[i] = NULL;
			}
		}
		free(listener.tls_services);
		listener.tls_services = NULL;
	}

	if(listener.dtls_services) {
		for(i=0;i<listener.services_number; i++) {
			if (listener.dtls_services[i]) {
				delete_dtls_listener_server(listener.dtls_services[i],0);
				listener.dtls_services[i] = NULL;
			}
		}
		free(listener.dtls_services);
		listener.dtls_services = NULL;
	}

	listener.services_number = 0;

	if (listener.ioa_eng) {
		close_ioa_engine(listener.ioa_eng);
		listener.ioa_eng = NULL;
	}

	if (listener.event_base) {
		event_base_free(listener.event_base);
		listener.event_base = NULL;
	}

	if (listener.rtcpmap) {
		rtcp_map_free(&(listener.rtcpmap));
	}

	if (listener.tp) {
	  turnipports_destroy(&(listener.tp));
	}

	if (relay_addrs) {
		for (i = 0; i < relays_number; i++) {
			if (relay_addrs[i]) {
				free(relay_addrs[i]);
				relay_addrs[i] = NULL;
			}
		}
		free(relay_addrs);
	}

	if(listener.addrs) {
		for(i=0;i<listener.addrs_number; i++) {
			if (listener.addrs[i]) {
				free(listener.addrs[i]);
				listener.addrs[i] = NULL;
			}
		}
		free(listener.addrs);
		listener.addrs = NULL;
	}

	if(listener.encaddrs) {
		for(i=0;i<listener.addrs_number; i++) {
			if (listener.encaddrs[i]) {
				free(listener.encaddrs[i]);
				listener.encaddrs[i] = NULL;
			}
		}
		free(listener.encaddrs);
		listener.encaddrs = NULL;
	}

	listener.addrs_number = 0;

	if(users) {
		ur_string_map_free(&(users->static_accounts));
		ur_string_map_free(&(users->dynamic_accounts));
		ur_string_map_free(&(users->alloc_counters));
		free(users);
	}
}

//////////////////////////////////////////////////

static int make_local_listeners_list(void)
{
	struct ifaddrs * ifs = NULL;
	struct ifaddrs * ifa = NULL;

	char saddr[INET6_ADDRSTRLEN] = "\0";

	if((getifaddrs(&ifs) == 0) && ifs) {

		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "===========Discovering listener addresses: =========\n");
		for (ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {

			if(!(ifa->ifa_addr))
				continue;

			if (ifa ->ifa_addr->sa_family == AF_INET) {
				if(!inet_ntop(AF_INET, &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr, saddr,
								INET_ADDRSTRLEN))
					continue;
			} else if (ifa->ifa_addr->sa_family == AF_INET6) {
				if(!inet_ntop(AF_INET6, &((struct sockaddr_in6 *) ifa->ifa_addr)->sin6_addr, saddr,
								INET6_ADDRSTRLEN))
					continue;
				if(strstr(saddr,"fe80") == saddr)
					continue;
			} else
				continue;

			add_listener_addr(saddr);
		}
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "=====================================================\n");
		freeifaddrs(ifs);
	}

	return 0;
}

static int make_local_relays_list(int allow_local)
{
	struct ifaddrs * ifs = NULL;
	struct ifaddrs * ifa = NULL;

	char saddr[INET6_ADDRSTRLEN] = "\0";

	getifaddrs(&ifs);

	if (ifs) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "===========Discovering relay addresses: =============\n");
		for (ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {

			if(!(ifa->ifa_name))
				continue;
			if(!(ifa ->ifa_addr))
				continue;

			if(!allow_local && (strstr(ifa->ifa_name,"lo") == ifa->ifa_name))
				continue;

			if (ifa ->ifa_addr->sa_family == AF_INET) {
				if(!inet_ntop(AF_INET, &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr, saddr,
								INET_ADDRSTRLEN))
					continue;
			} else if (ifa->ifa_addr->sa_family == AF_INET6) {
				if(!inet_ntop(AF_INET6, &((struct sockaddr_in6 *) ifa->ifa_addr)->sin6_addr, saddr,
								INET6_ADDRSTRLEN))
					continue;
				if(strstr(saddr,"fe80") == saddr)
					continue;
			} else
				continue;

			add_relay_addr(saddr);
		}
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "=====================================================\n");
		freeifaddrs(ifs);
	}

	return 0;
}

//////////////////////////////////////////////////

static char Usage[] = "Usage: turnserver [options]\n"
	"Options:\n"
	"	-d, --listening-device		Listener interface device (optional, Linux only).\n"
	"	-p, --listening-port		TURN listener port (Default: 3478).\n"
	"	    --tls-listening-port	TURN listener port for TLS and DTLS listeners\n"
	"					(Default: 5349).\n"
	"	    --alt-listening-port	Alternative listening port (in RFC 5780 sense, default 24378)\n"
	"	    --alt-tls-listening-port	Alternative listening port for TLS and DTLS (in RFC 5780 sense, default 23549)\n"
	"	-L, --listening-ip		Listener IP address of relay server. Multiple listeners can be specified.\n"
	"	-i, --relay-device		Relay interface device for relay sockets (optional, Linux only).\n"
	"	-E, --relay-ip			Relay address (the local IP address that will be used to relay the packets to the peer).\n"
	"	-X, --external-ip		\"External\" TURN Server address if the server is behind NAT.\n"
	"					In the server-behind-NAT situation, only one relay address must be used, and\n"
	"					that single relay address must be mapped by NAT to the 'external' IP.\n"
	"					For this 'external' IP, NAT must forward ports directly (relayed port 12345\n"
	"					must be always mapped to the same 'external' port 12345).\n"
	"	-m, --relay-threads		Number of extra threads to handle established connections (default is 0).\n"
	"	    --min-port			Lower bound of the UDP port range for relay endpoints allocation.\n"
	"					Default value is 49152, according to RFC 5766.\n"
	"	    --max-port			Upper bound of the UDP port range for relay endpoints allocation.\n"
	"					Default value is 65535, according to RFC 5766.\n"
	"	-v, --verbose			Verbose.\n"
	"	-o, --daemon			Start process as daemon (detach from current shell).\n"
	"	-f, --fingerprint		Use fingerprints in the TURN messages.\n"
	"	-a, --lt-cred-mech		Use long-term credential mechanism. Default - no authentication - anonymous access.\n"
	"	-z, --no-auth			Do not use any credential mechanism, allow anonymous access. Opposite to -a option.\n"
	"	-u, --user			User account, in form 'username:password'.\n"
	"	-r, --realm			Realm.\n"
	"	-q, --user-quota		Per-user allocation quota: how many concurrent allocations a user can create.\n"
	"	-Q, --total-quota		Total allocations quota: global limit on concurrent allocations.\n"
	"	-s, --max-bps			Max bytes-per-second bandwidth a TURN session is allowed to handle.\n"
	"					(input and output network streams combined).\n"
	"	-c				Configuration file name (default - turnserver.conf).\n"
	"	-b, --userdb			User database file name (default - turnuserdb.conf).\n"
#if !defined(TURN_NO_PQ)
	"	-e, --sql-userdb		PostgreSQL connection string, if used (default - empty, no connection).\n"
	"					See http://www.postgresql.org/docs/8.4/static/libpq-connect.html for 8.x PostgreSQL\n"
	"					versions format, see http://www.postgresql.org/docs/9.2/static/libpq-connect.html#LIBPQ-CONNSTRING\n"
	"					for 9.x and newer connection string formats.\n"
#endif
#if !defined(TURN_NO_MYSQL)
	"	-M, --mysql-userdb		MySQL connection string, if used (default - empty, no connection).\n"
	"					The connection string my be space-separated list of parameters:\n"
	"	                  		\"host=<host> dbname=<database-name> user=<database-user> password=<database-user-password> port=<port>\".\n"
	"	                  		All parameters are optional.\n"
#endif
	"		--use-auth-secret	Flag that sets a special authorization option that is based upon authentication secret\n"
	"					(TURN Server RESt API, see TURNServerRESTAPI.pdf). This option is used with timestamp.\n"
	"		--static-auth-secret	'Static' authentication secret value (a string). If not set, then the turn server\n"
	"					will try to use the 'dynamic' value in turn_secret table\n"
	"					in user database (if present). That value can be changed on-the-fly\n"
	"					by a separate program, so this is why it is 'dynamic'.\n"
	"		--secret-ts-exp-time 	Expiration time for timestamp used with authentication secret, in seconds.\n"
	"					The default value is 86400 (24 hours). This is 'TTL' in terms of TURNServerRESTAPI.pdf.\n"
	"	-n				Do not use configuration file.\n"
	"	    --cert			Certificate file, PEM format. Same file search rules\n"
	"					applied as for the configuration file.\n"
	"					If both --no-tls and --no_dtls options\n"
	"					are specified, then this parameter is not needed.\n"
	"	    --pkey			Private key file, PEM format. Same file search rules\n"
	"					applied as for the configuration file.\n"
	"					If both --no-tls and --no-dtls options\n"
	"					are specified, then this parameter is not needed.\n"
	"	    --no-udp			Do not start UDP client listeners.\n"
	"	    --no-tcp			Do not start TCP client listeners.\n"
	"	    --no-tls			Do not start TLS client listeners.\n"
	"	    --no-dtls			Do not start DTLS client listeners.\n"
	"	    --no-udp-relay		Do not allow UDP relay endpoints, use only TCP relay option.\n"
	"	    --no-tcp-relay		Do not allow TCP relay endpoints, use only UDP relay options.\n"
	"	-l, --log-file			Option to set the log file name.\n"
	"					By default, the turnserver tries to open a log file in\n"
	"					/var/log, /var/tmp, /tmp and current directories directories\n"
	"					(which open operation succeeds first that file will be used).\n"
	"					With this option you can set the definite log file name.\n"
	"					The special names are \"stdout\" and \"-\" - they will force everything\n"
	"					to the stdout.\n"
	"	    --no-stdout-log		Flag to prevent stdout log messages.\n"
	"					By default, all log messages are going to both stdout and to\n"
	"					a log file. With this option everything will be going to the log file only\n"
	"					(unless the log file itself is stdout).\n"
	"	    --stale-nonce		Use extra security with nonce value having limited lifetime (600 secs).\n"
	"	-h				Help\n";

static char AdminUsage[] = "Usage: turnadmin [command] [options]\n"
	"Commands:\n"
	"	-k, --key		generate key for a user\n"
	"	-a, --add		add/update a user\n"
	"	-d, --delete		delete a user\n"
	"Options:\n"
	"	-b, --userdb		User database file, if flat DB file is used.\n"
#if !defined(TURN_NO_PQ)
	"	-e, --sql-userdb	PostgreSQL user database connection string, if PostgreSQL DB is used.\n"
#endif
#if !defined(TURN_NO_MYSQL)
	"	-M, --mysql-userdb	MySQL user database connection string, if MySQL DB is used.\n"
#endif
	"	-u, --user		Username\n"
	"	-r, --realm		Realm\n"
	"	-p, --password		Password\n"
	"	-h, --help		Help\n";

#define OPTIONS "c:d:p:L:E:X:i:m:l:r:u:b:e:M:q:Q:s:vofhzna"

#define ADMIN_OPTIONS "kadb:e:M:u:r:p:h"

enum EXTRA_OPTS {
	NO_UDP_OPT=256,
	NO_TCP_OPT,
	NO_TLS_OPT,
	NO_DTLS_OPT,
	NO_UDP_RELAY_OPT,
	NO_TCP_RELAY_OPT,
	TLS_PORT_OPT,
	ALT_PORT_OPT,
	ALT_TLS_PORT_OPT,
	CERT_FILE_OPT,
	PKEY_FILE_OPT,
	MIN_PORT_OPT,
	MAX_PORT_OPT,
	STALE_NONCE_OPT,
	AUTH_SECRET_OPT,
	STATIC_AUTH_SECRET_VAL_OPT,
	AUTH_SECRET_TS_EXP,
	NO_STDOUT_LOG_OPT
};

static struct option long_options[] = {
				{ "listening-device", required_argument, NULL, 'd' },
				{ "listening-port", required_argument, NULL, 'p' },
				{ "tls-listening-port", required_argument, NULL, TLS_PORT_OPT },
				{ "alt-listening-port", required_argument, NULL, ALT_PORT_OPT },
				{ "alt-tls-listening-port", required_argument, NULL, ALT_TLS_PORT_OPT },
				{ "listening-ip", required_argument, NULL, 'L' },
				{ "relay-device", required_argument, NULL, 'i' },
				{ "relay-ip", required_argument, NULL, 'E' },
				{ "external-ip", required_argument, NULL, 'X' },
				{ "relay-threads", required_argument, NULL, 'm' },
				{ "min-port", required_argument, NULL, MIN_PORT_OPT },
				{ "max-port", required_argument, NULL, MAX_PORT_OPT },
				{ "lt-cred-mech", optional_argument, NULL, 'a' },
				{ "no-auth", optional_argument, NULL, 'z' },
				{ "user", required_argument, NULL, 'u' },
				{ "userdb", required_argument, NULL, 'b' },
#if !defined(TURN_NO_PQ)
				{ "sql-userdb", required_argument, NULL, 'e' },
#endif
#if !defined(TURN_NO_MYSQL)
				{ "mysql-userdb", required_argument, NULL, 'M' },
#endif
				{ "use-auth-secret", optional_argument, NULL, AUTH_SECRET_OPT },
				{ "static-auth-secret", required_argument, NULL, STATIC_AUTH_SECRET_VAL_OPT },
				{ "secret-ts-exp-time", required_argument, NULL, AUTH_SECRET_TS_EXP },
				{ "realm", required_argument, NULL, 'r' },
				{ "user-quota", required_argument, NULL, 'q' },
				{ "total-quota", required_argument, NULL, 'Q' },
				{ "max-bps", required_argument, NULL, 's' },
				{ "verbose", optional_argument, NULL, 'v' },
				{ "daemon", optional_argument, NULL, 'o' },
				{ "fingerprint", optional_argument, NULL, 'f' },
				{ "no-udp", optional_argument, NULL, NO_UDP_OPT },
				{ "no-tcp", optional_argument, NULL, NO_TCP_OPT },
				{ "no-tls", optional_argument, NULL, NO_TLS_OPT },
				{ "no-dtls", optional_argument, NULL, NO_DTLS_OPT },
				{ "no-udp-relay", optional_argument, NULL, NO_UDP_RELAY_OPT },
				{ "no-tcp-relay", optional_argument, NULL, NO_TCP_RELAY_OPT },
				{ "stale-nonce", optional_argument, NULL, STALE_NONCE_OPT },
				{ "cert", required_argument, NULL, CERT_FILE_OPT },
				{ "pkey", required_argument, NULL, PKEY_FILE_OPT },
				{ "log-file", required_argument, NULL, 'l' },
				{ "no-stdout-log", optional_argument, NULL, NO_STDOUT_LOG_OPT },
				{ NULL, no_argument, NULL, 0 }
};

static struct option admin_long_options[] = {
				{ "key", no_argument, NULL, 'k' },
				{ "add", no_argument, NULL, 'a' },
				{ "delete", no_argument, NULL, 'd' },
				{ "userdb", required_argument, NULL, 'b' },
#if !defined(TURN_NO_PQ)
				{ "sql-userdb", required_argument, NULL, 'e' },
#endif
#if !defined(TURN_NO_MYSQL)
				{ "mysql-userdb", required_argument, NULL, 'e' },
#endif
				{ "user", required_argument, NULL, 'u' },
				{ "realm", required_argument, NULL, 'r' },
				{ "password", required_argument, NULL, 'p' },
				{ "help", no_argument, NULL, 'h' },
				{ NULL, no_argument, NULL, 0 }
};

static int get_bool_value(const char* s)
{
	if(!s || !(s[0])) return 1;
	if(s[0]=='0' || s[0]=='n' || s[0]=='N' || s[0]=='f' || s[0]=='F') return 0;
	if(s[0]=='y' || s[0]=='Y' || s[0]=='t' || s[0]=='T') return 1;
	if(s[0]>'0' && s[0]<='9') return 1;
	if(!strcmp(s,"off") || !strcmp(s,"OFF") || !strcmp(s,"Off")) return 0;
	if(!strcmp(s,"on") || !strcmp(s,"ON") || !strcmp(s,"On")) return 1;
	TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Unknown boolean value: %s. You can use on/off, yes/no, 1/0, true/false.\n",s);
	exit(-1);
}

static void set_option(int c, char *value)
{
  if(value && value[0]=='=') {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: option -%c is possibly used incorrectly. The short form of the option must be used as this: -%c <value>, no \'equals\' sign may be used, that sign is used only with long form options (like --user=<username>).\n",(char)c,(char)c);
  }

	switch (c){
	case 'i':
		STRCPY(relay_ifname, value);
		break;
	case 'm':
#if defined(TURN_NO_THREADS)
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: threading is not supported,\n I am using single thread.\n");
#elif defined(OPENSSL_THREADS)
		relay_servers_number = atoi(value);
#else
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: OpenSSL version is too old OR does not support threading,\n I am using single thread for relaying.\n");
#endif
		break;
	case 'd':
		STRCPY(listener_ifname, value);
		break;
	case 'p':
		listener_port = atoi(value);
		break;
	case TLS_PORT_OPT:
		tls_listener_port = atoi(value);
		break;
	case ALT_PORT_OPT:
		alt_listener_port = atoi(value);
		break;
	case ALT_TLS_PORT_OPT:
		alt_tls_listener_port = atoi(value);
		break;
	case MIN_PORT_OPT:
		min_port = atoi(value);
		break;
	case MAX_PORT_OPT:
		max_port = atoi(value);
		break;
	case STALE_NONCE_OPT:
		stale_nonce = get_bool_value(value);
		break;
	case 'L':
		add_listener_addr(value);
		break;
	case 'E':
		add_relay_addr(value);
		break;
	case 'X':
		if(external_ip) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "You cannot define external IP more than once in the configuration\n");
		} else {
			external_ip = (ioa_addr*)turn_malloc(sizeof(ioa_addr));
			ns_bzero(external_ip,sizeof(ioa_addr));
			make_ioa_addr((const u08bits*)value,0,external_ip);
		}
		break;
	case 'v':
		verbose = get_bool_value(value);
		break;
	case 'o':
		turn_daemon = get_bool_value(value);
		break;
	case 'a':
		if (get_bool_value(value)) {
			users->ct = TURN_CREDENTIALS_LONG_TERM;
			use_lt_credentials=1;
		} else {
			users->ct = TURN_CREDENTIALS_NONE;
			use_lt_credentials=0;
		}
		break;
	case 'z':
		if (!get_bool_value(value)) {
			users->ct = TURN_CREDENTIALS_LONG_TERM;
			anon_credentials = 0;
		} else {
			users->ct = TURN_CREDENTIALS_NONE;
			anon_credentials = 1;
		}
		break;
	case 'f':
		fingerprint = get_bool_value(value);
		break;
	case 'u':
		add_user_account(value,0);
		break;
	case 'b':
		STRCPY(userdb, value);
		userdb_type = TURN_USERDB_TYPE_FILE;
		break;
#if !defined(TURN_NO_PQ)
	case 'e':
		STRCPY(userdb, value);
		userdb_type = TURN_USERDB_TYPE_PQ;
		break;
#endif
#if !defined(TURN_NO_MYSQL)
	case 'M':
		STRCPY(userdb, value);
		userdb_type = TURN_USERDB_TYPE_MYSQL;
		break;
#endif
	case AUTH_SECRET_OPT:
		use_auth_secret_with_timestamp = 1;
		break;
	case STATIC_AUTH_SECRET_VAL_OPT:
		STRCPY(static_auth_secret,value);
		use_auth_secret_with_timestamp = 1;
		break;
	case AUTH_SECRET_TS_EXP:
		auth_secret_timestamp_expiration_time = (turn_time_t)atol(value);
		break;
	case 'r':
		STRCPY(global_realm,value);
		STRCPY(users->realm, value);
		break;
	case 'q':
		users->user_quota = atoi(value);
		break;
	case 'Q':
		users->total_quota = atoi(value);
		break;
	case 's':
		max_bps = (band_limit_t)atol(value);
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%lu bytes per second is allowed per session\n",(unsigned long)max_bps);
		break;
	case NO_UDP_OPT:
		no_udp = get_bool_value(value);
		break;
	case NO_TCP_OPT:
		no_tcp = get_bool_value(value);
		break;
	case NO_UDP_RELAY_OPT:
		no_udp_relay = get_bool_value(value);
		break;
	case NO_TCP_RELAY_OPT:
		no_tcp_relay = get_bool_value(value);
		break;
	case NO_TLS_OPT:
#if defined(TURN_NO_TLS)
		no_tls = 1;
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: TLS is not supported\n");
#else
		no_tls = get_bool_value(value);
#endif
		break;
	case NO_DTLS_OPT:
#if defined(BIO_CTRL_DGRAM_QUERY_MTU)
		no_dtls = get_bool_value(value);
#else
		no_dtls = 1;
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: DTLS is not supported\n");
#endif
		break;
	case CERT_FILE_OPT:
		STRCPY(cert_file,value);
		break;
	case PKEY_FILE_OPT:
		STRCPY(pkey_file,value);
		break;
	/* these options have been already taken care of before: */
	case 'l':
	case NO_STDOUT_LOG_OPT:
	case 'c':
	case 'n':
	case 'h':
		break;
	default:
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "%s\n", Usage);
		exit(-1);
	}
}

static int parse_arg_string(char *sarg, int *c, char **value)
{
	int i = 0;
	char *name = sarg;
	while(*sarg) {
		if((*sarg==' ') || (*sarg=='=')) {
			*sarg=0;
			do {
				++sarg;
			} while((*sarg==' ') || (*sarg=='='));
			*value = sarg;
			break;
		}
		++sarg;
		*value=sarg;
	}

	while(long_options[i].name) {
		if(strcmp(long_options[i].name,name)) {
			++i;
			continue;
		}
		*c=long_options[i].val;
		return 0;
	}

	return -1;
}

static void read_config_file(int argc, char **argv, int pass)
{
	static char config_file[1025] = DEFAULT_CONFIG_FILE;

	if(pass == 0) {

	  if (argv) {
	    int i = 0;
	    for (i = 0; i < argc; i++) {
	      if (!strcmp(argv[i], "-c")) {
		if (i < argc - 1) {
		  STRCPY(config_file, argv[i + 1]);
		} else {
		  TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "Wrong usage of -c option\n");
		}
	      } else if (!strcmp(argv[i], "-n")) {
		do_not_use_config_file = 1;
		config_file[0]=0;
		return;
	      } else if (!strcmp(argv[i], "-h")) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s\n", Usage);
		exit(0);
	      }
	    }
	  }
	}

	if (!do_not_use_config_file && config_file[0]) {

		FILE *f = NULL;
		char *full_path_to_config_file = NULL;

		full_path_to_config_file = find_config_file(config_file, 1);
		if (full_path_to_config_file)
			f = fopen(full_path_to_config_file, "r");

		if (f && full_path_to_config_file) {

			char sbuf[1025];
			char sarg[1035];

			for (;;) {
				char *s = fgets(sbuf, sizeof(sbuf) - 1, f);
				if (!s)
					break;
				s = skip_blanks(s);
				if (s[0] == '#')
					continue;
				if (!s[0])
					continue;
				size_t slen = strlen(s);
				while (slen && (s[slen - 1] == 10 || s[slen - 1] == 13))
					s[--slen] = 0;
				if (slen) {
					int c = 0;
					char *value = NULL;
					STRCPY(sarg, s);
					if (parse_arg_string(sarg, &c, &value) < 0) {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "Bad configuration format: %s\n",
							sarg);
					} else if((pass == 0) && (c != 'u')) {
					  set_option(c, value);
					} else if((pass == 0) && (c == 'l')) {
						set_logfile(value);
					} else if((pass==0) && (c==NO_STDOUT_LOG_OPT)) {
						set_no_stdout_log(get_bool_value(value));
					} else if((pass > 0) && (c == 'u')) {
					  set_option(c, value);
					}
				}
			}

			fclose(f);

		} else
			TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: Cannot find config file: %s. Default settings will be used.\n",
				config_file);
	}
}

static int adminmain(int argc, char **argv)
{
	int c = 0;

	int kcommand = 0;
	int acommand = 0;
	int dcommand = 0;

	u08bits user[STUN_MAX_USERNAME_SIZE+1]="\0";
	u08bits realm[STUN_MAX_REALM_SIZE+1]="\0";
	u08bits pwd[STUN_MAX_PWD_SIZE+1]="\0";

	while (((c = getopt_long(argc, argv, ADMIN_OPTIONS, admin_long_options, NULL)) != -1)) {
		switch (c){
		case 'k':
			kcommand = 1;
			break;
		case 'a':
			acommand = 1;
			break;
		case 'd':
			dcommand = 1;
			break;
		case 'b':
			strcpy(userdb,optarg);
			userdb_type = TURN_USERDB_TYPE_FILE;
			break;
#if !defined(TURN_NO_PQ)
		case 'e':
			strcpy(userdb,optarg);
			userdb_type = TURN_USERDB_TYPE_PQ;
			break;
#endif
#if !defined(TURN_NO_MYSQL)
		case 'M':
			strcpy(userdb,optarg);
			userdb_type = TURN_USERDB_TYPE_MYSQL;
			break;
#endif
		case 'u':
			strcpy((char*)user,optarg);
			if(SASLprep((u08bits*)user)<0) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong user name: %s\n",user);
				exit(-1);
			}
			break;
		case 'r':
			strcpy((char*)realm,optarg);
			if(SASLprep((u08bits*)realm)<0) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong realm: %s\n",realm);
				exit(-1);
			}
			break;
		case 'p':
			strcpy((char*)pwd,optarg);
			if(SASLprep((u08bits*)pwd)<0) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong password: %s\n",pwd);
				exit(-1);
			}
			break;
		case 'h':
			TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s\n", AdminUsage);
			exit(0);
			break;
		default:
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s\n", AdminUsage);
			exit(-1);
		}
	}

	if(!strlen(userdb) && (userdb_type == TURN_USERDB_TYPE_FILE))
		strcpy(userdb,DEFAULT_USERDB_FILE);

	if(!user[0] || (kcommand + acommand + dcommand != 1)) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s\n", AdminUsage);
		exit(-1);
	}

	return adminuser(user, realm, pwd, kcommand, acommand, dcommand);
}

int main(int argc, char **argv)
{
	int c = 0;

	set_execdir();

	while (((c = getopt_long(argc, argv, OPTIONS, long_options, NULL)) != -1)) {
		switch(c) {
		case 'l':
			set_logfile(optarg);
			break;
		case NO_STDOUT_LOG_OPT:
			set_no_stdout_log(get_bool_value(optarg));
			break;
		default:
			;
		}
	}

	optind = 0;

#if defined(TURN_NO_TLS)
	no_tls = 1;
	TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "WARNING: TLS is not supported\n");
#endif

#if !defined(BIO_CTRL_DGRAM_QUERY_MTU)
	no_dtls = 1;
	TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "WARNING: OpenSSL version is too old, DTLS is not supported\n");
#endif

	set_system_parameters(1);

	if(strstr(argv[0],"turnadmin"))
		return adminmain(argc,argv);

	TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "RFC 5389/5766/6062/6156 STUN/TURN Server, version %s\n",TURN_SOFTWARE);

	users = (turn_user_db*)malloc(sizeof(turn_user_db));
	ns_bzero(users,sizeof(turn_user_db));
	users->ct = TURN_CREDENTIALS_NONE;
	users->static_accounts = ur_string_map_create(free);
	users->dynamic_accounts = ur_string_map_create(free);
	users->alloc_counters = ur_string_map_create(NULL);

	read_config_file(argc,argv,0);

	while (((c = getopt_long(argc, argv, OPTIONS, long_options, NULL)) != -1)) {
		if(c != 'u')
			set_option(c,optarg);
	}

	read_config_file(argc,argv,1);

	optind = 0;

	while (((c = getopt_long(argc, argv, OPTIONS, long_options, NULL)) != -1)) {
	  if(c == 'u') {
	    set_option(c,optarg);
	  }
	}

	if(no_udp_relay && no_tcp_relay) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "\nCONFIG ERROR: --no-udp-relay and --no-tcp-relay options cannot be used together.\n");
		exit(-1);
	}

	if(no_udp_relay) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "\nCONFIG: --no-udp-relay: UDP relay endpoints are not allowed.\n");
	}

	if(no_tcp_relay) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "\nCONFIG: --no-tcp-relay: TCP relay endpoints are not allowed.\n");
	}

	if(!strlen(userdb) && (userdb_type == TURN_USERDB_TYPE_FILE))
			strcpy(userdb,DEFAULT_USERDB_FILE);

	read_userdb_file();

	argc -= optind;
	argv += optind;

	if(argc>0) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "\nCONFIGURATION ALERT: Unknown argument: %s\n",argv[argc-1]);
	}

	if(use_lt_credentials && anon_credentials) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "\nCONFIG ERROR: -a and -z options cannot be used together.\n");
		exit(-1);
	}

	if(!use_lt_credentials && !anon_credentials) {
		if(users_number) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "\nCONFIGURATION ALERT: you specified user accounts, (-u option) \n	but you did not specify the long-term credentials option (-a or --lt-cred-mech option).\n 	I am turning --lt-cred-mech ON for you, but double-check your configuration.\n");
			users->ct = TURN_CREDENTIALS_LONG_TERM;
			use_lt_credentials=1;
		} else {
			users->ct = TURN_CREDENTIALS_NONE;
			use_lt_credentials=0;
		}
	}

	if(use_lt_credentials) {
		if(!users_number && (userdb_type == TURN_USERDB_TYPE_FILE) && !use_auth_secret_with_timestamp) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "\nCONFIGURATION ALERT: you did not specify any user account, (-u option) \n	but you did specified the long-term credentials option (-a option).\n	The TURN Server will be inaccessible.\n		Check your configuration.\n");
		} else if(!global_realm[0]) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "\nCONFIGURATION ALERT: you did specify the long-term credentials usage\n but you did not specify the realm option (-r option).\n	The TURN Server will be inaccessible.\n		Check your configuration.\n");
		}
	}

	if(anon_credentials) {
		if(users_number) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "\nCONFIGURATION ALERT: you specified user accounts, (-u option) \n	but you also specified the anonymous user access option (-z or --no-auth option).\n 	User accounts will be ignored.\n");
			users->ct = TURN_CREDENTIALS_NONE;
			use_lt_credentials=0;
		}
	}

	openssl_setup();

	int local_listeners = 0;
	if (!listener.addrs_number) {
		make_local_listeners_list();
		local_listeners = 1;
		if (!listener.addrs_number) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "You must specify the listener address(es)\n", __FUNCTION__);
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s\n", Usage);
			exit(-1);
		}
	}

	if (!relays_number) {
		if(!local_listeners && listener.addrs_number && listener.addrs) {
			size_t la = 0;
			for(la=0;la<listener.addrs_number;la++) {
				if(listener.addrs[la])
					add_relay_addr(listener.addrs[la]);
			}
		}
		if (!relays_number)
			make_local_relays_list(0);
		if (!relays_number) {
			make_local_relays_list(1);
			if (!relays_number) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "You must specify the relay address(es)\n",
								__FUNCTION__);
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s\n", Usage);
				exit(-1);
			}
		}
	}

	if(turn_daemon) {
#if !defined(HAS_DAEMON)
		pid_t pid = fork();
		if(pid>0)
			exit(0);
		if(pid<0) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "ERROR: Cannot start daemon process\n");
			exit(-1);
		}
#else
		if(daemon(1,0)<0) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "ERROR: Cannot start daemon process\n");
			exit(-1);
		}
		reset_rtpprintf();
#endif
	}

	setup_server();

	run_listener_server(listener.event_base);

	clean_server();

	openssl_cleanup();

	return 0;
}

////////// OpenSSL locking ////////////////////////////////////////

#if defined(OPENSSL_THREADS) && !defined(TURN_NO_THREADS)

static pthread_mutex_t* mutex_buf = NULL;

static void locking_function(int mode, int n, const char *file, int line) {
  UNUSED_ARG(file);
  UNUSED_ARG(line);
  if (mode & CRYPTO_LOCK)
    pthread_mutex_lock(&mutex_buf[n]);
  else
    pthread_mutex_unlock(&mutex_buf[n]);
}

#if OPENSSL_VERSION_NUMBER >= 0x10000000L
static void id_function(CRYPTO_THREADID *ctid)
{
    CRYPTO_THREADID_set_numeric(ctid, (unsigned long)pthread_self());
}
#else
static unsigned long id_function(void)
{
    return (unsigned long)pthread_self();
}
#endif

#endif

static int THREAD_setup(void) {

#if defined(OPENSSL_THREADS) && !defined(TURN_NO_THREADS)

	int i;

	mutex_buf = (pthread_mutex_t*) malloc(CRYPTO_num_locks()
			* sizeof(pthread_mutex_t));
	if (!mutex_buf)
		return 0;
	for (i = 0; i < CRYPTO_num_locks(); i++)
		pthread_mutex_init(&mutex_buf[i], NULL);

#if OPENSSL_VERSION_NUMBER >= 0x10000000L
	CRYPTO_THREADID_set_callback(id_function);
#else
	CRYPTO_set_id_callback(id_function);
#endif

	CRYPTO_set_locking_callback(locking_function);
#endif

	return 1;
}

static int THREAD_cleanup(void) {

#if defined(OPENSSL_THREADS) && !defined(TURN_NO_THREADS)

  int i;

  if (!mutex_buf)
    return 0;

#if OPENSSL_VERSION_NUMBER >= 0x10000000L
	CRYPTO_THREADID_set_callback(NULL);
#else
	CRYPTO_set_id_callback(NULL);
#endif

  CRYPTO_set_locking_callback(NULL);
  for (i = 0; i < CRYPTO_num_locks(); i++)
    pthread_mutex_destroy(&mutex_buf[i]);
  free(mutex_buf);
  mutex_buf = NULL;

#endif

  return 1;
}

static void set_ctx(SSL_CTX* ctx, const char *protocol)
{

	SSL_CTX_set_cipher_list(ctx, "DEFAULT");
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

	if (!SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM)) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s: ERROR: no certificate found\n",protocol);
	} else {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s: Certificate file %s found\n",protocol,cert_file);
	}

	if (!SSL_CTX_use_PrivateKey_file(ctx, pkey_file, SSL_FILETYPE_PEM)) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s: ERROR: no private key found\n",protocol);
	} else {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s: Private key file %s found\n",protocol,pkey_file);
	}

	if (!SSL_CTX_check_private_key(ctx)) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "%s: ERROR: invalid private key\n",protocol);
	}
}

static void adjust_key_file_name(char *fn, const char* file_title)
{
	char *full_path_to_file = NULL;

	if(!fn[0]) {
	  TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,"\nERROR: you must set the %s file parameter\n",file_title);
	  goto keyerr;
	} else {
	  
	  full_path_to_file = find_config_file(fn, 1);
	  FILE *f = full_path_to_file ? fopen(full_path_to_file,"r") : NULL;
	  if(!f) {
	    TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING,"WARNING: cannot find %s file: %s (1)\n",file_title,fn);
	    goto keyerr;
	  }
	  
	  if(!full_path_to_file) {
	    TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING,"WARNING: cannot find %s file: %s (2)\n",file_title,fn);
	    goto keyerr;
	  }
	  
	  strcpy(fn,full_path_to_file);
	  
	  if(full_path_to_file)
	    free(full_path_to_file);
	  return;
	}

	keyerr:
	{
	  no_tls = 1;
	  no_dtls = 1;
	  if(full_path_to_file)
	    free(full_path_to_file);
	  TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING,"WARNING: cannot start TLS and DTLS listeners because %s file is not set properly\n",file_title);
	  return;
	}
}

static void adjust_key_file_names(void)
{
	adjust_key_file_name(cert_file,"certificate");
	adjust_key_file_name(pkey_file,"private key");
}

static void openssl_setup(void)
{
	THREAD_setup();
	SSL_load_error_strings();
	OpenSSL_add_ssl_algorithms();

#if defined(TURN_NO_TLS)
	if(!no_tls) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "WARNING: TLS is not supported\n");
		no_tls = 1;
	}
#endif

	if(!(no_tls && no_dtls) && !cert_file[0]) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING,"\nWARNING: certificate file is not specified, I cannot start TLS/DTLS services.\nOnly 'plain' UDP/TCP listeners can be started.\n");
		no_tls = 1;
		no_dtls = 1;
	}

	if(!(no_tls && no_dtls) && !pkey_file[0]) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING,"\nWARNING: private key file is not specified, I cannot start TLS/DTLS services.\nOnly 'plain' UDP/TCP listeners can be started.\n");
		no_tls = 1;
		no_dtls = 1;
	}

	if(!(no_tls && no_dtls)) {
		adjust_key_file_names();
	}

	if(!no_tls) {
		tls_ctx = SSL_CTX_new(TLSv1_server_method());
		set_ctx(tls_ctx,"TLS");
	}

	if(!no_dtls) {
#if !defined(BIO_CTRL_DGRAM_QUERY_MTU)
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "ERROR: DTLS is not supported.\n");
#else
		if(OPENSSL_VERSION_NUMBER < 0x10000000L) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "WARNING: TURN Server was compiled with rather old OpenSSL version, DTLS may not be working correctly.\n");
		}
		dtls_ctx = SSL_CTX_new(DTLSv1_server_method());
		set_ctx(dtls_ctx,"DTLS");
#endif
	}
}

static void openssl_cleanup(void)
{
	if(tls_ctx) {
		SSL_CTX_free(tls_ctx);
		tls_ctx = NULL;
	}

	if(dtls_ctx) {
		SSL_CTX_free(dtls_ctx);
		dtls_ctx = NULL;
	}

	THREAD_cleanup();
}

///////////////////////////////

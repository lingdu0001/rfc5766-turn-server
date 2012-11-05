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
#include <pthread.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "ns_turn_utils.h"

#include "udp_listener.h"

#include "ns_turn_server.h"
#include "ns_turn_maps.h"

#include "apputils.h"

#include "ns_ioalib_impl.h"

//////////////// local definitions /////////////////

struct listener_server {
	size_t number;
	rtcp_map* rtcpmap;
	turnipports* tp;
	struct event_base* event_base;
	ioa_engine_handle ioa_eng;
	char **addrs;
	udp_listener_relay_server_type **services;
};

struct listener_server listener = {0, NULL, NULL, NULL, NULL, NULL, NULL};

static uint32_t stats=0;

//////////////////////////////////////////////////

static int port = RELAY_DEFAULT_PORT;

static size_t relays_number = 0;
static char **relay_addrs = NULL;

static size_t relay_servers_number = 1;
struct relay_server {
	struct event_base* event_base;
	struct bufferevent *in_buf;
	struct bufferevent *out_buf;
	ioa_engine_handle ioa_eng;
	turn_turnserver *server;
	pthread_t thr;
};
static struct relay_server **relay_servers = NULL;

/////////////////////////////////////////////////

static int verbose=0;

static char ifname[1025]="\0";
static char relay_ifname[1025]="\0";
static int fingerprint = 0;

static u16bits min_port = LOW_DEFAULT_PORTS_BOUNDARY;
static u16bits max_port = HIGH_DEFAULT_PORTS_BOUNDARY;

//////////////////////////////////////////////////

#define DEFAULT_CONFIG_FILE "turn.conf"
const char* config_file_search_dirs[] = {"", "etc/", "/etc/", "/usr/local/etc/", NULL };

//////////////////////////////////////////////////

static void add_listener_addr(const char* addr) {
	++listener.number;
	listener.addrs = realloc(listener.addrs, sizeof(char*)*listener.number);
	listener.addrs[listener.number-1]=strdup(addr);
	listener.services = realloc(listener.services, sizeof(udp_listener_relay_server_type*)*listener.number);
	listener.services[listener.number-1] = NULL;
}

static void add_relay_addr(const char* addr) {
	++relays_number;
	relay_addrs = realloc(relay_addrs, sizeof(char*)*relays_number);
	relay_addrs[relays_number-1]=strdup(addr);
}

//////////////////////////////////////////////////

static int send_socket(ioa_engine_handle e, ioa_socket_handle s, ioa_net_data *nd)
{
	static size_t current_relay_server = 0;

	UNUSED_ARG(e);

	current_relay_server = current_relay_server % relay_servers_number;

	struct socket_message sm;
	addr_cpy(&(sm.remote_addr),nd->remote_addr);
	sm.nbh = nd->nbh;
	nd->nbh = NULL;
	sm.s = s;
	size_t dest = current_relay_server++;
	sm.chnum = nd->chnum;

	struct evbuffer *output = bufferevent_get_output(relay_servers[dest]->out_buf);
	evbuffer_add(output,&sm,sizeof(sm));
	bufferevent_flush(relay_servers[dest]->out_buf, EV_READ|EV_WRITE, BEV_FLUSH);

	return 0;
}

static void acceptsocket(struct bufferevent *bev, void *ptr)
{
	struct socket_message sm;
	int n = 0;
	struct evbuffer *input = bufferevent_get_input(bev);
	while ((n = evbuffer_remove(input, &sm, sizeof(sm))) > 0) {
		if(n != sizeof(sm)) {
			perror("Weird buffer error\n");
			exit(-1);
		}
		struct relay_server *rs = ptr;
		open_client_connection_session(rs->server, &sm);
		ioa_network_buffer_delete(sm.nbh);
	}
}


static void setup_listener_servers(void)
{
	size_t i = 0;

	listener.tp = turnipports_create(min_port, max_port);

	listener.event_base = event_base_new();

	listener.ioa_eng = create_ioa_engine(listener.event_base, listener.tp, relay_ifname, relays_number, relay_addrs, verbose);

	if(!listener.ioa_eng)
		exit(-1);

	register_callback_on_ioa_engine_new_connection(listener.ioa_eng, send_socket);

	listener.rtcpmap = rtcp_map_create(listener.ioa_eng);

	ioa_engine_set_rtcp_map(listener.ioa_eng, listener.rtcpmap);

	for(i=0;i<listener.number;i++)
		listener.services[i] = create_udp_listener_server(ifname, listener.addrs[i], port, verbose, listener.ioa_eng, &stats);
}

static void run_events(struct event_base *eb)
{

	if (!eb)
		return;

	struct timeval timeout;

	timeout.tv_sec = 0;
	timeout.tv_usec = 1000000;

	event_base_loopexit(eb, &timeout);

	event_base_dispatch(eb);
}

static void run_server(struct event_base *eb)
{
	unsigned int cycle = 0;
	while (1) {

		if (verbose) {
			if ((cycle++ & 15) == 0) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s: cycle=%u, stats=%lu\n", __FUNCTION__, cycle,
								(unsigned long) stats);
			}
		}

		run_events(eb);
	}
}

static void *run_relay_thread(void *arg)
{
	struct relay_server *rs = arg;

	run_server(rs->event_base);

	return arg;
}

static void setup_relay_servers(void)
{
	size_t i = 0;

	relay_servers = malloc(sizeof(struct relay_server *)*relay_servers_number);

	for(i=0;i<relay_servers_number;i++) {

		struct bufferevent *pair[2];
		relay_servers[i] = malloc(sizeof(struct relay_server));

		if(relay_servers_number<2) {
			relay_servers[i]->event_base = listener.event_base;
			relay_servers[i]->ioa_eng = listener.ioa_eng;
		} else {
			relay_servers[i]->event_base = event_base_new();
			relay_servers[i]->ioa_eng = create_ioa_engine(relay_servers[i]->event_base, listener.tp, relay_ifname, relays_number, relay_addrs, verbose);
			register_callback_on_ioa_engine_new_connection(relay_servers[i]->ioa_eng, send_socket);
			ioa_engine_set_rtcp_map(relay_servers[i]->ioa_eng, listener.rtcpmap);
		}

		bufferevent_pair_new(relay_servers[i]->event_base, BEV_OPT_THREADSAFE, pair);
		relay_servers[i]->in_buf = pair[0];
		relay_servers[i]->out_buf = pair[1];
		bufferevent_setcb(relay_servers[i]->in_buf, acceptsocket, NULL, NULL, relay_servers[i]);
		bufferevent_enable(relay_servers[i]->in_buf, EV_READ);
		relay_servers[i]->server = create_turn_server(verbose, relay_servers[i]->ioa_eng, &stats, 0, fingerprint, DONT_FRAGMENT_SUPPORTED);

		if(relay_servers_number<2) {
			relay_servers[i]->thr = pthread_self();
		} else {
			if(pthread_create(&(relay_servers[i]->thr), NULL, run_relay_thread, relay_servers[i])<0) {
				perror("Cannot create relay thread\n");
				exit(-1);
			}
			pthread_detach(relay_servers[i]->thr);
		}
	}
}

static void setup_server(void)
{
	evthread_use_pthreads();

	setup_listener_servers();

	setup_relay_servers();
}

///////////////////////////////////////////////////////////////

static void clean_server(void)
{
	size_t i = 0;

	if (relay_servers) {
		for(i=0;i<relay_servers_number;i++) {
			if(relay_servers[i]) {
				delete_turn_server(relay_servers[i]->server);
				if(relay_servers[i]->ioa_eng != listener.ioa_eng)
					close_ioa_engine(relay_servers[i]->ioa_eng);
				if(relay_servers[i]->in_buf)
					bufferevent_free(relay_servers[i]->in_buf);
				if(relay_servers[i]->out_buf)
					bufferevent_free(relay_servers[i]->out_buf);
				if(relay_servers[i]->event_base != listener.event_base)
					event_base_free(relay_servers[i]->event_base);
				free(relay_servers[i]);
				relay_servers[i] = NULL;
			}
		}
		free(relay_servers);
	}

	if(listener.services) {
		for(i=0;i<listener.number; i++) {
			if (listener.services[i]) {
				delete_udp_listener_server(listener.services[i],0);
				listener.services[i] = NULL;
			}
		}
		free(listener.services);
		listener.services = NULL;
	}

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
		for(i=0;i<listener.number; i++) {
			if (listener.addrs[i]) {
				free(listener.addrs[i]);
				listener.addrs[i] = NULL;
			}
		}
		free(listener.addrs);
		listener.addrs = NULL;
	}

	listener.number = 0;
}

//////////////////////////////////////////////////

static int make_local_listeners_list(void)
{
	struct ifaddrs * ifs = NULL;
	struct ifaddrs * ifa = NULL;

	char saddr[INET6_ADDRSTRLEN] = "\0";

	getifaddrs(&ifs);

	if (ifs) {
		for (ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {

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
			printf("Added listener address %s (%s)\n",saddr,ifa->ifa_name);
		}
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
		for (ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {

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
			printf("Added relay address %s (%s)\n",saddr,ifa->ifa_name);
		}
		freeifaddrs(ifs);
	}

	return 0;
}

//////////////////////////////////////////////////

static char Usage[] = "Usage: turnserver [options]\n"
	"Options:\n"
	"	-d, --listening-device	Listener interface device (optional, Linux only)\n"
	"	-p, --listening-port	TURN listener port (Default: 3478)\n"
	"	-L, --listening-ip	Listener IP address of relay server. Multiple listeners can be specified\n"
	"	-i, --relay-device	Relay interface device for relay sockets (optional, Linux only)\n"
	"	-E, --relay-ip		Relay address (the local IP address that will be used to relay the packets to the peer)\n"
	"	-m, --relay-threads	Number of extra threads to handle established connections (default is 0)\n"
	"	-l, --min-port		Lower bound of the UDP port range for relay endpoints allocation.\n"
	"				Default value is 49152, according to RFC 5766.\n"
	"	-r, --max-port		Upper bound of the UDP port range for relay endpoints allocation.\n"
	"				Default value is 65535, according to RFC 5766.\n"
	"	-v, --verbose		Verbose\n"
	"	-f, --fingerprint	Use fingerprints in the TURN messages\n"
	"	-c			Configuration file name (default - turn.conf)\n"
	"	-n			Do not use configuration file\n"
	"	-h			Help\n";

static char *skip_blanks(char* s)
{
	while(*s==' ' || *s=='\t' || *s=='\n')
		++s;

	return s;
}

static FILE *find_config_file(const char *config_file)
{
	if (config_file && config_file[0]) {
		if (config_file[0] == '/') {
			FILE *f = fopen(config_file, "r");
			if (f)
				return f;
		} else {
			int i = 0;
			size_t cflen = strlen(config_file);

			while (config_file_search_dirs[i]) {
				size_t dirlen = strlen(config_file_search_dirs[i]);
				char *fn = malloc(sizeof(char) * (dirlen + cflen + 1));
				strcpy(fn,config_file_search_dirs[i]);
				strcpy(fn+dirlen,config_file);
				FILE *f = fopen(fn, "r");
				if (f) {
					fprintf(stdout,"Configuration file found: %s\n",fn);
					free(fn);
					return f;
				}
				free(fn);
				++i;
			}
		}
	}
	return NULL;
}

static char** read_config_file(char **argv, int argc, int *newargc)
{
	int i=0;
	char config_file[1025];
	char **newargv = NULL;

	strcpy(config_file,DEFAULT_CONFIG_FILE);

	newargv=malloc(sizeof(char*)*argc);
	*newargc=0;

	for(i=0;i<argc;i++) {
		if(!strcmp(argv[i],"-c")) {
			if(i<argc-1) {
				strncpy(config_file,argv[i+1],sizeof(config_file)-1);
			} else {
				fprintf(stderr,"Wrong usage of -c option\n");
			}
		} else if(!strcmp(argv[i],"-n")) {
			config_file[0]=0;
		} else if(!strcmp(argv[i],"-h")) {
			fprintf(stdout, "%s\n", Usage);
			exit(0);
		} else {
			newargv[(*newargc)++] = strdup(argv[i]);
		}
	}

	if(!config_file[0])
		return newargv;

	FILE *f = find_config_file(config_file);

	if(f) {
		char sbuf[1025];
		char sarg[1035];

		strcpy(sarg,"--");
		for(;;) {
			char *s = fgets(sbuf,sizeof(sbuf)-1,f);
			if(!s) break;
			s = skip_blanks(s);
			if(s[0]=='#')
				continue;
			size_t slen = strlen(s);
			while(slen && (s[slen-1]==10 || s[slen-1]==13)) s[--slen]=0;
			if(slen) {
				strcpy(sarg+2,s);
				newargv=realloc(newargv,sizeof(char*)*(*newargc+1));
				newargv[(*newargc)++] = strdup(sarg);
			}
		}

		fclose(f);

	} else {
		fprintf(stderr,"Cannot file config file: %s\n",config_file);
		exit(-1);
	}

	return newargv;
}

#define OPTIONS "d:p:L:E:i:m:l:r:vfh"

static struct option long_options[] = {
				{ "listening-device", required_argument, NULL, 'd' },
				{ "listening-port", required_argument, NULL, 'p' },
				{ "listening-ip", required_argument, NULL, 'L' },
				{ "relay-device", required_argument, NULL, 'i' },
				{ "relay-ip", required_argument, NULL, 'E' },
				{ "relay-threads", required_argument, NULL, 'm' },
				{ "min-port", required_argument, NULL, 'l' },
				{ "max-port", required_argument, NULL, 'r' },
				{ "verbose", optional_argument, NULL, 'v' },
				{ "fingerprint", optional_argument, NULL, 'f' },
				{ NULL, no_argument, NULL, 0 }
};

static int get_bool_value(const char* s)
{
	if(!s || !(s[0])) return 1;
	if(s[0]=='0' || s[0]=='n' || s[0]=='N') return 0;
	if(s[0]=='y' || s[0]=='Y') return 1;
	if(s[0]>'0' && s[0]<='9') return 1;
	if(!strcmp(s,"off") || !strcmp(s,"OFF") || !strcmp(s,"Off")) return 0;
	if(!strcmp(s,"on") || !strcmp(s,"ON") || !strcmp(s,"On")) return 1;
	fprintf(stderr,"Unknown boolean value: %s. You can use on/off, yes/no, 1/0.\n",s);
	exit(-1);
	return 0;
}

int main(int argc, char **argv)
{
	char c = 0;

	srandom((unsigned int) time(NULL));

	int newargc=0;
	char** newargv=read_config_file(argv,argc,&newargc);

	argc=newargc;
	argv=newargv;

	while (((c = getopt_long(argc, argv, OPTIONS, long_options, NULL)) != -1)) {
		switch (c){
		case 'i':
			strcpy(relay_ifname, optarg);
			break;
		case 'm':
			relay_servers_number = atoi(optarg) + 1;
			break;
		case 'd':
			strcpy(ifname, optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'l':
			min_port = atoi(optarg);
			break;
		case 'r':
			max_port = atoi(optarg);
			break;
		case 'L':
			add_listener_addr(optarg);
			break;
		case 'E':
			add_relay_addr(optarg);
			break;
		case 'v':
			verbose = get_bool_value(optarg);
			break;
		case 'f':
			fingerprint = get_bool_value(optarg);
			break;
		default:
			fprintf(stderr, "%s\n", Usage);
			exit(-1);
		}
	}

	argc -= optind;
	argv += optind;

	if(argc>0) {
		fprintf(stderr,"Unknown argument: %s\n",argv[argc-1]);
		exit(-1);
	}

	if (!listener.number) {
		make_local_listeners_list();
		if (!listener.number) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "You must specify the listener address(es)\n", __FUNCTION__);
			fprintf(stderr, "%s\n", Usage);
			exit(-1);
		}
	}

	if (!relays_number) {
		make_local_relays_list(0);
		if (!relays_number) {
			make_local_relays_list(1);
			if (!relays_number) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "You must specify the relay address(es)\n",
								__FUNCTION__);
				fprintf(stderr, "%s\n", Usage);
				exit(-1);
			}
		}
	}

	setup_server();

	run_server(listener.event_base);

	clean_server();

	return 0;
}


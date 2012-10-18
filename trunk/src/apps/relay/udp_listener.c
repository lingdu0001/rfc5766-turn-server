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

#include "apputils.h"

#include "ns_turn_utils.h"
#include "ns_turn_msg.h"

#include "udp_listener.h"
#include "ns_ioalib_impl.h"

/* #define REQUEST_CLIENT_CERT */

///////////////////////////////////////////////////

typedef struct {
  ioa_addr local_addr;
  ioa_addr remote_addr;
  ioa_socket_raw fd;
} ur_conn_info;

///////////////////////////////////////////////////

#define FUNCSTART if(server && server->verbose) TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"%s:%d:start\n",__FUNCTION__,__LINE__)
#define FUNCEND if(server && server->verbose) TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"%s:%d:end\n",__FUNCTION__,__LINE__)

struct udp_listener_relay_server_info {
  char ifname[1025];
  ioa_addr addr;
  ioa_addr relay_addr;
  ioa_engine_handle e;
  int verbose;
  struct event *udp_listen_ev;
  evutil_socket_t udp_listen_fd;
  uint32_t *stats;
};

/////////////// io handlers ///////////////////

static evutil_socket_t open_client_connection_socket(udp_listener_relay_server_type* server, ur_conn_info *pinfo);

static void server_input_handler(evutil_socket_t fd, short what, void* arg)
{

	udp_listener_relay_server_type* server = (udp_listener_relay_server_type*) arg;

	FUNCSTART;

	if (!server)
		return;

	if (!(what & EV_READ))
		return;

	if (server->stats)
		++(*(server->stats));

	ioa_addr client_addr;

	int rc = 0;

	unsigned char buf[0xFFFF + 1];
	ioa_addr si_other;
	int slen = get_ioa_addr_len(&(server->addr));

	int flags = MSG_DONTWAIT;

	do {
		rc = recvfrom(fd, buf, sizeof(buf), flags, (struct sockaddr*) &si_other, (socklen_t*) &slen);
	} while (rc < 0 && ((errno == EINTR) || (errno == EAGAIN)));

	if (rc > 0) {

		buf[rc] = 0;

		if (stun_is_request_str((u08bits*)buf, rc)) {

			addr_cpy(&client_addr, &si_other);

			ur_conn_info info;

			memset(&info, 0, sizeof(info));
			info.fd = -1;
			addr_cpy(&(info.remote_addr), &client_addr);
			addr_cpy(&(info.local_addr), &(server->addr));

			if (open_client_connection_socket(server, &info) >= 0) {

				ioa_socket_handle ioas = create_ioa_socket_from_fd(server->e, info.fd,
								&info.remote_addr, &info.local_addr);

				if (ioas) {

					rc = server->e->connect_cb(server->e, ioas, buf, rc);

					if(rc < 0) {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot create UDP session\n");
						close_ioa_socket(ioas);
					}
				} else {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot create ioa_socket from FD\n");
					close(info.fd);
				}
			}

		}
	}

	if (server->stats)
		--(*(server->stats));

	FUNCEND	;
}

///////////////////// operations //////////////////////////

static evutil_socket_t open_client_connection_socket(udp_listener_relay_server_type* server, ur_conn_info *pinfo) {

  FUNCSTART;

  if(!server) return -1;

  if(!pinfo) return -1;

  if(server->verbose) 
  {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"%s: AF: %d:%d\n",__FUNCTION__,
	   (int)pinfo->remote_addr.ss.ss_family,(int)server->addr.ss.ss_family);
  }

  pinfo->fd = socket(pinfo->remote_addr.ss.ss_family, SOCK_DGRAM, 0);
  if (pinfo->fd < 0) {
    perror("socket");
    return -1;
  }

  if(sock_bind_to_device(pinfo->fd, (unsigned char*)server->ifname)<0) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"Cannot bind client socket to device %s\n",server->ifname);
  }

  set_sock_buf_size(pinfo->fd,UR_CLIENT_SOCK_BUF_SIZE);
  
  socket_set_reusable(pinfo->fd);

  if(server->verbose) {
	  TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"Binding socket %d to addr\n",pinfo->fd);
	  addr_debug_print(server->verbose,&server->addr,"Bind to");
  }

  if(addr_bind(pinfo->fd,&server->addr)<0) {
    evutil_closesocket(pinfo->fd);
    pinfo->fd=-1;
    return -1;
  }

  if(addr_connect(pinfo->fd,&pinfo->remote_addr)<0) {
    evutil_closesocket(pinfo->fd);
    pinfo->fd=-1;
    return -1;
  }

  addr_debug_print(server->verbose, &pinfo->remote_addr,"UDP connected to");
  
  evutil_make_socket_nonblocking(pinfo->fd);

  set_socket_df(pinfo->fd, pinfo->remote_addr.ss.ss_family, 1);

  FUNCEND;

  return pinfo->fd;
}

static int create_server_socket(udp_listener_relay_server_type* server) {

  FUNCSTART;

  if(!server) return -1;

  server->udp_listen_fd = -1;

  server->udp_listen_fd = socket(server->addr.ss.ss_family, SOCK_DGRAM, 0);
  if (server->udp_listen_fd < 0) {
    perror("socket");
    return -1;
  }

  set_sock_buf_size(server->udp_listen_fd,UR_SERVER_SOCK_BUF_SIZE);

  socket_set_reusable(server->udp_listen_fd);

  if(sock_bind_to_device(server->udp_listen_fd, (unsigned char*)server->ifname)<0) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"Cannot bind listener socket to device %s\n",server->ifname);
  }

  addr_bind(server->udp_listen_fd,&server->addr);

  set_socket_df(server->udp_listen_fd, server->addr.ss.ss_family, 1);

  evutil_make_socket_nonblocking(server->udp_listen_fd);

  server->udp_listen_ev = event_new(server->e->event_base,server->udp_listen_fd,
				    EV_READ|EV_PERSIST,server_input_handler,server);

  event_add(server->udp_listen_ev,NULL);

  if(addr_get_from_sock(server->udp_listen_fd, &(server->addr))) {
    perror("Cannot get local socket addr");
    return -1;
  }

  addr_debug_print(server->verbose, &server->addr,"Listener opened on ");

  FUNCEND;
  
  return 0;
}

static int init_server(udp_listener_relay_server_type* server,
		       const char* ifname,
		       const char *local_address, 
		       int port, 
		       int verbose,
		       ioa_engine_handle e,
		       uint32_t *stats) {

  if(!server) return -1;

  server->stats=stats;

  if(ifname) strncpy(server->ifname,ifname,sizeof(server->ifname)-1);

  if(make_ioa_addr((const u08bits*)local_address, port, &server->addr)<0) {
    return -1;
  }

  server->verbose=verbose;
  
  server->e = e;
  
  TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,"IO method: %s\n",event_base_get_method(server->e->event_base));
  
  return create_server_socket(server);
}

static int clean_server(udp_listener_relay_server_type* server) {
  if(server) {
    EVENT_DEL(server->udp_listen_ev);
    if(server->udp_listen_fd>=0) {
      evutil_closesocket(server->udp_listen_fd);
      server->udp_listen_fd=-1;
    }
  }
  return 0;
}

///////////////////////////////////////////////////////////


udp_listener_relay_server_type* create_udp_listener_server(const char* ifname,
							     const char *local_address, 
							     int port, 
							     int verbose,
							     ioa_engine_handle e,
							     uint32_t *stats) {
  
  udp_listener_relay_server_type* server=malloc(sizeof(udp_listener_relay_server_type));

  memset(server,0,sizeof(udp_listener_relay_server_type));

  if(init_server(server,
		 ifname, local_address, port,
		 verbose,
		 e,
		 stats)<0) {
    free(server);
    return NULL;
  } else {
    return server;
  }
}

void delete_udp_listener_server(udp_listener_relay_server_type* server, int delete_engine) {
  if(server) {
    clean_server(server);
    if(delete_engine)
    	close_ioa_engine(server->e);
    free(server);
  }
}

//////////////////////////////////////////////////////////////////

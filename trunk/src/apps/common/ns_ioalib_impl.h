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

/*
 * IO Abstraction library
 */

#ifndef __IOA_LIBIMPL__
#define __IOA_LIBIMPL__

#include <event2/event.h>

#include "ns_turn_ioalib.h"
#include "turn_ports.h"
#include "ns_turn_maps_rtcp.h"

#include "apputils.h"

struct _ioa_engine
{
  struct event_base *event_base;
  int deallocate_eb;
  ioa_engine_new_connection_event_handler connect_cb;
  s08bits relay_ifname[1025];
  s08bits relay_address[1025];
  ioa_addr relay_addr;
  int verbose;
  turnipports* tp;
  rtcp_map *rtcp_map;
};

struct _ioa_socket
{
	evutil_socket_t fd;
	int family;
	SOCKET_TYPE st;
	int bound;
	int local_addr_known;
	ioa_addr local_addr;
	int connected;
	ioa_addr remote_addr;
	ioa_engine_handle e;
	struct event *read_event;
	ioa_net_event_handler read_cb;
	void *read_ctx;
	volatile int done;
	unsigned long tid;
	void* session;
	TURN_MUTEX_DECLARE(mutex)
};

typedef struct _timer_event
{
	struct event *ev;
	ioa_engine_handle e;
	ioa_timer_event_handler cb;
	void *ctx;
	s08bits* txt;
} timer_event;

/* engine handling */

ioa_engine_handle create_ioa_engine(struct event_base *eb, turnipports* tp, 
				    const s08bits* relay_if, const s08bits* relay_addr, int verbose);
void close_ioa_engine(ioa_engine_handle e);

void ioa_engine_set_rtcp_map(ioa_engine_handle e, rtcp_map *rtcpmap);

const ioa_addr* ioa_engine_get_relay_addr(ioa_engine_handle e);

ioa_socket_handle create_ioa_socket_from_fd(ioa_engine_handle e, ioa_socket_raw fd, const ioa_addr *remote_addr, const ioa_addr *local_addr);

void ioa_engine_update_relay_address(ioa_engine_handle e, const s08bits* relay_ifname, const s08bits* relay_address);
int register_callback_on_ioa_engine_new_connection(ioa_engine_handle e, ioa_engine_new_connection_event_handler cb);

#endif /* __IOA_LIBIMPL__ */

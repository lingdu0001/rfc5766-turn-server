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

#include "ns_turn_utils.h"

///////////////////////// LOG ///////////////////////////////////

#if defined(TURN_LOG_FUNC_IMPL)
extern void TURN_LOG_FUNC_IMPL(TURN_LOG_LEVEL level, const s08bits* format, va_list args);
#endif

void turn_log_func_default(TURN_LOG_LEVEL level, const s08bits* format, ...) {
  va_list args;
  va_start(args,format);
#if defined(TURN_LOG_FUNC_IMPL)
  TURN_LOG_FUNC_IMPL(level,format,args);
#else
  printf("<%d>: ",(int)level);
  vprintf(format,args);
#endif
  va_end(args);
}

void addr_debug_print(int verbose, const ioa_addr *addr, const s08bits* s)
{
	if (verbose) {
		if (!addr) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "\n%s: EMPTY\n", s);
		} else {
			s08bits addrbuf[INET6_ADDRSTRLEN];
			if (!s)
				s = "";
			if (addr->ss.ss_family == AF_INET) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "\nIPv4. %s: %s:%d\n", s, inet_ntop(AF_INET,
								&addr->s4.sin_addr, addrbuf, INET6_ADDRSTRLEN),
								nswap16(addr->s4.sin_port));
			} else if (addr->ss.ss_family == AF_INET6) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "\nIPv6. %s: %s:%d\n", s, inet_ntop(AF_INET6,
								&addr->s6.sin6_addr, addrbuf, INET6_ADDRSTRLEN),
								nswap16(addr->s6.sin6_port));
			} else {
				if (addr_any_no_port(addr)) {
					TURN_LOG_FUNC(
									TURN_LOG_LEVEL_INFO,
									"\nIP. %s: 0.0.0.0:%d\n",
									s,
									nswap16(addr->s4.sin_port));
				} else {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "%s: wrong IP address family: %d\n", s,
									(int) (addr->ss.ss_family));
				}
			}
		}
	}
}

//////////////////////////////////////////////////////////////////

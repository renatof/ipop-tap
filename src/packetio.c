/*
 * ipop-tap
 * Copyright 2013, University of Florida
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#if defined(LINUX) || defined(ANDROID)
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#elif defined(WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include <win32_tap.h>
#endif

#include "peerlist.h"
#include "headers.h"
#include "translator.h"
#include "tap.h"
#include "ipop_tap.h"
#include "packetio.h"

/**
 * Reads packet data from the tap device that was locally written, and sends it
 * off through a socket to the relevant peer(s).
 */
void *
ipop_send_thread(void *data)
{
    thread_opts_t *opts = (thread_opts_t *) data;
    int sock4 = opts->sock4;
    int sock6 = opts->sock6;
#if defined(LINUX) || defined(ANDROID)
    int tap = opts->tap;
#elif defined(WIN32)
    windows_tap *win32_tap = opts->win32_tap;
#endif

    int rcount, ncount;
    unsigned char enc_buf[BUFLEN];
    unsigned char *buf = enc_buf + BUF_OFFSET ;
    struct in_addr local_ipv4_addr;
    struct in6_addr local_ipv6_addr;
    struct peer_state *peer = NULL;
    int result, is_ipv4;

    while (1) {
#if defined(LINUX) || defined(ANDROID)
        if ((rcount = read(tap, buf, BUFLEN-BUF_OFFSET)) < 0) {
#elif defined(WIN32)
        if ((rcount = read_tap(win32_tap, (char *)buf, BUFLEN-BUF_OFFSET)) < 0) {
#endif
            fprintf(stderr, "tap read failed\n");
            break;
        }

        if (buf[12] == 0x08 && buf[13] == 0x06 && buf[21] == 0x01) {
            if (create_arp_response(buf) == 0) {
#if defined(LINUX) || defined(ANDROID)
                write(tap, buf, rcount);
#elif defined(WIN32)
                write_tap(win32_tap, (char *)buf, rcount);
#endif
            }
            continue;
        }

        if ((buf[14] >> 4) == 0x04) { // ipv4 packet
            memcpy(&local_ipv4_addr.s_addr, buf + 30, 4);
            is_ipv4 = 1;
        } else if ((buf[14] >> 4) == 0x06) { // ipv6 packet
            memcpy(&local_ipv6_addr.s6_addr, buf + 38, 16);
            is_ipv4 = 0;
        } else {
            fprintf(stderr, "unknown IP packet type: 0x%x\n", buf[14] >> 4);
            continue;
        }
        ncount = rcount + BUF_OFFSET;
        peerlist_reset_iterators();
        while (1) {
            if (is_ipv4) {
                result = peerlist_get_by_local_ipv4_addr(&local_ipv4_addr,
                                                         &peer);
            }
            else {
                result = peerlist_get_by_local_ipv6_addr(&local_ipv6_addr,
                                                         &peer);
            }
            if (result == -1) break;
            set_headers(enc_buf, peerlist_local.id, peer->id);
            if (is_ipv4 && opts->translate) {
                translate_packet(buf, NULL, NULL, rcount);
            }
            if (opts->send_func != NULL) {
                if (opts->send_func((const char*)enc_buf, ncount) < 0) {
                    fprintf(stderr, "send_func failed\n");
                }
            }
            else {
                struct sockaddr_in dest_ipv4_addr_sock = {
                    .sin_family = AF_INET,
                    .sin_port = htons(peer->port),
                    .sin_addr = peer->dest_ipv4_addr,
                    .sin_zero = { 0 }
                };
                // send our processed packet off
                if (sendto(sock4, (const char *)enc_buf, ncount, 0,
                           (struct sockaddr *)(&dest_ipv4_addr_sock),
                           sizeof(struct sockaddr_in)) < 0) {
                    fprintf(stderr, "sendto failed\n");
                }
            }
            if (result == 0) break;
        }
    }

    close(sock4);
    close(sock6);
#if defined(LINUX) || defined(ANDROID)
    tap_close();
#elif defined(WIN32)
    // TODO - Add close socket for tap
    WSACleanup();
#endif
    pthread_exit(NULL);
    return NULL;
}

/**
 * Reads packet data from the socket that we received from a remote peer, and
 * writes it to the local tap device, making the traffic show up locally.
 */
void *
ipop_recv_thread(void *data)
{
    thread_opts_t *opts = (thread_opts_t *) data;
    int sock4 = opts->sock4;
    int sock6 = opts->sock6;
#if defined(LINUX) || defined(ANDROID)
    int tap = opts->tap;
#elif defined(WIN32)
    windows_tap *win32_tap = opts->win32_tap;
#endif

    int rcount;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    unsigned char enc_buf[BUFLEN];
    unsigned char *buf = enc_buf + BUF_OFFSET;
    char source_id[ID_SIZE] = { 0 };
    char dest_id[ID_SIZE] = { 0 };
    struct peer_state *peer = NULL;

    while (1) {
        if (opts->recv_func != NULL) {
            // read from ipop-tincan
            if ((rcount = opts->recv_func((char *)enc_buf, BUFLEN)) < 0) {
              fprintf(stderr, "recv_func failed\n");
              break;
            }
        }
        else if ((rcount = recvfrom(sock4, (char *)enc_buf, BUFLEN, 0,
                               (struct sockaddr*) &addr, &addrlen)) < 0) {
            // read from UDP socket (useful for testing)
            fprintf(stderr, "udp recv failed\n");
            break;
        }

        rcount -= BUF_OFFSET;
        get_headers(enc_buf, source_id, dest_id);
        if ((buf[14] >> 4) == 0x04 && opts->translate) {
            int peer_found = peerlist_get_by_id(source_id, &peer);
            if (peer_found != -1) {
                translate_packet(buf, (char *)(&peer->local_ipv4_addr.s_addr),
                                (char *)(&peerlist_local.local_ipv4_addr.s_addr),
                                rcount);
                translate_headers(buf, (char *)(&peer->local_ipv4_addr.s_addr),
                                  (char *)(&peerlist_local.local_ipv4_addr.s_addr),
                                  rcount);
            }
        }

        update_mac(buf, opts->mac);
#if defined(LINUX) || defined(ANDROID)
        if (write(tap, buf, rcount) < 0) {
#elif defined(WIN32)
        if (write_tap(win32_tap, (char *)buf, rcount) < 0) {
#endif
            fprintf(stderr, "write to tap error\n");
            break;
        }
    }

    close(sock4);
    close(sock6);
#if defined(LINUX) || defined(ANDROID)
    tap_close();
#elif defined(WIN32)
    // TODO - Add close for windows tap
    WSACleanup();
#endif
    pthread_exit(NULL);
    return NULL;
}


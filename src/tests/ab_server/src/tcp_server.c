/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include "slice.h"
#include "socket.h"
#include "tcp_server.h"
#include "utils.h"

struct tcp_server {
    int sock_fd;
    slice_s buffer;
    slice_s (*handler)(slice_s input, slice_s output, void *context);
    void *context;
    int num_accepted_sock;
    fd_set accept_fd_set;
};


tcp_server_p tcp_server_create(const char *host, const char *port, slice_s buffer, slice_s (*handler)(slice_s input, slice_s output, void *context), void *context)
{
    tcp_server_p server = calloc(1, sizeof(*server));

    if(server) {
        server->sock_fd = socket_open(host, port);

        if(server->sock_fd < 0) {
            error("ERROR: Unable to open TCP socket, error code %d!", server->sock_fd);
        }

        server->buffer = buffer;
        server->handler = handler;
        server->context = context;
    }

    return server;
}

void tcp_server_start(tcp_server_p server, volatile sig_atomic_t *terminate)
{
    int client_fd;

    int num_accept_ready;

    fd_set temp_fd_set;

    server->num_accepted_sock = 0;
    /* zero out the file descriptor set. */
    FD_ZERO(&server->accept_fd_set);

    /* set our socket's bit in the set. */
    FD_SET(server->sock_fd, &server->accept_fd_set);

    /* set the timeout to zero */
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    /* set the maximum number of fds */
    int fd_max = server->sock_fd;

    do {
        temp_fd_set = server->accept_fd_set;

        /* check changed fd */
        int num_accept_ready = select(fd_max + 1, &temp_fd_set, NULL, NULL, &timeout);
        if(num_accept_ready < 0) {
            info("Error selecting the listen socket!");
            continue;
        } else if(num_accept_ready == 0) {
            util_sleep_ms(1);
            continue;
        } else if(num_accept_ready > MAX_CLIENTS) {
            info("WARN: Too much client required");
            continue;
        }
        
        if(server->num_accepted_sock != num_accept_ready) {
            info("Ready to accept on %d sockets.", num_accept_ready);
            server->num_accepted_sock = num_accept_ready;
        }

#ifdef IS_WINDOWS
        HANDLE threads[MAX_CLIENTS];
#else
        pthread_t threads[MAX_CLIENTS];
#endif
        int client_seq = 0;

        if(FD_ISSET(server->sock_fd, &temp_fd_set)) {
            client_fd = (int)accept(server->sock_fd, NULL, NULL);
            info("Accept socket %d.", client_fd);
            FD_SET(client_fd, &server->accept_fd_set);
            if(fd_max < client_fd) {
                fd_max = client_fd;
            }

            thread_param param;
            param.server = server;
            param.client_sock = client_fd;
            param.client_seq = client_seq;

#ifdef IS_WINDOWS
            threads[client_seq] = CreateThread(NULL, 0, process_loop,
                                               (void *)&param, (DWORD)0, (LPDWORD)NULL);
#else
            pthread_create(&threads[client_seq], NULL, process_loop, (void *)&param);
#endif
            client_seq++;
        }

        /* wait a bit to give back the CPU. */
        util_sleep_ms(1);

    } while(!*terminate);
    terminated = true;

    for(int fd = 0; fd < fd_max + 1; fd++) {
        if(fd != server->sock_fd && FD_ISSET(fd, &server->accept_fd_set)) {
            socket_close(fd);
        }
    }
}



void tcp_server_destroy(tcp_server_p server)
{
    if(server) {
        if(server->sock_fd >= 0) {
            socket_close(server->sock_fd);
            server->sock_fd = INT_MIN;
        }
        free(server);
    }
}

#ifdef IS_WINDOWS
void process_loop(LPVOID data)
#else
void process_loop(void *data)
#endif
{
    thread_param *param = (thread_param *)data;
    tcp_server_p server = param->server;
    int client_fd = param->client_sock;
    int client_seq = param->client_seq;

    bool done = false;
    do
    {
        if(client_fd >= 0) {
            slice_s tmp_input = get_server_buffer(server, client_seq);
            slice_s tmp_output;
            int rc;

            info("Got new client connection, going into processing loop.");

            do {
                rc = TCP_SERVER_PROCESSED;

                /* get an incoming packet or a partial packet. */
                tmp_input = socket_read(client_fd, tmp_input);

                if((rc = slice_has_err(tmp_input))) {
                    info("WARN: error response reading socket! error %d", rc);
                    rc = TCP_SERVER_DONE;
                    remove_client(server, client_fd);
                    return;
                }

                /* try to process the packet. */
                tmp_output = server->handler(tmp_input, get_server_buffer(server, client_seq), server->context);

                /* check the response. */
                if(!slice_has_err(tmp_output)) {
                    /* FIXME - this should be in a loop to make sure all data is pushed. */
                    rc = socket_write(client_fd, tmp_output);

                    /* error writing? */
                    if(rc < 0) {
                        info("ERROR: error writing output packet! Error: %d", rc);
                        rc = TCP_SERVER_DONE;
                        break;
                    } else {
                        /* all good. Reset the buffers etc. */
                        tmp_input = slice_from_slice(server->buffer, 0, 2100);
                        rc = TCP_SERVER_PROCESSED;
                    }
                } else {
                    /* there was some sort of error or exceptional condition. */
                    switch((rc = slice_get_err(tmp_input))) {
                        case TCP_SERVER_DONE:
                            done = true;
                            break;

                        case TCP_SERVER_INCOMPLETE:
                            tmp_input = get_server_buffer(server, client_seq);
                            break;

                        case TCP_SERVER_PROCESSED:
                            break;

                        case TCP_SERVER_UNSUPPORTED:
                            info("WARN: Unsupported packet!");
                            slice_dump(tmp_input);
                            break;

                        default:
                            info("WARN: Unsupported return code %d!", rc);
                            break;
                    }
                }
            } while(rc == TCP_SERVER_INCOMPLETE || rc == TCP_SERVER_PROCESSED);

            /* done with the socket */
            remove_client(server, client_fd);
        } else if (client_fd != SOCKET_STATUS_OK) {
            /* There was an error either opening or accepting! */
            info("WARN: error while trying to open/accept the client socket.");
        }
    } while(!done && !terminated);
}

slice_s get_server_buffer(tcp_server_p server, int seq)
{
    return slice_from_slice(server->buffer, seq, 4200 / MAX_CLIENTS);
}

void remove_client(tcp_server_p server, int fd)
{
    FD_CLR(fd, &server->accept_fd_set);
    socket_close(fd);
    server->num_accepted_sock--;
}
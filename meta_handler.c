/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Metadata request handler
 *
 * Copyright (C) 2017 Nick Kossifidis <mickflemm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <netinet/ip.h>	/* For IP stuff (also brings in socket etc) */
#include <arpa/inet.h>	/* For inet_aton */
#include <stdlib.h>	/* For malloc() / free() */
#include <string.h>	/* For memset() */
#include <stdio.h>	/* For snprintf() */
#include <unistd.h>	/* For read/write */
#include <errno.h>	/* For errno */
#include <time.h>	/* For time() / gmtime() / strftime() */
#include "meta_handler.h"
#include "utils.h"


/*********\
* HELPERS *
\*********/

static int
meta_create_server_socket(uint16_t port, const char* ip4addr)
{
	struct sockaddr_in name = {0};
	int sockfd = 0;
	int ret = 0;

	/* Create the socket. */
	sockfd = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sockfd < 0) {
		utils_perr(META, "Could not create server socket");
		return -errno;
	}

	/* Check if it's an ipv4 address */
	if(ip4addr != NULL) {
		ret = inet_aton(ip4addr, &name.sin_addr);
		if(ret != 0)
			return -EINVAL;
	} else
		name.sin_addr.s_addr = htonl(INADDR_ANY);

	/* Give the socket a name. */
	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	ret = bind(sockfd, (struct sockaddr *) &name, sizeof (name));
	if (ret < 0) {
		utils_perr(META, "Could not bind server socket");
		return -errno;
	}

	return sockfd;
}

static void
meta_server_thread_cleanup(void* arg)
{
	struct meta_handler *mh = (struct meta_handler*) arg;
	mh->active = 0;
	pthread_mutex_unlock(&mh->state.proc_mutex);
	close(mh->sockfd);
	utils_dbg(META, "Server thread terminated\n");
}


/*****************\
* SERVER CALLBACK *
\*****************/

static int
meta_server_callback(struct meta_handler *mh, int sockfd)
{
	struct current_state *st = &mh->state;
	struct song_info *curr = &st->current;
	struct song_info *next = &st->next;
	time_t now = 0;
	struct tm *tm = NULL;
	char date_str[64] = {0};
	int len = 0;
	int ret = 0;

	/* Ignore input */
	while(recv(sockfd, NULL, 0, MSG_TRUNC | MSG_OOB) > 0);

	/* Buffer freed */
	if(mh->msg_buff == NULL)
		return -1;

	/* Create JSON message */
	pthread_mutex_lock(&st->proc_mutex);
	snprintf(mh->msg_buff, ST_STRING_LEN,
		"{\n\t\"current_song\": {\n\t\t"
		"\"Artist\": \"%s\",\n\t\t"
		"\"Album\": \"%s\",\n\t\t"
		"\"Title\": \"%s\",\n\t\t"
		"\"Path\": \"%s\",\n\t\t"
		"\"Duration\": \"%i\",\n\t\t"
		"\"Elapsed\": \"%i\"\n\t\t},\n"
		"\t\"next_song\": {\n\t\t"
		"\"Artist\": \"%s\",\n\t\t"
		"\"Album\": \"%s\",\n\t\t"
		"\"Title\": \"%s\",\n\t\t"
		"\"Path\": \"%s\",\n\t\t"
		"\"Duration\": \"%i\",\n\t\t"
		"\"Elapsed\": \"%i\"\n\t\t},\n"
		"\t\"overlap\": \"%i\"\n}\r\n",
		curr->artist,
		curr->album,
		curr->title,
		curr->path,
		curr->duration_sec,
		curr->elapsed_sec,
		next->artist,
		next->album,
		next->title,
		next->path,
		next->duration_sec,
		next->elapsed_sec,
		st->overlap_sec);
	pthread_mutex_unlock(&st->proc_mutex);

	now = time(NULL);
	tm = gmtime(&now);
	strftime(date_str, 64, "%a, %d %b %Y %H:%M:%S %Z", tm);

	len = strnlen(mh->msg_buff, ST_STRING_LEN);

	dprintf(sockfd, "HTTP/1.1 200 OK\r\n"
			"Server: audio-scheduler\r\n"
			"Date: %s\r\n"
			"Content-Length: %i\r\n"
			"Content-type: application/json; charset=utf-8\r\n"
			"Pragma: no-cache\r\n"
			"Cache-Control: no-cache\r\n"
			"Connection: Closed\r\n\r\n",
			date_str, len);

	ret = write(sockfd, mh->msg_buff, len);
	if(ret < 0 || ret != len)
		utils_pwrn(META, "Write error");

	shutdown(sockfd, SHUT_WR);

	return 0;
}

/***************\
* SERVER THREAD *
\***************/

static void*
meta_server_thread(void* arg)
{
	struct meta_handler *mh = (struct meta_handler*) arg;
	struct sockaddr_in clientname = {0};
	fd_set active_set;
	int client_sockfd = 0;
	socklen_t size = 0;
	int i = 0;
	int ret = 0;

	utils_info(META, "Waiting for connections...\n");

	/* Register cleanup function */
	pthread_cleanup_push(meta_server_thread_cleanup, arg);

	/* Initialize the set of active sockets. */
	FD_ZERO(&active_set);
	FD_SET(mh->sockfd, &active_set);

	while(mh->active) {
		/* Block until input arrives on one or more active sockets. */
		ret = select(FD_SETSIZE, &active_set, NULL, NULL, NULL);
		if (ret < 0) {
			utils_perr(META, "select() failed");
			ret = -errno;
			goto cleanup;
		} if (!ret)
			continue;	/* No connection within timeout */

		/* Loop on active sockets */
		for (i = 0; i < FD_SETSIZE && mh->active; ++i) {

			if (!FD_ISSET(i, &active_set))
				continue;

			/* Data on master socket: Move the connection to
			 * a new socket and add it to the set of sockets
			 * to monitor. */
			if (i == mh->sockfd) {

				size = sizeof(clientname);
				client_sockfd = accept(mh->sockfd,
						(struct sockaddr *) &clientname,
						&size);

				if (client_sockfd < 0) {
					utils_perr(META, "accept() failed");
					ret = -errno;
					goto cleanup;
				}

				utils_info(META, "Connection from host %s.\n",
						inet_ntoa(clientname.sin_addr),
						ntohs(clientname.sin_port));

				FD_SET(client_sockfd, &active_set);

			/* Previously assigned socket */
			} else {
				ret = meta_server_callback(mh, i);
				if (ret < 0)
					goto cleanup;
				close(i);
				FD_CLR(i, &active_set);
			}
		}
	}

 cleanup:
	pthread_cleanup_pop(arg);
	return arg;
}


/**************\
* ENTRY POINTS *
\**************/

int
meta_handler_init(struct meta_handler *mh, uint16_t port, const char* ip4addr)
{
	int ret = 0;

	memset(mh, 0, sizeof(struct meta_handler));
	mh->port = port;
	mh->ipaddr = ip4addr;
	pthread_mutex_init(&mh->state.proc_mutex, NULL);

	/* Allocate output buffer */
	mh->msg_buff = malloc(ST_STRING_LEN);
	if(mh->msg_buff == NULL) {
		utils_perr(META, "Could not allocate output buffer");
		return  -errno;
	}

	/* Create the socket and set it up to accept connections. */
	mh->sockfd = meta_create_server_socket(port, ip4addr);
	if(mh->sockfd < 0)
		return mh->sockfd;

	ret = listen(mh->sockfd, 1);
	if (ret < 0) {
		utils_perr(META, "Could not mark socket as passive");
		return -errno;
	}

	/* Start server thread */
	mh->active = 1;
	ret = pthread_create(&mh->tid, NULL, meta_server_thread, (void*) mh);
	if(ret < 0) {
		utils_err(META, "Could not start server thread");
		return -ret;
	}

	return 0;
}

void
meta_handler_destroy(struct meta_handler *mh)
{
	if(mh->tid!=NULL){
		pthread_cancel(mh->tid);
		pthread_join(mh->tid, NULL);
	}
	free(mh->msg_buff);
	mh->msg_buff = NULL;
}

struct current_state*
meta_get_state(struct meta_handler *mh)
{
	return &mh->state;
}

/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2017 - 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This is a small and very simple HTTP server, that just replies with a JSON
 * representation of the current / next audiofile_info of the player, plus
 * the elapsed time in seconds of the current song. It's used mainly for
 * the station's website, or any other app that wants to know the player's
 * current state.
 */

#define _GNU_SOURCE	/* For accept4, TEMP_FAILURE_RETRY */
#include <netinet/ip.h>	/* For IP stuff (also brings in socket etc) */
#include <arpa/inet.h>	/* For inet_aton */
#include <string.h>	/* For memset() */
#include <stdio.h>	/* For snprintf() */
#include <stdlib.h>	/* For malloc()/free() */
#include <unistd.h>	/* For read/write */
#include <time.h>	/* For time() */
#include <errno.h>	/* For errno and error codes */
#include <sys/epoll.h>	/* For epoll, epoll_event etc */
#include <sys/socket.h> /* For Socket and options */
#include <netinet/in.h>	/* For inet_ntoa etc */
#include <netinet/tcp.h> /* For TCP flags */

#include "meta_handler.h"
#include "utils.h"

/****************\
* SIGNAL HANDLER *
\****************/

static void mh_signal_handler(int signal_number, void *userdata)
{
	struct meta_handler *mh = (struct meta_handler*) userdata;

	switch (signal_number) {
	case SIGINT:
	case SIGTERM:
		mh_stop(mh);
		break;
	default:
		break;
	}
}

/*****************\
* JSON FORMATTING *
\*****************/

/*
 * We need to make sure that the strings we'll put in the JSON won't break
 * the parser, we know that albumid and release_track_id can be used as-is
 * since they are just hashes, but for filenames and album/artist/title anything
 * is possible except control characters. This unfortunately includes double
 * quotes and backslashes, which both break the parser, and although we can
 * replace double quotes with single quotes, for backslashes we'll need to
 * escape them. This means that we can't work in-place, and pre-allocating
 * PATH_MAX or something large in stack for both current and next songs etc
 * is an overkill, so we'll go for dynamic allocations. At least we know they
 * are properly terminated.
 */

 static void mh_count_special_chars(const char *str, int *backslashes, int *dquotes) {
	*backslashes = 0;
	*dquotes = 0;
	const char *found;
	while ((found = strpbrk(str, "\\\"")) != NULL) {
		if (*found == '\\')
			(*backslashes)++;
		else
			(*dquotes)++;
		str = found + 1;
	}
}

static void mh_replace_inplace(char *str, char orig, char new) {
	char *ptr = str;
	while ((ptr = strchr(ptr, orig)) != NULL) {
	    *ptr = new;
	    ptr++;
	}
}

static char* mh_json_escape_string(char* str, int is_filename)
{
	if (!str)
		return "(null)";

	int backslashes = 0;
	int dquotes = 0;

	mh_count_special_chars(str, &backslashes, &dquotes);

	/* Nothing to do, use as-is */
	if (!backslashes && !dquotes)
		return str;

	/* If string is a filename, we can't go with replacing stuff
	 * since the filename from the JSON needs to be usable. No
	 * easy way out of this. */
	if (is_filename)
		goto no_replace;

	/* Low-hanging fruit: just replace double quotes
	 * with single ones in-place */
	if (dquotes)
		mh_replace_inplace(str, '"', '\'');

	/* Replace backslash with slash for album/artist */
	if (backslashes)
		mh_replace_inplace(str, '\\', '/');

	return str;

 no_replace:
	/* Calculate new length for the escaped string, for each
	 * character to escape, we need an extra backslash. */
	size_t new_len = strlen(str) + backslashes + dquotes + 1;
	char* new_str = malloc(new_len);
	if (!new_str) {
		utils_perr(META, "couldn't allocate buffer for escaping");
		return "(null)";
	}

	char *src = str;
	char *dst = new_str;
	char *found;

	/* Copy initial part */
	while ((found = strpbrk(src, "\\\"")) != NULL) {
	    /* Copy everything up to the special character */
	    size_t len = found - src;
	    memcpy(dst, src, len);
	    dst += len;

	    /* Add escape character and special character */
	    *dst++ = '\\';
	    *dst++ = *found;

	    /* Move src past the special character */
	    src = found + 1;
	}

	/* Copy remaining part including null terminator */
	strcpy(dst, src);

	return new_str;
}

static size_t mh_format_json_response(char *buf, size_t bufsize,
				 const struct audiofile_info *cur,
				 const struct audiofile_info *next,
				 uint32_t elapsed)
{
	/* Fill the buffer with the response body */

	/* We always need to operate on the original string or else we'll keep re-escaping */
	char* escaped_curr_filepath = mh_json_escape_string((char*) cur->filepath, 1);
	char* escaped_next_filepath = mh_json_escape_string((char*) next->filepath, 1);

	int ret = snprintf(buf, bufsize,
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Connection: close\r\n"
		"\r\n"
		"{"
		"\"current_song\": {"
			"\"Artist\": \"%s\","
			"\"Album\": \"%s\","
			"\"Title\": \"%s\","
			"\"Path\": \"%s\","
			"\"Duration\": \"%u\","
			"\"Elapsed\": \"%u\","
			"\"Zone\": \"%s\","
			"\"MusicBrainz Album Id\": \"%s\","
			"\"MusicBrainz Release Track Id\": \"%s\""
		"},"
		"\"next_song\": {"
			"\"Artist\": \"%s\","
			"\"Album\": \"%s\","
			"\"Title\": \"%s\","
			"\"Path\": \"%s\","
			"\"Duration\": \"%u\","
			"\"Zone\": \"%s\","
			"\"MusicBrainz Album Id\": \"%s\","
			"\"MusicBrainz Release Track Id\": \"%s\""
		"}"
		"}\r\n",
		mh_json_escape_string(cur->artist, 0),
		mh_json_escape_string(cur->album, 0),
		mh_json_escape_string(cur->title, 0),
		escaped_curr_filepath,
		(uint32_t) cur->duration_secs,
		elapsed,
		cur->zone_name,
		cur->albumid,
		cur->release_trackid,
		mh_json_escape_string(next->artist, 0),
		mh_json_escape_string(next->album, 0),
		mh_json_escape_string(next->title, 0),
		escaped_next_filepath,
		(uint32_t) next->duration_secs,
		next->zone_name,
		next->albumid,
		next->release_trackid);

		if (escaped_curr_filepath != cur->filepath)
			free(escaped_curr_filepath);
		if (escaped_next_filepath != next->filepath)
			free(escaped_next_filepath);

	return ret;
}


/****************\
* SERVER ACTIONS *
\****************/

static int
mh_create_server_socket(uint16_t port, const char* ip4addr)
{
	struct sockaddr_in name = {0};
	int sockfd = 0;
	int opt = 1;
	int ret = 0;

	/* Create the socket. */
	sockfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (sockfd < 0) {
		utils_perr(META, "Could not create server socket");
		return -errno;
	}

	/* Make sure we can re-bind imediately after a quick-restart */
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	/* We'll only send a small responce so skip Nagle's buffering */
	setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

	/* We don't expect the client to send more stuff (we won't even
	 * read them anyway), don't delay ACKs */
	setsockopt(sockfd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt));

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

	if (listen(sockfd, SOMAXCONN) < 0) {
		utils_perr(META, "Could not make socket passive");
		close(sockfd);
		return -EIO;
	}

	return sockfd;
}

static int mh_update_response(struct meta_handler *mh)
{
	static struct audiofile_info cur = {0};
	static struct audiofile_info next = {0};
	uint32_t elapsed;
	time_t now = time(NULL);
	int ret = 0;

	if (!mh->state_cb)
		return -1;

	/* Only update once per second */
	if (now == mh->last_update)
		return 0;

	pthread_mutex_lock(&mh->update_mutex);

	/* Get elapsed first */
	if (mh->state_cb(NULL, NULL, &elapsed, mh->player_data) < 0) {
		ret = -1;
		goto cleanup;
	}

	/* Get full info if needed */
	if (now > mh->next_update) {
		mldr_cleanup_audiofile(&cur);
		mldr_cleanup_audiofile(&next);
		if (mh->state_cb(&cur, &next, NULL, mh->player_data) < 0) {
			ret = -1;
			goto cleanup;
		}
		mh->next_update = now + (cur.duration_secs - elapsed) + 1;
	}

	/* Format response */
	mh->response_len = mh_format_json_response(mh->response,
						   sizeof(mh->response),
						   &cur, &next, elapsed);
	mh->last_update = now;

 cleanup:
	pthread_mutex_unlock(&mh->update_mutex);
	return ret;
}

static void mh_handle_client(struct meta_handler *mh, int client_fd)
{
	int ret = 0;

	/* Update response if needed */
	if (mh_update_response(mh) < 0)
		goto done;

	/* Send response directly, we don't care about the request */
	ret = TEMP_FAILURE_RETRY(send(client_fd, mh->response, mh->response_len, MSG_NOSIGNAL));
	if (ret < 0) {
		utils_perr(META, "write failed");
	} else if (ret != mh->response_len) {
		utils_wrn(META, "wrote partial response (%i vs %i)\n",
			  mh->response_len);
	}

 done:
	/* Send FIN to client, since we won't be
	 * sending any more data. */
	shutdown(client_fd, SHUT_WR);

	/* Close the connection but don't let unresponsive clients
	 * keep the socket in TIMED_WAIT sstate for too long. Allow
	 * them 5 secs and them force a reset. */
	struct linger l = {
		.l_onoff = 1,
		.l_linger = 5
	};
	setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
	close(client_fd);
}

static void mh_handle_new_connection(struct meta_handler *mh)
{
	struct sockaddr_in clientname = {0};
	socklen_t clientname_sz = sizeof(clientname);
	int client_fd;

	while (mh->running &&
	       (client_fd = accept4(mh->listen_fd,
				    (struct sockaddr *) &clientname,
				    &clientname_sz, SOCK_NONBLOCK)) > 0) {

					/* Leave this for debugging, under normal operation
		 * we expect frequent connections so this would polute
		 * the log otherwise */
		utils_dbg(META, "Connection from host %s at port %i\n",
			   inet_ntoa(clientname.sin_addr),
			   ntohs(clientname.sin_port));

		  mh_handle_client(mh, client_fd);
	}
}


/***************\
* SERVER THREAD *
\***************/

static void *mh_server_thread(void *arg)
{
	struct meta_handler *mh = arg;
	struct epoll_event events[32];
	int nfds;

	utils_info(META, "Waiting for connections...\n");

	while (mh->running) {
		nfds = epoll_wait(mh->epoll_fd, events, 32, 1000);
		if (nfds < 0 && errno != EINTR)
			break;

		for (int i = 0; i < nfds; i++) {
			if (events[i].data.fd == mh->listen_fd)
				mh_handle_new_connection(mh);
		}
	}

	return NULL;
}


/**************\
* ENTRY POINTS *
\**************/

void mh_stop(struct meta_handler *mh)
{
	if (!mh)
		return;

	utils_dbg(META, "Stopping\n");

	mh->running = 0;
	pthread_join(mh->thread, NULL);

	utils_dbg(META, "Stopped\n");
}

void mh_cleanup(struct meta_handler *mh)
{
	if (!mh)
		return;

	if (mh->listen_fd >= 0)
		close(mh->listen_fd);
	if (mh->epoll_fd >= 0)
		close(mh->epoll_fd);
	pthread_mutex_destroy(&mh->update_mutex);
}

int mh_init(struct meta_handler *mh, uint16_t port, const char* ip4addr, struct sig_dispatcher *sd)
{
	struct epoll_event ev;

	memset(mh, 0, sizeof(struct meta_handler));

	pthread_mutex_init(&mh->update_mutex, NULL);

	mh->epoll_fd = epoll_create1(0);
	if (mh->epoll_fd < 0) {
		utils_perr(META, "Could not create epoll_fd");
		goto cleanup;
	}

	mh->listen_fd = mh_create_server_socket(port, ip4addr);
	if (mh->listen_fd < 0)
		goto cleanup;

	/* Add listening socket to epoll */
	ev.events = EPOLLIN;
	ev.data.fd = mh->listen_fd;
	if (epoll_ctl(mh->epoll_fd, EPOLL_CTL_ADD, mh->listen_fd, &ev) < 0) {
		utils_perr(META, "epoll_ctl failed");
		goto cleanup;
	}

	/* Register with the signal dispatcher */
	sig_dispatcher_register(sd, SIG_UNIT_META, mh_signal_handler, mh);

	utils_dbg(META, "Initialized\n");
	return 0;

cleanup:
	mh_cleanup(mh);
	return -1;
}

int mh_start(struct meta_handler *mh)
{
	utils_dbg(META, "Starting\n");
	mh->running = 1;
	if (pthread_create(&mh->thread, NULL, mh_server_thread, mh) != 0) {
		utils_perr(META, "failed to create server thread\n");
		mh->thread = 0;
		return -1;
	}
	return 0;
}

int mh_register_state_callback(struct meta_handler *mh, mh_state_cb cb, void *player_data)
{
	if (!mh || !cb || !player_data)
		return -1;

	mh->state_cb = cb;
	mh->player_data = player_data;

	/* Force response update on next request */
	mh->last_update = 0;

	return 0;
}
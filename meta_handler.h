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

#include <stdint.h>	/* For typed ints */
#include <pthread.h>	/* For pthread stuff */
#include <linux/limits.h>	/* For PATH_MAX */

struct song_info {
	char*	artist;
	char*	album;
	char*	title;
	char*	path;
	uint32_t duration_sec;
	uint32_t elapsed_sec;
};

/* On IDv2 artist/album/title are up to 60chars,
 * Vorbis (ogg/flac) don't have that limitation.
 * 64 should be enough in any case */
#define SI_STRING_LEN	(64 + 64 + 64 + PATH_MAX + 10 + 10)

struct current_state {
	struct song_info current;
	struct song_info next;
	/* Overlap between next and current
	 * (how many secs of next will be played before
	 * current finishes) */
	uint32_t overlap_sec;
	pthread_mutex_t proc_mutex;
};

#define ST_STRING_LEN	((2 * SI_STRING_LEN) + 10) + 128

struct meta_handler {
	struct current_state state;
	char*	msg_buff;
	const char* ipaddr;
	int	sockfd;
	int	active;
	pthread_t tid;
	uint16_t port;
};

int meta_handler_init(struct meta_handler *mh, uint16_t port, const char* ip4addr);
void meta_handler_destroy(struct meta_handler *mh);
struct current_state* meta_get_state(struct meta_handler *mh);

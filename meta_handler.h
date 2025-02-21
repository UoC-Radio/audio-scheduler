/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2017 - 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __META_HANDLER_H__
#define __META_HANDLER_H__

#include <stdint.h>	/* For size_t */
#include <pthread.h>	/* For pthread stuff */
#include "scheduler.h"	/* For audiofile_info and time_t (through time.h) */
#include "sig_dispatcher.h" /* For registering with signal dispatcher */

/* Callback to the player for updating current state */
typedef int (*mh_state_cb)(struct audiofile_info *cur, struct audiofile_info *next,
			   uint32_t *elapsed_sec, void *player_data);

struct meta_handler {
	int	epoll_fd;
	int	listen_fd;
	void	*player_data;
	mh_state_cb state_cb;
	volatile int running;
	pthread_t thread;

	/* Cache last response */
	char response[2048];
	size_t response_len;
	time_t last_update;
	time_t next_update;
	pthread_mutex_t update_mutex;
};


void mh_stop(struct meta_handler *mh);
int mh_start(struct meta_handler *mh);
void mh_cleanup(struct meta_handler *mh);
int mh_init(struct meta_handler *mh, uint16_t port, const char* ip4addr, struct sig_dispatcher *sd);
int mh_register_state_callback(struct meta_handler *mh, mh_state_cb cb, void *player_data);

#endif /* __META_HANDLER_H__ */

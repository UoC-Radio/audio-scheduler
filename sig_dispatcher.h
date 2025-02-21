/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __SIG_DISPATCHER_H__
#define __SIG_DISPATCHER_H__
#include <signal.h>	/* For sig_atomic_t */

enum sig_unit {
	SIG_UNIT_PLAYER,
	SIG_UNIT_META,
	SIG_UNIT_MAX
};

typedef void (*sig_cb)(int signo, void *data);

struct sig_handler {
	enum sig_unit unit;
	sig_cb cb;
	void *data;
};

struct sig_dispatcher {
	int signal_fd;
	int epoll_fd;
	pthread_t thread;
	volatile sig_atomic_t running;
	
	/* Registered handlers */
	struct sig_handler handlers[SIG_UNIT_MAX];
	pthread_mutex_t handlers_mutex;
};

void sig_dispatcher_cleanup(struct sig_dispatcher *sd);
int sig_dispatcher_init(struct sig_dispatcher *sd);
int sig_dispatcher_start(struct sig_dispatcher *sd);
int sig_dispatcher_register(struct sig_dispatcher *sd, enum sig_unit unit, sig_cb cb, void *data);

#endif /* __SIG_DISPATCHER_H__ */
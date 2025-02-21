/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This is the application's signal dispatcher. Since we have multiple
 * threads we need to control which thread receives signals, and also
 * be able to access shared data (the various state structs) which is
 * not safe to do from a normal signal handler. For this we use Linux's
 * signalfd mechanism and we epoll it on a thread, normal signal delivery
 * is blocked. Each component that spawns threads (currently the player
 * and the metadata handler) registers a callback to the dispatcher which
 * is called when a signal is delivered through signalfd, passing on its
 * state structure to it for convenience.
 */

#include <errno.h>	/* For errno */
#include <string.h>	/* For memset */
#include <stdint.h>	/* For size_t */
#include <signal.h>	/* For sigfillset/delset/strsignal etc */
#include <sys/signalfd.h> /* For signalfd creation */
#include <sys/epoll.h>	/* For epoll on signalfd */
#include <unistd.h>	/* For read() */
#include <pthread.h>	/* For pthread_create and mutexes */
#include "sig_dispatcher.h"
#include "utils.h"

/* Make sure this follows the enum */
static const char* unit_names[2] = {"PLAYER", "META"};

static void *sig_thread(void *arg)
{
	struct sig_dispatcher *sd = arg;
	struct signalfd_siginfo si;
	struct epoll_event events[1];
	int i;
	
	while (sd->running) {
		int n = epoll_wait(sd->epoll_fd, events, 1, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (read(sd->signal_fd, &si, sizeof(si)) != sizeof(si))
			continue;
			
		/* Dispatch signal to registered handlers */
		pthread_mutex_lock(&sd->handlers_mutex);
		for (i = 0; i < SIG_UNIT_MAX; i++) {
			if (sd->handlers[i].cb) {
				utils_dbg(SIGDISP, "Sending %s, to %s\n", strsignal(si.ssi_signo),
					  unit_names[i]);
				sd->handlers[i].cb(si.ssi_signo, sd->handlers[i].data);
			}
		}
		pthread_mutex_unlock(&sd->handlers_mutex);

		/* If we got a SIGINT/SIGTERM also terminate this thread */
		if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
			utils_dbg(SIGDISP, "Stopped\n");
			break;
		}
	}
	
	return NULL;
}


/**************\
* ENTRY POITNS *
\**************/

int sig_dispatcher_start(struct sig_dispatcher *sd)
{
	utils_dbg(SIGDISP, "Starting\n");
	sd->running = 1;
	if (pthread_create(&sd->thread, NULL, sig_thread, sd) != 0) {
		utils_perr(SIGDISP, "Couldn't create sig_thread");
		return -1;
	}

	return 0;
}

void sig_dispatcher_cleanup(struct sig_dispatcher *sd)
{
	if (!sd)
		return;
	sd->running = 0;
	pthread_join(sd->thread, NULL);
	
	pthread_mutex_destroy(&sd->handlers_mutex);
	
	if (sd->signal_fd >= 0)
		close(sd->signal_fd);
	if (sd->epoll_fd >= 0)
		close(sd->epoll_fd);
}

int sig_dispatcher_init(struct sig_dispatcher *sd)
{;
	int ret = 0;
	
	memset(sd, 0, sizeof(struct sig_dispatcher));

	/* Block all incomming signals, except those that result
	 * crashing, we'll handle them from the signal dispatcher
	 * thread. Do this early on so that all threads inherit the
	 * sigmask. */
	sigset_t mask;
	sigfillset(&mask);
	sigdelset(&mask, SIGFPE);
	sigdelset(&mask, SIGILL);
	sigdelset(&mask, SIGSEGV);
	sigdelset(&mask, SIGBUS);
	sigdelset(&mask, SIGABRT);
	if (pthread_sigmask(SIG_BLOCK, &mask, NULL) < 0) {
		utils_perr(SIGDISP, "Couldn't block signals");
		ret = -1;
		goto cleanup;
	}

	/* Create signalfd */
	sd->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sd->signal_fd < 0) {
		utils_perr(SIGDISP, "Could not create signalfd");
		ret = -1;
		goto cleanup;
	}	

	sd->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (sd->epoll_fd < 0) {
		utils_perr(SIGDISP, "Could not create epoll_fd");
		ret = -1;
		goto cleanup;
	}

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
	pthread_mutex_init(&sd->handlers_mutex, &attr);

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = sd->signal_fd;
	if (epoll_ctl(sd->epoll_fd, EPOLL_CTL_ADD, sd->signal_fd, &ev) < 0) {
		utils_perr(SIGDISP, "epoll_ctl failed");
		ret = -1;
		goto cleanup;
	}

	return 0;

cleanup:
	sig_dispatcher_cleanup(sd);
	return ret;
}

int sig_dispatcher_register(struct sig_dispatcher *sd, enum sig_unit unit,
			    sig_cb cb, void *data)
{
	if (!sd || unit >= SIG_UNIT_MAX || !cb)
		return -1;

	pthread_mutex_lock(&sd->handlers_mutex);
	sd->handlers[unit].cb = cb;
	sd->handlers[unit].data = data;
	pthread_mutex_unlock(&sd->handlers_mutex);

	return 0;
}
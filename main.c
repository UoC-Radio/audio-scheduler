/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2016 - 2025 Nick Kossifidis <mickflemm@gmail.com>
 * SPDX-FileCopyrightText: 2017 George Kiagiadakis <gkiagia@tolabaki.gr>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <unistd.h>	/* For getopt() */
#include <signal.h>	/* For sig_atomic_t and signal handling */
#include <stdlib.h>	/* For strtol() */
#include <stdio.h>	/* For perror() */
#include <libavutil/log.h>	/* For av_log_set_level() */
#include "utils.h"
#include "scheduler.h"
//#include "gst_player.h"
#include "fsp_player.h"
#include "meta_handler.h"
#include "sig_dispatcher.h"

//static const char * usage_str =
//  "Usage: %s [-s audio_sink_bin] [-d debug_level] [-m debug_mask] [-p port] <config_file>\n";

static const char * usage_str =
  "Usage: %s [-d debug_level] [-m debug_mask] [-p port] <config_file>\n";

int
main(int argc, char **argv)
{
	struct scheduler sched = {0};
	struct meta_handler mh = {0};
	struct fsp_player fsp = {0};
	struct sig_dispatcher sd = {0};
	int ret = 0, opt, tmp;
	int dbg_lvl = INFO;
	int dbg_mask = PLR|SCHED|META;
	uint16_t port = 9670;
	//char *sink = NULL;

	while ((opt = getopt(argc, argv, "s:d:m:p:")) != -1) {
		switch (opt) {
		/*
		case 's':
			sink = optarg;
			break;
		*/
		case 'd':
			tmp = strtol(optarg, NULL, 10);
			if (errno != 0)
				perror("Failed to parse debug level");
			else
				dbg_lvl = tmp;
			break;
		case 'm':
			tmp = strtol(optarg, NULL, 16);
			if (errno != 0)
				perror("Failed to parse debug mask");
			else
				dbg_mask = tmp;
			break;
		case 'p':
			tmp = strtol(optarg, NULL, 10);
			if (errno != 0)
				perror("Failed to parse port number");
			else
				port = tmp;
			break;
		default:
			printf(usage_str, argv[0]);
			return(0);
		}
	}

	if (optind >= argc) {
		printf(usage_str, argv[0]);
		return(0);
	}

	/* Configure log output */
	utils_set_log_level(dbg_lvl);
	utils_set_debug_mask(dbg_mask);
	/* Prevent ffmpeg from spamming us, we report errors anyway */
	av_log_set_level(AV_LOG_ERROR);

	ret = sig_dispatcher_init(&sd);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize signal dispatcher\n");
		ret = -1;
		goto cleanup;
	}
	sig_dispatcher_start(&sd);

	ret = sched_init(&sched, argv[optind]);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize scheduler\n");
		ret = -1;
		goto cleanup;
	}

	ret = mh_init(&mh, port, NULL, &sd);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize metadata request hanlder\n");
		ret = -2;
		goto cleanup;
	}
	mh_start(&mh);

#if 0
	ret = gst_player_init(&player, &sched, &mh, sink);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize player\n");
		ret = -3;
		goto cleanup;
	}

	/* This will spawn a g_main_loop and block */
	gst_player_loop(&player);
#endif

	ret = fsp_init(&fsp, &sched, &mh, &sd);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize player\n");
		ret = -3;
		goto cleanup;
	}

	/* This will spawn a pw_main_loop and block */
	fsp_start(&fsp);

	utils_info(NONE, "Graceful exit...\n");

 cleanup:
	mh_cleanup(&mh);
	fsp_cleanup(&fsp);
//	gst_player_cleanup(&player);
	sched_cleanup(&sched);
	sig_dispatcher_cleanup(&sd);
	return ret;
}

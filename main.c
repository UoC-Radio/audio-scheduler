#include "scheduler.h"
#include "player.h"
#include "meta_handler.h"
#include "utils.h"
#include <unistd.h>	/* For getopt() */
#include <signal.h>	/* For sig_atomic_t and signal handling */
#include <stdlib.h>	/* For strtol() */
#include <stdio.h>	/* For perror() */
#include <libavutil/log.h>	/* For av_log_set_level() */

static struct player player = {0};

static const char * usage_str =
  "Usage: %s [-s audio_sink_bin] [-d debug_level] [-m debug_mask] [-p port] <config_file>\n";

static void
signal_handler(int sig, siginfo_t * info, void *extra)
{
	player_loop_quit (&player);
}

int
main(int argc, char **argv)
{
	struct scheduler sched = {0};
	struct sigaction sa = {0};
	struct meta_handler mh = {0};
	int ret = 0, opt, tmp;
	int dbg_lvl = INFO;
	int dbg_mask = PLR|SCHED|META;
	uint16_t port = 9670;
	char *sink = NULL;

	while ((opt = getopt(argc, argv, "s:d:m:p:")) != -1) {
		switch (opt) {
		case 's':
			sink = optarg;
			break;
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

	utils_set_log_level(dbg_lvl);
	utils_set_debug_mask(dbg_mask);
	/* Prevent ffmpeg from spamming us, we report errors anyway */
	av_log_set_level(AV_LOG_ERROR);

	ret = sched_init(&sched, argv[optind]);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize scheduler\n");
		ret = -1;
		goto cleanup;
	}

	ret = meta_handler_init(&mh, port, NULL);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize metadata request hanlder\n");
		ret = -2;
		goto cleanup;
	}

	ret = player_init(&player, &sched, &mh, sink);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize player\n");
		ret = -3;
		goto cleanup;
	}

	/* Install signal handler */
	/* Install a signal handler for graceful exit */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = signal_handler;
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	player_loop(&player);

	utils_info(PLR, "Graceful exit...\n");

 cleanup:
	player_cleanup(&player);
	sched_cleanup(&sched);
	meta_handler_destroy(&mh);
	return ret;
}

#include "scheduler.h"
#include "player.h"
#include "meta_handler.h"
#include "utils.h"
#include <unistd.h>	/* For sleep() */
#include <signal.h>	/* For sig_atomic_t and signal handling */

static struct player player = {0};

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
	int ret = 0;

	if (argc < 2) {
		utils_info(NONE, "Usage: %s <config_file>\n", argv[0]);
		return(0);
	}

	utils_set_log_level(DBG);
	utils_set_debug_mask(CFG|PLS|PLR|SHUF|SCHED|UTILS|META);

	ret = sched_init(&sched, argv[1]);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize scheduler\n");
		ret = -1;
		goto cleanup;
	}

	ret = meta_handler_init(&mh, 9670, NULL);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize metadata request hanlder\n");
		ret = -2;
		goto cleanup;
	}

	ret = player_init(&player, &sched);
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

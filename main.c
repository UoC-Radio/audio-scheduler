#include "scheduler.h"
#include "utils.h"
#include <unistd.h>	/* For sleep() */
#include <signal.h>	/* For sig_atomic_t and signal handling */

static volatile sig_atomic_t	scheduler_active = 0;

static void
signal_handler(int sig, siginfo_t * info, void *extra)
{
	scheduler_active = 0;
}

int
main(int argc, char **argv)
{
	struct scheduler sched = {0};
	struct sigaction sa = {0};
	int ret = 0;
	char* next;
	int fade_duration = 0;

	if (argc < 2) {
		utils_info(NONE, "Usage: %s <config_file>\n", argv[0]);
		return(0);
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

	utils_set_log_level(DBG);
	utils_set_debug_mask(CFG|PLS|SHUF|SCHED|UTILS);
//	utils_set_debug_mask(CFG|SCHED);

	ret = sched_init(&sched, argv[1]);
	if (ret < 0) {
		utils_err(NONE, "Unable to initialize scheduler\n");
		return -1;
	}

	scheduler_active = 1;

	while(scheduler_active) {
		ret = sched_get_next(&sched, &next, &fade_duration);
		utils_info(PLR, "Playing: %s, fade duration: %i\n", next, fade_duration);
		sleep(1);
	}

	utils_info(PLR, "Graceful exit...\n");
	sched_cleanup(&sched);
	return ret;
}

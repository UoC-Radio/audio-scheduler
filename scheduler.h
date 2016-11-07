#include <time.h> /* For time_t */

struct playlist {
	char*	filepath;
	int	num_items;
	char**	items;
	int	shuffle;
	time_t	last_mtime;
	int	fade_duration;
	int	curr_idx;
};

struct intermediate_playlist {
	/* Anonymous struct as per C11
	 * Note that this declaration is cleaner
	 * and I preffer it but it needs -fms-extensions
	 * to work on GCC, for more infos check out:
	 * https://gcc.gnu.org/onlinedocs/gcc-5.1.0/gcc/Unnamed-Fields.html
	 */
	struct playlist;

	char*	name;
	int	sched_interval;
	time_t	last_scheduled;
	int	num_sched_items;
};

struct zone {
	char*	name;
	struct	tm start_time;
	char*	maintainer;
	char*	description;
	char*	comment;
	struct	playlist *main_pls;
	struct	playlist *fallback_pls;
	int	num_others;
	struct	intermediate_playlist **others;
};

struct day_schedule {
	int num_zones;
	struct zone **zones;
};

struct week_schedule {
	struct day_schedule *days[7];
};

struct config {
	char*	filepath;
	char*	schema_filepath;
	time_t	last_mtime;
	struct week_schedule *ws;
};

struct scheduler {
	struct config *cfg;
	int state_flags;
};

enum state_flags {
	SCHED_LOADING_NEW	= 1
};


/* Playlist handling */
void pls_files_cleanup(struct playlist* pls);
int pls_process(struct playlist* pls);
int pls_reload_if_needed(struct playlist* pls);

/* Config handling */
void cfg_cleanup(struct config *cfg);
int cfg_process(struct config *cfg);
int cfg_reload_if_needed(struct config *cfg);

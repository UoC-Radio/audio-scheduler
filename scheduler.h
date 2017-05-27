/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Main header file
 *
 * Copyright (C) 2016 Nick Kossifidis <mickflemm@gmail.com>
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

#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include <time.h> /* For time_t */

struct fader {
	int	fadein_duration_secs;
	int	fadeout_duration_secs;
	float	min_lvl;
	float	max_lvl;
};

struct playlist {
	char*	filepath;
	int	num_items;
	char**	items;
	int	shuffle;
	time_t	last_mtime;
	int	curr_idx;
	struct fader *fader;
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
	int	sched_interval_mins;
	time_t	last_scheduled;
	int	num_sched_items;
	int	sched_items_pending;
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
	SCHED_FAILED		= 2,
	SCHED_LOADING_NEW	= 4,
};


/* Playlist handling */
void pls_files_cleanup(struct playlist* pls);
int pls_shuffle(struct playlist* pls);
int pls_process(struct playlist* pls);
int pls_reload_if_needed(struct playlist* pls);

/* Config handling */
void cfg_cleanup(struct config *cfg);
int cfg_process(struct config *cfg);
int cfg_reload_if_needed(struct config *cfg);

/* Scheduler entry points */
int sched_get_next(struct scheduler* sched, char** next, time_t sched_time, struct fader** fader);
int sched_init(struct scheduler* sched, char* config_filepath);
void sched_cleanup(struct scheduler* sched);

#endif /* __SCHEDULER_H__ */

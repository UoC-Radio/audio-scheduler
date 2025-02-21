/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2016 - 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include <time.h> /* For time_t */

struct fader_info {
	int	fadein_duration_secs;
	int	fadeout_duration_secs;
};

struct audiofile_info {
	const char* filepath;	/* from pls->items[] */

	char* artist;
	char* album;
	char* title;
	char* albumid;
	char* release_trackid;

	float album_gain;
	float album_peak;
	float track_gain;
	float track_peak;

	time_t duration_secs;

	/* zone->name of current zone */
	const char* zone_name;
	/* playlist->fader of current playlist*/
	const struct fader_info *fader_info;

	/* Marks a clone, where all string fields are copies,
	 * and so should be freed. */
	int is_copy;
};

struct playlist {
	char*	filepath;
	int	num_items;
	char**	items;
	int	shuffle;
	time_t	last_mtime;
	int	curr_idx;
	struct fader_info *fader;
};

struct intermediate_playlist {
	/* Anonymous struct as per C11
	 * Note that this declaration is cleaner
	 * and I prefer it but it needs -fms-extensions
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

/* File handling */
void mldr_copy_audiofile(struct audiofile_info *dst, struct audiofile_info *src);
void mldr_cleanup_audiofile(struct audiofile_info *info);
int mldr_init_audiofile(char* filepath, const char* zone_name, const struct fader_info *fdr, struct audiofile_info *info, int strict);

/* Playlist handling */
void pls_files_cleanup(struct playlist* pls);
void pls_shuffle(struct playlist* pls);
int pls_process(struct playlist* pls);
int pls_reload_if_needed(struct playlist* pls);

/* Config handling */
void cfg_cleanup(struct config *cfg);
int cfg_process(struct config *cfg);
int cfg_reload_if_needed(struct config *cfg);

/* Scheduler entry points */
int sched_get_next(struct scheduler* sched, time_t sched_time, struct audiofile_info* next_info);
int sched_init(struct scheduler* sched, char* config_filepath);
void sched_cleanup(struct scheduler* sched);

#endif /* __SCHEDULER_H__ */

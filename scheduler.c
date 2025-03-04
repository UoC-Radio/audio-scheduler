/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2016 - 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This part is the scheduler core, based on the currently loaded config, it provides
 * a audiofile_info struct to the player, to be played at a provided time_t. This allows
 * the player to ask for songs to be played in the future, or at an updated time e.g.
 * after pause/resume.
 */

#include <stdlib.h>	/* For malloc */
#include <string.h>	/* For memset */
#include "scheduler.h"
#include "utils.h"

/*********\
* HELPERS *
\*********/

static int
sched_is_ipls_ready(struct intermediate_playlist* ipls, time_t sched_time)
{
	struct tm tm_curr = *localtime(&sched_time);
	struct tm ipls_rdy_tm = *localtime(&ipls->last_scheduled);
	int mins = ipls_rdy_tm.tm_min;
	int ret = 0;

	if (!ipls)
		return 0;

	/* Add interval to ipls ready time */
	mins += ipls->sched_interval_mins;
	ipls_rdy_tm.tm_hour += mins / 60;
	ipls_rdy_tm.tm_min = mins % 60;

	ret = utils_compare_time(&tm_curr, &ipls_rdy_tm, 0);
	if(ret <= 0)
		return 0;

	utils_dbg(SCHED, "Intermediate playlist ready: %s\n", ipls->name);
	return 1;
}

static int
sched_get_next_item(struct audiofile_info* next_info, struct playlist* pls, const char* zone_name)
{
	int ret = 0;
	int idx = 0;
	char* next = NULL;

	if (!pls)
		return -1;

	/* Re-load playlist if needed */
	ret = pls_reload_if_needed(pls);
	if(ret < 0) {
		utils_err(SCHED, "Re-loading playlist %s failed\n", pls->filepath);
		return -1;
	}

	/* We've played the whole list, reset index and
	 * re-shuffle if needed */
	if((pls->curr_idx + 1) >= pls->num_items) {
		pls->curr_idx = 0;
		if(pls->shuffle) {
			utils_dbg(SCHED, "Re-shuffling playlist\n");
			pls_shuffle(pls);
		}
	}

	/* Check if next item is readable, if not
	 * loop until we find a readable one. If we
	 * don't find any readable file on the playlist
	 * return NULL */
	for(idx = pls->curr_idx; idx < pls->num_items; idx++) {
		next = pls->items[idx];
		if(utils_is_readable_file(next)) {
			pls->curr_idx = idx + 1;
			ret = mldr_init_audiofile(next, zone_name, pls->fader, next_info, 1);
			if (!ret)
				return 0;
			else {
				utils_wrn(SCHED, "Failed to load file: %s\n", next);
				/* Non fatal */
				continue;
			}
		}
		utils_wrn(SCHED, "File unreadable %s\n", next);
	}

	return -1;
}


/**************\
* ENTRY POINTS *
\**************/

/* Note that failing to re-load config or get an item from a
 * playlist or intermediate playlist is not fatal. It
 * might be a temporary issue e.g. with network storage.
 * however if we can't get an item from any playlist
 * then we can't do anything about it. */

int
sched_get_next(struct scheduler* sched, time_t sched_time, struct audiofile_info* next_info)
{
	struct playlist *pls = NULL;
	struct intermediate_playlist *ipls = NULL;
	struct zone *zn = NULL;
	struct day_schedule *ds = NULL;
	struct week_schedule *ws = NULL;
	int i = 0;
	int ret = 0;
	struct tm tm = *localtime(&sched_time);
	char datestr[26];

	if (!sched)
		return -1;

	/* format: Day DD Mon YYYY, HH:MM:SS */
	strftime (datestr, 26, "%a %d %b %Y, %H:%M:%S", &tm);
	utils_info (SCHED, "Scheduling item for: %s\n", datestr);

	/* Reload config if needed */
	ret = cfg_reload_if_needed(sched->cfg);
	if(ret < 0) {
		utils_wrn(SCHED, "Re-loading config failed\n");
		return -1;
	}

	/* Get current day */
	ws = sched->cfg->ws;
	ds = ws->days[tm.tm_wday];

	/* Find a zone with a start time less
	 * than the current time. In order to
	 * get the latest one and since the zones
	 * are stored in ascending order, do
	 * the lookup backwards */
	for(i = ds->num_zones - 1; i >= 0; i--) {
		zn = ds->zones[i];
		ret = utils_compare_time(&tm, &zn->start_time, 1);

		if (utils_is_debug_enabled (SCHED)) {
			strftime (datestr, 26, "%H:%M:%S", &zn->start_time);
			utils_dbg (SCHED, "considering zone '%s' at: %s -> %i\n",
					zn->name, datestr, ret);
		}
		if(ret > 0)
			break;
	}

	if(i < 0) {
		utils_wrn(SCHED, "Nothing is scheduled for now ");
		utils_wrn(SCHED|SKIP, "using first zone of the day\n");
		zn = ds->zones[0];
	}

	/* Is it time to load an item from an intermediate
	 * playlist ? Note: We assume here that intermediate
	 * playlists are sorted in descending order from higher
	 * to lower priority. */
	for(i = 0; i < zn->num_others && zn->others; i++) {
		if(sched_is_ipls_ready(zn->others[i], sched_time)) {
			ipls = zn->others[i];

			/* Only update last_scheduled after we've
			 * scheduled num_sched_items */
			if(ipls->sched_items_pending == -1)
				ipls->sched_items_pending = ipls->num_sched_items;
			else if(!ipls->sched_items_pending) {
				/* Done with this one, mark it as scheduled
				 * and move on to the next */
				ipls->sched_items_pending = -1;
				ipls->last_scheduled = sched_time;
				continue;
			}
			utils_dbg(SCHED, "Pending items: %i\n",
				  ipls->sched_items_pending);

			pls = (struct playlist*) ipls;
			ipls->sched_items_pending--;
			break;
		}
	}

	ret = sched_get_next_item(next_info, pls, zn->name);
	if(!ret) {
		utils_dbg(SCHED, "Using intermediate playlist\n");
		goto done;
	}

	/* Go for the main playlist */
	pls = zn->main_pls;
	ret = sched_get_next_item(next_info, pls, zn->name);
	if(!ret) {
		utils_dbg(SCHED, "Using main playlist\n");
		goto done;
	}

	/* Go for the fallback playlist */
	pls = zn->fallback_pls;
	ret = sched_get_next_item(next_info, pls, zn->name);
	if(!ret) {
		utils_wrn(SCHED, "Using fallback playlist\n");
		goto done;
	}

done:
	if(!ret) {
		utils_info(SCHED, "Got next item from zone '%s': %s (fader: %s)\n",
			zn->name, next_info->filepath, pls->fader ? "true" : "false");
		return 0;
	}

	/* Nothing we can do */
	utils_err(SCHED, "could not find anything to schedule\n");
	return -1;
}

int
sched_init(struct scheduler* sched, char* config_filepath)
{
	struct config *cfg = NULL;
	int ret = 0;

	memset(sched, 0, sizeof(struct scheduler));

	cfg = (struct config*) malloc(sizeof(struct config));
	if(!cfg) {
		utils_err(SCHED, "Could not allocate config structure\n");
		return -1;
	}

	cfg->filepath = config_filepath;

	ret = cfg_process(cfg);
	if (ret < 0)
		return -1;

	sched->cfg = cfg;

	utils_dbg(SCHED, "Initialized\n");
	return 0;
}

void
sched_cleanup(struct scheduler* sched)
{
	if(sched->cfg!=NULL) {
		cfg_cleanup(sched->cfg);
		free(sched->cfg);
	}
	sched->cfg=NULL;
}

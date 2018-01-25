/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Scheduler core
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

#include "scheduler.h"
#include "utils.h"
#include <stdlib.h>	/* For malloc */

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

static char*
sched_get_next_item(struct playlist* pls)
{
	int ret = 0;
	int idx = 0;
	char* next = NULL;

	/* Re-load playlist if needed */
	ret = pls_reload_if_needed(pls);
	if(ret < 0) {
		utils_wrn(SCHED, "Re-loading playlist %s failed\n", pls->filepath);
		return NULL;
	}

	/* We've played the whole list, reset index and
	 * re-shuffle if needed */
	if((pls->curr_idx + 1) >= pls->num_items) {
		pls->curr_idx = 0;
		if(pls->shuffle) {
			utils_dbg(SCHED, "Re-shuffling playlist\n");
			ret = pls_shuffle(pls);
			if (ret < 0) {
				utils_err(SCHED, "Re-shuffling playlist %s failed\n",
					  pls->filepath);
				return NULL;
			}
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
			return next;
		}
		utils_wrn(SCHED, "File unreadable %s\n", next);
	}

	return NULL;
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
sched_get_next(struct scheduler* sched, time_t sched_time, char** next,
	       struct fader** fader)
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
	if(ret < 0)
		utils_wrn(SCHED, "Re-loading config failed\n");

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
		if(ret > 0)
			break;
	}

	if(i < 0) {
		utils_wrn(SCHED, "Nothing is scheduled for now ");
		utils_wrn(SCHED|SKIP, "using first zone of the day\n");
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

	if(pls) {
		(*next) = sched_get_next_item(pls);
		if((*next) != NULL) {
			utils_dbg(SCHED, "Using intermediate playlist\n");
			if(pls->fader)
				(*fader) = pls->fader;
			else
				(*fader) = NULL;
			goto done;
		}
	}

	/* Go for the main playlist */
	pls = zn->main_pls;
	(*next) = sched_get_next_item(pls);
	if((*next) != NULL) {
		utils_dbg(SCHED, "Using main playlist\n");
		if(pls->fader)
			(*fader) = pls->fader;
		else
			(*fader) = NULL;
		goto done;
	}

	/* Go for the fallback playlist */
	pls = zn->fallback_pls;
	(*next) = sched_get_next_item(pls);
	if((*next) != NULL) {
		utils_wrn(SCHED, "Using fallback playlist\n");
		if(pls->fader)
			(*fader) = pls->fader;
		else
			(*fader) = NULL;
		goto done;
	}

done:
	if((*next) != NULL) {
		utils_info(SCHED, "Got next item: %s (fader: %s)\n",
			  (*next), (*fader) ? "true" : "false");
		return 0;
	}

	/* Nothing we can do */
	return -1;
}

int
sched_init(struct scheduler* sched, char* config_filepath)
{
	struct config *cfg = NULL;
	int ret = 0;

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
	return 0;
}

void
sched_cleanup(struct scheduler* sched)
{
	if(sched->cfg!=NULL)
		cfg_cleanup(sched->cfg);
	sched->cfg=NULL;	
}
/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Playlist data handler
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
#include <stdlib.h>	/* For malloc/realloc/free */
#include <string.h>	/* For strncmp() and strchr() */
#include <stdio.h>	/* For FILE handling */
#include <limits.h>	/* For PATH_MAX */

enum pls_type {
	TYPE_PLS = 1,
	TYPE_M3U = 2,
};


/*********\
* HELPERS *
\*********/

static int
pls_check_type(char* filepath)
{
	int filepath_len = 0;
	int type = -1;
	char* ext = NULL;

	filepath_len = strnlen(filepath, PATH_MAX);
	ext = filepath + filepath_len + 1 - 4;
	if(!strncmp(ext, "pls", 4))
		type = TYPE_PLS;
	else if(!strncmp(ext, "m3u", 4))
		type = TYPE_M3U;
	else
		utils_err(PLS, "Unknown file type: %s\n", filepath);

	return type;
}

static void
pls_files_cleanup_internal(char** files, int num_files)
{
	if(!files)
		return;

	while(num_files > 0) {
		free(files[num_files - 1]);
		num_files--;
	}

	free(files);
}

static int
pls_add_file(char* filepath, char ***files, int *num_files)
{
	int len = 0;
	int ret = 0;
	char* file = NULL;
	char** temp = NULL;

	utils_trim_string(filepath);

	/* Is it a file that we can read ?
	 * Note that M3Us may also contain
	 * folders, this is not supported here
	 * for now */
	if(!utils_is_readable_file(filepath))
		goto cleanup;

	/* Get size of the filepath string, including
	 * null terminator */
	len = strnlen(filepath, PATH_MAX);
	len++;

	file = (char*) malloc(len);
	if(!file) {
		utils_err(PLS, "Could not allocate filename on files array\n");
		ret = -1;
		goto cleanup;
	}
	memcpy(file, filepath, len);

	temp = realloc(*files, ((*num_files) + 1) * sizeof(char*));
	if(!temp) {
		utils_err(PLS, "Could not expand files array\n");
		free(file);
		ret = -1;
		goto cleanup;
	}
	*files = temp;

	(*files)[(*num_files)] = file;
	utils_dbg(PLS, "Added file: %s\n", (*files)[(*num_files)]);
	(*num_files)++;

cleanup:
	if(ret < 0) {
		pls_files_cleanup_internal(*files, (*num_files));
		(*num_files) = 0;
		*files = NULL;
	}
	return ret;
}


/**********\
* SHUFFLER *
\**********/

int
pls_shuffle(struct playlist* pls)
{
	char** temp = NULL;
	int array_size = pls->num_items * sizeof(char*);
	int remaining = pls->num_items;
	unsigned int temp_idx = 0;
	unsigned int items_idx = 0;
	int i = 0;

	temp = malloc(array_size);
	if(!temp) {
		utils_err(PLS, "Could not allocate temporary file array\n");
		return -1;
	}
	memset(temp, 0, array_size);

	/* A random distribution is uniform so
	 * each slot has the same propability
	 * which means that we shouldn't get
	 * many collisions here and this should
	 * complete fast enough */
	while(remaining > 0) {
		temp_idx = utils_get_random_uint() % pls->num_items;

		/* Slot taken, re-try */
		if(temp[temp_idx] != NULL)
			continue;

		temp[temp_idx] = pls->items[items_idx++];
		remaining--;
	}

	free(pls->items);
	pls->items = temp;

	if(utils_is_debug_enabled(SHUF)) {
		utils_dbg(SHUF, "--== Shuffled list ==--\n");
		for(i = 0; i < pls->num_items; i++)
			utils_dbg(SHUF|SKIP, "%i %s\n", i, pls->items[i]);
	}

	return 0;
}


/**************\
* ENTRY POINTS *
\**************/

void
pls_files_cleanup(struct playlist* pls)
{
	pls_files_cleanup_internal(pls->items, pls->num_items);
}

int
pls_process(struct playlist* pls)
{
	char line[PATH_MAX] = {0};
	char* delim = NULL;
	FILE *pls_file = NULL;
	int type = 0;
	int ret = 0;

	/* Sanity checks */
	if(pls->filepath == NULL) {
		utils_err(PLS, "Called with null argument\n");
		ret = -1;
		goto cleanup;
	}

	ret = pls_check_type(pls->filepath);
	if(ret < 0)
		goto cleanup;
	type = ret;


	if(!utils_is_readable_file(pls->filepath)) {
		ret = -1;
		goto cleanup;
	}

	/* Store mtime for later checks */
	pls->last_mtime = utils_get_mtime(pls->filepath);
	if(!pls->last_mtime) {
		ret = -1;
		goto cleanup;
	}

	/* Open playlist file and start parsing its contents */
	pls_file = fopen(pls->filepath, "rb");
	if (pls_file == NULL) {
		utils_perr(PLS, "Couldn't open file %s", pls->filepath);
		ret = -1;
		goto cleanup;
	}

	switch(type) {
	case TYPE_PLS:
		/* Grab the first line and see if it's the expected header */
		if(fgets(line, PATH_MAX, pls_file) != NULL) {
			utils_trim_string(line);
			if(strncmp(line, "[playlist]", 11)) {
				utils_err(PLS, "Invalid header on %s: %s\n",
					  pls->filepath, line);
				ret = -1;
				goto cleanup;
			}
		}

		while(fgets(line, PATH_MAX, pls_file) != NULL) {
			/* Not a file */
			if(strncmp(line, "File", 4))
				continue; 

			delim = strchr(line, '=');
			delim++;

			ret = pls_add_file(delim, &pls->items, &pls->num_items);
			if(ret < 0) {
				ret = -1;
				goto cleanup;
			}
		}
		break;
	case TYPE_M3U:
		while(fgets(line, PATH_MAX, pls_file) != NULL) {
			/* EXTINF etc */
			if(line[0] == '#')
				continue; 

			delim = strchr(line, '=');
			delim++;

			ret = pls_add_file(line, &pls->items, &pls->num_items);
			if(ret < 0) {
				ret = -1;
				goto cleanup;
			}
		}
		break;
	default:
		/* Shouldn't reach this */
		ret = -1;
		goto cleanup;
	}

	/* Shuffle contents if needed */
	if(pls->shuffle) {
		ret = pls_shuffle(pls);
		if(ret < 0) {
			utils_err(PLS, "Shuffling failed for %s\n", pls->filepath);
			goto cleanup;
		}
	}

	utils_dbg(PLS, "Got %i files from %s\n", pls->num_items, pls->filepath);

cleanup:
	if(pls_file)
		fclose(pls_file);

	if(ret < 0) {
		if(pls->items)
			pls_files_cleanup_internal(pls->items, pls->num_items);
		pls->num_items = 0;
	}
	return ret;
}

int
pls_reload_if_needed(struct playlist* pls)
{
	time_t mtime = utils_get_mtime(pls->filepath);
	if(!mtime) {
		utils_err(PLS, "Unable to check mtime for %s\n", pls->filepath);
		return -1;
	}

	/* mtime didn't change, no need to reload */
	if(mtime == pls->last_mtime)
		return 0;

	utils_info(PLS, "Got different mtime, reloading %s\n", pls->filepath);

	/* Re-load playlist */
	pls_files_cleanup(pls);
	return pls_process(pls);
}

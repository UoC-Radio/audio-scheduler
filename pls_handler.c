/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2016 - 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This part parses a playlist file (m3u and pls are supported) and populates
 * a playlist struct, it also supports shuffling the playlist, and does some
 * basic checks to make sure each file exists and is readable.
 */

 #include <stdlib.h>	/* For malloc/realloc/free */
#include <string.h>	/* For strncmp() and strchr() */
#include <stdio.h>	/* For FILE handling/getline() */
#include <limits.h>	/* For PATH_MAX */
#include "scheduler.h"
#include "utils.h"

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
	int i = 0;

	if(!files)
		return;

	for(i = 0; i < num_files; i++) {
		if(files[i]) {
			free(files[i]);
			files[i] = NULL;
		}
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
		return -1;

	/* Get size of the filepath string, including
	 * null terminator */
	len = strnlen(filepath, PATH_MAX) + 1;

	file = (char*) malloc(len);
	if(!file) {
		utils_err(PLS, "Could not allocate filename on files array\n");
		return -1;
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

static inline void
pls_file_swap(char** items, int x, int y)
{
	char* tmp = items[x];
	items[x] = items[y];
	items[y] = tmp;
}

void
pls_shuffle(struct playlist* pls)
{
	unsigned int next_file_idx = 0;
	int target_slot = 0;

	/* Nothing to shuffle */
	if(pls->num_items <= 1)
		return;

	/* Shuffle playlist using Durstenfeld's algorithm:
	 * Pick a random number from the remaining ones,
	 * and stack it up the end of the array. */
	for (target_slot = pls->num_items-1; target_slot > 0; target_slot--) {
		next_file_idx = utils_get_random_uint() % target_slot;
		pls_file_swap(pls->items, next_file_idx, target_slot);
	}

	if(utils_is_debug_enabled(PLS)) {
		utils_dbg(PLS, "--== Shuffled list ==--\n");
		int i = 0;
		for(i = 0; i < pls->num_items; i++)
			utils_dbg(PLS|SKIP, "%i %s\n", i, pls->items[i]);
	}

	return;
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
	char* line = NULL;
	size_t line_size = 0;
	ssize_t line_len = 0;
	int line_num = 0;
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
		utils_err(PLS, "Could not read playlist: %s\n", pls->filepath);
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
		line_len = getline(&line, &line_size, pls_file);
		if (line_len > 0) {
			utils_trim_string(line);
			if(strncmp(line, "[playlist]", 11)) {
				utils_err(PLS, "Invalid header on %s\n",
					  pls->filepath);
				ret = -1;
				goto cleanup;
			}
		}

		line_num = 2;
		while ((line_len = getline(&line, &line_size, pls_file)) > 0) {
			/* Not a file */
			if(strncmp(line, "File", 4))
				continue;

			delim = strchr(line, '=');
			if(!delim){
				utils_err(PLS, "malformed line %i in pls file: %s\n",
					  line_num, pls->filepath);
				ret = -1;
				goto cleanup;
			}
			delim++;

			ret = pls_add_file(delim, &pls->items, &pls->num_items);
			if(ret < 0) {
				utils_wrn(PLS, "couldn't add file: %s\n", delim);
				/* Non-fatal */
				ret = 0;
			}
			line_num++;
		}
		break;
	case TYPE_M3U:
		line_num = 0;
		while ((line_len = getline(&line, &line_size, pls_file)) > 0) {
			line_num++;
			/* EXTINF etc */
			if(line[0] == '#')
				continue;

			ret = pls_add_file(line, &pls->items, &pls->num_items);
			if(ret < 0) {
				utils_wrn(PLS, "couldn't add file on line number %i: %s\n",
					  line_num, pls->filepath);
				/* Non-fatal */
				ret = 0;
			}
		}
		break;
	default:
		/* Shouldn't reach this */
		ret = -1;
		goto cleanup;
	}

	if (!pls->num_items) {
		utils_err(PLS, "got empty playlist: %s\n", pls->filepath);
		ret = -1;
		goto cleanup;
	}

	/* Shuffle contents if needed */
	if(pls->shuffle)
		pls_shuffle(pls);

	utils_dbg(PLS, "Got %i files from %s\n", pls->num_items, pls->filepath);

cleanup:
	if(pls_file)
		fclose(pls_file);

	if (line)
		free(line);

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

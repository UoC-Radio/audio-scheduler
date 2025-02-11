/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Various utilities/helpers
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

#ifndef __UTILS_H__
#define __UTILS_H__

#include <stdarg.h>		/* For va_list handling */
#include <time.h>		/* For time_t */
#include "config.h"

enum facilities {
	NONE	= 0,
	SCHED	= 1 << 0,
	PLR	= 1 << 1,
	CFG	= 1 << 2,
	PLS	= 1 << 3,
	LDR	= 1 << 4,
	SHUF	= 1 << 5,
	UTILS	= 1 << 6,
	META	= 1 << 7,
	SKIP	= 1 << 8,
};

enum log_levels {
	SILENT	= 0,
	ERROR	= 1,
	WARN	= 2,
	INFO	= 3,
	DBG	= 4,
};

/* Log configuration */
void utils_set_debug_mask(int debug_msk);
int utils_is_debug_enabled(int facility);
void utils_set_log_level(int log_lvl);

/* Log output */
void utils_verr(int facility, const char* fmt, va_list args);
void utils_vperr(int facility, const char* fmt, va_list args);
void utils_vwrn(int facility, const char* fmt, va_list args);
void utils_vpwrn(int facility, const char* fmt, va_list args);
void utils_vinfo(int facility, const char* fmt, va_list args);
void utils_vdbg(int facility, const char* fmt, va_list args);

void utils_err(int facility, const char* fmt,...);
void utils_perr(int facility, const char* fmt,...);
void utils_wrn(int facility, const char* fmt,...);
void utils_pwrn(int facility, const char* fmt,...);
void utils_info(int facility, const char* fmt,...);
void utils_dbg(int facility, const char* fmt,...);

/* File operations */
time_t utils_get_mtime(char* filepath);
int utils_is_regular_file(char* filepath);
int utils_is_readable_file(char*filepath);

/* Misc */
void utils_trim_string(char* string);
unsigned int utils_get_random_uint();
int utils_compare_time(struct tm *tm1, struct tm* tm2, int no_date);

#endif /* __UTILS_H__ */

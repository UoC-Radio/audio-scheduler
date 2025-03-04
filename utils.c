/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2016 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This part includes various utility functions for convinience
 */

#define _GNU_SOURCE	/* Needed for vasprintf() */
#include "utils.h"
#include <stdio.h>	/* For v/printf() */
#include <stdlib.h>	/* For free()/random() */
#include <errno.h>	/* For errno */
#include <string.h>	/* For strerror()/memmove() */
#include <limits.h>	/* For PATH_MAX */
#include <sys/stat.h>	/* For struct stat */
#include <unistd.h>	/* For stat()/access()/syscall() */

/* For getrandom syscall available on linux 3.17+ */
#if defined(__linux__) && defined(__GLIBC__)
	#include <sys/syscall.h>
	#if defined(SYS_getrandom)
		#define	GETRANDOM_DEFINED
	#endif
#endif


/* Some codes for prety output on the terminal */
#define NORMAL	"\x1B[0m"
#define	BRIGHT	"\x1B[1m"
#define	DIM	"\x1B[2m"
#define RED	"\x1B[31m"
#define GREEN	"\x1B[32m"
#define YELLOW	"\x1B[33m"
#define BLUE	"\x1B[34m"
#define MAGENTA	"\x1B[35m"
#define CYAN	"\x1B[36m"
#define WHITE	"\x1B[37m"

static int log_level = 0;

#ifdef DEBUG
static int debug_mask = 0;

void
utils_set_debug_mask(int debug_msk)
{
	debug_mask = debug_msk;
}

int
utils_is_debug_enabled(int facility)
{
	return ((debug_mask & facility) == facility ? 1 : 0);
}

#else
void utils_set_debug_mask(int debug_msk) {}
int utils_is_debug_enabled(int facility) {return 0;}
#endif

void
utils_set_log_level(int log_lvl)
{
	log_level = log_lvl;
}

static const char*
utils_get_facility_name(int facility)
{
	if(facility & SKIP)
		return "";

	switch(facility & 0xFF) {
	case NONE:
		return "";
	case SCHED:
		return "[SCHED] ";
	case PLR:
		return "[PLR] ";
	case CFG:
		return "[CFG] ";
	case PLS:
		return "[PLS] ";
	case LDR:
		return "[LDR] ";
	case SIGDISP:
		return "[SIGDISP] ";
	case META:
		return "[META] ";
	case UTILS:
		return "[UTILS] ";
	default:
		return "[UNK] ";
	}
}


void
utils_verr(int facility, const char* fmt, va_list args)
{
	char *msg = NULL;
	int ret = 0;

	if(log_level < ERROR)
		return;

	ret = vasprintf(&msg, fmt, args);
	if(ret < 0)
		return;

	fprintf(stderr, RED"%s%s"NORMAL,
		utils_get_facility_name(facility), msg);
	fflush(stderr);
	free(msg);
}

void
utils_err(int facility, const char* fmt,...)
{
	va_list args;
	if(log_level < ERROR)
		return;
	va_start(args, fmt);
	utils_verr(facility, fmt, args);
	va_end(args);
}


void
utils_vperr(int facility, const char* fmt, va_list args)
{
	char *msg = NULL;
	int ret = 0;

	if(log_level < ERROR)
		return;

	ret = vasprintf(&msg, fmt, args);
	if(ret < 0)
		return;

	fprintf(stderr, RED"%s%s: %s"NORMAL"\n",
		utils_get_facility_name(facility), msg, strerror(errno));
	fflush(stderr);
	free(msg);
}

void
utils_perr(int facility, const char* fmt,...)
{
	va_list args;
	if(log_level < ERROR)
		return;
	va_start(args, fmt);
	utils_vperr(facility, fmt, args);
	va_end(args);
}


void
utils_vwrn(int facility, const char* fmt, va_list args)
{
	char *msg = NULL;
	int ret = 0;

	if(log_level < WARN)
		return;

	ret = vasprintf(&msg, fmt, args);
	if(ret < 0)
		return;

	fprintf(stderr, YELLOW"%s%s"NORMAL, utils_get_facility_name(facility), msg);
	fflush(stderr);
	free(msg);
}

void
utils_wrn(int facility, const char* fmt,...)
{
	va_list args;
	if(log_level < WARN)
		return;
	va_start(args, fmt);
	utils_vwrn(facility, fmt, args);
	va_end(args);
}


void
utils_vpwrn(int facility, const char* fmt, va_list args)
{
	char *msg = NULL;;
	int ret = 0;

	if(log_level < WARN)
		return;

	ret = vasprintf(&msg, fmt, args);
	if(ret < 0)
		return;

	fprintf(stderr, YELLOW"%s%s: %s"NORMAL"\n",
		utils_get_facility_name(facility), msg, strerror(errno));
	fflush(stderr);
	free(msg);
}

void
utils_pwrn(int facility, const char* fmt,...)
{
	va_list args;
	if(log_level < WARN)
		return;
	va_start(args, fmt);
	utils_vpwrn(facility, fmt, args);
	va_end(args);
}


void
utils_vinfo(int facility, const char* fmt, va_list args)
{
	char *msg = NULL;
	int ret = 0;

	if(log_level < INFO)
		return;

	ret = vasprintf(&msg, fmt, args);
	if(ret < 0)
		return;

	printf(CYAN"%s%s"NORMAL, utils_get_facility_name(facility), msg);
	fflush(stdout);
	free(msg);
}

void
utils_info(int facility, const char* fmt,...)
{
	va_list args;
	if(log_level < INFO)
		return;
	va_start(args, fmt);
	utils_vinfo(facility, fmt, args);
	va_end(args);
}


#ifdef DEBUG
void
utils_vdbg(int facility, const char* fmt, va_list args)
{
	char *msg = NULL;
	int ret = 0;

	if(log_level < DBG)
		return;

	if(!(facility & debug_mask))
		return;

	ret = vasprintf(&msg, fmt, args);
	if(ret < 0)
		return;

	fprintf(stderr, MAGENTA"%s%s"NORMAL,
		utils_get_facility_name(facility), msg);
	fflush(stderr);
	free(msg);
}

void
utils_dbg(int facility, const char* fmt,...)
{
	va_list args;
	if(log_level < DBG)
		return;
	va_start(args, fmt);
	utils_vdbg(facility, fmt, args);
	va_end(args);
}
#else
void utils_vdbg(int facility, const char* fmt, va_list args) {}
void utils_dbg(int facility, const char* fmt,...){}
#endif


void
utils_trim_string(char* string)
{
	char* start = NULL;
	char* end = NULL;
	int len = strnlen(string, PATH_MAX);
	int i = 0;

	/* Find start/end of actual string */
	while(i < len) {
		start = string + i;
		if(((*start) != ' ') && ((*start) != '\n') &&
		   ((*start) != '\r'))
			break;
		i++;
	}

	i = len - 1;
	while(i >= 0) {
		end = string + i;
		if(((*end) != ' ') && ((*end) != '\n') &&
		   ((*end) != '\r') && ((*end) != '\0'))
			break;
		i--;
	}

	/* NULL-terminate it */
	(*(end + 1)) = '\0';

	/* Move it to the beginning of the buffer */
	len = end - start + 1;
	memmove(string, start, len);
}


static int
utils_platform_getrandom(void *buf, size_t len)
{
	static int source_reported = 0;
	int ret = 0;
	FILE *file = NULL;

#if defined(GETRANDOM_DEFINED)
	ret = syscall(SYS_getrandom, buf, len, 0);
	if (ret == len) {
		if(!source_reported) {
			utils_dbg(UTILS, "Got random data through syscall\n");
			source_reported = 1;
		}
		return 0;
	}
#endif
	/* Syscall failed, open urandom instead */
	file = fopen("/dev/urandom", "rb");
	if (file == NULL)
		return -1;

	ret = fread(buf, 1, len, file);
	if (ret != len) {
		fclose(file);
		return -1;
	}

	if(!source_reported) {
		utils_dbg(UTILS, "Got random data through /dev/urandom\n");
		source_reported = 1;
	}
	fclose(file);
	return ret;
}

unsigned int
utils_get_random_uint()
{
	static int source_reported = 0;
	unsigned int value = 0;
	int ret = 0;

	/* Try to get a random number from the os, if it fails
	 * fallback to libc's random() */
	ret = utils_platform_getrandom(&value, sizeof(unsigned int));
	if(ret < 0) {
		value = (unsigned int) random();
		if(!source_reported) {
			utils_dbg(UTILS, "Got random data through random()\n");
			source_reported = 1;
		}
	}

	return value;
}

time_t
utils_get_mtime(char* filepath)
{
	struct stat st;

	if (stat(filepath, &st) < 0) {
		utils_perr(UTILS, "Could not stat(%s)", filepath);
		return 0;
	}

	return st.st_mtime;
}

int
utils_is_regular_file(char* filepath)
{
	struct stat st;
	int ret = 1;

	ret = stat(filepath, &st);
	if (ret < 0) {
		utils_pwrn(UTILS, "Could not stat(%s)", filepath);
		ret = 0;
	} else if (!S_ISREG(st.st_mode)) {
		utils_wrn(UTILS, "Not a regular file: %s\n", filepath);
		ret = 0;
	}

	return 1;
}

int
utils_is_readable_file(char*filepath)
{
#ifndef TEST
	int ret = 0;

	if(!utils_is_regular_file(filepath))
		return 0;

	ret = access(filepath, R_OK);
	if(ret < 0) {
		utils_pwrn(UTILS, "access(%s) failed", filepath);
		return 0;
	}
#endif
	return 1;
}


static void
utils_tm_cleanup_date(struct tm *tm)
{
	/* Zero-out the date part */
	tm->tm_mday = 1;
	tm->tm_mon = 0;
	tm->tm_year = 70;
	tm->tm_wday = 0;
	tm->tm_yday = 0;
	tm->tm_isdst = -1;
}

int
utils_compare_time(struct tm *tm1, struct tm* tm0, int no_date)
{
	time_t t1 = 0;
	time_t t0 = 0;
	double diff = 0.0L;

	if(no_date) {
		utils_tm_cleanup_date(tm0);
		utils_tm_cleanup_date(tm1);
	}

	errno = 0;
	t1 = mktime(tm1);
	t0 = mktime(tm0);
	diff = difftime(t1, t0);

	if (errno != 0)
		utils_perr(UTILS, "compare_time");

	if(diff > 0.0L)
		return 1;
	else if(diff < 0.0L)
		return -1;
	else
		return 0;
}

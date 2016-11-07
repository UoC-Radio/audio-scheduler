#include <stdarg.h>		/* For va_list handling */
#include <time.h>		/* For time_t */

enum facilities {
	NONE	= 0x0,
	SCHED	= 0x2,
	PLR	= 0x4,
	CFG	= 0x8,
	PLS	= 0x10,
	SHUF	= 0x12,
	UTILS	= 0x14,
	SKIP	= 0x100,
};

enum log_levels {
	SILENT	= 0x0,
	ERROR	= 0x1,
	WARN	= 0x2,
	INFO	= 0x3,
	DBG	= 0x4,
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

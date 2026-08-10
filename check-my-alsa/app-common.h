/*
 * app-common.h - applet registry and shared helpers.
 *
 * References pipewire's spa/include/spa/support/log.h (the enum
 * spa_log_level mirrored by enum log_level) and spa/plugins/alsa/
 * alsa-pcm.c (the CHECK() macro pattern).
 */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include <alsa/asoundlib.h>

extern volatile sig_atomic_t g_stop;

bool stop_requested(void);
uint64_t now_ns(void);

/* log levels mirror enum spa_log_level */
enum log_level {
	LOG_ERROR = 1,
	LOG_WARN = 2,
	LOG_INFO = 3,
	LOG_DEBUG = 4,
	LOG_TRACE = 5,
};

/* global verbosity, default LOG_WARN: only errors and warnings.
 * -v -> LOG_INFO, -vv -> LOG_DEBUG, -vvv -> LOG_TRACE */
extern int g_log_level;

/* gated stderr log (pipewire logs go to stderr as well) */
void log_msg(int level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_trace(...) log_msg(LOG_TRACE, __VA_ARGS__)

/* map a -v repeat count to g_log_level (0 -> LOG_WARN) */
void log_set_verbose(int count);
void log_install_alsa_handler(void);

/* pipewire's CHECK(): log the error and return it to the caller
 * instead of exiting; needs an `int err;` in scope and an int-returning
 * function.  Applet mains normalize the negative errno to a 1 exit code. */
#define CHECK(s, msg, ...)                                                     \
	do {                                                                   \
		if ((err = (s)) < 0) {                                         \
			log_error(msg ": %s", ##__VA_ARGS__,                   \
				  snd_strerror(err));                          \
			return err;                                            \
		}                                                              \
	} while (0)

/* --- applet registration (implementation in app-common.c) --- */

struct applet {
	const char *name;
	const char *desc;
	int (*main)(int argc, char **argv);
	void (*usage)(const char *prog);
	struct applet *next;
};

void applet_register(struct applet *a);
const struct applet *applet_first(void);
const struct applet *applet_find(const char *name);

#define APPLET_REGISTER(sym)                                                   \
	__attribute__((constructor)) static void __register_##sym(void) {      \
		applet_register(&sym);                                         \
	}

#define FOR_APPLET(p)                                                          \
	for (const struct applet *p = applet_first(); p; p = p->next)

/* --- shared option strings (aplay-style short options, see aplay(1)) ---
 *
 * Each applet concatenates the fragments it needs into a single getopt
 * short-option string (e.g. PCM_OPTSTRING COMMON_OPTSTRING).  No long
 * options: the earlier getopt_long build made every --long a 1:1 alias
 * of a short option, so dropping the long surface loses no
 * functionality.  Keep in sync with pcm_setup_parse_opt() (the switch
 * on the short character) and pcm_setup_usage(). */

#define PCM_OPTSTRING "D:r:c:f:p:b:d:"
#define CARD_OPTSTRING "c:"
#define COMMON_OPTSTRING "vh"

/* --- pcm test configuration --- */

struct pcm_setup {
	const char *dev; /* -D, default "default" */
	unsigned int rate;
	unsigned int channels;
	snd_pcm_format_t format;
	snd_pcm_uframes_t period;
	snd_pcm_uframes_t buffer;
	unsigned int duration_sec; /* 0 = run until stopped */
};

void pcm_setup_defaults(struct pcm_setup *s);
/* returns false on invalid value (message already printed) */
bool pcm_setup_parse_opt(struct pcm_setup *s, int opt, const char *arg);
/* returns false if a required option is missing (message already printed) */
bool pcm_setup_check(const struct pcm_setup *s);
void pcm_setup_usage(const char *prog);

/* strict number parsing, aplay-style "invalid <what> argument '%s'";
 * returns the value or -1 with *err set */
long parse_long(const char *str, const char *what, int *err);
/* card number 0..31, amixer-style "hw:N" */
int parse_card(const char *arg);

void usage_header(const char *prog, const char *args);
void usage_opt(const char *opt, const char *desc);

/* --- report helpers ---
 *
 * Summary blocks go to stdout (never gated), log lines to stderr.
 * Sections use report_section(); key/value rows are buffered in a
 * struct report_tab and flushed by report_tab_end() with the fixed column
 * width REPORT_KEY_WIDTH, so keys never misalign the values.  Verdict
 * lines are prefixed with a fixed tag (OK/FAIL/WARNING/note) and
 * colored only when stdout is a tty; NO_COLOR disables colors.
 */

#define REPORT_KEY_MAX 48
#define REPORT_KEY_WIDTH 32
#define REPORT_VAL_MAX 1024
#define REPORT_MAX_ROWS 32

struct report_row {
	char key[REPORT_KEY_MAX];
	char val[REPORT_VAL_MAX];
};

struct report_tab {
	struct report_row row[REPORT_MAX_ROWS];
	int n;
	bool truncated;
};

void __attribute__((format(printf, 1, 2))) report_section(const char *fmt, ...);
struct report_tab *report_tab_begin(void);
void report_tab_end(struct report_tab *t);
void __attribute__((format(printf, 3, 4)))
report_kv(struct report_tab *t, const char *key, const char *fmt, ...);
void __attribute__((format(printf, 1, 2))) report_ok(const char *fmt, ...);
void __attribute__((format(printf, 1, 2))) report_fail(const char *fmt, ...);
void __attribute__((format(printf, 1, 2))) report_warn(const char *fmt, ...);
void __attribute__((format(printf, 1, 2))) report_note(const char *fmt, ...);

#endif

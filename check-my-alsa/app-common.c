/*
 * app-common.c - applet registry and shared helpers.
 *
 * Applets self-register via APPLET_REGISTER() (constructor in
 * app-common.h); main.c only dispatches and prints help.
 * Option parsing follows the alsa-utils conventions (aplay/amixer):
 * options before positional arguments, getopt (short options only),
 * strict numeric argument validation.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app-common.h"

/* --- applet registry --- */

static struct applet *applet_head;

void applet_register(struct applet *a) {
	assert(a->next == NULL);
	struct applet **pp = &applet_head;
	while (*pp && strcmp((*pp)->name, a->name) < 0)
		pp = &(*pp)->next;
	a->next = *pp;
	*pp = a;
}

const struct applet *applet_first(void) {
	return applet_head;
}

const struct applet *applet_find(const char *name) {
	for (struct applet *p = applet_head; p; p = p->next)
		if (strcmp(p->name, name) == 0)
			return p;
	return NULL;
}

/* --- misc --- */

bool stop_requested(void) {
	return g_stop;
}

uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* --- logging --- */

int g_log_level = LOG_WARN;
static uint64_t log_start_ns;

void log_msg(int level, const char *fmt, ...) {
	if (level > g_log_level)
		return;
	static const char *const names[] = {
		[LOG_ERROR] = "error", [LOG_WARN] = "warn",
		[LOG_INFO] = "info",   [LOG_DEBUG] = "debug",
		[LOG_TRACE] = "trace",
	};

	fprintf(stderr, "%5s: ", names[level]);
	if (level == LOG_TRACE) {
		uint64_t elapsed = now_ns() - log_start_ns;
		fprintf(stderr, "+%3" PRIu64 ".%06" PRIu64 " s  ",
			(uint64_t) (elapsed / 1000000000ULL),
			(uint64_t) ((elapsed % 1000000000ULL) / 1000ULL));
	}

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (*fmt == '\0' || fmt[strlen(fmt) - 1] != '\n')
		fputc('\n', stderr);
}

void log_set_verbose(int count) {
	log_start_ns = now_ns();
	switch (count) {
	case 0:
		g_log_level = LOG_WARN;
		break;
	case 1:
		g_log_level = LOG_INFO;
		break;
	case 2:
		g_log_level = LOG_DEBUG;
		break;
	default:
		g_log_level = LOG_TRACE;
		break;
	}
}

static void alsa_log_handler(const char *file, int line, const char *function,
			     int err, const char *fmt, ...) {
	(void) file;
	(void) line;
	(void) function;
	(void) err;
	if (g_log_level < LOG_DEBUG)
		return;

	char message[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	log_debug("ALSA library: %s", message);
}

void log_install_alsa_handler(void) {
	snd_lib_error_set_handler(alsa_log_handler);
}

/* --- pcm test configuration --- */

void pcm_setup_defaults(struct pcm_setup *s) {
	memset(s, 0, sizeof(*s));
	s->dev = NULL; /* -D is required */
	s->rate = 48000;
	s->channels = 2;
	s->format = SND_PCM_FORMAT_S16_LE;
	s->period = 1024;
	s->buffer = 0;
	s->duration_sec = 30;
}

/* returns false if a required option is missing (message already
 * printed); applets then print their usage and return 2 */
bool pcm_setup_check(const struct pcm_setup *s) {
	if (s->dev == NULL) {
		fprintf(stderr, "missing required -D device argument\n");
		return false;
	}
	return true;
}

/* aplay(1): strict numeric parsing, "invalid X argument '%s'" */
long parse_long(const char *str, const char *what, int *err) {
	long val;
	char *endptr;

	errno = 0;
	val = strtol(str, &endptr, 0);
	if (errno != 0 || *endptr != '\0') {
		fprintf(stderr, "invalid %s argument '%s'\n", what, str);
		*err = -1;
		return -1;
	}
	*err = 0;
	return val;
}

/* amixer(1): -c card, device name created as 'hw:N' */
int parse_card(const char *arg) {
	int err;
	long val = parse_long(arg, "card", &err);
	if (err < 0)
		return -1;
	if (val < 0 || val > 31) {
		fprintf(stderr, "invalid card argument '%s'\n", arg);
		return -1;
	}
	return (int) val;
}

bool pcm_setup_parse_opt(struct pcm_setup *s, int opt, const char *arg) {
	long val;
	int err;

	switch (opt) {
	case 'D':
		s->dev = arg;
		break;
	case 'r':
		val = parse_long(arg, "rate", &err);
		if (err < 0)
			return false;
		if (val < 2000 || val > 768000) {
			fprintf(stderr, "invalid rate argument '%s'\n", arg);
			return false;
		}
		s->rate = (unsigned int) val;
		break;
	case 'c':
		val = parse_long(arg, "channels", &err);
		if (err < 0)
			return false;
		if (val < 1 || val > 256) {
			fprintf(stderr, "invalid channels argument '%s'\n",
				arg);
			return false;
		}
		s->channels = (unsigned int) val;
		break;
	case 'f':
		s->format = snd_pcm_format_value(arg);
		if (s->format == SND_PCM_FORMAT_UNKNOWN) {
			fprintf(stderr, "unknown format: %s\n", arg);
			return false;
		}
		break;
	case 'p':
		val = parse_long(arg, "period-size", &err);
		if (err < 0)
			return false;
		if (val < 1) {
			fprintf(stderr, "invalid period-size argument '%s'\n",
				arg);
			return false;
		}
		s->period = (snd_pcm_uframes_t) val;
		break;
	case 'b':
		val = parse_long(arg, "buffer-size", &err);
		if (err < 0)
			return false;
		if (val < 0) {
			fprintf(stderr, "invalid buffer-size argument '%s'\n",
				arg);
			return false;
		}
		s->buffer = (snd_pcm_uframes_t) val;
		break;
	case 'd':
		val = parse_long(arg, "duration", &err);
		if (err < 0)
			return false;
		if (val < 0) {
			fprintf(stderr, "invalid duration argument '%s'\n",
				arg);
			return false;
		}
		s->duration_sec = (unsigned int) val;
		break;
	default:
		return false;
	}
	return true;
}

void pcm_setup_usage(const char *prog) {
	usage_header(prog, "[OPTION]...");
	usage_opt("-D NAME", "select PCM by name (required)");
	usage_opt("-r RATE", "sample rate (default 48000)");
	usage_opt("-c CHANNELS", "channels (default 2)");
	usage_opt("-f FORMAT", "sample format (default S16_LE)");
	usage_opt("-p PERIOD", "period size in frames (default 1024)");
	usage_opt("-b BUFFER", "accepted but ignored (driver-derived buffer)");
	usage_opt("-d DURATION",
		  "test duration in seconds (0 = infinite, default 30)");
}

/* --- usage/report helpers --- */

void usage_header(const char *prog, const char *args) {
	fprintf(stderr, "usage: %s %s\n\n", prog, args);
}

void usage_opt(const char *opt, const char *desc) {
	fprintf(stderr, "  %-24s %s\n", opt, desc);
}

/* --- report helpers --- */

#define ANSI_RESET "\033[0m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RED "\033[31m"

static bool color_enabled(void) {
	static bool known;
	static bool enable;

	if (!known) {
		known = true;
		enable = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
	}
	return enable;
}

static void print_colored(const char *color, const char *tag, const char *msg) {
	if (color != NULL && color_enabled())
		printf("%s%s%s %s", color, tag, ANSI_RESET, msg);
	else
		printf("%s %s", tag, msg);
}

void report_section(const char *fmt, ...) {
	static bool first = true;
	char title[256]; /* long enough for device headers like
			  * "hw:0,3 [playback] 'HDA Intel PCH HDMI 0'" */
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(title, sizeof(title), fmt, ap);
	va_end(ap);

	if (!first)
		putchar('\n');
	first = false;
	printf("%s\n", title);
	for (size_t i = 0; i < strlen(title); i++)
		putchar('=');
	putchar('\n');
}

struct report_tab *report_tab_begin(void) {
	static struct report_tab t;
	t.n = 0;
	t.truncated = false;
	return &t;
}

void report_tab_end(struct report_tab *t) {
	for (int i = 0; i < t->n; i++)
		printf("  %-*s : %s\n", REPORT_KEY_WIDTH, t->row[i].key,
		       t->row[i].val);
}

void report_kv(struct report_tab *t, const char *key, const char *fmt, ...) {
	if (t->n >= REPORT_MAX_ROWS) {
		if (!t->truncated) {
			t->truncated = true;
			log_warn("The report contains more than %d rows; "
				 "additional "
				 "rows were omitted",
				 REPORT_MAX_ROWS);
		}
		return;
	}

	snprintf(t->row[t->n].key, sizeof(t->row[t->n].key), "%s", key);

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(t->row[t->n].val, sizeof(t->row[t->n].val), fmt, ap);
	va_end(ap);
	t->n++;
}

void report_ok(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	print_colored(ANSI_GREEN, "OK:", buf);
	putchar('\n');
}

void report_fail(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	print_colored(ANSI_RED, "FAIL:", buf);
	putchar('\n');
}

void report_warn(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	print_colored(ANSI_YELLOW, "WARNING:", buf);
	putchar('\n');
}

void report_note(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	print_colored(NULL, "note:", buf);
	putchar('\n');
}

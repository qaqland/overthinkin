#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "app-common.h"

/* written from the SIGINT/SIGTERM handler, polled by the applet main
 * thread: volatile forces the compiler to re-read memory on every poll
 * instead of caching the value in a register; sig_atomic_t guarantees
 * the read/write is not torn by an interrupt (C11 7.14.1.1) */
volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
	(void) sig;
	g_stop = 1;
}

static void usage(void) {
	fprintf(stderr, "usage: check-my-alsa <applet> [args...]\n"
			"       check-my-alsa -h | --help\n"
			"\n"
			"applets:\n");

	size_t max = 0;
	FOR_APPLET (p) {
		size_t n = strlen(p->name);
		if (n > max)
			max = n;
	}
	FOR_APPLET (p) {
		fprintf(stderr, "  %-*s  %s\n", (int) max, p->name,
			p->desc ? p->desc : "");
	}
	fprintf(stderr, "\nrun 'check-my-alsa <applet> -h' for applet-specific "
			"options.\n");
}

int main(int argc, char **argv) {
	log_install_alsa_handler();
	if (argc < 2) {
		usage();
		return 2;
	}

	const char *want = argv[1];

	if (strcmp(want, "-h") == 0 || strcmp(want, "--help") == 0) {
		usage();
		return 0;
	}

	if (strcmp(want, "help") == 0) {
		if (argc < 3) {
			usage();
			return 2;
		}
		const char *sub = argv[2];
		const struct applet *p = applet_find(sub);
		if (p) {
			if (p->usage)
				p->usage(p->name);
			return 0;
		}
		fprintf(stderr, "check-my-alsa: unknown applet '%s'\n", sub);
		usage();
		return 2;
	}

	const struct applet *p = applet_find(want);
	if (!p) {
		fprintf(stderr, "check-my-alsa: unknown applet '%s'\n", want);
		usage();
		return 2;
	}

	/* no arguments at all: print the applet's help (usage error, exit
	 * code 2); with arguments, the applet validates its own required
	 * options */
	if (argc == 2) {
		if (p->usage)
			p->usage(p->name);
		return 2;
	}

	struct sigaction sa = {0};
	sa.sa_handler = on_sigint;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	/* applet mains return pipewire-style negative errno on
	 * failure; normalize it to a 1 exit code */

	int r = p->main(argc - 1, argv + 1);
	return r < 0 ? 1 : r;
}

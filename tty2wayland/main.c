#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/kd.h>

#include "drm.h"
#include "wayland.h"

#define SOCKET_NAME "tty2wayland-0"

struct options {
	int card;
	char **command;
};

struct options options = {
	.card = 0,
	.command = (char *[]){"weston-simple-shm", NULL},
};

static void
parse_options(int argc, char **argv)
{
	int ok = true;
	int option;
	while ((option = getopt(argc, argv, "+hc:")) != -1) {
		switch (option) {
		case 'h':
			goto usage;
		case 'c':
			options.card = atoi(optarg);
			break;
		default:
			ok = false;
			goto usage;
		}
	}
	if (optind < argc)
		options.command = &argv[optind];

	printf("card-%d with %s\n", options.card, options.command[0]);
	return;

usage:
	fprintf(stdout,
		"usage: %s [-h] [-c N] [-- command args...]\n"
		"  -c N    open /dev/dri/cardN (default: 0)\n"
		"  -h      show this help\n",
		argv[0]);
	exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	parse_options(argc, argv);

	if (geteuid() != 0) {
		fputs("tty2wayland: must run as root\n", stderr);
		exit(EXIT_FAILURE);
	}

	/* Keep termination signals pending until the Wayland event loop handles them. */
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &signals, NULL);

	int status = EXIT_FAILURE;
	int tty_mode = KD_TEXT;
	bool tty_graphics = false;
	struct drm_display *drm = NULL;
	struct wayland_server *server = NULL;

	/* shortcut: assume runtime setup and small allocations succeed; check for non-demo use. */
	char runtime_dir[] = "/tmp/tty2wayland-XXXXXX";
	mkdtemp(runtime_dir);
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	setenv("WAYLAND_DISPLAY", SOCKET_NAME, 1);

	int tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (tty_fd < 0 || ioctl(tty_fd, KDGETMODE, &tty_mode) < 0 ||
	    ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
		perror("tty2wayland: prepare VT");
		goto out;
	}
	tty_graphics = true;

	char device[64];
	snprintf(device, sizeof(device), "/dev/dri/card%d", options.card);
	if (drm_display_open(&drm, device) < 0) {
		perror("tty2wayland: initialize DRM");
		goto out;
	}
	if (wayland_server_create(&server, SOCKET_NAME, drm, 10) < 0) {
		perror("tty2wayland: initialize Wayland");
		goto out;
	}

	printf("Wayland socket: %s/%s\n", runtime_dir, SOCKET_NAME);
	status = wayland_server_run(server, options.command);

out:
	wayland_server_destroy(server);
	drm_display_close(drm);
	if (tty_graphics)
		ioctl(tty_fd, KDSETMODE, tty_mode);
	if (tty_fd >= 0)
		close(tty_fd);
	rmdir(runtime_dir);
	return status;
}

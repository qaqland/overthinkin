#define _POSIX_C_SOURCE 200809L

#include <err.h>
#include <getopt.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-util.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#ifndef PROG_VERSION
#define PROG_VERSION "(dev)"
#endif

#define PROG_USAGE                                                             \
	"usage: jump-or-exec -j REGEX -- COMMAND [ARG...]\n"                   \
	"   or: jump-or-exec -l\n"

struct window {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title;
	char *app_id;
	bool activated;
	struct wl_list link;
};

struct app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
	struct wl_list windows;
};

static void window_destroy(struct window *win) {
	if (win == NULL) {
		return;
	}
	if (win->handle != NULL) {
		zwlr_foreign_toplevel_handle_v1_destroy(win->handle);
		win->handle = NULL;
	}
	wl_list_remove(&win->link);
	free(win->title);
	free(win->app_id);
	free(win);
}

static void set_optional_string(char **dst, const char *src) {
	char *copy = NULL;
	if (src != NULL && src[0] != '\0') {
		copy = strdup(src);
	}
	free(*dst);
	*dst = copy;
}

static void toplevel_title(void *data,
			   struct zwlr_foreign_toplevel_handle_v1 *handle,
			   const char *title) {
	struct window *win = data;
	(void) handle;
	set_optional_string(&win->title, title);
}

static void toplevel_app_id(void *data,
			    struct zwlr_foreign_toplevel_handle_v1 *handle,
			    const char *app_id) {
	struct window *win = data;
	(void) handle;
	set_optional_string(&win->app_id, app_id);
}

static void
toplevel_output_enter(void *data,
		      struct zwlr_foreign_toplevel_handle_v1 *handle,
		      struct wl_output *output) {
	(void) data;
	(void) handle;
	(void) output;
}

static void
toplevel_output_leave(void *data,
		      struct zwlr_foreign_toplevel_handle_v1 *handle,
		      struct wl_output *output) {
	(void) data;
	(void) handle;
	(void) output;
}

static void toplevel_state(void *data,
			   struct zwlr_foreign_toplevel_handle_v1 *handle,
			   struct wl_array *state) {
	struct window *win = data;
	(void) handle;

	win->activated = false;
	uint32_t *entry;
	wl_array_for_each(entry, state) {
		if (*entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
			win->activated = true;
			break;
		}
	}
}

static void toplevel_done(void *data,
			  struct zwlr_foreign_toplevel_handle_v1 *handle) {
	(void) data;
	(void) handle;
}

static void toplevel_closed(void *data,
			    struct zwlr_foreign_toplevel_handle_v1 *handle) {
	struct window *win = data;
	if (win->handle == handle) {
		win->handle = NULL;
	}
	window_destroy(win);
}

static void toplevel_parent(void *data,
			    struct zwlr_foreign_toplevel_handle_v1 *handle,
			    struct zwlr_foreign_toplevel_handle_v1 *parent) {
	(void) data;
	(void) handle;
	(void) parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener =
	{
		.title = toplevel_title,
		.app_id = toplevel_app_id,
		.output_enter = toplevel_output_enter,
		.output_leave = toplevel_output_leave,
		.state = toplevel_state,
		.done = toplevel_done,
		.closed = toplevel_closed,
		.parent = toplevel_parent,
};

static void manager_toplevel(void *data,
			     struct zwlr_foreign_toplevel_manager_v1 *manager,
			     struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
	struct app *app = data;
	(void) manager;

	struct window *win = calloc(1, sizeof(*win));
	if (win == NULL) {
		return;
	}
	win->handle = toplevel;
	wl_list_insert(&app->windows, &win->link);
	zwlr_foreign_toplevel_handle_v1_add_listener(toplevel,
						     &toplevel_listener, win);
}

static void manager_finished(void *data,
			     struct zwlr_foreign_toplevel_manager_v1 *manager) {
	(void) data;
	(void) manager;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener =
	{
		.toplevel = manager_toplevel,
		.finished = manager_finished,
};

static void global_handler(void *data, struct wl_registry *registry,
			   uint32_t id, const char *interface,
			   uint32_t version) {
	struct app *app = data;
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		app->seat =
			wl_registry_bind(registry, id, &wl_seat_interface, 1);
		return;
	}
	if (strcmp(interface,
		   zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		uint32_t bind_version = version < 3 ? version : 3;
		app->toplevel_manager = wl_registry_bind(
			registry, id,
			&zwlr_foreign_toplevel_manager_v1_interface,
			bind_version);
		zwlr_foreign_toplevel_manager_v1_add_listener(
			app->toplevel_manager, &manager_listener, app);
	}
}

static void global_remove_handler(void *data, struct wl_registry *registry,
				  uint32_t name) {
	(void) data;
	(void) registry;
	(void) name;
}

static const struct wl_registry_listener registry_listener = {
	.global = global_handler,
	.global_remove = global_remove_handler,
};

static bool window_matches(struct window *win, regex_t *regex) {
	const char *app_id = win->app_id ? win->app_id : "";
	const char *title = win->title ? win->title : "";

	char joined[512] = {0};
	snprintf(joined, sizeof(joined), "%s\n%s", app_id, title);
	int match = regexec(regex, joined, 0, NULL, 0);
	return match == 0;
}

static struct window *select_window(struct app *app, regex_t *regex) {
	struct window *candidate = NULL;
	struct window *win;
	wl_list_for_each(win, &app->windows, link) {
		if (!window_matches(win, regex)) {
			continue;
		}
		if (win->activated) {
			return win;
		}
		if (candidate == NULL) {
			candidate = win;
		}
	}
	return candidate;
}

static void list_windows(struct app *app) {
	struct window *win;
	wl_list_for_each(win, &app->windows, link) {
		const char *app_id = win->app_id ? win->app_id : "";
		const char *title = win->title ? win->title : "";
		printf("app_id: '%s'\n title: '%s'\n\n", app_id, title);
	}
}

static void spawn_command(char **argv) {
	pid_t pid = fork();
	if (pid < 0) {
		err(1, "fork failed");
	}
	if (pid == 0) {
		execvp(argv[0], argv);
		err(1, "execvp failed");
	}
}

static void destroy_app(struct app *app) {
	struct window *win, *tmp;
	wl_list_for_each_safe(win, tmp, &app->windows, link) {
		window_destroy(win);
	}

	if (app->toplevel_manager != NULL) {
		zwlr_foreign_toplevel_manager_v1_stop(app->toplevel_manager);
		zwlr_foreign_toplevel_manager_v1_destroy(app->toplevel_manager);
	}
	if (app->seat != NULL) {
		wl_seat_destroy(app->seat);
	}
	if (app->registry != NULL) {
		wl_registry_destroy(app->registry);
	}
	if (app->display != NULL) {
		wl_display_disconnect(app->display);
	}
}

int main(int argc, char **argv) {
	const char *pattern = NULL;
	bool list_only = false;

	int c;
	while ((c = getopt(argc, argv, "j:hvl")) != -1) {
		switch (c) {
		case 'h':
			fprintf(stdout, PROG_USAGE);
			exit(0);
		case 'v':
			printf("jump-or-exec version %s\n", PROG_VERSION);
			exit(0);
		case 'j':
			pattern = optarg;
			break;
		case 'l':
			list_only = true;
			break;
		default:
			fprintf(stderr, PROG_USAGE);
			exit(1);
		}
	}

	if (!list_only && (pattern == NULL || argc == optind)) {
		fprintf(stderr, PROG_USAGE);
		exit(1);
	}

	regex_t regex = {0};
	if (!list_only) {
		int ret = regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB);
		if (ret != 0) {
			char errbuf[256] = {0};
			regerror(ret, &regex, errbuf, sizeof(errbuf));
			errx(1, "failed to compile regex: %s", errbuf);
		}
	}

	struct app app = {0};
	wl_list_init(&app.windows);

	app.display = wl_display_connect(NULL);
	if (app.display == NULL) {
		errx(1, "failed to connect to wayland display");
	}

	app.registry = wl_display_get_registry(app.display);
	if (app.registry == NULL) {
		errx(1, "failed to get wayland registry");
	}
	wl_registry_add_listener(app.registry, &registry_listener, &app);

	if (wl_display_roundtrip(app.display) < 0 ||
	    wl_display_roundtrip(app.display) < 0) {
		errx(1, "failed to sync with wayland compositor");
	}

	if (app.seat == NULL || app.toplevel_manager == NULL) {
		errx(1, "required protocols are missing: wl_seat or "
			"zwlr_foreign_toplevel_manager_v1");
	}

	if (list_only) {
		list_windows(&app);
		destroy_app(&app);
		exit(0);
	}

	struct window *match = select_window(&app, &regex);
	if (match != NULL) {
		zwlr_foreign_toplevel_handle_v1_activate(match->handle,
							 app.seat);
		wl_display_roundtrip(app.display);
	} else {
		spawn_command(&argv[optind]);
	}

	destroy_app(&app);
	regfree(&regex);
	exit(0);
}

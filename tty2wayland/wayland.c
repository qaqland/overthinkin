#include "wayland.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "drm.h"
#include "xdg-shell-server-protocol.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct surface {
	struct wayland_server *server;
	struct wl_resource *wl_surface, *xdg_surface, *toplevel;
	struct wl_resource *buffer, *callback;
};

struct wayland_server {
	struct wl_display *display;
	struct wl_event_source *sigalrm, *drm_source, *sigint, *sigterm, *sigchld;
	bool timer_started;
	struct surface *surface;
	struct drm_display *drm;
	int timeout, status;
	pid_t child;
};

static void
frame_done(struct wayland_server *s, uint32_t time_ms);

static int
stop_timer(int signal, void *data)
{
	struct wayland_server *s = data;
	puts("timeout elapsed");
	wl_display_terminate(s->display);
	return 0;
}

static int
stop_signal(int signal, void *data)
{
	struct wayland_server *s = data;
	wl_display_terminate(s->display);
	return 0;
}

static int
drm_event(int fd, uint32_t mask, void *data)
{
	struct wayland_server *s = data;
	uint32_t time_ms;
	if (drm_display_dispatch(s->drm, &time_ms) < 0) {
		perror("tty2wayland: drmHandleEvent");
		s->status = EXIT_FAILURE;
		wl_display_terminate(s->display);
	} else {
		frame_done(s, time_ms);
	}
	return 0;
}

static void
copy_buffer(struct wayland_server *s, struct wl_resource *resource)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(resource);

	int sw = wl_shm_buffer_get_width(shm);
	int sh = wl_shm_buffer_get_height(shm);
	int stride = wl_shm_buffer_get_stride(shm);
	const struct drm_target *dst = drm_display_backbuffer(s->drm);
	int width = MIN(sw, dst->width);
	int height = MIN(sh, dst->height);

	wl_shm_buffer_begin_access(shm);
	uint8_t *src_data = wl_shm_buffer_get_data(shm);
	memset(dst->map, 0, dst->size);
	for (int y = 0; y < height; y++) {
		uint32_t *src = (uint32_t *) (src_data + (size_t) y * stride);
		uint32_t *out = (uint32_t *) ((uint8_t *) dst->map + (size_t) y * dst->pitch);
		memcpy(out, src, (size_t) width * sizeof(*out));
	}
	wl_shm_buffer_end_access(shm);
	wl_buffer_send_release(resource);
	if (drm_display_present(s->drm) < 0) {
		perror("tty2wayland: atomic page flip");
		s->status = EXIT_FAILURE;
		wl_display_terminate(s->display);
	}
}

static void
surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
surface_attach(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer,
	       int32_t x, int32_t y)
{
	((struct surface *) wl_resource_get_user_data(resource))->buffer = buffer;
}

static void
surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
	       int32_t width, int32_t height)
{
}

static void
surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	surface->callback = wl_resource_create(client, &wl_callback_interface, 1, id);
}

static void
surface_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	/* shortcut: simple-shm ordering only; validate for other clients. */
	if (!surface->buffer) {
		struct wl_array states = {0};
		xdg_toplevel_send_configure(surface->toplevel, 0, 0, &states);
		xdg_surface_send_configure(surface->xdg_surface,
					   wl_display_next_serial(surface->server->display));
		return;
	}
	struct wl_resource *buffer = surface->buffer;
	surface->buffer = NULL;
	copy_buffer(surface->server, buffer);
}

static const struct wl_surface_interface surface_impl = {
	.destroy = surface_destroy,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.commit = surface_commit,
};

static void
free_surface(struct wl_resource *resource)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	surface->server->surface = NULL;
	free(surface);
}

static void
create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct wayland_server *s = wl_resource_get_user_data(resource);
	struct surface *surface = calloc(1, sizeof(*surface));
	surface->server = s;
	surface->wl_surface = wl_resource_create(client, &wl_surface_interface, 1, id);
	wl_resource_set_implementation(surface->wl_surface, &surface_impl, surface, free_surface);
	s->surface = surface;
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = create_surface,
};

static void
bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource =
		wl_resource_create(client, &wl_compositor_interface, version, id);
	wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}

static void
resource_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
ignore_string(struct wl_client *client, struct wl_resource *resource, const char *text)
{
}

static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = resource_destroy,
	.set_title = ignore_string,
	.set_app_id = ignore_string,
};

static void
get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct surface *surface = wl_resource_get_user_data(resource);
	surface->toplevel = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
	wl_resource_set_implementation(surface->toplevel, &toplevel_impl, surface, NULL);
}

static void
ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = resource_destroy,
	.get_toplevel = get_toplevel,
	.ack_configure = ack_configure,
};

static void
get_xdg_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *wl_surface)
{
	struct surface *surface = wl_resource_get_user_data(wl_surface);
	surface->xdg_surface = wl_resource_create(client, &xdg_surface_interface, 1, id);
	wl_resource_set_implementation(surface->xdg_surface, &xdg_surface_impl, surface, NULL);
}

static const struct xdg_wm_base_interface wm_base_impl = {
	.destroy = resource_destroy,
	.get_xdg_surface = get_xdg_surface,
};

static void
bind_wm_base(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource =
		wl_resource_create(client, &xdg_wm_base_interface, version, id);
	wl_resource_set_implementation(resource, &wm_base_impl, NULL, NULL);
}

static void
frame_done(struct wayland_server *s, uint32_t time_ms)
{
	if (s->surface && s->surface->callback) {
		wl_callback_send_done(s->surface->callback, time_ms);
		wl_resource_destroy(s->surface->callback);
		s->surface->callback = NULL;
	}
	if (!s->timer_started) {
		s->timer_started = true;
		alarm(s->timeout);
		printf("first frame displayed; exiting in %d seconds\n", s->timeout);
	}
}

int
wayland_server_create(struct wayland_server **out, const char *socket, struct drm_display *drm,
		      int timeout)
{
	struct wayland_server *s = calloc(1, sizeof(*s));
	s->drm = drm;
	s->timeout = timeout;

	s->display = wl_display_create();
	if (!s->display)
		goto fail;
	if (wl_display_init_shm(s->display) < 0 ||
	    !wl_global_create(s->display, &wl_compositor_interface, 1, s, bind_compositor) ||
	    !wl_global_create(s->display, &xdg_wm_base_interface, 1, NULL, bind_wm_base) ||
	    wl_display_add_socket(s->display, socket) < 0)
		goto fail;

	struct wl_event_loop *loop = wl_display_get_event_loop(s->display);
	s->sigalrm = wl_event_loop_add_signal(loop, SIGALRM, stop_timer, s);
	s->drm_source =
		wl_event_loop_add_fd(loop, drm_display_fd(drm), WL_EVENT_READABLE, drm_event, s);
	if (!s->drm_source || !s->sigalrm)
		goto fail;

	*out = s;
	return 0;
fail:
	wayland_server_destroy(s);
	return -1;
}

static int
child_exited(int signal, void *data)
{
	struct wayland_server *s = data;
	int status;
	if (s->child > 0 && waitpid(s->child, &status, WNOHANG) == s->child) {
		s->child = 0;
		s->status = WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
		wl_display_terminate(s->display);
	}
	return 0;
}

int
wayland_server_run(struct wayland_server *s, char **command)
{
	struct wl_event_loop *loop = wl_display_get_event_loop(s->display);
	s->sigint = wl_event_loop_add_signal(loop, SIGINT, stop_signal, s);
	s->sigterm = wl_event_loop_add_signal(loop, SIGTERM, stop_signal, s);
	s->sigchld = wl_event_loop_add_signal(loop, SIGCHLD, child_exited, s);
	if (!s->sigint || !s->sigterm || !s->sigchld) {
		perror("tty2wayland: watch signals");
		return EXIT_FAILURE;
	}

	s->child = fork();
	if (s->child < 0) {
		perror("tty2wayland: fork");
		return EXIT_FAILURE;
	}
	if (s->child == 0) {
		/* Wayland's signal event sources block signals in the parent. */
		sigset_t mask;
		sigemptyset(&mask);
		sigprocmask(SIG_SETMASK, &mask, NULL);
		execvp(command[0], command);
		perror(command[0]);
		_exit(127);
	}

	wl_display_run(s->display);
	if (s->child > 0) {
		/* shortcut: child must honor SIGTERM; add supervision for arbitrary commands. */
		kill(s->child, SIGTERM);
		while (waitpid(s->child, NULL, 0) < 0 && errno == EINTR)
			;
		s->child = 0;
	}
	return s->status;
}

void
wayland_server_destroy(struct wayland_server *s)
{
	if (!s)
		return;
	alarm(0);
	if (s->display) {
		struct wl_event_source *sources[] = {
			s->sigalrm, s->drm_source, s->sigint, s->sigterm, s->sigchld,
		};
		for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
			if (sources[i])
				wl_event_source_remove(sources[i]);
		wl_display_destroy_clients(s->display);
		wl_display_destroy(s->display);
	}
	free(s);
}

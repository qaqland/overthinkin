#ifndef TTY2WAYLAND_WAYLAND_H
#define TTY2WAYLAND_WAYLAND_H

struct drm_display;
struct wayland_server;

int
wayland_server_create(struct wayland_server **out, const char *socket, struct drm_display *drm,
		      int timeout);
void
wayland_server_destroy(struct wayland_server *s);
int
wayland_server_run(struct wayland_server *s, char **command);

#endif

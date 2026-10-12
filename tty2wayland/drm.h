#ifndef TTY2WAYLAND_DRM_H
#define TTY2WAYLAND_DRM_H

#include <stdint.h>

struct drm_display;

/* Pixel view of the current back buffer, for the Wayland side to copy into. */
struct drm_target {
	void *map;
	uint32_t pitch;
	uint64_t size;
	int width, height;
};

int
drm_display_open(struct drm_display **out, const char *device);
void
drm_display_close(struct drm_display *d);
int
drm_display_fd(const struct drm_display *d);
int
drm_display_dispatch(struct drm_display *d, uint32_t *time_ms);
const struct drm_target *
drm_display_backbuffer(struct drm_display *d);
int
drm_display_present(struct drm_display *d);

#endif

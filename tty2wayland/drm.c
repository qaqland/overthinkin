#include "drm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct property {
	uint32_t id;
	uint64_t old;
};

struct kms {
	int fd;
	uint32_t connector, crtc, crtc_index, plane, mode_blob, saved_mode_blob;
	drmModeModeInfo mode;
	struct property connector_crtc, crtc_mode, crtc_active;
	struct property plane_fb, plane_crtc;
	struct property src_x, src_y, src_w, src_h;
	struct property dst_x, dst_y, dst_w, dst_h;
	bool active;
};

struct framebuffer {
	uint32_t handle, pitch, id;
	uint64_t size;
	void *map;
};

struct drm_display {
	struct kms kms;
	struct framebuffer fb[2];
	int front;
	struct drm_target target;
	uint32_t time_ms;
};

static int
get_property(int fd, uint32_t object, uint32_t type, const char *name, struct property *out)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object, type);
	if (!props)
		return -1;

	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
		if (prop && strcmp(prop->name, name) == 0) {
			out->id = prop->prop_id;
			out->old = props->prop_values[i];
			drmModeFreeProperty(prop);
			drmModeFreeObjectProperties(props);
			return 0;
		}
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	errno = ENOENT;
	return -1;
}

static bool
supports_xrgb(const drmModePlane *plane)
{
	for (uint32_t i = 0; i < plane->count_formats; i++)
		if (plane->formats[i] == DRM_FORMAT_XRGB8888)
			return true;
	return false;
}

static int
find_pipeline(struct kms *kms)
{
	drmModeRes *res = drmModeGetResources(kms->fd);
	if (!res)
		return -1;

	drmModeConnector *connector = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		connector = drmModeGetConnector(kms->fd, res->connectors[i]);
		if (connector && connector->connection == DRM_MODE_CONNECTED &&
		    connector->count_modes)
			break;
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (!connector) {
		drmModeFreeResources(res);
		errno = ENODEV;
		return -1;
	}

	int mode = 0;
	for (int i = 0; i < connector->count_modes; i++)
		if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = i;
			break;
		}
	kms->connector = connector->connector_id;
	kms->mode = connector->modes[mode];

	bool found = false;
	for (int i = 0; i < connector->count_encoders && !found; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(kms->fd, connector->encoders[i]);
		if (!encoder)
			continue;
		for (int j = 0; j < res->count_crtcs; j++) {
			if (encoder->possible_crtcs & (1u << j)) {
				kms->crtc = res->crtcs[j];
				kms->crtc_index = (uint32_t) j;
				found = true;
				break;
			}
		}
		drmModeFreeEncoder(encoder);
	}
	drmModeFreeConnector(connector);
	drmModeFreeResources(res);
	if (!found) {
		errno = ENODEV;
		return -1;
	}

	drmModePlaneRes *planes = drmModeGetPlaneResources(kms->fd);
	if (!planes)
		return -1;
	for (uint32_t i = 0; i < planes->count_planes; i++) {
		drmModePlane *plane = drmModeGetPlane(kms->fd, planes->planes[i]);
		struct property type = {0};
		if (plane && (plane->possible_crtcs & (1u << kms->crtc_index)) &&
		    supports_xrgb(plane) &&
		    get_property(kms->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type) ==
			    0 &&
		    type.old == DRM_PLANE_TYPE_PRIMARY) {
			kms->plane = plane->plane_id;
			drmModeFreePlane(plane);
			break;
		}
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(planes);
	if (!kms->plane) {
		errno = ENODEV;
		return -1;
	}

	printf("connector %u, CRTC %u, primary plane %u, mode %s\n", kms->connector, kms->crtc,
	       kms->plane, kms->mode.name);
	return 0;
}

static int
load_properties(struct kms *kms)
{
#define GET(object, type, name, member)                                                            \
	do {                                                                                       \
		if (get_property(kms->fd, object, type, name, &kms->member) < 0)                   \
			return -1;                                                                 \
	} while (0)
	GET(kms->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", connector_crtc);
	GET(kms->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", crtc_mode);
	GET(kms->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", crtc_active);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "FB_ID", plane_fb);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", plane_crtc);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "SRC_X", src_x);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", src_y);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "SRC_W", src_w);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "SRC_H", src_h);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", dst_x);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", dst_y);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", dst_w);
	GET(kms->plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", dst_h);
#undef GET
	/* The old MODE_ID may disappear after modesetting; keep our own blob. */
	if (kms->crtc_mode.old) {
		drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(kms->fd, kms->crtc_mode.old);
		if (!blob)
			return -1;
		int rc = drmModeCreatePropertyBlob(kms->fd, blob->data, blob->length,
						   &kms->saved_mode_blob);
		drmModeFreePropertyBlob(blob);
		if (rc < 0)
			return -1;
		kms->crtc_mode.old = kms->saved_mode_blob;
	}
	return 0;
}

static void
add_prop(drmModeAtomicReq *req, uint32_t object, const struct property *prop, uint64_t value)
{
	drmModeAtomicAddProperty(req, object, prop->id, value);
}

static int
commit_req(struct drm_display *d, drmModeAtomicReq *req, uint32_t flags, void *data)
{
	int rc = drmModeAtomicCommit(d->kms.fd, req, flags, data);
	drmModeAtomicFree(req);
	return rc ? -1 : 0;
}

static int
create_fb(struct drm_display *d, struct framebuffer *fb)
{
	struct drm_mode_create_dumb create = {
		.width = d->kms.mode.hdisplay,
		.height = d->kms.mode.vdisplay,
		.bpp = 32,
	};
	if (drmIoctl(d->kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
		return -1;
	fb->handle = create.handle;
	fb->pitch = create.pitch;
	fb->size = create.size;

	uint32_t handles[4] = {fb->handle};
	uint32_t pitches[4] = {fb->pitch};
	uint32_t offsets[4] = {0};
	if (drmModeAddFB2(d->kms.fd, create.width, create.height, DRM_FORMAT_XRGB8888, handles,
			  pitches, offsets, &fb->id, 0) < 0)
		return -1;

	struct drm_mode_map_dumb map = {.handle = fb->handle};
	if (drmIoctl(d->kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
		return -1;
	fb->map = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->kms.fd, map.offset);
	if (fb->map == MAP_FAILED) {
		fb->map = NULL;
		return -1;
	}
	memset(fb->map, 0, fb->size);
	return 0;
}

static int
modeset(struct drm_display *d)
{
	struct kms *k = &d->kms;
	if (drmModeCreatePropertyBlob(k->fd, &k->mode, sizeof(k->mode), &k->mode_blob) < 0)
		return -1;
	drmModeAtomicReq *req = drmModeAtomicAlloc();

	uint64_t w = k->mode.hdisplay, h = k->mode.vdisplay;
	add_prop(req, k->connector, &k->connector_crtc, k->crtc);
	add_prop(req, k->crtc, &k->crtc_mode, k->mode_blob);
	add_prop(req, k->crtc, &k->crtc_active, 1);
	add_prop(req, k->plane, &k->plane_fb, d->fb[0].id);
	add_prop(req, k->plane, &k->plane_crtc, k->crtc);
	add_prop(req, k->plane, &k->src_x, 0);
	add_prop(req, k->plane, &k->src_y, 0);
	add_prop(req, k->plane, &k->src_w, w << 16);
	add_prop(req, k->plane, &k->src_h, h << 16);
	add_prop(req, k->plane, &k->dst_x, 0);
	add_prop(req, k->plane, &k->dst_y, 0);
	add_prop(req, k->plane, &k->dst_w, w);
	add_prop(req, k->plane, &k->dst_h, h);
	int rc = commit_req(d, req, DRM_MODE_ATOMIC_ALLOW_MODESET, d);
	if (!rc)
		k->active = true;
	return rc;
}

static int
restore_kms(struct drm_display *d)
{
	struct kms *k = &d->kms;
	if (!k->active)
		return 0;
	drmModeAtomicReq *req = drmModeAtomicAlloc();
#define OLD(object, member) add_prop(req, object, &k->member, k->member.old)
	OLD(k->connector, connector_crtc);
	OLD(k->crtc, crtc_mode);
	OLD(k->crtc, crtc_active);
	OLD(k->plane, plane_fb);
	OLD(k->plane, plane_crtc);
	OLD(k->plane, src_x);
	OLD(k->plane, src_y);
	OLD(k->plane, src_w);
	OLD(k->plane, src_h);
	OLD(k->plane, dst_x);
	OLD(k->plane, dst_y);
	OLD(k->plane, dst_w);
	OLD(k->plane, dst_h);
#undef OLD
	return commit_req(d, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
}

static void
page_flip(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
	struct drm_display *d = data;
	d->front = 1 - d->front;
	d->time_ms = sec * 1000u + usec / 1000u;
}

int
drm_display_dispatch(struct drm_display *d, uint32_t *time_ms)
{
	drmEventContext context = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip,
	};
	if (drmHandleEvent(d->kms.fd, &context) < 0)
		return -1;
	*time_ms = d->time_ms;
	return 0;
}

const struct drm_target *
drm_display_backbuffer(struct drm_display *d)
{
	struct framebuffer *fb = &d->fb[1 - d->front];
	d->target.map = fb->map;
	d->target.pitch = fb->pitch;
	d->target.size = fb->size;
	d->target.width = d->kms.mode.hdisplay;
	d->target.height = d->kms.mode.vdisplay;
	return &d->target;
}

int
drm_display_present(struct drm_display *d)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	add_prop(req, d->kms.plane, &d->kms.plane_fb, d->fb[1 - d->front].id);
	return commit_req(d, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, d);
}

static void
destroy_fb(struct drm_display *d, struct framebuffer *fb)
{
	if (fb->map)
		munmap(fb->map, fb->size);
	if (fb->id)
		drmModeRmFB(d->kms.fd, fb->id);
	if (fb->handle) {
		struct drm_mode_destroy_dumb destroy = {.handle = fb->handle};
		drmIoctl(d->kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
}

int
drm_display_open(struct drm_display **out, const char *device)
{
	struct drm_display *d = calloc(1, sizeof(*d));
	d->kms.fd = open(device, O_RDWR | O_CLOEXEC);
	if (d->kms.fd < 0 || drmSetClientCap(d->kms.fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0 ||
	    drmSetMaster(d->kms.fd) < 0 || find_pipeline(&d->kms) < 0 ||
	    load_properties(&d->kms) < 0 || create_fb(d, &d->fb[0]) < 0 ||
	    create_fb(d, &d->fb[1]) < 0 || modeset(d) < 0) {
		int saved_errno = errno;
		drm_display_close(d);
		errno = saved_errno;
		return -1;
	}
	*out = d;
	return 0;
}

int
drm_display_fd(const struct drm_display *d)
{
	return d->kms.fd;
}

void
drm_display_close(struct drm_display *d)
{
	if (!d)
		return;
	if (restore_kms(d) < 0)
		fprintf(stderr, "tty2wayland: restore KMS: %s\n", strerror(errno));
	destroy_fb(d, &d->fb[0]);
	destroy_fb(d, &d->fb[1]);
	if (d->kms.mode_blob)
		drmModeDestroyPropertyBlob(d->kms.fd, d->kms.mode_blob);
	if (d->kms.saved_mode_blob)
		drmModeDestroyPropertyBlob(d->kms.fd, d->kms.saved_mode_blob);
	if (d->kms.fd >= 0) {
		drmDropMaster(d->kms.fd);
		close(d->kms.fd);
	}
	free(d);
}

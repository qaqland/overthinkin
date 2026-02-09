#include <stdio.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

static void global_handler(void *data, struct wl_registry *registry,
			   uint32_t id, const char *interface,
			   uint32_t version) {
	(void) registry;

	unsigned int *count = data;
	printf("%s#%u:%d\n", interface, id, version);
	(*count)++;
}

static void global_remove_handler(void *data, struct wl_registry *registry,
				  uint32_t name) {
	(void) data;
	(void) registry;
	(void) name;
}

static const struct wl_registry_listener listener = {
	global_handler,
	global_remove_handler,
};

int main(int argc, char *argv[]) {

	if (argc > 1) {
		printf("wayland registry oom\n");
		return 1;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 1;
	}

	unsigned int count = 1; // wl_display
	struct wl_registry *registry;

	for (;;) {
		registry = wl_display_get_registry(display);
		if (!registry) {
			continue;
		}
		count++;
		wl_registry_add_listener(registry, &listener, &count);
		wl_display_roundtrip(display);
		wl_registry_destroy(registry);
	}

	wl_display_disconnect(display);
	return 0;
}

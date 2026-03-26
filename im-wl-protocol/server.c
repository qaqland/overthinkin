#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

#include "im-chat-v1-server-protocol.h"

#define MAX_NICKNAME_LEN 8

struct im_server {
	struct wl_display *display;
	struct wl_global *chat_global;
	struct wl_list clients; // im_client.link
};

struct im_client {
	struct wl_resource *resource;
	char nickname[MAX_NICKNAME_LEN];
	struct wl_list link; // im_server.clients
};

static struct im_server server;

static struct im_client *find_client_by_nickname(const char *nickname) {
	struct im_client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (strcmp(c->nickname, nickname) == 0)
			return c;
	}
	return NULL;
}

static void broadcast_user_joined(const char *nickname) {
	struct im_client *c;
	wl_list_for_each(c, &server.clients, link) {
		im_chat_manager_v1_send_user_joined(c->resource, nickname);
	}
	fprintf(stderr, "--\t%s joined\n", nickname);
}

static void broadcast_user_left(const char *nickname) {
	struct im_client *c;
	wl_list_for_each(c, &server.clients, link) {
		im_chat_manager_v1_send_user_left(c->resource, nickname);
	}
	fprintf(stderr, "--\t%s left\n", nickname);
}

static void handle_set_nickname(struct wl_client *wl_client,
				struct wl_resource *resource,
				const char *nickname) {
	(void) wl_client;
	struct im_client *client = wl_resource_get_user_data(resource);

	if (nickname == NULL || strlen(nickname) == 0 ||
	    strlen(nickname) >= MAX_NICKNAME_LEN) {
		im_chat_manager_v1_send_nickname_rejected(
			resource, "nickname is empty or too long");
		return;
	}

	/* check for duplicate */
	struct im_client *existing = find_client_by_nickname(nickname);
	if (existing != NULL && existing != client) {
		im_chat_manager_v1_send_nickname_rejected(
			resource, "nickname already in use");
		return;
	}

	if (client->nickname[0])
		broadcast_user_left(client->nickname);

	snprintf(client->nickname, MAX_NICKNAME_LEN, "%s", nickname);

	im_chat_manager_v1_send_nickname_accepted(resource, client->nickname);
	broadcast_user_joined(client->nickname);
}

static void handle_send_message(struct wl_client *wl_client,
				struct wl_resource *resource,
				const char *content) {
	(void) wl_client;
	struct im_client *client = wl_resource_get_user_data(resource);

	if (!client->nickname[0]) {
		/* silently ignore messages from unnamed clients */
		return;
	}

	if (content == NULL || strlen(content) == 0)
		return;

	fprintf(stderr, "%s\t%s\n", client->nickname, content);

	struct im_client *c;
	wl_list_for_each(c, &server.clients, link) {
		im_chat_manager_v1_send_message(c->resource, client->nickname,
						content);
	}
}

static void handle_destroy(struct wl_client *wl_client,
			   struct wl_resource *resource) {
	(void) wl_client;
	wl_resource_destroy(resource);
}

static const struct im_chat_manager_v1_interface chat_impl = {
	.set_nickname = handle_set_nickname,
	.send_message = handle_send_message,
	.destroy = handle_destroy,
};

static void client_resource_destroy(struct wl_resource *resource) {
	struct im_client *client = wl_resource_get_user_data(resource);
	if (client == NULL)
		return;

	wl_list_remove(&client->link);

	if (client->nickname[0])
		broadcast_user_left(client->nickname);

	free(client);
}

static void chat_bind(struct wl_client *wl_client, void *data, uint32_t version,
		      uint32_t id) {
	(void) data;

	struct im_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	client->resource = wl_resource_create(
		wl_client, &im_chat_manager_v1_interface, version, id);
	if (client->resource == NULL) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(client->resource, &chat_impl, client,
				       client_resource_destroy);
	wl_list_insert(&server.clients, &client->link);

	fprintf(stderr, "--\tnew client connected\n");
}

static void handle_signal(int sig) {
	(void) sig;
	if (server.display)
		wl_display_terminate(server.display);
}

int main(void) {
	wl_list_init(&server.clients);

	server.display = wl_display_create();
	if (server.display == NULL) {
		fprintf(stderr, "failed to create wl_display\n");
		return 0;
	}

	if (wl_display_add_socket(server.display, SOCKET_NAME) != 0) {
		fprintf(stderr, "failed to add socket '%s'\n", SOCKET_NAME);
		goto out;
	}

	server.chat_global =
		wl_global_create(server.display, &im_chat_manager_v1_interface,
				 1, &server, chat_bind);
	if (server.chat_global == NULL) {
		fprintf(stderr, "failed to create global\n");
		goto out;
	}

	struct sigaction sa = {.sa_handler = handle_signal};
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	fprintf(stderr, "--\tlistening on WAYLAND_DISPLAY=%s\n", SOCKET_NAME);
	wl_display_run(server.display);

	wl_global_destroy(server.chat_global);
out:
	wl_display_destroy(server.display);
	fprintf(stderr, "--\tshutdown\n");
	return 0;
}

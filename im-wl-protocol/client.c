#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wayland-client-core.h>

#include "im-chat-v1-client-protocol.h"

#define MAX_INPUT_LEN 128

struct im_app {
	struct wl_display *display;
	struct im_chat_manager_v1 *chat;
	int has_nickname;
	int running;
};

/* ---- event handlers ---- */

static void on_nickname_accepted(void *data, struct im_chat_manager_v1 *chat,
				 const char *nickname) {
	(void) chat;
	struct im_app *app = data;
	app->has_nickname = 1;
	printf("--\tnickname set to: %s\n", nickname);
}

static void on_nickname_rejected(void *data, struct im_chat_manager_v1 *chat,
				 const char *reason) {
	(void) data;
	(void) chat;
	printf("--\tnickname rejected: %s\n", reason);
	printf("--\tuse /nick <name> to try again\n");
}

static void on_message(void *data, struct im_chat_manager_v1 *chat,
		       const char *nickname, const char *content) {
	(void) data;
	(void) chat;
	printf("%s\t%s\n", nickname, content);
}

static void on_user_joined(void *data, struct im_chat_manager_v1 *chat,
			   const char *nickname) {
	(void) data;
	(void) chat;
	printf("--\t%s joined the room\n", nickname);
}

static void on_user_left(void *data, struct im_chat_manager_v1 *chat,
			 const char *nickname) {
	(void) data;
	(void) chat;
	printf("--\t%s left the room\n", nickname);
}

static const struct im_chat_manager_v1_listener chat_listener = {
	.nickname_accepted = on_nickname_accepted,
	.nickname_rejected = on_nickname_rejected,
	.message = on_message,
	.user_joined = on_user_joined,
	.user_left = on_user_left,
};

/* ---- registry ---- */

static void registry_global(void *data, struct wl_registry *registry,
			    uint32_t name, const char *interface,
			    uint32_t version) {
	struct im_app *app = data;

	if (strcmp(interface, im_chat_manager_v1_interface.name) == 0) {
		app->chat = wl_registry_bind(registry, name,
					     &im_chat_manager_v1_interface,
					     version < 1 ? version : 1);
		im_chat_manager_v1_add_listener(app->chat, &chat_listener, app);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
				   uint32_t name) {
	(void) data;
	(void) registry;
	(void) name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---- input processing ---- */

static void process_input(struct im_app *app, const char *line) {
	/* strip trailing newline */
	size_t len = strlen(line);
	char buf[MAX_INPUT_LEN];
	if (len == 0)
		return;
	if (len >= MAX_INPUT_LEN)
		len = MAX_INPUT_LEN - 1;
	memcpy(buf, line, len);
	if (buf[len - 1] == '\n')
		buf[len - 1] = '\0';
	else
		buf[len] = '\0';

	if (strlen(buf) == 0)
		return;

	/* commands */
	if (strncmp(buf, "/nick ", 6) == 0) {
		const char *nick = buf + 6;
		while (*nick == ' ')
			nick++;
		if (*nick != '\0') {
			im_chat_manager_v1_set_nickname(app->chat, nick);
		}
	} else if (strcmp(buf, "/quit") == 0) {
		app->running = 0;
	} else if (buf[0] == '/') {
		printf("--\tunknown command. available: /nick <name>, /quit\n");
	} else {
		if (!app->has_nickname) {
			printf("--\tset a nickname first: /nick <name>\n");
			return;
		}
		im_chat_manager_v1_send_message(app->chat, buf);
	}

	wl_display_flush(app->display);
}

/* ---- main ---- */

static void print_usage(void) {
	printf("=== im-wl-chat client ===\n");
	printf("Commands:\n");
	printf("  /nick <name>  - set your nickname\n");
	printf("  /quit         - exit\n");
	printf("  <text>        - send a message\n");
	printf("=========================\n");
}

static struct termios orig_termios;

static void restore_echo(void) {
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void disable_echo(void) {
	tcgetattr(STDIN_FILENO, &orig_termios);
	struct termios raw = orig_termios;
	raw.c_lflag &= ~ECHO;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	atexit(restore_echo);
}

int main(void) {
	struct im_app app = {0};
	app.running = 1;

	setenv("WAYLAND_DISPLAY", SOCKET_NAME, 0);

	app.display = wl_display_connect(NULL);
	if (app.display == NULL) {
		fprintf(stderr, "failed to connect to wayland display '%s'\n",
			SOCKET_NAME);
		return 0;
	}

	struct wl_registry *registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(registry, &registry_listener, &app);
	wl_display_roundtrip(app.display);

	if (app.chat == NULL) {
		fprintf(stderr, "server does not support im_chat_manager_v1\n");
		goto out;
	}

	print_usage();

	// disable stdin echo so typed input doesn't mix with incoming messages
	// disable_echo();

	int wl_fd = wl_display_get_fd(app.display);
	struct pollfd fds[2];
	fds[0].fd = STDIN_FILENO;
	fds[0].events = POLLIN;
	fds[1].fd = wl_fd;
	fds[1].events = POLLIN;

	char input_buf[MAX_INPUT_LEN];

	while (app.running) {
		/* flush pending requests before polling */
		while (wl_display_prepare_read(app.display) != 0)
			wl_display_dispatch_pending(app.display);
		wl_display_flush(app.display);

		int ret = poll(fds, 2, -1);
		if (ret < 0) {
			wl_display_cancel_read(app.display);
			break;
		}

		/* handle wayland events */
		if (fds[1].revents & POLLIN) {
			wl_display_read_events(app.display);
			wl_display_dispatch_pending(app.display);
		} else {
			wl_display_cancel_read(app.display);
		}

		if (fds[1].revents & (POLLERR | POLLHUP)) {
			printf("--\tdisconnected from server\n");
			break;
		}

		/* handle stdin */
		if (fds[0].revents & POLLIN) {
			if (fgets(input_buf, sizeof(input_buf), stdin) ==
			    NULL) {
				app.running = 0;
				break;
			}
			process_input(&app, input_buf);
		}
	}

	im_chat_manager_v1_destroy(app.chat);
	wl_display_roundtrip(app.display);
out:
	wl_registry_destroy(registry);
	wl_display_disconnect(app.display);
	return 0;
}

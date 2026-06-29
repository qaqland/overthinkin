#include <pthread.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STR_SIZE 256

// event queue

struct event {
	struct event *next;
	int facility;
	int type;
	uint32_t idx;
	struct timespec ts;
};

static pa_threaded_mainloop *mainloop;
static pa_context *context;
static bool ready;
static bool failed;
static char errmsg[STR_SIZE];

static struct event *ev_head;
static struct event *ev_tail;
static pthread_mutex_t ev_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;

static volatile sig_atomic_t quit;

// helpers

static void set_error(const char *msg) {
	snprintf(errmsg, STR_SIZE, "%s", msg);
}

static void set_str(char *dst, const char *src) {
	snprintf(dst, STR_SIZE, "%s", src ? src : "");
}

// PA callbacks

static void state_cb(pa_context *ctx, void *userdata) {
	(void) userdata;
	switch (pa_context_get_state(ctx)) {
	case PA_CONTEXT_READY:
		ready = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		break;
	case PA_CONTEXT_FAILED:
		failed = true;
		set_error(pa_strerror(pa_context_errno(ctx)));
		pa_threaded_mainloop_signal(mainloop, 0);
		break;
	default:
		break;
	}
}

static void sub_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx,
		   void *userdata) {
	(void) c;
	(void) userdata;
	struct event *ev = calloc(1, sizeof(*ev));
	if (!ev)
		return;
	ev->facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	ev->type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
	ev->idx = idx;
	clock_gettime(CLOCK_REALTIME, &ev->ts);

	pthread_mutex_lock(&ev_mtx);
	if (ev_tail)
		ev_tail->next = ev;
	else
		ev_head = ev;
	ev_tail = ev;
	pthread_cond_signal(&ev_cond);
	pthread_mutex_unlock(&ev_mtx);
}

// sync callback for subscribe / control ops

struct sync {
	bool done;
	bool success;
};

static void sync_cb(pa_context *c, int success, void *userdata) {
	(void) c;
	struct sync *s = userdata;
	s->success = success;
	s->done = true;
	pa_threaded_mainloop_signal(mainloop, 0);
}

// query: sink

struct sink_data {
	char name[STR_SIZE];
	char desc[STR_SIZE];
	char port[STR_SIZE];
	pa_volume_t volume;
	int mute;
	pa_sink_state_t state;
};

struct sink_query {
	struct sync s;
	struct sink_data d;
};

static void sk_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud) {
	(void) c;
	struct sink_query *q = ud;
	if (eol < 0) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (eol > 0) {
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (!i)
		return;
	q->s.success = true;
	set_str(q->d.name, i->name);
	set_str(q->d.desc, i->description);
	q->d.volume = pa_cvolume_avg(&i->volume);
	q->d.mute = i->mute;
	q->d.state = i->state;
	set_str(q->d.port, i->active_port ? i->active_port->name : NULL);
}

static bool query_sink(uint32_t idx, struct sink_data *d) {
	struct sink_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op =
		pa_context_get_sink_info_by_index(context, idx, sk_cb, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// query: source

struct source_data {
	char name[STR_SIZE];
	char desc[STR_SIZE];
	char port[STR_SIZE];
	pa_volume_t volume;
	int mute;
	pa_source_state_t state;
};

struct source_query {
	struct sync s;
	struct source_data d;
};

static void source_cb(pa_context *c, const pa_source_info *i, int eol,
		      void *ud) {
	(void) c;
	struct source_query *q = ud;
	if (eol < 0) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (eol > 0) {
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (!i)
		return;
	q->s.success = true;
	set_str(q->d.name, i->name);
	set_str(q->d.desc, i->description);
	q->d.volume = pa_cvolume_avg(&i->volume);
	q->d.mute = i->mute;
	q->d.state = i->state;
	set_str(q->d.port, i->active_port ? i->active_port->name : NULL);
}

static bool query_source(uint32_t idx, struct source_data *d) {
	struct source_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op = pa_context_get_source_info_by_index(context, idx,
							       source_cb, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// query: sink input

struct si_data {
	char name[STR_SIZE];
	uint32_t sink;
	uint32_t client;
	pa_volume_t volume;
	int mute;
	int corked;
};

struct si_query {
	struct sync s;
	struct si_data d;
};

static void si_cb(pa_context *c, const pa_sink_input_info *i, int eol,
		  void *ud) {
	(void) c;
	struct si_query *q = ud;
	if (eol < 0) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (eol > 0) {
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (!i)
		return;
	q->s.success = true;
	set_str(q->d.name, i->name);
	q->d.sink = i->sink;
	q->d.client = i->client;
	q->d.volume = pa_cvolume_avg(&i->volume);
	q->d.mute = i->mute;
	q->d.corked = i->corked;
}

static bool query_si(uint32_t idx, struct si_data *d) {
	struct si_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op =
		pa_context_get_sink_input_info(context, idx, si_cb, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// query: source output

struct so_data {
	char name[STR_SIZE];
	uint32_t source;
	uint32_t client;
	pa_volume_t volume;
	int mute;
	int corked;
};

struct so_query {
	struct sync s;
	struct so_data d;
};

static void so_cb(pa_context *c, const pa_source_output_info *i, int eol,
		  void *ud) {
	(void) c;
	struct so_query *q = ud;
	if (eol < 0) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (eol > 0) {
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (!i)
		return;
	q->s.success = true;
	set_str(q->d.name, i->name);
	q->d.source = i->source;
	q->d.client = i->client;
	q->d.volume = pa_cvolume_avg(&i->volume);
	q->d.mute = i->mute;
	q->d.corked = i->corked;
}

static bool query_so(uint32_t idx, struct so_data *d) {
	struct so_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op =
		pa_context_get_source_output_info(context, idx, so_cb, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// query: card

struct card_data {
	char name[STR_SIZE];
	char profile[STR_SIZE];
	uint32_t n_profiles;
};

struct card_query {
	struct sync s;
	struct card_data d;
};

static void card_cb(pa_context *c, const pa_card_info *i, int eol, void *ud) {
	(void) c;
	struct card_query *q = ud;
	if (eol < 0) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (eol > 0) {
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (!i)
		return;
	q->s.success = true;
	set_str(q->d.name, i->name);
	set_str(q->d.profile,
		i->active_profile2 ? i->active_profile2->name : NULL);
	uint32_t n = 0;
	if (i->profiles2) {
		for (pa_card_profile_info2 **p = i->profiles2; *p; p++) {
			if ((*p)->available == 0)
				continue;
			const char *name = (*p)->name;
			if (strcmp(name, "off") == 0 ||
			    strcmp(name, "pro-audio") == 0)
				continue;
			n++;
		}
	}
	q->d.n_profiles = n;
}

static bool query_card(uint32_t idx, struct card_data *d) {
	struct card_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op =
		pa_context_get_card_info_by_index(context, idx, card_cb, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// query: server

struct server_data {
	char default_sink[STR_SIZE];
	char default_source[STR_SIZE];
};

struct server_query {
	struct sync s;
	struct server_data d;
};

static void server_cb2(pa_context *c, const pa_server_info *i, void *ud) {
	(void) c;
	struct server_query *q = ud;
	if (!i) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	q->s.success = true;
	set_str(q->d.default_sink, i->default_sink_name);
	set_str(q->d.default_source, i->default_source_name);
	q->s.done = true;
	pa_threaded_mainloop_signal(mainloop, 0);
}

static bool query_server(struct server_data *d) {
	struct server_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op = pa_context_get_server_info(context, server_cb2, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// query: client

struct client_data {
	char name[STR_SIZE];
};

struct client_query {
	struct sync s;
	struct client_data d;
};

static void client_cb(pa_context *c, const pa_client_info *i, int eol,
		      void *ud) {
	(void) c;
	struct client_query *q = ud;
	if (eol < 0) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (eol > 0) {
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (!i)
		return;
	q->s.success = true;
	set_str(q->d.name, i->name);
}

static bool query_client(uint32_t idx, struct client_data *d) {
	struct client_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op =
		pa_context_get_client_info(context, idx, client_cb, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// query: module

struct module_data {
	char name[STR_SIZE];
	char argument[STR_SIZE];
};

struct module_query {
	struct sync s;
	struct module_data d;
};

static void module_cb(pa_context *c, const pa_module_info *i, int eol,
		      void *ud) {
	(void) c;
	struct module_query *q = ud;
	if (eol < 0) {
		q->s.success = false;
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (eol > 0) {
		q->s.done = true;
		pa_threaded_mainloop_signal(mainloop, 0);
		return;
	}
	if (!i)
		return;
	q->s.success = true;
	set_str(q->d.name, i->name);
	set_str(q->d.argument, i->argument);
}

static bool query_module(uint32_t idx, struct module_data *d) {
	struct module_query q = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op =
		pa_context_get_module_info(context, idx, module_cb, &q);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		return false;
	}
	while (!q.s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);
	if (q.s.success)
		*d = q.d;
	return q.s.success;
}

// output

static const char *facility_str(int f) {
	switch (f) {
	case PA_SUBSCRIPTION_EVENT_SINK:
		return "sink";
	case PA_SUBSCRIPTION_EVENT_SOURCE:
		return "source";
	case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
		return "sink-input";
	case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
		return "source-output";
	case PA_SUBSCRIPTION_EVENT_MODULE:
		return "module";
	case PA_SUBSCRIPTION_EVENT_CLIENT:
		return "client";
	case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE:
		return "sample-cache";
	case PA_SUBSCRIPTION_EVENT_SERVER:
		return "server";
	case PA_SUBSCRIPTION_EVENT_CARD:
		return "card";
	default:
		return "?";
	}
}

static const char *type_str(int t) {
	switch (t) {
	case PA_SUBSCRIPTION_EVENT_NEW:
		return "new";
	case PA_SUBSCRIPTION_EVENT_CHANGE:
		return "change";
	case PA_SUBSCRIPTION_EVENT_REMOVE:
		return "remove";
	default:
		return "?";
	}
}

static const char *sink_state_name(pa_sink_state_t s) {
	if (s == PA_SINK_RUNNING)
		return "RUNNING";
	if (s == PA_SINK_IDLE)
		return "IDLE";
	if (s == PA_SINK_SUSPENDED)
		return "SUSPENDED";
	return "?";
}

static const char *source_state_name(pa_source_state_t s) {
	if (s == PA_SOURCE_RUNNING)
		return "RUNNING";
	if (s == PA_SOURCE_IDLE)
		return "IDLE";
	if (s == PA_SOURCE_SUSPENDED)
		return "SUSPENDED";
	return "?";
}

static int vol_pct(pa_volume_t v) {
	return (int) (v * 100.0 / PA_VOLUME_NORM + 0.5);
}

static void print_time(const struct timespec *ts) {
	struct tm lt;
	localtime_r(&ts->tv_sec, &lt);
	printf("[%02d:%02d:%02d.%03ld]", lt.tm_hour, lt.tm_min, lt.tm_sec,
	       ts->tv_nsec / 1000000);
}

static void print_idx(uint32_t idx) {
	if (idx == PA_INVALID_INDEX)
		printf("#-    ");
	else
		printf("#%-5u", idx);
}

static void process_event(struct event *ev) {
	int fac = ev->facility;
	int typ = ev->type;
	uint32_t idx = ev->idx;

	print_time(&ev->ts);
	printf(" %-6s %-13s ", type_str(typ), facility_str(fac));
	print_idx(idx);

	if (typ == PA_SUBSCRIPTION_EVENT_REMOVE) {
		printf(" (deleted)\n");
		fflush(stdout);
		return;
	}

	switch (fac) {
	case PA_SUBSCRIPTION_EVENT_SINK: {
		struct sink_data d;
		if (query_sink(idx, &d)) {
			printf(" [%s]", d.desc);
			printf(" %3d%% %s", vol_pct(d.volume),
			       d.mute ? "  muted" : "unmuted");
			if (d.port[0])
				printf(" port:%s", d.port);
			printf(" state:%s", sink_state_name(d.state));
		} else {
			printf(" (deleted)");
		}
		break;
	}
	case PA_SUBSCRIPTION_EVENT_SOURCE: {
		struct source_data d;
		if (query_source(idx, &d)) {
			printf(" [%s]", d.desc);
			if (d.port[0])
				printf(" port:%s", d.port);
			printf(" %3d%% %s state:%s", vol_pct(d.volume),
			       d.mute ? "  muted" : "unmuted",
			       source_state_name(d.state));
		} else {
			printf(" (deleted)");
		}
		break;
	}
	case PA_SUBSCRIPTION_EVENT_SINK_INPUT: {
		struct si_data d;
		if (query_si(idx, &d)) {
			printf(" [%s]", d.name);
			printf(" %3d%% %s %s", vol_pct(d.volume),
			       d.mute ? "  muted" : "unmuted",
			       d.corked ? "  corked" : "uncorked");
			printf(" client");
			print_idx(d.client);
			printf(" sink");
			print_idx(d.sink);
		} else {
			printf(" (deleted)");
		}
		break;
	}
	case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT: {
		struct so_data d;
		if (query_so(idx, &d)) {
			printf(" [%s]", d.name);
			printf(" %3d%% %s %s", vol_pct(d.volume),
			       d.mute ? "  muted" : "unmuted",
			       d.corked ? "  corked" : "uncorked");
			printf(" client");
			print_idx(d.client);
			printf(" source");
			print_idx(d.source);
		} else {
			printf(" (deleted)");
		}
		break;
	}
	case PA_SUBSCRIPTION_EVENT_CARD: {
		struct card_data d;
		if (query_card(idx, &d)) {
			printf(" [%s]", d.name);
			if (d.profile[0])
				printf(" profile:%s (%u)", d.profile,
				       d.n_profiles);
		} else {
			printf(" (deleted)");
		}
		break;
	}
	case PA_SUBSCRIPTION_EVENT_SERVER: {
		struct server_data d;
		if (query_server(&d)) {
			printf(" (default)");
			printf(" sink:%s", d.default_sink);
			printf(" source:%s", d.default_source);
		} else {
			printf(" (query failed)");
		}
		break;
	}
	case PA_SUBSCRIPTION_EVENT_CLIENT: {
		struct client_data d;
		if (query_client(idx, &d)) {
			printf(" [%s]", d.name);
		} else {
			printf(" (deleted)");
		}
		break;
	}
	case PA_SUBSCRIPTION_EVENT_MODULE: {
		struct module_data d;
		if (query_module(idx, &d)) {
			printf(" [%s]", d.name);
			if (d.argument[0])
				printf(" %s", d.argument);
		} else {
			printf(" (deleted)");
		}
		break;
	}
	default:
		break;
	}

	printf("\n");
	fflush(stdout);
}

// signal handler

static void sighandler(int signo) {
	(void) signo;
	quit = 1;
	pthread_cond_signal(&ev_cond);
}

// main

int main(void) {
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	mainloop = pa_threaded_mainloop_new();
	if (!mainloop) {
		fprintf(stderr, "error: failed to create mainloop\n");
		return 1;
	}

	context = pa_context_new(pa_threaded_mainloop_get_api(mainloop),
				 "pulse-monitor");
	if (!context) {
		fprintf(stderr, "error: failed to create context\n");
		return 1;
	}

	pa_context_set_state_callback(context, state_cb, NULL);

	if (pa_context_connect(context, NULL, PA_CONTEXT_NOFAIL, NULL) < 0) {
		fprintf(stderr, "error: connect: %s\n",
			pa_strerror(pa_context_errno(context)));
		return 1;
	}

	if (pa_threaded_mainloop_start(mainloop) < 0) {
		fprintf(stderr, "error: failed to start mainloop\n");
		return 1;
	}

	pa_threaded_mainloop_lock(mainloop);
	while (!ready && !failed)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);

	if (failed) {
		fprintf(stderr, "error: %s\n", errmsg);
		return 1;
	}

	pa_context_set_subscribe_callback(context, sub_cb, NULL);

	struct sync sub_s = {0};
	pa_threaded_mainloop_lock(mainloop);
	pa_operation *op = pa_context_subscribe(
		context, PA_SUBSCRIPTION_MASK_ALL, sync_cb, &sub_s);
	if (!op) {
		pa_threaded_mainloop_unlock(mainloop);
		fprintf(stderr, "error: subscribe failed\n");
		return 1;
	}
	while (!sub_s.done)
		pa_threaded_mainloop_wait(mainloop);
	pa_threaded_mainloop_unlock(mainloop);
	pa_operation_unref(op);

	if (!sub_s.success) {
		fprintf(stderr, "error: subscribe rejected\n");
		return 1;
	}

	fprintf(stderr, "PulseAudio event monitor, more details than pactl "
			"subscribe. ^C to quit.\n");

	while (!quit) {
		pthread_mutex_lock(&ev_mtx);
		while (!ev_head && !quit)
			pthread_cond_wait(&ev_cond, &ev_mtx);
		if (quit) {
			pthread_mutex_unlock(&ev_mtx);
			break;
		}
		struct event *ev = ev_head;
		ev_head = ev->next;
		if (!ev_head)
			ev_tail = NULL;
		pthread_mutex_unlock(&ev_mtx);

		ev->next = NULL;
		process_event(ev);
		free(ev);
	}

	pa_threaded_mainloop_lock(mainloop);
	pa_context_disconnect(context);
	pa_context_unref(context);
	pa_threaded_mainloop_unlock(mainloop);
	pa_threaded_mainloop_stop(mainloop);
	pa_threaded_mainloop_free(mainloop);

	// free any remaining events
	pthread_mutex_lock(&ev_mtx);
	struct event *ev = ev_head;
	while (ev) {
		struct event *next = ev->next;
		free(ev);
		ev = next;
	}
	pthread_mutex_unlock(&ev_mtx);

	fprintf(stderr, "stopped\n");
	return 0;
}

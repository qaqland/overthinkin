/* Jack plug/unplug monitoring follows the bind_ctl machinery in
 * pipewire's spa/plugins/alsa/alsa-pcm.c (functions open_card_ctl,
 * bind_ctls_for_params, fetch_bind_ctls, bind_ctl_event).
 *
 * Deviation: PipeWire gets the bound control names from the
 * api.alsa.bind-ctls property; we auto-discover controls whose name
 * contains "Jack" instead, and restrict to boolean controls (the value
 * display reads a boolean).  The binding and event matching themselves
 * are the PipeWire code path.
 */

#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "app-common.h"

#define MAX_BIND_CTLS 16
#define JACK_NAME_WIDTH_MAX 40

/* mirrors struct bound_ctl */
struct bound_ctl {
	char name[64];
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
	bool prev_plugged;
};

static void print_time(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	printf("%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec,
	       ts.tv_nsec / 1000000);
}

static int jack_name_width(const struct bound_ctl *entries, size_t n) {
	int width = 0;
	for (size_t i = 0; i < n; i++) {
		int len = (int) strlen(entries[i].name);
		if (len > width)
			width = len;
	}
	return width > JACK_NAME_WIDTH_MAX ? JACK_NAME_WIDTH_MAX : width;
}

/* elem_list scan + per-control binding, fetch_bind_ctls;
 * the "Jack" name filter stands in for the api.alsa.bind-ctls list */
static int fetch_jack_ctls(snd_ctl_t *ctl, struct bound_ctl *entries,
			   size_t max) {
	snd_ctl_elem_list_t *list;
	snd_ctl_elem_list_alloca(&list);
	int err;

	CHECK(snd_ctl_elem_list(ctl, list), "snd_ctl_elem_list");

	unsigned int total = snd_ctl_elem_list_get_count(list);

	CHECK(snd_ctl_elem_list_alloc_space(list, total), "alloc_space");

	CHECK(snd_ctl_elem_list(ctl, list), "snd_ctl_elem_list (2nd)");

	size_t n = 0;
	bool truncated = false;
	for (unsigned int i = 0; i < total; i++) {
		const char *name = snd_ctl_elem_list_get_name(list, i);
		if (!name || !strstr(name, "Jack"))
			continue;

		unsigned int numid = snd_ctl_elem_list_get_numid(list, i);

		snd_ctl_elem_info_t *info;
		snd_ctl_elem_info_malloc(&info);
		snd_ctl_elem_info_set_numid(info, numid);

		err = snd_ctl_elem_info(ctl, info);
		if (err < 0) {
			log_warn("Could not inspect jack control '%s' (ALSA "
				 "control "
				 "%u): %s",
				 name, numid, snd_strerror(err));
			snd_ctl_elem_info_free(info);
			continue;
		}

		/* deviation: only boolean Jack controls are monitored */
		if (snd_ctl_elem_info_get_type(info) !=
		    SND_CTL_ELEM_TYPE_BOOLEAN) {
			snd_ctl_elem_info_free(info);
			continue;
		}
		if (n >= max) {
			truncated = true;
			snd_ctl_elem_info_free(info);
			continue;
		}

		snd_ctl_elem_value_t *value;
		snd_ctl_elem_value_malloc(&value);
		snd_ctl_elem_value_set_numid(value, numid);

		entries[n].info = info;
		entries[n].value = value;
		snprintf(entries[n].name, sizeof(entries[n].name), "%s", name);
		log_debug("Monitoring '%s'; ALSA control %u", name, numid);
		n++;
	}

	snd_ctl_elem_list_free_space(list);
	if (truncated)
		log_warn("More than %zu boolean jack controls were found; "
			 "monitoring the first %zu",
			 max, max);
	return (int) n;
}

static int print_jack_baseline(snd_ctl_t *ctl, struct bound_ctl *entries,
			       size_t n) {
	int width = jack_name_width(entries, n);
	for (size_t i = 0; i < n; i++) {
		int err = snd_ctl_elem_read(ctl, entries[i].value);
		if (err < 0) {
			log_error("Could not read jack control '%s': %s",
				  entries[i].name, snd_strerror(err));
			return err;
		}
		bool val = snd_ctl_elem_value_get_boolean(entries[i].value, 0);
		entries[i].prev_plugged = val;
		print_time();
		printf("%-*s  %s\n", width, entries[i].name,
		       val ? "plugged" : "unplugged");
	}
	return 0;
}

/* bind_ctl_event: read events, match by elem id,
 * re-read the bound value and compare */
static int handle_jack_events(snd_ctl_t *ctl, struct bound_ctl *entries,
			      size_t n_entries) {
	snd_ctl_event_t *ev;
	snd_ctl_elem_id_t *id, *bound_id;
	snd_ctl_elem_value_t *old_value;
	int err, read_res;

	snd_ctl_event_alloca(&ev);
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_alloca(&bound_id);
	snd_ctl_elem_value_alloca(&old_value);
	int width = jack_name_width(entries, n_entries);

	while ((read_res = snd_ctl_read(ctl, ev)) > 0) {
		if (snd_ctl_event_get_type(ev) != SND_CTL_EVENT_ELEM)
			continue;

		snd_ctl_event_elem_get_id(ev, id);

		for (size_t i = 0; i < n_entries; i++) {
			if (entries[i].value == NULL || entries[i].info == NULL)
				continue;

			/* match against the bound element */
			snd_ctl_elem_value_get_id(entries[i].value, bound_id);
			if (snd_ctl_elem_id_compare_set(id, bound_id) ||
			    snd_ctl_elem_id_compare_numid(id, bound_id))
				continue;

			snd_ctl_elem_value_copy(old_value, entries[i].value);
			err = snd_ctl_elem_read(ctl, entries[i].value);
			if (err < 0) {
				log_error("Could not refresh jack control "
					  "'%s': %s",
					  entries[i].name, snd_strerror(err));
				return err;
			}

			if (snd_ctl_elem_value_compare(old_value,
						       entries[i].value) != 0) {
				bool now = snd_ctl_elem_value_get_boolean(
					entries[i].value, 0);
				print_time();
				printf("%-*s  %s -> %s\n", width,
				       entries[i].name,
				       entries[i].prev_plugged ? "plugged"
							       : "unplugged",
				       now ? "plugged" : "unplugged");
				entries[i].prev_plugged = now;
			}
		}
	}

	if (read_res < 0 && read_res != -EAGAIN) {
		log_error("Could not read a jack control event: %s",
			  snd_strerror(read_res));
		return read_res;
	}
	return 0;
}

static int run_jack_event_loop(snd_ctl_t *ctl, struct bound_ctl *entries,
			       size_t n_entries, struct pollfd *pfds,
			       int nfds) {
	while (!stop_requested()) {
		int r = poll(pfds, (nfds_t) nfds, 500);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			int err = -errno;
			log_error("Jack control polling failed: %s",
				  strerror(errno));
			return err;
		}
		if (r == 0)
			continue;

		unsigned short revents;
		int err = snd_ctl_poll_descriptors_revents(
			ctl, pfds, (unsigned int) nfds, &revents);
		if (err < 0) {
			log_error("Could not decode jack control events: %s",
				  snd_strerror(err));
			return err;
		}
		if (!revents) {
			/* bind_ctl_event */
			log_trace("Control poll woke without a readable ALSA "
				  "event");
			continue;
		}
		if (!(revents & (POLLIN | POLLPRI)))
			continue;

		err = handle_jack_events(ctl, entries, n_entries);
		if (err < 0)
			return err;
	}

	return 0;
}

static void free_bound_ctls(struct bound_ctl *entries, size_t n_entries) {
	for (size_t i = 0; i < n_entries; i++) {
		if (entries[i].info)
			snd_ctl_elem_info_free(entries[i].info);
		if (entries[i].value)
			snd_ctl_elem_value_free(entries[i].value);
	}
}

static void jack_usage(const char *prog) {
	usage_header(prog, "[OPTION]...");
	usage_opt("-c CARD", "card number, device 'hw:N' (required)");
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

static int jack_run(int argc, char **argv) {
	int card = -1; /* -c is required */
	int verbose = 0;
	int opt;

	while ((opt = getopt(argc, argv, CARD_OPTSTRING COMMON_OPTSTRING)) !=
	       -1) {
		switch (opt) {
		case 'c':
			card = parse_card(optarg);
			if (card < 0) {
				jack_usage(argv[0]);
				return 2;
			}
			break;
		case 'v':
			verbose++;
			break;
		case 'h':
			jack_usage(argv[0]);
			return 0;
		default:
			jack_usage(argv[0]);
			return 2;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		jack_usage(argv[0]);
		return 2;
	}
	if (card < 0) {
		fprintf(stderr, "missing required -c card argument\n");
		jack_usage(argv[0]);
		return 2;
	}

	log_set_verbose(verbose);

	char devname[32] = {0};
	snprintf(devname, sizeof(devname), "hw:%d", card);

	/* open_card_ctl */
	snd_ctl_t *ctl;
	int err;
	CHECK(snd_ctl_open(&ctl, devname, SND_CTL_NONBLOCK), "snd_ctl_open");

	/* bind_ctls_for_params */
	int nfds = snd_ctl_poll_descriptors_count(ctl);
	if (nfds <= 0) {
		log_error("The ALSA control device has no poll descriptors");
		snd_ctl_close(ctl);
		return 1;
	}
	struct pollfd *pfds = calloc((size_t) nfds, sizeof(*pfds));
	if (pfds == NULL) {
		log_error("Could not allocate jack poll descriptors");
		snd_ctl_close(ctl);
		return -ENOMEM;
	}
	err = snd_ctl_poll_descriptors(ctl, pfds, (unsigned int) nfds);
	if (err < 0) {
		log_error("Could not get jack poll descriptors: %s",
			  snd_strerror(err));
		free(pfds);
		snd_ctl_close(ctl);
		return err;
	}

	err = snd_ctl_subscribe_events(ctl, 1);
	if (err < 0) {
		log_error("Could not subscribe to jack events: %s",
			  snd_strerror(err));
		free(pfds);
		snd_ctl_close(ctl);
		return err;
	}

	struct bound_ctl entries[MAX_BIND_CTLS] = {0};
	int n_entries = fetch_jack_ctls(ctl, entries, MAX_BIND_CTLS);
	if (n_entries < 0) {
		err = n_entries;
		n_entries = 0;
		goto out;
	}
	if (n_entries == 0) {
		report_note("no Jack controls found on hw:%d", card);
		err = 0;
		goto out;
	}

	report_section("Jack Controls on Card %d", card);
	err = print_jack_baseline(ctl, entries, (size_t) n_entries);
	if (err < 0)
		goto out;
	printf("\nMonitoring changes; press Ctrl-C to stop.\n");
	log_info("Monitoring %d jack controls on card %d", n_entries, card);
	err = run_jack_event_loop(ctl, entries, (size_t) n_entries, pfds, nfds);
	if (err < 0)
		goto out;

out:
	free_bound_ctls(entries, n_entries);
	free(pfds);
	snd_ctl_close(ctl);
	return err < 0 ? err : 0;
}

static struct applet jack_applet = {
	.name = "jack",
	.desc = "monitor jack plug/unplug events",
	.main = jack_run,
	.usage = jack_usage,
	.next = NULL,
};

APPLET_REGISTER(jack_applet);

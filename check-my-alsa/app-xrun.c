/*
 * xrun applet: runs the pipewire playback path (alsa-pcm-sink.c driver)
 * and tallies callback intervals and xrun events. Xruns are detected the
 * way pipewire detects them: alsa_avail() goes negative, alsa_recover()
 * reads the status and accounts the missing frames from the trigger
 * timestamp; the xrun_cb hook fires where pipewire would call
 * spa_node_call_xrun().
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "alsa-pcm.h"
#include "app-common.h"

#define MAX_HISTOGRAM_BUCKETS 128

struct xrun_stats {
	uint64_t callback_count;
	uint64_t xrun_count;
	uint64_t xrun_frames;
	uint64_t sum_ns;
	uint64_t max_ns;
	uint64_t last_callback_ns;
	uint64_t histogram[MAX_HISTOGRAM_BUCKETS];
	uint64_t first_xrun_ns;
	uint64_t last_xrun_ns;
	uint64_t start_ns;
};

static void on_cycle(struct alsa_state *st, void *data) {
	struct xrun_stats *s = data;
	uint64_t now = now_ns();
	uint64_t last = s->last_callback_ns;
	s->last_callback_ns = now;
	if (last != 0) {
		uint64_t interval = now - last;
		s->callback_count++;
		s->sum_ns += interval;
		if (interval > s->max_ns)
			s->max_ns = interval;

		uint64_t expected_period_ns =
			(uint64_t) (1e9 * st->threshold / st->rate);
		uint64_t bucket = expected_period_ns > 0
					  ? (interval * 16) / expected_period_ns
					  : 0;
		if (bucket >= MAX_HISTOGRAM_BUCKETS)
			bucket = MAX_HISTOGRAM_BUCKETS - 1;
		s->histogram[bucket]++;
	}
}

static void on_xrun(struct alsa_state *st, uint64_t missing, void *data) {
	struct xrun_stats *s = data;
	uint64_t elapsed = now_ns() - s->start_ns;

	s->xrun_count++;
	s->xrun_frames += missing;
	if (s->xrun_count == 1)
		s->first_xrun_ns = elapsed;
	s->last_xrun_ns = elapsed;

	log_warn("XRUN %" PRIu64 " at +%.3f s: %" PRIu64
		 " frames lost; %" PRIu64 " total",
		 s->xrun_count, elapsed / 1e9, missing, s->xrun_frames);
	(void) st;
}

static void print_report(const struct xrun_stats *s, uint64_t test_duration_ns,
			 const struct pcm_setup *cfg,
			 const struct alsa_state *st, int runtime_error) {
	uint64_t cb = s->callback_count;
	uint64_t xr = s->xrun_count;
	uint64_t sum = s->sum_ns;
	uint64_t max_ns = s->max_ns;

	double avg_us = cb > 0 ? (double) sum / cb / 1000.0 : 0.0;
	double max_us = max_ns / 1000.0;
	double period_us = (1e9 * st->period_frames / st->rate) / 1000.0;

	report_section("XRUN Detection Summary");

	struct report_tab *t = report_tab_begin();
	report_kv(t, "Device", "%s", cfg->dev);
	report_kv(t, "Direction", "playback (PipeWire path)");
	report_kv(t, "Rate", "%u Hz", st->rate);
	report_kv(t, "Period size", "%lu frames", st->period_frames);
	report_kv(t, "Buffer size", "%lu frames", st->buffer_frames);
	report_kv(t, "Format", "%s", snd_pcm_format_name(st->format));
	report_kv(t, "Channels", "%u", st->channels);
	report_kv(t, "Duration", "%.3f s", test_duration_ns / 1e9);
	report_kv(t, "Termination", "%s",
		  runtime_error	     ? "runtime error"
		  : stop_requested() ? "interrupted"
				     : "duration reached");
	report_kv(t, "Theoretical period", "%.2f us", period_us);
	report_tab_end(t);

	printf("Callback statistics:\n");
	t = report_tab_begin();
	report_kv(t, "Total callbacks", "%" PRIu64, cb);
	report_kv(t, "Avg interval", "%.2f us", avg_us);
	report_kv(t, "Max interval", "%.2f us", max_us);
	report_tab_end(t);

	t = report_tab_begin();
	report_kv(t, "XRUN events", "%" PRIu64, xr);
	if (xr > 0) {
		report_kv(t, "XRUN frames", "%" PRIu64, s->xrun_frames);
		report_kv(t, "First XRUN at", "+%.3f s",
			  s->first_xrun_ns / 1e9);
		report_kv(t, "Last XRUN at", "+%.3f s", s->last_xrun_ns / 1e9);
	}
	report_tab_end(t);

	printf("Histogram (interval / theoretical period):\n");
	for (int i = 0; i < MAX_HISTOGRAM_BUCKETS; i++) {
		uint64_t v = s->histogram[i];
		if (v == 0)
			continue;
		double lo = i * period_us / 16.0;
		double hi = (i + 1) * period_us / 16.0;
		if (i == MAX_HISTOGRAM_BUCKETS - 1)
			printf("  [>= %6.1f us]       : %" PRIu64 "\n", lo, v);
		else
			printf("  [%6.1f - %6.1f) us: %" PRIu64 "\n", lo, hi,
			       v);
	}

	if (runtime_error)
		report_fail("XRUN monitoring did not complete");
	else if (xr > 0)
		report_warn("%" PRIu64 " XRUNs detected", xr);
	else
		report_ok("no XRUNs detected");
}

static void xrun_usage(const char *prog) {
	pcm_setup_usage(prog);
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

static int xrun_run(int argc, char **argv) {
	struct pcm_setup cfg;

	pcm_setup_defaults(&cfg);

	int verbose = 0;
	int opt;
	while ((opt = getopt(argc, argv, PCM_OPTSTRING COMMON_OPTSTRING)) !=
	       -1) {
		switch (opt) {
		case 'v':
			verbose++;
			break;
		case 'h':
			xrun_usage(argv[0]);
			return 0;
		default:
			if (!pcm_setup_parse_opt(&cfg, opt, optarg)) {
				xrun_usage(argv[0]);
				return 2;
			}
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		xrun_usage(argv[0]);
		return 2;
	}
	if (!pcm_setup_check(&cfg)) {
		xrun_usage(argv[0]);
		return 2;
	}

	log_set_verbose(verbose);

	struct alsa_state st;
	alsa_state_init(&st);
	if (pcm_sink_open(&st, &cfg) < 0)
		return 1;

	struct xrun_stats stats = {0};
	stats.start_ns = now_ns();
	st.cycle_cb = on_cycle;
	st.cycle_data = &stats;
	st.xrun_cb = on_xrun;
	st.xrun_data = &stats;

	if (pcm_sink_start(&st) < 0) {
		pcm_sink_stop(&st);
		return 1;
	}
	log_info("XRUN monitoring started: %s, %u channels, %u Hz",
		 snd_pcm_format_name(st.format), st.channels, st.rate);
	log_info("Period duration %.2f ms; buffer %lu frames",
		 (double) st.period_frames * 1000.0 / st.rate,
		 st.buffer_frames);

	uint64_t end_ns =
		cfg.duration_sec
			? now_ns() + (uint64_t) cfg.duration_sec * 1000000000ULL
			: UINT64_MAX;
	int runtime_error = 0;

	while (!stop_requested() && now_ns() < end_ns) {
		int remaining_ms =
			end_ns != UINT64_MAX
				? (int) ((end_ns - now_ns()) / 1000000)
				: 500;
		if (remaining_ms <= 0)
			break;

		int res = pcm_sink_iterate(&st, remaining_ms);
		if (res == -EINTR || res == -ETIMEDOUT) {
			if (end_ns == UINT64_MAX && !stop_requested())
				continue;
			break;
		}
		if (res < 0 && res != -EAGAIN) {
			log_error("XRUN monitoring stopped: %s",
				  snd_strerror(res));
			runtime_error = res;
			break;
		}
	}
	if (!runtime_error)
		log_info("XRUN monitoring stopped %s",
			 stop_requested() ? "after interruption"
					  : "after the requested duration");

	uint64_t end = now_ns();
	pcm_sink_stop(&st);

	print_report(&stats, end - stats.start_ns, &cfg, &st, runtime_error);

	return runtime_error || stats.xrun_count > 0 ? 1 : 0;
}

static struct applet xrun_applet = {
	.name = "xrun",
	.desc = "monitor XRUN (under/overrun) events",
	.main = xrun_run,
	.usage = xrun_usage,
	.next = NULL,
};

APPLET_REGISTER(xrun_applet);

/*
 * latency applet: runs the pipewire playback path (alsa-pcm-sink.c
 * driver) and measures clock drift between the wall clock and the
 * hardware clock, using the delay values from get_status() (pipewire's
 * get_avail path). The stream clock position is the number of frames the
 * DMA consumed: sample_count - delay (written minus in-flight), which is
 * how pipewire's clock position advances.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "alsa-pcm.h"
#include "app-common.h"

struct latency_stats {
	uint64_t sample_count;
	uint64_t wall_start_ns;
	uint64_t last_cycle_ns;
	uint64_t last_consumed;
	snd_pcm_sframes_t delay_first;
	snd_pcm_sframes_t delay_last;
	uint64_t polls;
	/* per-cycle delta drift averaged over all cycles (frames over the
	 * wall delta): immune to the start/stop transient offsets and to
	 * the write-ahead pacing */
	double drift_sum;
	double drift_ppm;
	bool got_first_delay;
};

static void on_cycle(struct alsa_state *st, void *data) {
	struct latency_stats *s = data;
	snd_pcm_sframes_t delay = (snd_pcm_sframes_t) st->last_delay;
	uint64_t now = now_ns();
	uint64_t consumed = st->sample_count - (uint64_t) delay;

	if (!s->got_first_delay) {
		s->delay_first = delay;
		s->got_first_delay = true;
	}
	s->delay_last = delay;
	s->sample_count = st->sample_count;
	s->polls++;

	if (s->polls > 1) {
		uint64_t d_consumed = consumed - s->last_consumed;
		uint64_t d_wall = now - s->last_cycle_ns;
		double expected = (double) d_wall * st->rate / 1e9;
		if (expected > 0)
			s->drift_sum +=
				(d_consumed - expected) / expected * 1e6;
		s->drift_ppm = s->drift_sum / (s->polls - 1);
	}

	s->last_consumed = consumed;
	s->last_cycle_ns = now;

	if ((s->polls % 100) == 0)
		log_trace("Clock sample %" PRIu64 "; position %" PRIu64
			  " frames; delay %ld; drift %+.2f ppm",
			  s->polls, s->last_consumed, delay, s->drift_ppm);
}

static void print_report(const struct latency_stats *s,
			 const struct pcm_setup *cfg,
			 const struct alsa_state *st, double elapsed_s,
			 int runtime_error) {
	double measurement_s =
		s->polls > 0 ? (s->last_cycle_ns - s->wall_start_ns) / 1e9
			     : 0.0;
	double wall_samples = measurement_s * st->rate;
	double consumed = (double) s->last_consumed;
	double drift_samples = consumed - wall_samples;
	double drift_ppm = s->drift_ppm;

	report_section("Playback Clock Drift Summary");

	struct report_tab *t = report_tab_begin();
	report_kv(t, "Device", "%s", cfg->dev);
	report_kv(t, "Direction", "playback (PipeWire path)");
	report_kv(t, "Rate", "%u Hz", st->rate);
	report_kv(t, "Period size", "%lu frames", st->period_frames);
	report_kv(t, "Buffer size", "%lu frames", st->buffer_frames);
	report_kv(t, "Format", "%s", snd_pcm_format_name(st->format));
	report_kv(t, "Channels", "%u", st->channels);
	report_tab_end(t);

	t = report_tab_begin();
	report_kv(t, "Duration", "%.3f s", elapsed_s);
	report_kv(t, "Measurement window", "%.3f s", measurement_s);
	report_kv(t, "Termination", "%s",
		  runtime_error	     ? "runtime error"
		  : stop_requested() ? "interrupted"
				     : "duration reached");
	report_kv(t, "Clock samples", "%" PRIu64, s->polls);
	report_kv(t, "Frames written", "%" PRIu64, s->sample_count);
	report_kv(t, "Clock position", "%.0f frames", consumed);
	report_kv(t, "Expected position", "%.0f frames", wall_samples);
	report_kv(t, "Drift samples", "%+.2f", drift_samples);
	report_kv(t, "Drift rate (avg over cycles)", "%.2f ppm", drift_ppm);
	report_tab_end(t);

	printf("Reported playback delay:\n");
	t = report_tab_begin();
	report_kv(t, "First", "%ld frames", s->delay_first);
	report_kv(t, "Last", "%ld frames", s->delay_last);
	report_tab_end(t);

	if (runtime_error)
		report_fail("playback clock measurement did not complete");
	else if (s->polls == 0)
		report_fail("no clock samples collected");
	else if (drift_ppm > 5000.0 || drift_ppm < -5000.0)
		report_warn("drift exceeds 5000 ppm, clock source "
			    "mismatch likely");
	else if (drift_ppm > 1000.0 || drift_ppm < -1000.0)
		report_note("moderate drift; position updates on this "
			    "driver may be quantized (HDA updates per period, "
			    "run longer to average out)");
	else
		report_ok("playback clock drift is within 1000 ppm");
}

static void latency_usage(const char *prog) {
	pcm_setup_usage(prog);
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

static int latency_run(int argc, char **argv) {
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
			latency_usage(argv[0]);
			return 0;
		default:
			if (!pcm_setup_parse_opt(&cfg, opt, optarg)) {
				latency_usage(argv[0]);
				return 2;
			}
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		latency_usage(argv[0]);
		return 2;
	}
	if (!pcm_setup_check(&cfg)) {
		latency_usage(argv[0]);
		return 2;
	}

	log_set_verbose(verbose);

	struct alsa_state st;
	alsa_state_init(&st);
	if (pcm_sink_open(&st, &cfg) < 0)
		return 1;

	struct latency_stats stats = {0};
	stats.wall_start_ns = now_ns();
	stats.last_cycle_ns = stats.wall_start_ns;
	st.cycle_cb = on_cycle;
	st.cycle_data = &stats;

	if (pcm_sink_start(&st) < 0) {
		pcm_sink_stop(&st);
		return 1;
	}
	log_info("Playback clock measurement started for %s",
		 cfg.duration_sec ? "the requested duration"
				  : "an unlimited duration");
	log_info("Using ALSA playback delay; %s, %u channels, %u Hz",
		 snd_pcm_format_name(st.format), st.channels, st.rate);

	uint64_t end_ns =
		cfg.duration_sec
			? stats.wall_start_ns +
				  (uint64_t) cfg.duration_sec * 1000000000ULL
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
			log_error("Clock measurement stopped: %s",
				  snd_strerror(res));
			runtime_error = res;
			break;
		}
	}
	if (!runtime_error)
		log_info("Clock measurement stopped %s",
			 stop_requested() ? "after interruption"
					  : "after the requested duration");

	double elapsed_s = (now_ns() - stats.wall_start_ns) / 1e9;
	pcm_sink_stop(&st);

	print_report(&stats, &cfg, &st, elapsed_s, runtime_error);
	return runtime_error || stats.polls == 0 ? 1 : 0;
}

static struct applet latency_applet = {
	.name = "latency",
	.desc = "measure playback clock drift and reported delay",
	.main = latency_run,
	.usage = latency_usage,
	.next = NULL,
};

APPLET_REGISTER(latency_applet);

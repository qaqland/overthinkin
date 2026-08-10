/*
 * play applet: plain playback smoke test on the pipewire path. Writes
 * silence for the given duration and reports cycle/sample counts, the
 * negotiated hw params and any xruns encountered on the way.
 */

#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#include "alsa-pcm.h"
#include "app-common.h"

struct play_stats {
	uint64_t cycles;
	uint64_t early_wakeups;
	uint64_t start_ns;
};

static void on_cycle(struct alsa_state *st, void *data) {
	struct play_stats *s = data;
	s->cycles++;

	if ((s->cycles % 100) == 0) {
		log_trace("Playback cycle %" PRIu64 "; %" PRIu64
			  " frames written; available %lu; delay %lu",
			  s->cycles, st->sample_count, st->last_avail,
			  st->last_delay);
	}
}

static void play_usage(const char *prog) {
	pcm_setup_usage(prog);
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

static int play_run(int argc, char **argv) {
	struct pcm_setup cfg;
	int verbose = 0;

	pcm_setup_defaults(&cfg);

	int opt;
	while ((opt = getopt(argc, argv, PCM_OPTSTRING COMMON_OPTSTRING)) !=
	       -1) {
		switch (opt) {
		case 'v':
			verbose++;
			break;
		case 'h':
			play_usage(argv[0]);
			return 0;
		default:
			if (!pcm_setup_parse_opt(&cfg, opt, optarg)) {
				play_usage(argv[0]);
				return 2;
			}
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		play_usage(argv[0]);
		return 2;
	}
	if (!pcm_setup_check(&cfg)) {
		play_usage(argv[0]);
		return 2;
	}

	log_set_verbose(verbose);

	struct alsa_state st;
	alsa_state_init(&st);
	if (pcm_sink_open(&st, &cfg) < 0)
		return 1;

	struct play_stats stats = {0};
	stats.start_ns = now_ns();
	st.cycle_cb = on_cycle;
	st.cycle_data = &stats;

	if (pcm_sink_start(&st) < 0) {
		pcm_sink_stop(&st);
		return 1;
	}
	log_info("Playback started: %s, %u channels, %u Hz",
		 snd_pcm_format_name(st.format), st.channels, st.rate);
	log_info("Using interleaved %s; period %lu frames; buffer %lu frames",
		 st.use_mmap ? "MMAP" : "read/write", st.period_frames,
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
		if (res == -EAGAIN) {
			stats.early_wakeups++;
			continue;
		}
		if (res < 0) {
			log_error("Playback loop stopped: %s",
				  snd_strerror(res));
			runtime_error = res;
			break;
		}
	}
	if (!runtime_error)
		log_info("Playback stopped %s",
			 stop_requested() ? "after interruption"
					  : "after the requested duration");

	pcm_sink_stop(&st);

	double elapsed = (now_ns() - stats.start_ns) / 1e9;
	report_section("Playback Smoke Test");

	struct report_tab *t = report_tab_begin();
	report_kv(t, "Device", "%s", cfg.dev);
	report_kv(t, "Rate", "%u Hz", st.rate);
	report_kv(t, "Period size", "%lu frames (%.2f ms)", st.period_frames,
		  (double) st.period_frames * 1000.0 / st.rate);
	report_kv(t, "Buffer size", "%lu frames", st.buffer_frames);
	report_kv(t, "Format", "%s", snd_pcm_format_name(st.format));
	report_kv(t, "Channels", "%u", st.channels);
	report_kv(t, "Duration", "%.3f s", elapsed);
	report_kv(t, "Termination", "%s",
		  runtime_error	     ? "runtime error"
		  : stop_requested() ? "interrupted"
				     : "duration reached");
	report_kv(t, "Cycles", "%" PRIu64, stats.cycles);
	report_kv(t, "Early wakeups", "%" PRIu64, stats.early_wakeups);
	report_kv(t, "Frames written", "%" PRIu64, st.sample_count);
	report_kv(t, "XRUN loss", "%" PRIu64 " frames", st.xrun);
	report_tab_end(t);

	if (runtime_error)
		report_fail("playback did not complete");
	else if (st.xrun > 0)
		report_warn("%" PRIu64 " frames were lost to XRUNs", st.xrun);
	else
		report_ok("playback completed without XRUNs");
	return runtime_error || st.xrun > 0 ? 1 : 0;
}

static struct applet play_applet = {
	.name = "play",
	.desc = "playback smoke test (PipeWire path)",
	.main = play_run,
	.usage = play_usage,
	.next = NULL,
};

APPLET_REGISTER(play_applet);

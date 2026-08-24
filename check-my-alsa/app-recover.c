/*
 * recover applet: induces playback XRUNs and exercises the alsa_recover()
 * path mirrored from pipewire's spa/plugins/alsa/alsa-pcm.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "alsa-pcm.h"
#include "app-common.h"

static const char *state_name(snd_pcm_state_t s) {
	switch (s) {
	case SND_PCM_STATE_OPEN:
		return "OPEN";
	case SND_PCM_STATE_SETUP:
		return "SETUP";
	case SND_PCM_STATE_PREPARED:
		return "PREPARED";
	case SND_PCM_STATE_RUNNING:
		return "RUNNING";
	case SND_PCM_STATE_XRUN:
		return "XRUN";
	case SND_PCM_STATE_DRAINING:
		return "DRAINING";
	case SND_PCM_STATE_PAUSED:
		return "PAUSED";
	case SND_PCM_STATE_SUSPENDED:
		return "SUSPENDED";
	case SND_PCM_STATE_DISCONNECTED:
		return "DISCONNECTED";
	default:
		return "?";
	}
}

struct recover_stats {
	int xruns_induced;
	int induction_failed;
	int recoveries_ok;
	int recoveries_failed;
	int returned_usable;
	snd_pcm_state_t last_state;
};

static void print_report(const struct recover_stats *s,
			 const struct pcm_setup *cfg,
			 const struct alsa_state *st) {
	report_section("XRUN Recovery Summary");

	struct report_tab *t = report_tab_begin();
	report_kv(t, "Device", "%s", cfg->dev);
	report_kv(t, "Direction", "playback");
	report_kv(t, "Rate", "%u Hz", cfg->rate);
	report_kv(t, "Period", "%lu frames", cfg->period);
	report_tab_end(t);

	t = report_tab_begin();
	report_kv(t, "XRUNs induced", "%d", s->xruns_induced);
	report_kv(t, "XRUN induction failures", "%d", s->induction_failed);
	report_kv(t, "Successful recoveries", "%d", s->recoveries_ok);
	report_kv(t, "Failed recoveries", "%d", s->recoveries_failed);
	report_kv(t, "Returned to usable state", "%d / %d", s->returned_usable,
		  s->xruns_induced);
	report_kv(t, "PipeWire-style XRUN estimate", "%llu frames",
		  (unsigned long long) st->xrun);
	report_kv(t, "Post-recovery avail failures", "%u", st->recover_fails);
	report_tab_end(t);

	t = report_tab_begin();
	report_kv(t, "Final state", "%s", state_name(s->last_state));
	report_tab_end(t);

	if (st->xrun > (uint64_t) cfg->duration_sec * cfg->rate * 2)
		report_warn("XRUN frame count is implausible, likely a "
			    "driver timestamp problem (PipeWire's clock->xrun "
			    "would be wrong too)");
	if (s->induction_failed > 0)
		report_fail("%d iterations did not induce an XRUN",
			    s->induction_failed);
	else if (s->recoveries_failed > 0)
		report_fail("some recoveries failed, driver may not recover "
			    "from an XRUN");
	else if (s->returned_usable == s->xruns_induced && s->xruns_induced > 0)
		report_ok("all induced XRUNs recovered to PREPARED or RUNNING");
}

static void recover_usage(const char *prog) {
	pcm_setup_usage(prog);
	usage_opt("-n ITERATIONS", "number of XRUN/recover iterations "
				   "(default 3)");
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

static int recover_run(int argc, char **argv) {
	struct pcm_setup cfg;

	pcm_setup_defaults(&cfg);
	cfg.period = 256; /* small quantum: xrun is easy to trigger */

	int iterations = 3;
	int verbose = 0;
	int opt, err;

	while ((opt = getopt(argc, argv,
			     PCM_OPTSTRING "n:" COMMON_OPTSTRING)) != -1) {
		switch (opt) {
		case 'n':
			iterations =
				(int) parse_long(optarg, "iterations", &err);
			if (err < 0 || iterations < 1) {
				fprintf(stderr,
					"invalid iterations argument '%s'\n",
					optarg);
				recover_usage(argv[0]);
				return 2;
			}
			break;
		case 'v':
			verbose++;
			break;
		case 'h':
			recover_usage(argv[0]);
			return 0;
		default:
			if (!pcm_setup_parse_opt(&cfg, opt, optarg)) {
				recover_usage(argv[0]);
				return 2;
			}
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		recover_usage(argv[0]);
		return 2;
	}
	if (!pcm_setup_check(&cfg)) {
		recover_usage(argv[0]);
		return 2;
	}

	struct alsa_state st;
	alsa_state_init(&st);
	log_set_verbose(verbose);
	if (pcm_sink_open(&st, &cfg) < 0)
		return 1;
	if (getenv("CMA_NEGOTIATED_THRESHOLD")) {
		st.threshold = st.period_frames;
		st.last_threshold = st.period_frames;
	}

	struct recover_stats stats = {0};

	/* pipewire runs spa_alsa_prepare() before the first graph cycle;
	 * this is what sets the sw params (tstamp enable). Without it the
	 * first iteration would hit the driver with tstamp_mode=NONE and
	 * snd_pcm_status() would return a zero tstamp (app-recover only) */
	CHECK(do_prepare(&st), "prepare");

	log_info("Recovery test started: %d attempt%s; period %lu frames; "
		 "rate %u Hz",
		 iterations, iterations == 1 ? "" : "s", st.period_frames,
		 st.rate);

	for (int i = 0; i < iterations && !stop_requested(); i++) {
		log_info("Recovery attempt %d of %d", i + 1, iterations);

		snd_pcm_state_t st_state = snd_pcm_state(st.hndl);
		log_debug("State before XRUN induction: %s",
			  state_name(st_state));

		if (st_state != SND_PCM_STATE_PREPARED &&
		    st_state != SND_PCM_STATE_RUNNING) {
			log_debug("Stream is not ready; preparing it again");
			CHECK(do_prepare(&st), "prepare");
			st_state = snd_pcm_state(st.hndl);
		}

		/* pipewire graph process cycle: write one quantum worth of
		 * frames, then stop refilling so the DMA underruns */
		err = spa_alsa_write(&st);
		if (err < 0) {
			log_warn("Could not write frames before XRUN "
				 "induction: %s",
				 snd_strerror(err));
			stats.recoveries_failed++;
			continue;
		}

		log_debug("Initial frames written; waiting for an XRUN");

		/*
		 * Detection matches pipewire: the xrun is only seen when
		 * the kernel is asked for the avail (snd_pcm_avail forces
		 * the hw pointer update); snd_pcm_state() alone never
		 * triggers it on a driver with period wakeups disabled.
		 * alsa_avail() returns -EPIPE once the DMA underruns.
		 */
		snd_pcm_state_t last = snd_pcm_state(st.hndl);
		int waited_ms = 0;
		int wait_limit = 10000;
		while (!stop_requested() && waited_ms < wait_limit) {
			usleep(10000);
			waited_ms += 10;
			snd_pcm_sframes_t av = alsa_avail(&st);
			st_state = snd_pcm_state(st.hndl);
			if (st_state != last) {
				log_trace("Stream state changed from %s to %s",
					  state_name(last),
					  state_name(st_state));
				last = st_state;
			}
			if (av < 0)
				break;
			if (st_state == SND_PCM_STATE_XRUN)
				break;
			if (st_state == SND_PCM_STATE_DRAINING ||
			    st_state == SND_PCM_STATE_SETUP)
				break;
		}

		if (st_state != SND_PCM_STATE_XRUN && alsa_avail(&st) >= 0) {
			log_warn("No XRUN occurred after %d ms; resetting the "
				 "stream",
				 waited_ms);
			stats.induction_failed++;
			/* pipewire does not drain, it drops */
			do_drop(&st);
			CHECK(do_prepare(&st), "prepare");
			continue;
		}

		stats.xruns_induced++;
		log_info("XRUN detected after %d ms; stream state %s",
			 waited_ms, state_name(st_state));

		/* pipewire alsa_recover: drop -> prepare -> start in one
		 * go, no delay in between */
		unsigned int fails_before = st.recover_fails;
		int rec = alsa_recover(&st);

		/* pipewire immediately verifies with get_avail; a failed
		 * recovery shows up here as "snd_pcm_avail after recover:
		 * Broken pipe" */
		snd_pcm_uframes_t delay;
		int av = get_avail(&st, now_ns(), &delay);

		st_state = snd_pcm_state(st.hndl);
		log_debug("State after recovery: %s", state_name(st_state));
		stats.last_state = st_state;

		if (rec < 0 || av < 0 || st.recover_fails > fails_before) {
			stats.recoveries_failed++;
			log_error("Recovery verification failed: %s",
				  snd_strerror(rec < 0	? rec
					       : av < 0 ? av
							: -EPIPE));
			continue;
		}
		if (st_state == SND_PCM_STATE_PREPARED ||
		    st_state == SND_PCM_STATE_RUNNING) {
			stats.recoveries_ok++;
			stats.returned_usable++;
			log_info("Recovery succeeded; the stream is %s",
				 state_name(st_state));
		} else {
			stats.recoveries_failed++;
			log_warn(
				"Recovery ended in unusable state %s; expected "
				"PREPARED or RUNNING",
				state_name(st_state));
		}
	}

	pcm_sink_stop(&st);

	print_report(&stats, &cfg, &st);

	return stats.induction_failed > 0 || stats.recoveries_failed > 0 ? 1
									 : 0;
}

static struct applet recover_applet = {
	.name = "recover",
	.desc = "test PCM XRUN recovery behaviour (PipeWire alsa_recover)",
	.main = recover_run,
	.usage = recover_usage,
	.next = NULL,
};

APPLET_REGISTER(recover_applet);

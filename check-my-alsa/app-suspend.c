/*
 * suspend applet: holds a playback stream open across a system
 * suspend/resume and exercises the recovery path PipeWire's process
 * thread runs on wake (alsa_avail -> alsa_recover, mirrored from
 * uos-pipewire spa/plugins/alsa/alsa-pcm.c). The stream stays RUNNING
 * through the sleep by default; -i drops it (SETUP) the way PipeWire's
 * SPA_NODE_COMMAND_Suspend -> spa_alsa_pause does, as a control.
 *
 * Test-side helpers, not part of the mirrored call sequence: suspend
 * detection via the CLOCK_BOOTTIME/CLOCK_MONOTONIC gap, the rtcwake
 * auto-trigger child, and the fresh reopen control probe.
 */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "alsa-pcm.h"
#include "app-common.h"

#define SUSPEND_GAP_THRESHOLD_NS 1000000000ULL
#define WARMUP_MS 500
#define VERIFY_MS 2000
#define DETECT_POLL_MS 100

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

/* CLOCK_BOOTTIME counts time spent in suspend, CLOCK_MONOTONIC does not;
 * the gap is the accumulated suspend time */
static uint64_t boottime_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

struct suspend_stats {
	bool idle_mode;
	bool suspend_detected;
	uint64_t suspend_duration_ns;
	snd_pcm_state_t state_at_suspend;
	snd_pcm_state_t state_after_wake;
	int wake_cycle_rc;
	unsigned int recoveries_delta;
	unsigned int recover_fails_delta;
	snd_pcm_state_t state_after_recover;
	uint64_t verify_cycles;
	uint64_t verify_frames;
	uint64_t verify_xrun;
	int verify_err;
	/* fresh open control probe (test-only) */
	bool fresh_reopen_ok;
	int fresh_reopen_err;
	uint64_t fresh_reopen_frames;
	/* auto trigger */
	pid_t auto_child;
	int auto_child_exit;
	bool auto_failed;
	/* the stream broke before any suspend was detected */
	bool pre_fail;
};

/* fork a child that arms the RTC alarm and suspends for `seconds`; the
 * machine wakes itself when the alarm fires */
static pid_t spawn_suspend(unsigned int seconds) {
	char secbuf[16];
	char *const argv[] = {
		"/usr/sbin/rtcwake", "-m", "mem", "-s", secbuf, NULL};

	snprintf(secbuf, sizeof(secbuf), "%u", seconds);

	pid_t pid = fork();
	if (pid < 0) {
		log_error("fork failed for auto suspend: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	return pid;
}

/* run one playback iteration without failing the wait on a recoverable
 * stream error; returns 0 on success, -1 on an unexpected error */
static int run_tolerant(struct alsa_state *st) {
	int res = pcm_sink_iterate(st, DETECT_POLL_MS);

	if (res == -EINTR || res == -ETIMEDOUT || res == -EAGAIN)
		return 0;
	if (res < 0) {
		log_warn("Stream cycle failed: %s", snd_strerror(res));
		return -1;
	}
	return 0;
}

static void print_report(const struct suspend_stats *s,
			 const struct pcm_setup *cfg) {
	report_section("Suspend Resume Test");

	struct report_tab *t = report_tab_begin();
	report_kv(t, "Device", "%s", cfg->dev);
	report_kv(t, "Rate", "%u Hz", cfg->rate);
	report_kv(t, "Period", "%lu frames", cfg->period);
	report_kv(t, "Channels", "%u", cfg->channels);
	report_kv(t, "Mode", "%s",
		  s->idle_mode ? "idle (dropped across suspend)"
			       : "running (playing across suspend)");
	report_tab_end(t);

	t = report_tab_begin();
	report_kv(t, "Stream state at suspend", "%s",
		  state_name(s->state_at_suspend));
	if (s->suspend_detected)
		report_kv(t, "Suspend detected", "yes, %llu ms",
			  (unsigned long long) (s->suspend_duration_ns /
						1000000));
	else
		report_kv(t, "Suspend detected", "no");
	report_tab_end(t);

	if (s->suspend_detected) {
		t = report_tab_begin();
		report_kv(t, "State after wake", "%s",
			  state_name(s->state_after_wake));
		if (s->wake_cycle_rc < 0 && s->wake_cycle_rc != -EAGAIN)
			report_kv(t, "Wake processing", "failed: %s",
				  snd_strerror(s->wake_cycle_rc));
		else
			report_kv(t, "Wake processing", "ok");
		report_kv(t, "Recovery attempts", "%u", s->recoveries_delta);
		report_kv(t, "Recover fails", "%u", s->recover_fails_delta);
		report_kv(t, "State after processing", "%s",
			  state_name(s->state_after_recover));
		report_kv(t, "Verification playback",
			  "%" PRIu64 " cycles, %" PRIu64 " frames, %" PRIu64
			  " xrun frames",
			  s->verify_cycles, s->verify_frames, s->verify_xrun);
		report_tab_end(t);
	}

	t = report_tab_begin();
	report_kv(t, "Fresh open + play", "%s",
		  s->fresh_reopen_ok ? "ok" : "failed");
	report_kv(t, "Fresh playback frames", "%" PRIu64,
		  s->fresh_reopen_frames);
	if (!s->fresh_reopen_ok && s->fresh_reopen_err < 0)
		report_kv(t, "Fresh open error", "%s",
			  snd_strerror(s->fresh_reopen_err));
	report_tab_end(t);
	report_note("fresh open is a test-only probe, not a PipeWire path");
}

static void suspend_usage(const char *prog) {
	usage_header(prog, "[OPTION]...");
	usage_opt("-D NAME", "select PCM by name (required)");
	usage_opt("-r RATE", "sample rate (default 48000)");
	usage_opt("-c CHANNELS", "channels (default 2)");
	usage_opt("-f FORMAT", "sample format (default S16_LE)");
	usage_opt("-p PERIOD", "period size in frames (default 1024)");
	usage_opt("-i", "idle control: drop to SETUP across the suspend "
			"(PipeWire SPA_NODE_COMMAND_Suspend)");
	usage_opt("-T SECONDS", "auto-suspend via '/usr/sbin/rtcwake -m mem -s "
				"SECONDS' (requires root)");
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

static int suspend_run(int argc, char **argv) {
	struct pcm_setup cfg;
	bool idle_mode = false;
	long auto_seconds = 0;
	int verbose = 0;
	int opt, err;

	pcm_setup_defaults(&cfg);

	while ((opt = getopt(argc, argv, "D:r:c:f:p:iT:vh")) != -1) {
		switch (opt) {
		case 'i':
			idle_mode = true;
			break;
		case 'T':
			auto_seconds = parse_long(optarg, "suspend", &err);
			if (err < 0 || auto_seconds < 1) {
				fprintf(stderr,
					"invalid suspend argument '%s'\n",
					optarg);
				suspend_usage(argv[0]);
				return 2;
			}
			break;
		case 'v':
			verbose++;
			break;
		case 'h':
			suspend_usage(argv[0]);
			return 0;
		default:
			if (!pcm_setup_parse_opt(&cfg, opt, optarg)) {
				suspend_usage(argv[0]);
				return 2;
			}
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		suspend_usage(argv[0]);
		return 2;
	}
	if (!pcm_setup_check(&cfg)) {
		suspend_usage(argv[0]);
		return 2;
	}
	if (auto_seconds > UINT_MAX) {
		fprintf(stderr, "invalid suspend argument '%ld'\n",
			auto_seconds);
		suspend_usage(argv[0]);
		return 2;
	}
	if (auto_seconds > 0 && geteuid() != 0) {
		fprintf(stderr,
			"%s: -T auto-suspend must run as root: the child runs "
			"'/usr/sbin/rtcwake -m mem -s N', which writes "
			"/sys/power/state\n",
			argv[0]);
		return 1;
	}

	log_set_verbose(verbose);

	struct alsa_state stream;
	struct suspend_stats s = {
		.idle_mode = idle_mode,
		.auto_child = -1,
		.auto_child_exit = -1,
	};
	alsa_state_init(&stream);
	if (pcm_sink_open(&stream, &cfg) < 0)
		return 1;
	if (pcm_sink_start(&stream) < 0) {
		pcm_sink_stop(&stream);
		return 1;
	}

	/* warm up so the stream is genuinely running before the sleep */
	{
		uint64_t warm_end = now_ns() + WARMUP_MS * 1000000ULL;
		while (!stop_requested() && now_ns() < warm_end) {
			if (run_tolerant(&stream) < 0) {
				log_error("Warmup playback failed");
				pcm_sink_stop(&stream);
				return 1;
			}
		}
	}

	if (idle_mode) {
		int res = spa_alsa_pause(&stream);
		if (res < 0) {
			log_error("Could not pause the stream: %s",
				  snd_strerror(res));
			pcm_sink_stop(&stream);
			return 1;
		}
		s.state_at_suspend = snd_pcm_state(stream.hndl);
		log_info("Idle control: stream %s before the sleep",
			 state_name(s.state_at_suspend));
	} else {
		s.state_at_suspend = snd_pcm_state(stream.hndl);
		log_info("Running: stream %s before the sleep",
			 state_name(s.state_at_suspend));
	}

	if (auto_seconds > 0) {
		log_info("Auto suspend: '/usr/sbin/rtcwake -m mem -s %ld'",
			 auto_seconds);
		s.auto_child = spawn_suspend((unsigned int) auto_seconds);
		if (s.auto_child < 0) {
			pcm_sink_stop(&stream);
			return 1;
		}
	}

	uint64_t prev_gap = boottime_ns() - now_ns();
	int wait_errs = 0;
	unsigned int recoveries_before = 0;
	unsigned int recover_fails_before = 0;

	while (!stop_requested() && !s.suspend_detected && !s.auto_failed &&
	       !s.pre_fail) {
		int ready = 0;
		if (!idle_mode) {
			struct pollfd pfd = {
				.fd = stream.timerfd,
				.events = POLLIN,
			};
			ready = poll(&pfd, 1, DETECT_POLL_MS);
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				log_error("Could not wait for playback: %s",
					  strerror(errno));
				s.pre_fail = true;
				break;
			}
		} else {
			poll(NULL, 0, DETECT_POLL_MS);
		}

		/* a real suspend adds its whole duration in a single sample
		 * (BOOTTIME counts sleep time, MONOTONIC does not); clock
		 * drift shows up as a small per-sample increment and is
		 * ignored by comparing consecutive samples. The signed
		 * delta keeps machines where BOOTTIME is behind MONOTONIC
		 * (no suspend time yet) from wrapping the unsigned gap */
		uint64_t gap = boottime_ns() - now_ns();
		int64_t delta = (int64_t) (gap - prev_gap);
		if (delta >= (int64_t) SUSPEND_GAP_THRESHOLD_NS) {
			s.suspend_detected = true;
			s.suspend_duration_ns = (uint64_t) delta;
			s.state_after_wake = snd_pcm_state(stream.hndl);
			recoveries_before = stream.recoveries;
			recover_fails_before = stream.recover_fails;
			if (idle_mode)
				s.wake_cycle_rc = spa_alsa_start(&stream);
			else
				s.wake_cycle_rc = pcm_sink_iterate(&stream, 0);
			s.state_after_recover = snd_pcm_state(stream.hndl);
			break;
		}
		prev_gap = gap;

		if (!idle_mode && ready > 0) {
			if (run_tolerant(&stream) < 0) {
				if (++wait_errs >= 3) {
					s.pre_fail = true;
					log_error("Stream broke before any "
						  "suspend was detected");
					break;
				}
			} else {
				wait_errs = 0;
			}
		}

		if (s.auto_child >= 0) {
			int status;
			pid_t r = waitpid(s.auto_child, &status, WNOHANG);
			if (r == s.auto_child) {
				s.auto_child = -1;
				s.auto_child_exit =
					WIFEXITED(status) ? WEXITSTATUS(status)
							  : -1;
				if (!s.suspend_detected) {
					s.auto_failed = true;
					log_error("Auto suspend command exited "
						  "(%d) before any suspend was "
						  "detected; the RTC alarm or "
						  "permissions may be missing",
						  s.auto_child_exit);
				}
			}
		}
	}

	if (s.auto_child >= 0) {
		int status;
		waitpid(s.auto_child, &status, 0);
		s.auto_child = -1;
	}

	if (s.suspend_detected) {
		/* verify the stream actually plays after the resume */
		uint64_t verify_until = now_ns() + VERIFY_MS * 1000000ULL;
		uint64_t frames_before = stream.sample_count;
		uint64_t xrun_before = stream.xrun;
		while (!stop_requested() && now_ns() < verify_until) {
			int res = pcm_sink_iterate(&stream, 50);
			if (res == -EINTR || res == -ETIMEDOUT ||
			    res == -EAGAIN)
				continue;
			if (res < 0) {
				log_warn("Verification playback failed: %s",
					 snd_strerror(res));
				s.verify_err = res;
				break;
			}
			s.verify_cycles++;
		}
		s.verify_frames = stream.sample_count - frames_before;
		s.verify_xrun = stream.xrun - xrun_before;
		s.recoveries_delta = stream.recoveries - recoveries_before;
		s.recover_fails_delta =
			stream.recover_fails - recover_fails_before;
	}

	pcm_sink_stop(&stream);

	/* fresh open control: distinguishes a stale handle from a dead or
	 * re-enumerated device */
	{
		struct alsa_state fresh;
		alsa_state_init(&fresh);
		int res = pcm_sink_open(&fresh, &cfg);
		if (res == 0)
			res = pcm_sink_start(&fresh);
		s.fresh_reopen_err = res;
		if (res == 0) {
			for (int i = 0; i < 10 && !stop_requested(); i++) {
				int r = pcm_sink_iterate(&fresh, 50);
				if (r < 0 && r != -EINTR && r != -ETIMEDOUT &&
				    r != -EAGAIN) {
					s.fresh_reopen_err = r;
					break;
				}
			}
		}
		s.fresh_reopen_frames = fresh.sample_count;
		pcm_sink_stop(&fresh);
		s.fresh_reopen_ok =
			s.fresh_reopen_err == 0 && s.fresh_reopen_frames > 0;
	}

	print_report(&s, &cfg);

	if (!s.suspend_detected) {
		if (s.pre_fail) {
			report_fail("the stream broke before any suspend was "
				    "detected");
			return 1;
		}
		if (s.auto_failed) {
			report_fail("auto suspend failed (exit %d)",
				    s.auto_child_exit);
			return 1;
		}
		report_note("no suspend was detected; test inconclusive");
		return 0;
	}

	if ((s.wake_cycle_rc < 0 && s.wake_cycle_rc != -EAGAIN) ||
	    s.recover_fails_delta > 0 || s.verify_err != 0) {
		report_fail("the stream did not recover after wake");
		if (s.fresh_reopen_ok)
			report_note("fresh open works; the stale handle that "
				    "crossed the suspend is the problem");
		else
			report_note("fresh open also failed; the device may "
				    "have been re-enumerated or is gone");
		return 1;
	}

	if (s.verify_frames == 0)
		report_warn("no frames were written during the verification "
			    "window");
	else
		report_ok("stream recovered and played after wake like "
			  "PipeWire");
	return 0;
}

static struct applet suspend_applet = {
	.name = "suspend",
	.desc = "test PCM recovery across a system suspend/resume",
	.main = suspend_run,
	.usage = suspend_usage,
	.next = NULL,
};

APPLET_REGISTER(suspend_applet);

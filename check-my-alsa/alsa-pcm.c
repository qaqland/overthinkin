/*
 * alsa-pcm.c - pipewire ALSA playback path ported into check-my-alsa.
 *
 * The function bodies below mirror uos-pipewire
 * spa/plugins/alsa/alsa-pcm.c (1.6.0-1deepin9): same function names, same
 * snd_* call sequence, same error handling and log semantics. Log lines
 * printed by these functions can therefore be correlated with pipewire's
 * "alsa-pcm.c" messages when debugging xrun recovery failures.
 *
 * Deviations from pipewire (each marked with a comment at the site):
 *  - struct state -> struct alsa_state (see alsa-pcm.h)
 *  - graph-only parts are omitted: driver/follower list iteration, clock,
 *    spa_node_call_xrun, event loop sources, buffer queue plumbing
 *  - branches that are dead in the test configuration are removed
 *    (linked/following/matching/resample, disable_tsched, htimestamp,
 *    non-mmap access, capture); the remaining path matches pipewire's
 *    playback and tsched configuration
 *  - spa_log_* -> log_*() (stderr). Messages retain ALSA function and
 *    state keywords for correlation, but use human-readable prose,
 *    relative timestamps, and no state/device prefix. Raw ALSA dumps are
 *    TRACE rather than DEBUG
 *  - spa_ratelimit suppression removed: messages are printed every time
 *  - alsa_write_frames() writes silence instead of the graph buffers;
 *    alsa_read_frames() (the capture path) is not ported, the test only
 *    exercises playback
 */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>

#include "alsa-pcm.h"

#define SPA_ALSA_DLL_BW_MIN 0.001

static uint64_t get_time_ns(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t) now.tv_sec * NSEC_PER_SEC + (uint64_t) now.tv_nsec;
}

static uint32_t flp2(uint32_t x) {
	x = x | (x >> 1);
	x = x | (x >> 2);
	x = x | (x >> 4);
	x = x | (x >> 8);
	x = x | (x >> 16);
	return x - (x >> 1);
}

/* pipewire logs this via spa_log_debug */
static void debug_hw_params(const char *prefix, snd_pcm_hw_params_t *params) {
	if (g_log_level < LOG_TRACE)
		return;
	log_trace("Raw ALSA hardware parameters (%s):", prefix);
	snd_output_t *output;
	if (snd_output_stdio_attach(&output, stderr, 0) >= 0) {
		snd_pcm_hw_params_dump(params, output);
		snd_output_close(output);
	}
	log_trace("End of ALSA hardware parameters");
}

/* pipewire: spa_log_debug("state after sw_params:") + dump */
static void debug_pcm_state(snd_pcm_t *hndl) {
	if (g_log_level < LOG_TRACE)
		return;
	log_trace("Raw ALSA PCM state after software parameters:");
	snd_output_t *output;
	if (snd_output_stdio_attach(&output, stderr, 0) >= 0) {
		snd_pcm_dump(hndl, output);
		snd_output_close(output);
	}
	log_trace("End of ALSA PCM state");
}

/* fill_device_name(); the test takes the device
 * name verbatim from the command line, pipewire would apply the node
 * properties here */
static void fill_device_name(struct alsa_state *state, const char *params,
			     char device_name[], size_t len) {
	(void) state;
	(void) params;
	/* deviation: pipewire applies props (SPA_KEY_API_ALSA_PATH /
	 * api.alsa.pcm.card) and the _alibpref UCM prefix here; the
	 * 63-char cap leaves room for the "p" suffix in state->name */
	snprintf(device_name, len, "%.63s", state->name);
}

int spa_alsa_open(struct alsa_state *state, const char *params) {
	int err;
	/* pipewire: device_name is filled from props->device (char[64])
	 * in fill_device_name(); the test copies state->name */
	char device_name[64];

	if (state->opened)
		return 0;

	fill_device_name(state, params, device_name, sizeof(device_name));
	snprintf(state->name, sizeof(state->name), "%.63s", device_name);
	strncat(state->name, "p",
		sizeof(state->name) - strlen(state->name) - 1);

	CHECK(snd_pcm_open(&state->hndl, device_name, SND_PCM_STREAM_PLAYBACK,
			   SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE |
				   SND_PCM_NO_AUTO_CHANNELS |
				   SND_PCM_NO_AUTO_FORMAT),
	      "Could not open the playback PCM");

	/* tsched mode (disable_tsched=false)
	 * creates a timerfd that schedules the graph */
	state->timerfd =
		timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (state->timerfd < 0) {
		err = -errno;
		log_error("Could not create the playback timer: %s",
			  strerror(errno));
		goto error_exit_close;
	}

	/* probe_pitch_ctl(); not ported (no
	 * rate-matching in the test) */

	state->opened = true;
	state->sample_count = 0;

	return 0;

error_exit_close:
	snd_pcm_close(state->hndl);
	return err;
}

/* simplified: no position clock, not batch
 * unless the driver says so (is_batch set by spa_alsa_set_format) */
static void recalc_headroom(struct alsa_state *state) {
	if (state->use_period_size_min_as_headroom)
		state->headroom = state->period_size_min;
	else
		state->headroom = 0;

	/* tsched always adds headroom for batch
	 * devices (missed pointer updates) */
	if (state->is_batch)
		state->headroom += state->period_frames;

	if (state->buffer_frames >= state->threshold)
		state->headroom = MIN(state->headroom,
				      state->buffer_frames - state->threshold);
	else
		state->headroom = 0;
}

/*
 * playback/raw/mmap/tsched subset: the format is taken from the command
 * line instead of a SPA format pod */
int spa_alsa_set_format(struct alsa_state *state, unsigned int rate,
			unsigned int channels, snd_pcm_format_t format,
			unsigned int period_size) {
	unsigned int rrate, rchannels, val, rscale = 1, period_scale = 1;
	snd_pcm_uframes_t period_size_frames;
	int err, dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_t rformat;
	snd_pcm_access_mask_t *amask;
	snd_pcm_uframes_t default_period;
	snd_pcm_t *hndl;

	rrate = rate;
	rchannels = channels;
	rformat = format;

	if (rformat == SND_PCM_FORMAT_UNKNOWN) {
		log_warn("The requested sample format is unknown");
		return -EINVAL;
	}

	/* if (!state->started && state->have_format)
	 *   spa_alsa_close(state); the test only sets the format once */

	if ((err = spa_alsa_open(state, NULL)) < 0)
		return err;

	hndl = state->hndl;

	snd_pcm_hw_params_alloca(&params);
	/* choose all parameters */
	CHECK(snd_pcm_hw_params_any(hndl, params),
	      "Broken configuration for playback: no configurations "
	      "available");

	debug_hw_params(__func__, params);

	/* set hardware resampling, no resample */
	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0),
	      "set_rate_resample");

	/* set the interleaved/planar read/write format */
	snd_pcm_access_mask_alloca(&amask);
	snd_pcm_hw_params_get_access_mask(params, amask);

	/* use_mmap is always true in the test */
	if ((err = snd_pcm_hw_params_set_access(
		     hndl, params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {
		log_debug("Interleaved MMAP is unavailable; trying read/write "
			  "access: %s",
			  snd_strerror(err));
		state->use_mmap = false;
	}
	if (!state->use_mmap) {
		if ((err = snd_pcm_hw_params_set_access(
			     hndl, params, SND_PCM_ACCESS_RW_INTERLEAVED)) <
		    0) {
			log_error("Interleaved read/write access is "
				  "unavailable: %s",
				  snd_strerror(err));
			return err;
		}
	}

	/* set the sample format */
	log_debug("Requested format: %s; rate: %i Hz; channels: %i; "
		  "access: interleaved %s",
		  snd_pcm_format_name(rformat), rrate, rchannels,
		  state->use_mmap ? "MMAP" : "read/write");
	CHECK(snd_pcm_hw_params_set_format(hndl, params, rformat),
	      "set_format");

	/* prepare channel map: the test does not
	 * set a channel map (deviation) */

	/* set the count of channels */
	val = rchannels;
	CHECK(snd_pcm_hw_params_set_channels_near(hndl, params, &val),
	      "set_channels");
	if (rchannels != val) {
		log_warn("The driver changed the channel count from %u to %u",
			 rchannels, val);
		rchannels = val;
	}

	/* multi_rate/card check, not applicable
	 * in the test (deviation) */

	/* set the stream rate */
	val = rrate;
	CHECK(snd_pcm_hw_params_set_rate_near(hndl, params, &val, 0),
	      "set_rate_near");
	if (rrate != val) {
		log_warn("The driver changed the sample rate from %i to %i Hz",
			 rrate, val);
		rrate = val;
	}
	if (rchannels == 0 || rrate == 0) {
		log_error(
			"The driver returned invalid parameters: %d channels; "
			"%d Hz",
			rchannels, rrate);
		return -EIO;
	}

	state->format = rformat;
	state->channels = rchannels;
	state->rate = rrate;
	state->frame_size = snd_pcm_format_physical_width(rformat) / 8;
	state->frame_scale = rscale;
	state->planar = false;
	state->frame_size *= rchannels;

	/* driver_duration/driver_rate reset
	 * (set by check_position_config in pipewire); the test fixes them
	 * at init time instead */
	state->have_format = true;

	dir = 0;
	state->is_batch =
		snd_pcm_hw_params_is_batch(params) && !state->disable_batch;

	period_size_frames = period_size;
	default_period = (uint64_t) DEFAULT_PERIOD * state->rate / DEFAULT_RATE;
	default_period = flp2(2 * default_period - 1);

	/* no period size specified and batch or
	 * force_quantum -> use graph quantum; the test always passes a
	 * period size, so this is skipped */

	/* tsched mode: disable ALSA wakeups */
	if (snd_pcm_hw_params_can_disable_period_wakeup(params))
		CHECK(snd_pcm_hw_params_set_period_wakeup(hndl, params, 0),
		      "set_period_wakeup");

	/* default_period_num is not used by the
	 * test, tsched -> periods = UINT_MAX */

	/* Query the minimum period size for this configuration */
	CHECK(snd_pcm_hw_params_get_period_size_min(
		      params, &state->period_size_min, &dir),
	      "snd_pcm_hw_params_get_period_size_min");

	/* default_period_size is set by the
	 * test, so the FireWire minimum-periods workaround is skipped */

	CHECK(snd_pcm_hw_params_set_period_size_near(hndl, params,
						     &period_size_frames, &dir),
	      "set_period_size_near");

	if (period_size_frames == 0) {
		log_error("The driver returned a period size of zero");
		return -EIO;
	}

	state->period_frames = period_size_frames;
	state->duration = period_size_frames * period_scale;

	/* periods == UINT_MAX (tsched), so the
	 * buffer size is derived from the maximum and then clamped */
	CHECK(snd_pcm_hw_params_get_buffer_size_max(params,
						    &state->buffer_frames),
	      "get_buffer_size_max");

	state->buffer_frames =
		MIN(state->buffer_frames,
		    state->quantum_limit * 4 * state->frame_scale);

	CHECK(snd_pcm_hw_params_set_buffer_size_min(hndl, params,
						    &state->buffer_frames),
	      "set_buffer_size_min");
	CHECK(snd_pcm_hw_params_set_buffer_size_near(hndl, params,
						     &state->buffer_frames),
	      "set_buffer_size_near");
	if (state->buffer_frames == 0) {
		log_error("The driver returned a buffer size of zero");
		return -EIO;
	}

	state->max_delay = state->buffer_frames / 2;
	state->min_delay = 0;

	state->start_delay = state->default_start_delay;

	recalc_headroom(state);

	log_debug("Negotiated format: %s; rate: %d Hz; channels: %d; "
		  "access: interleaved %s",
		  snd_pcm_format_name(state->format), state->rate,
		  state->channels, state->use_mmap ? "MMAP" : "read/write");
	log_debug("Hardware buffer: %lu frames; period %lu; minimum period %lu",
		  state->buffer_frames, state->period_frames,
		  state->period_size_min);
	log_debug("Scheduling: headroom %u frames; batch mode %s; timer "
		  "scheduling enabled",
		  state->headroom, state->is_batch ? "enabled" : "disabled");

	/* write the parameters to device */
	CHECK(snd_pcm_hw_params(hndl, params), "set_hw_params");

	return 0;
}

int set_swparams(struct alsa_state *state) {
	snd_pcm_t *hndl = state->hndl;
	int err = 0;
	snd_pcm_sw_params_t *params;

	snd_pcm_sw_params_alloca(&params);

	/* get the current params */
	CHECK(snd_pcm_sw_params_current(hndl, params), "sw_params_current");

	CHECK(snd_pcm_sw_params_set_tstamp_mode(hndl, params,
						SND_PCM_TSTAMP_ENABLE),
	      "sw_params_set_tstamp_mode");
	CHECK(snd_pcm_sw_params_set_tstamp_type(hndl, params,
						SND_PCM_TSTAMP_TYPE_MONOTONIC),
	      "sw_params_set_tstamp_type");

	/* start the transfer */
	CHECK(snd_pcm_sw_params_set_start_threshold(hndl, params, LONG_MAX),
	      "set_start_threshold");

	/* pipewire sets avail_min here when disable_tsched is enabled;
	 * the test always runs with disable_tsched=false */

	/* write the parameters to the playback device */
	CHECK(snd_pcm_sw_params(hndl, params), "sw_params");

	debug_pcm_state(hndl);

	return 0;
}

static int set_timeout(struct alsa_state *state, uint64_t time) {
	struct itimerspec ts;
	ts.it_value.tv_sec = time / NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	if (timerfd_settime(state->timerfd, TFD_TIMER_ABSTIME, &ts, NULL) < 0) {
		int err = -errno;
		log_error("Could not arm the playback timer: %s",
			  strerror(errno));
		return err;
	}
	return 0;
}

/*
 * only the mmap path is kept; pipewire falls back to snd_pcm_writei()
 * when use_mmap is not set, the test always uses mmap */
int spa_alsa_silence(struct alsa_state *state, snd_pcm_uframes_t silence) {
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t frames, offset;
	int res;

	if (state->use_mmap) {
		frames = state->buffer_frames;

		if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset,
					      &frames)) < 0) {
			log_error("snd_pcm_mmap_begin failed while writing "
				  "silence: %s",
				  snd_strerror(res));
			return res;
		}
		silence = MIN(silence, frames);

		log_trace(
			"Silence mapping: %ld frames; offset %ld; writing %ld",
			(long) frames, (long) offset, (long) silence);
		snd_pcm_areas_silence(my_areas, offset, state->channels,
				      silence, state->format);

		if ((res = snd_pcm_mmap_commit(hndl, offset, silence)) < 0) {
			log_error("snd_pcm_mmap_commit failed while writing "
				  "silence: %s",
				  snd_strerror(res));
			return res;
		}
	} else {
		/* deviation: pipewire writes a VLA
		 * buffer through snd_pcm_writei(); the test never gets
		 * here */
		return -EIO;
	}
	return 0;
}

/*
 * resets the graph buffer queue; the test has no graph buffers, so this
 * is a no-op (deviation) */
static void reset_buffers(struct alsa_state *state) {
	(void) state;
}

int do_prepare(struct alsa_state *state) {
	int err;

	state->last_threshold = state->threshold;

	log_debug("Preparing stream: threshold %d frames; graph period %d; "
		  "rate %d Hz",
		  (int) state->threshold, (int) state->driver_duration,
		  (int) state->driver_rate_denom);

	CHECK(set_swparams(state), "swparams");

	/* pipewire only calls snd_pcm_prepare() when the stream is not
	 * linked to a driver; the test is never linked
	 * and tolerates -EBUSY like pipewire */
	if ((err = snd_pcm_prepare(state->hndl)) < 0 && err != -EBUSY) {
		log_error("snd_pcm_prepare failed: %s", snd_strerror(err));
		return err;
	}

	/* pipewire sets the channel map here (state->alsa_chmap); not
	 * ported */

	/* pipewire only prefills silence for playback streams;
	 * the test only exercises playback */
	{
		snd_pcm_uframes_t silence =
			state->start_delay + state->threshold + state->headroom;
		/* pipewire adds another threshold when disable_tsched is
		 * enabled; the test runs with
		 * disable_tsched=false */
		spa_alsa_silence(state, silence);
	}

	reset_buffers(state);
	state->alsa_sync = true;
	state->alsa_sync_warning = false;
	state->alsa_started = false;
	spa_dll_init(&state->dll);

	/* pipewire sets these in check_position_config() when the graph
	 * position changes; the test has no graph so
	 * they are fixed at prepare time (deviation) */
	state->max_error =
		MAX(256.0, (state->threshold + state->headroom) / 2.0);
	state->max_resync =
		MIN(state->threshold + state->headroom, state->max_error);
	state->err_wdw =
		(double) state->driver_rate_denom / state->driver_duration;

	return 0;
}

/*
 * pipewire only drops the stream when it is not linked;
 * the test is never linked */
int do_drop(struct alsa_state *state) {
	int res;
	log_debug("Dropping the playback stream with snd_pcm_drop");
	if ((res = snd_pcm_drop(state->hndl)) < 0) {
		log_error("snd_pcm_drop failed: %s", snd_strerror(res));
		return res;
	}
	return 0;
}

/*
 * pipewire only starts the stream when it is not linked;
 * the test is never linked */
int do_start(struct alsa_state *state) {
	int res;
	if (!state->alsa_started) {
		log_debug("Starting the playback stream with snd_pcm_start");
		if ((res = snd_pcm_start(state->hndl)) < 0) {
			log_error("snd_pcm_start failed: %s",
				  snd_strerror(res));
			return res;
		}
		state->alsa_started = true;
	}
	return 0;
}

int alsa_recover(struct alsa_state *state) {
	int res, st, retry = 0;
	snd_pcm_status_t *status;

	snd_pcm_status_alloca(&status);
	if ((res = snd_pcm_status(state->hndl, status)) < 0) {
		log_error("snd_pcm_status failed during recovery: %s",
			  snd_strerror(res));
		goto recover;
	}

	st = snd_pcm_status_get_state(status);
	switch (st) {
	case SND_PCM_STATE_XRUN: {
		struct timeval now, trigger, diff;
		uint64_t delay, missing;

		snd_pcm_status_get_tstamp(status, &now);
		snd_pcm_status_get_trigger_tstamp(status, &trigger);
		timersub(&now, &trigger, &diff);

		delay = TIMEVAL_TO_USEC(&diff);
		missing = delay * state->rate / USEC_PER_SEC;
		missing +=
			state->start_delay + state->threshold + state->headroom;

		log_trace("XRUN status: trigger delay %" PRIu64
			  " us; estimated loss %" PRIu64 " frames",
			  delay, missing);

		/* pipewire: state->clock->xrun += SPA_SCALE32_UP(missing,
		 * state->clock->rate.denom, state->rate); with the test
		 * clock rate == state->rate this simplifies to missing */
		state->xrun += missing;

		/* pipewire: spa_node_call_xrun(&state->callbacks, trigger,
		 * delay, NULL); (no log line) */
		if (state->xrun_cb)
			state->xrun_cb(state, missing, state->xrun_data);
		break;
	}
	case SND_PCM_STATE_SUSPENDED:
		log_info("Recovering the stream from %s",
			 snd_pcm_state_name(st));
		while (retry++ < 5 &&
		       (res = snd_pcm_resume(state->hndl)) == -EAGAIN)
			/* wait until suspend flag is released */
			poll(NULL, 0, 1000);
		if (res >= 0)
			return res;
		/* try to drop and prepare below */
		break;
	default:
		log_error("Cannot recover the stream from state %s",
			  snd_pcm_state_name(st));
		break;
	}

recover:
	/* pipewire also drops/prepares/starts all linked followers here;
	 * the test has a single device
	 * (driver == state) */
	do_drop(state);
	do_prepare(state);
	do_start(state);

	return 0;
}

/*
 * pipewire uses snd_pcm_avail_update() when not matching and
 * disable_tsched and not resampling, snd_pcm_avail() otherwise; all three
 * conditions are false in the test so it always uses snd_pcm_avail() */
snd_pcm_sframes_t alsa_avail(struct alsa_state *state) {
	snd_pcm_sframes_t avail;
	if (!state->matching && state->disable_tsched && !state->resample)
		avail = snd_pcm_avail_update(state->hndl);
	else
		avail = snd_pcm_avail(state->hndl);
	return avail;
}

/*
 * this is where pipewire reports "snd_pcm_avail after recover: Broken
 * pipe": the recover itself always returns 0, a failed recovery only
 * shows up here (or in the next mmap_begin)
 * the pipewire htimestamp path is kept but disabled by default */
int get_avail(struct alsa_state *state, uint64_t current_time,
	      snd_pcm_uframes_t *delay) {
	int res;
	snd_pcm_sframes_t avail;

	if ((avail = alsa_avail(state)) < 0) {
		if ((res = alsa_recover(state)) < 0)
			return res;
		if ((avail = alsa_avail(state)) < 0) {
			/* pipewire rate-limits this with spa_ratelimit_test()
			 * and logs the number of suppressed messages; the
			 * test prints every time */
			log_warn("snd_pcm_avail still fails after recovery: %s",
				 snd_strerror(avail));
			/* test-only counter, no pipewire equivalent */
			state->recover_fails++;
			avail = state->threshold * 2;
		}
	}
	*delay = avail;

	if (state->htimestamp) {
		snd_pcm_uframes_t havail;
		snd_htimestamp_t tstamp;
		uint64_t then;

		if ((res = snd_pcm_htimestamp(state->hndl, &havail, &tstamp)) <
		    0) {
			log_warn("snd_pcm_htimestamp failed: %s",
				 snd_strerror(res));
			return avail;
		}
		avail = havail;
		*delay = havail;
		if ((then = (uint64_t) tstamp.tv_sec * NSEC_PER_SEC +
			    (uint64_t) tstamp.tv_nsec) != 0) {
			int64_t diff;

			if (then < current_time)
				diff = ((int64_t) (current_time - then)) *
				       state->rate / NSEC_PER_SEC;
			else
				diff = -((int64_t) (then - current_time)) *
				       state->rate / NSEC_PER_SEC;

			log_trace("Hardware timestamp: position time %" PRIu64
				  " ns; offset %" PRIi64
				  " frames; available %lu",
				  then, diff, (unsigned long) havail);

			if (diff < (int64_t) state->threshold * 3 &&
			    diff > -(int64_t) state->threshold * 3) {
				*delay +=
					CLAMP(diff, -(int64_t) state->threshold,
					      (int64_t) state->threshold);
				state->htimestamp_error = 0;
			} else if (state->htimestamp_max_errors) {
				if (++state->htimestamp_error >
				    state->htimestamp_max_errors) {
					log_error("The driver returned invalid "
						  "hardware "
						  "timestamps; disabling them");
					state->htimestamp_error = 0;
					state->htimestamp = false;
				} else {
					log_warn("The hardware timestamp "
						 "differs by %" PRIi64
						 " frames; expected less than "
						 "%lu",
						 diff,
						 (unsigned long) state
								 ->threshold *
							 3);
				}
			}
		}
	}
	return avail;
}

/* playback branch only */
int get_status(struct alsa_state *state, uint64_t current_time,
	       snd_pcm_uframes_t *avail, snd_pcm_uframes_t *delay,
	       snd_pcm_uframes_t *target) {
	int res;
	snd_pcm_uframes_t a, d;

	if ((res = get_avail(state, current_time, &d)) < 0)
		return res;

	a = MIN(res, (int) state->buffer_frames);

	/* resample/rate_match branch removed,
	 * state->delay stays 0 */
	state->delay = 0;

	*avail = state->buffer_frames - a;
	*delay = state->buffer_frames - MIN(d, state->buffer_frames);
	*target = state->threshold + state->headroom;

	*target = CLAMP(*target, state->min_delay, state->max_delay);

	/* test-only bookkeeping for the cycle hook */
	state->last_avail = *avail;
	state->last_delay = *delay;
	state->last_target = *target;

	return 0;
}

/*
 * syncs rate/duration with the graph position clock; there is no graph in
 * the test, the values are fixed at init time (deviation) */
static int check_position_config(struct alsa_state *state, bool starting) {
	(void) state;
	(void) starting;
	return 0;
}

/*
 * updates the poll fd masks in the event loop when disable_tsched is set;
 * there is no event loop in the test */
static void update_sources(struct alsa_state *state, bool active) {
	(void) state;
	(void) active;
}

/*
 * spa_dll rate correction active; clock/rate_match outputs omitted (no
 * graph). The clamping and next_time arithmetic match pipewire */
int update_time(struct alsa_state *state, uint64_t current_time,
		snd_pcm_sframes_t delay, snd_pcm_sframes_t target,
		bool follower) {
	double err, corr, avg;
	int32_t diff;
	(void) follower;

	if (state->dll.bw == 0.0) {
		spa_dll_set_bw(&state->dll, state->dll_bw_max, state->threshold,
			       state->rate);
		state->next_time = current_time;
		state->base_time = current_time;
	}

	/* disable_tsched && !follower branch
	 * removed, the test runs tsched */
	err = delay - target;

	diff = (int32_t) (state->last_threshold - state->threshold);

	if (diff != 0) {
		err -= diff;
		log_trace("Period changed from %d to %d frames; difference %d; "
			  "timing error %+.2f frames",
			  state->last_threshold, state->threshold, diff, err);
		state->last_threshold = state->threshold;
		state->alsa_sync = true;
		state->alsa_sync_warning = false;
	}
	if (err > state->max_resync) {
		state->alsa_sync = true;
		if (err > state->max_error)
			err = state->max_error;
	} else if (err < -state->max_resync) {
		state->alsa_sync = true;
		if (err < -state->max_error)
			err = -state->max_error;
	}

	/* pipewire's spa_dll_update() */
	corr = spa_dll_update(&state->dll, err);

	/* err_avg/err_var bookkeeping kept for
	 * the BW_PERIOD debug print */
	avg = (state->err_avg * state->err_wdw + (err - state->err_avg)) /
	      (state->err_wdw + 1.0);
	state->err_var = (state->err_var * state->err_wdw +
			  (err - state->err_avg) * (err - avg)) /
			 (state->err_wdw + 1.0);
	state->err_avg = avg;

	if (diff < 0)
		state->next_time +=
			(uint64_t) (diff / corr * 1e9 / state->rate);

	if ((state->next_time - state->base_time) > BW_PERIOD) {
		double bw;

		state->base_time = state->next_time;

		bw = (fabs(state->err_avg) + sqrt(fabs(state->err_var))) /
		     1000.0;

		log_debug("Clock control: correction %.5f; bandwidth %.4f; "
			  "delay %ld frames; target %ld; error %+.2f; "
			  "average %+.2f; variance %.2f",
			  corr, state->dll.bw, delay, target, err,
			  state->err_avg, state->err_var);

		spa_dll_set_bw(
			&state->dll,
			CLAMPD(bw, SPA_ALSA_DLL_BW_MIN, state->dll_bw_max),
			state->threshold, state->rate);
	}

	/* rate_match branch removed */

	state->next_time +=
		(uint64_t) (state->threshold / corr * 1e9 / state->rate);

	/* clock update removed */

	log_trace("Timing update: next wakeup %+8.3f ms; correction %.5f; "
		  "delay %6ld frames; target %6ld; error %+7.2f",
		  ((int64_t) state->next_time - (int64_t) current_time) /
			  1000000.0,
		  corr, delay, target, err);

	return 0;
}

/*
 * driver path only (following == false); the follower resync branch
 * is removed */
int alsa_write_sync(struct alsa_state *state, uint64_t current_time) {
	int res;
	snd_pcm_uframes_t avail, delay, target;
	bool following = state->following;

	if ((res = check_position_config(state, false)) < 0)
		return res;

	if ((res = get_status(state, current_time, &avail, &delay, &target)) <
	    0) {
		log_error("Could not read playback status: %s",
			  snd_strerror(res));
		state->next_time +=
			(uint64_t) (state->threshold * 1e9 / state->rate);
		return res;
	}

	if (!following && state->alsa_started &&
	    delay > target + state->max_error) {
		log_trace("Early wakeup: available %6lu frames; delay %6lu; "
			  "target %6lu; rescheduling",
			  avail, delay, target);
		if (delay > target * 3)
			delay = target * 3;
		state->next_time = current_time + (delay - target) *
							  NSEC_PER_SEC /
							  state->rate;
		return -EAGAIN;
	}
	if ((res = update_time(state, current_time, delay, target, following)) <
	    0)
		return res;

	return 0;
}

/*
 * simplified: pipewire copies the graph buffers into the mmap areas and
 * recycles them, the test writes silence; only the mmap path is kept */
int alsa_write_frames(struct alsa_state *state) {
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t frames, offset, written = 0;
	snd_pcm_sframes_t commitres;
	int res = 0;

	frames = state->buffer_frames;
	if (state->use_mmap && frames > 0) {
		if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset,
					      &frames)) < 0) {
			log_error(
				"snd_pcm_mmap_begin failed during playback: %s",
				snd_strerror(res));
			alsa_recover(state);
			return res;
		}
		snd_pcm_areas_silence(my_areas, offset, state->channels, frames,
				      state->format);
		written = frames;
	}

	if (state->use_mmap && written > 0) {
		if ((commitres = snd_pcm_mmap_commit(hndl, offset, written)) <
		    0) {
			if (commitres == -EPIPE || commitres == -ESTRPIPE) {
				log_warn("snd_pcm_mmap_commit reported an "
					 "XRUN: %s",
					 snd_strerror(commitres));
			} else {
				log_error("snd_pcm_mmap_commit failed during "
					  "playback: %s",
					  snd_strerror(commitres));
				return commitres;
			}
		}
		if (commitres > 0 && written != (snd_pcm_uframes_t) commitres) {
			log_warn("snd_pcm_mmap_commit wrote %ld frames instead "
				 "of %ld",
				 (long) commitres, (long) written);
		}
	}

	state->sample_count += written;

	if (!state->alsa_started && (written > 0 || frames == 0))
		do_start(state);

	update_sources(state, true);

	return 0;
}

/*
 * pipewire calls alsa_write_sync() here when following; the test is the
 * driver, so it only writes frames */
int spa_alsa_write(struct alsa_state *state) {
	return alsa_write_frames(state);
}

/*
 * triggers the graph; in the test the per-cycle hook plays the part of
 * the graph consumer */
static int playback_ready(struct alsa_state *state) {
	if (state->cycle_cb)
		state->cycle_cb(state, state->cycle_data);
	return 0;
}

/*
 * first do all the sync, then trigger the graph; followers and the
 * capture path are removed */
int alsa_do_wakeup_work(struct alsa_state *state, uint64_t current_time) {
	int res;

	res = alsa_write_sync(state, current_time);
	/* we can get -EAGAIN when we need to wait some more */
	if (res == -EAGAIN)
		return res;

	/* and then trigger the graph */
	playback_ready(state);

	return 0;
}

/* alsa_timer_wakeup_event()
 * called directly by the sink driver when the timerfd fires, instead of
 * through the event loop; the timerfd read may return -EAGAIN when the
 * timer was changed since the last wakeup.
 * deviation: pipewire's event handler is void; the return value (0 or
 * -EAGAIN, from alsa_do_wakeup_work()) replaces the event loop's flow
 * control so the sink driver knows whether the graph should run */
int alsa_timer_wakeup(struct alsa_state *state) {
	uint64_t expire, current_time;
	int res;

	if (state->started) {
		res = read(state->timerfd, &expire, sizeof(expire));
		if (res < 0 && errno != EAGAIN) {
			log_warn("Could not read the playback timer: %s",
				 strerror(errno));
			return -errno;
		}
	}
	current_time = state->next_time;

	res = alsa_do_wakeup_work(state, current_time);

	/* pipewire always re-arms the timer here, also on -EAGAIN (the
	 * event handler ignores the return value); the status is passed
	 * back so the sink driver can skip the write on early wakeup */
	if (state->next_time > current_time + NSEC_PER_SEC ||
	    current_time > state->next_time + NSEC_PER_SEC) {
		log_error("The computed playback wakeup is outside the valid "
			  "one-second range; resetting the timer");
		log_debug("Invalid wakeup: current time %" PRIu64
			  " ns; next time %" PRIu64 "; difference %" PRIi64
			  " ns; period %d frames; sample count %" PRIi64,
			  current_time, state->next_time,
			  state->next_time - current_time, state->threshold,
			  state->sample_count);
		state->next_time =
			(uint64_t) (current_time +
				    state->threshold * 1e9 / state->rate);
	}
	int timeout_res = set_timeout(state, state->next_time);
	return timeout_res < 0 ? timeout_res : res;
}

int spa_alsa_prepare(struct alsa_state *state) {
	int err;

	if (!state->opened)
		return -EIO;

	spa_alsa_pause(state);

	if (state->prepared)
		return 0;

	if (check_position_config(state, true) < 0) {
		log_error("The playback position configuration is invalid");
		return -EIO;
	}
	if ((err = do_prepare(state)) < 0)
		return err;

	state->prepared = true;

	return 0;
}

/*
 * tsched branch only: the timerfd is the only source; playback is started
 * by the first write unless start_delay > 0 */
int spa_alsa_start(struct alsa_state *state) {
	int err;

	if (state->started)
		return 0;
	else if (!state->opened)
		return -EIO;

	if ((err = spa_alsa_prepare(state)) < 0)
		return err;

	/* tsched mode, timer source setup is
	 * done by the sink driver in the test (deviation) */

	/* playback starts right away when
	 * disable_tsched or start_delay > 0 */
	if (state->start_delay > 0)
		if ((err = do_start(state)) < 0)
			return err;

	state->started = true;

	/* do_state_sync(): no event loop in the
	 * test, arm the timer directly (deviation) */
	state->next_time = get_time_ns();
	if ((err = set_timeout(state, state->next_time)) < 0)
		return err;

	return 0;
}

int spa_alsa_pause(struct alsa_state *state) {
	if (!state->started)
		return 0;

	state->started = false;

	/* pipewire: set_timeout(0) to disarm the timer */
	set_timeout(state, 0);

	do_drop(state);

	state->prepared = false;

	return 0;
}

int spa_alsa_close(struct alsa_state *state) {
	int err = 0;

	if (!state->opened)
		return 0;

	/* try_unlink(); the test is never linked */

	spa_alsa_pause(state);

	if ((err = snd_pcm_close(state->hndl)) < 0)
		log_warn("Could not close the playback PCM: %s",
			 snd_strerror(err));

	close(state->timerfd);

	state->opened = false;

	return err;
}

/* local helper, not from pipewire: fill alsa_state defaults */
void alsa_state_init(struct alsa_state *state) {
	memset(state, 0, sizeof(*state));
	state->hndl = NULL;
	state->timerfd = -1;
	state->dll_bw_max = SPA_DLL_BW_MAX;
	state->use_mmap = true;
	state->planar = false;
	state->frame_scale = 1;
	state->quantum_limit = DEFAULT_QUANTUM_LIMIT;
	state->use_period_size_min_as_headroom =
		DEFAULT_USE_PERIOD_SIZE_MIN_AS_HEADROOM;
	state->default_start_delay = DEFAULT_START_DELAY;
	state->htimestamp = DEFAULT_HTIMESTAMP;
	state->htimestamp_max_errors = MAX_HTIMESTAMP_ERROR;
	state->disable_batch = false;
	state->force_quantum = false;
	state->is_firewire = false;
}

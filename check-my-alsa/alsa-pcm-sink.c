/*
 * alsa-pcm-sink.c - playback stream driver for check-my-alsa.
 *
 * This is the counterpart of pipewire's spa/plugins/alsa/alsa-pcm-sink.c:
 * it drives the alsa-pcm.c state machine the way the pipewire sink node
 * drives it, minus the SPA node/port/graph plumbing. In pipewire the
 * cycle is: timerfd wakeup -> alsa_do_wakeup_work() (in the driver) ->
 * playback_ready() triggers the graph -> the graph process callback calls
 * spa_alsa_write() -> alsa_write_frames(). The test plays the part of
 * both the driver and the graph: each timer wakeup runs
 * alsa_do_wakeup_work() followed by spa_alsa_write().
 */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "alsa-pcm.h"

/* local helper, not from pipewire: configure and open the stream for the
 * pipewire playback path described in alsa-pcm.c */
int pcm_sink_open(struct alsa_state *state, const struct pcm_setup *cfg) {
	snprintf(state->name, sizeof(state->name), "%s", cfg->dev);

	state->rate = cfg->rate;
	state->channels = cfg->channels;
	state->format = cfg->format;
	state->threshold = cfg->period;
	state->default_period_size = cfg->period;
	state->driver_duration = cfg->period;
	state->driver_rate_denom = cfg->rate;

	return spa_alsa_set_format(state, cfg->rate, cfg->channels, cfg->format,
				   cfg->period);
}

/* pipewire alsa-pcm-sink.c prepare/start sequence */
int pcm_sink_start(struct alsa_state *state) {
	return spa_alsa_start(state);
}

/*
 * run one timer wakeup cycle: wait on the timerfd and process the
 * wakeup. Returns:
 *   0        one cycle done (sync + write)
 *   -EAGAIN  early wakeup, timer re-armed, nothing to write
 *   -EINTR   poll interrupted (stop requested)
 *   -ETIMEDOUT wait expired before the timer fired (duration over)
 *   other    negative alsa error from the write path
 */
int pcm_sink_iterate(struct alsa_state *state, int timeout_ms) {
	struct pollfd pfd = {
		.fd = state->timerfd,
		.events = POLLIN,
	};
	int res;

	res = poll(&pfd, 1, timeout_ms);
	if (res < 0) {
		if (errno == EINTR)
			return -EINTR;
		return -errno;
	}
	if (res == 0)
		return -ETIMEDOUT;

	/* the pipewire wakeup: alsa_do_wakeup_work() runs the sync and
	 * the cycle hook, then the graph would call spa_alsa_write() */
	res = alsa_timer_wakeup(state);
	if (res < 0)
		return res;

	return spa_alsa_write(state);
}

/* pipewire alsa-pcm-sink.c stop sequence */
int pcm_sink_stop(struct alsa_state *state) {
	spa_alsa_pause(state);
	spa_alsa_close(state);
	return 0;
}

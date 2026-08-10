/*
 * alsa-pcm.h - pipewire ALSA playback path ported into check-my-alsa.
 *
 * The functions in alsa-pcm.c mirror uos-pipewire
 * spa/plugins/alsa/alsa-pcm.c (1.6.0-1deepin9) with the same function
 * names, same snd_* call sequence, same error handling and identical log
 * message text. Log lines printed by these functions can therefore be
 * correlated one-to-one with pipewire's "alsa-pcm.c" messages when
 * debugging xrun recovery failures. Function names match pipewire's, so
 * the corresponding source can be located by name.
 *
 * Deviations from pipewire (each marked with a comment at the site in
 * alsa-pcm.c):
 *  - struct state -> struct alsa_state (see below): only the fields used
 *    by the playback + tsched (timer-scheduled) path are kept. Fields whose
 *    branches are dead in the test configuration were removed:
 *    linked/following/matching/resample, disable_tsched (the test only
 *    runs tsched), use_mmap=false, stream (playback only), capture.
 *  - graph-only parts are omitted: driver/follower list iteration, clock,
 *    spa_node_call_xrun, event loop sources, buffer queue plumbing. The
 *    spa_dll rate correction is active (see spa-dll.h); only its graph
 *    outputs (clock->rate_diff, rate_match->rate) are not wired.
 *  - spa_log_* -> stderr logging. Messages retain ALSA function and state
 *    keywords for correlation, but use human-readable prose, relative
 *    timestamps, and no state/device prefix. Raw ALSA dumps are TRACE.
 *  - spa_ratelimit suppression removed: messages are printed every time.
 *  - alsa_write_frames() writes silence instead of the graph buffers;
 *    alsa_read_frames() (the capture path) is not ported, the test only
 *    exercises playback.
 *  - the test adds a per-cycle hook (state->cycle_cb) where pipewire's
 *    graph would be triggered by playback_ready(), and an xrun notify
 *    hook (state->xrun_cb) where pipewire calls spa_node_call_xrun().
 */

#ifndef ALSA_PCM_H
#define ALSA_PCM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include <alsa/asoundlib.h>

#include "app-common.h"
#include "spa-dll.h"

#define NSEC_PER_SEC 1000000000ULL
#define USEC_PER_SEC 1000000ULL

#define TIMEVAL_TO_USEC(tv)                                                    \
	((((uint64_t) (tv)->tv_sec) * 1000000ULL) + (tv)->tv_usec)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, min, max) ((v) < (min) ? (min) : (v) > (max) ? (max) : (v))
#define CLAMPD(v, min, max) ((v) < (min) ? (min) : (v) > (max) ? (max) : (v))

/* pipewire alsa-pcm.h constants (defaults used by spa_alsa_init) */
#define DEFAULT_PERIOD 1024u
#define DEFAULT_RATE 48000u
#define MAX_HTIMESTAMP_ERROR 64
/* BW_PERIOD, used for the update_time debug print */
#define BW_PERIOD (3 * NSEC_PER_SEC)

/* pipewire defaults, see spa_alsa_init()
 * and src/daemon/pipewire.conf.in: api.alsa.htimestamp is not set in the
 * default config, so htimestamp stays disabled */
#define DEFAULT_START_DELAY 0u
#define DEFAULT_QUANTUM_LIMIT 8192u
#define DEFAULT_USE_PERIOD_SIZE_MIN_AS_HEADROOM false
#define DEFAULT_HTIMESTAMP false

/*
 * struct alsa_state - simplified stand-in for pipewire's struct state
 * (spa/plugins/alsa/alsa-pcm.h). Only the fields used by the ported
 * playback + tsched path are kept; field names match pipewire so that the
 * code stays diffable.
 */
struct alsa_state {
	snd_pcm_t *hndl;
	/* pipewire: state->name, e.g. "hw:0p" */
	char name[64];

	/* pipewire: state->timerfd, tsched only */
	int timerfd;

	/* negotiated format (pipewire: set in spa_alsa_set_format) */
	unsigned int rate;
	unsigned int channels;
	snd_pcm_format_t format;
	/* pipewire: state->frame_size, bytes per interleaved frame */
	size_t frame_size;
	uint32_t frame_scale;
	bool planar;
	bool use_mmap;

	/* negotiated hw params */
	snd_pcm_uframes_t buffer_frames;
	snd_pcm_uframes_t period_frames;
	snd_pcm_uframes_t period_size_min;
	snd_pcm_uframes_t max_delay;
	snd_pcm_uframes_t min_delay;

	/* timing configuration */
	uint32_t threshold;
	uint32_t headroom;
	uint32_t start_delay;
	uint32_t default_start_delay;
	uint32_t duration;
	uint32_t driver_duration;
	uint32_t driver_rate_denom;
	uint32_t quantum_limit;
	uint32_t default_period_size;
	uint32_t default_period_num;
	bool use_period_size_min_as_headroom;

	/* dll state (pipewire struct state dll fields); the spa_dll rate
	 * correction is active, the fields below drive update_time() */
	struct spa_dll dll;
	double dll_bw_max;
	double max_error;
	double max_resync;
	double err_avg, err_var, err_wdw;
	uint64_t next_time;
	uint64_t base_time;
	/* pipewire: state->sample_count */
	uint64_t sample_count;
	snd_pcm_sframes_t delay;
	uint32_t last_threshold;
	bool alsa_sync;
	bool alsa_sync_warning;

	/* htimestamp */
	bool htimestamp;
	uint32_t htimestamp_error;
	uint32_t htimestamp_max_errors;

	/* driver flags */
	bool opened;
	bool prepared;
	bool started;
	bool alsa_started;
	bool is_batch;
	bool disable_batch;
	bool is_firewire;
	bool force_quantum;
	/* pipewire: state->matching / state->resample / state->disable_tsched
	 * / state->have_format / state->following; always false/false/false/
	 * true/false in the test, kept for the alsa_avail() and set_format()
	 * branches */
	bool matching;
	bool resample;
	bool disable_tsched;
	bool have_format;
	bool following;

	/* pipewire: state->clock->xrun, flattened here */
	uint64_t xrun;

	/* test-only counters, no pipewire equivalent */
	unsigned int recover_fails;
	/* pipewire: "snd_pcm_avail after recover" counter (0 suppressed) */
	unsigned int avail_recover_fails;

	/* last cycle values, test-only bookkeeping (no pipewire
	 * equivalent): filled by get_status() so that the cycle hook can
	 * report them */
	snd_pcm_uframes_t last_avail;
	snd_pcm_uframes_t last_delay;
	snd_pcm_uframes_t last_target;

	/* test-only hooks, see header comment:
	 * cycle_cb: called from playback_ready(), where pipewire would
	 *            trigger the graph
	 * xrun_cb:  called from alsa_recover(), where pipewire calls
	 *           spa_node_call_xrun() */
	void (*cycle_cb)(struct alsa_state *state, void *data);
	void *cycle_data;
	void (*xrun_cb)(struct alsa_state *state, uint64_t missing, void *data);
	void *xrun_data;
};

int spa_alsa_open(struct alsa_state *state, const char *params);
int spa_alsa_close(struct alsa_state *state);
/* playback/raw/mmap/tsched subset */
int spa_alsa_set_format(struct alsa_state *state, unsigned int rate,
			unsigned int channels, snd_pcm_format_t format,
			unsigned int period_size);
int set_swparams(struct alsa_state *state);
int spa_alsa_silence(struct alsa_state *state, snd_pcm_uframes_t silence);
int do_prepare(struct alsa_state *state);
int do_drop(struct alsa_state *state);
int do_start(struct alsa_state *state);
int alsa_recover(struct alsa_state *state);
snd_pcm_sframes_t alsa_avail(struct alsa_state *state);
int get_avail(struct alsa_state *state, uint64_t current_time,
	      snd_pcm_uframes_t *delay);
int get_status(struct alsa_state *state, uint64_t current_time,
	       snd_pcm_uframes_t *avail, snd_pcm_uframes_t *delay,
	       snd_pcm_uframes_t *target);
/* DLL active; clock/rate_match plumbing omitted (no graph) */
int update_time(struct alsa_state *state, uint64_t current_time,
		snd_pcm_sframes_t delay, snd_pcm_sframes_t target,
		bool follower);
/* driver path only (not following) */
int alsa_write_sync(struct alsa_state *state, uint64_t current_time);
/* simplified: fills silence */
int alsa_write_frames(struct alsa_state *state);
int spa_alsa_write(struct alsa_state *state);
int alsa_do_wakeup_work(struct alsa_state *state, uint64_t current_time);
/* called directly by the sink driver instead of an event
 * loop; returns 0 or -EAGAIN */
int alsa_timer_wakeup(struct alsa_state *state);
int spa_alsa_prepare(struct alsa_state *state);
int spa_alsa_start(struct alsa_state *state);
int spa_alsa_pause(struct alsa_state *state);

/* alsa_read_frames() is capture-only and not ported; the
 * test only exercises playback */

/* local helper, not from pipewire: fill alsa_state defaults before
 * spa_alsa_open() */
void alsa_state_init(struct alsa_state *state);

/* alsa-pcm-sink.c: playback stream driver, the counterpart of pipewire's
 * alsa-pcm-sink.c without the SPA node plumbing */
int pcm_sink_open(struct alsa_state *state, const struct pcm_setup *cfg);
int pcm_sink_start(struct alsa_state *state);
int pcm_sink_iterate(struct alsa_state *state, int timeout_ms);
int pcm_sink_stop(struct alsa_state *state);

#endif

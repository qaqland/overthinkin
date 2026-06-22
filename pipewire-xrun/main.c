#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>

#define MAX_HISTOGRAM_BUCKETS 128

struct bench_config {
	uint32_t rate;
	uint32_t channels;
	uint32_t quantum;
	uint32_t duration_sec;
	uint32_t format;
	double threshold_1;
	double threshold_2;
	double threshold_3;
};

struct rt_stats {
	atomic_uint_least64_t callback_count;
	atomic_uint_least64_t last_callback_ns;

	atomic_uint_least64_t sum_ns;
	atomic_uint_least64_t sum_sq_ns;
	atomic_uint_least64_t max_ns;
	uint64_t p95_ns;
	uint64_t p99_ns;

	atomic_uint_least64_t over_threshold_1;
	atomic_uint_least64_t over_threshold_2;
	atomic_uint_least64_t over_threshold_3;

	atomic_uint_least64_t dequeue_fail;
	atomic_uint_least64_t null_buffer;
	atomic_uint_least64_t state_errors;
	atomic_uint_least64_t zero_interval;

	atomic_uint_least64_t histogram[MAX_HISTOGRAM_BUCKETS];

	uint64_t *raw_intervals;
	uint64_t raw_capacity;
	atomic_uint_least64_t raw_count;

	uint64_t test_start_ns;
	uint64_t test_end_ns;

	atomic_uint_least32_t actual_rate;
	atomic_uint_least32_t actual_quantum;
	atomic_uint_least32_t actual_format;
};

struct synth_state {
	double phase;
	double phase_increment;
	double freq;
};

struct bench_context {
	struct bench_config config;
	struct rt_stats stats;

	struct pw_main_loop *loop;
	struct pw_stream *stream;

	struct synth_state synth;

	atomic_bool should_stop;
	uint64_t run_start_ns;
	int remaining_width;
};

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void synth_init(struct synth_state *s, uint32_t rate, double freq) {
	s->phase = 0.0;
	s->freq = freq;
	s->phase_increment = 2.0 * M_PI * freq / (double) rate;
}

static void synth_fill_f32(struct synth_state *s, float *dst, uint32_t n_frames,
			   uint32_t channels) {
	for (uint32_t i = 0; i < n_frames; i++) {
		float sample = (float) sin(s->phase);
		s->phase += s->phase_increment;
		if (s->phase >= 2.0 * M_PI)
			s->phase -= 2.0 * M_PI;

		for (uint32_t c = 0; c < channels; c++)
			dst[i * channels + c] = sample;
	}
}

static void synth_fill_s16(struct synth_state *s, int16_t *dst,
			   uint32_t n_frames, uint32_t channels) {
	for (uint32_t i = 0; i < n_frames; i++) {
		int16_t sample = (int16_t) (sin(s->phase) * 32767.0);
		s->phase += s->phase_increment;
		if (s->phase >= 2.0 * M_PI)
			s->phase -= 2.0 * M_PI;

		for (uint32_t c = 0; c < channels; c++)
			dst[i * channels + c] = sample;
	}
}

static uint32_t format_sample_size(uint32_t format) {
	switch (format) {
	case SPA_AUDIO_FORMAT_S16_LE:
	case SPA_AUDIO_FORMAT_S16_BE:
	case SPA_AUDIO_FORMAT_U16_LE:
	case SPA_AUDIO_FORMAT_U16_BE:
		return 2;
	case SPA_AUDIO_FORMAT_F32_LE:
	case SPA_AUDIO_FORMAT_F32_BE:
	case SPA_AUDIO_FORMAT_S32_LE:
	case SPA_AUDIO_FORMAT_S32_BE:
	case SPA_AUDIO_FORMAT_U32_LE:
	case SPA_AUDIO_FORMAT_U32_BE:
	case SPA_AUDIO_FORMAT_S24_32_LE:
	case SPA_AUDIO_FORMAT_S24_32_BE:
	case SPA_AUDIO_FORMAT_U24_32_LE:
	case SPA_AUDIO_FORMAT_U24_32_BE:
		return 4;
	case SPA_AUDIO_FORMAT_F64_LE:
	case SPA_AUDIO_FORMAT_F64_BE:
		return 8;
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_U8:
		return 1;
	default:
		return 0;
	}
}

static void stats_alloc(struct rt_stats *s, const struct bench_config *cfg) {
	uint64_t max_callbacks =
		(uint64_t) cfg->duration_sec * cfg->rate / cfg->quantum + 4096;

	s->raw_intervals = calloc(max_callbacks, sizeof(uint64_t));
	s->raw_capacity = max_callbacks;
}

static void stats_free(struct rt_stats *s) {
	free(s->raw_intervals);
	s->raw_intervals = NULL;
	s->raw_capacity = 0;
}

static void stats_record_interval(struct rt_stats *s, uint64_t interval_ns,
				  const struct bench_config *cfg) {
	double expected_period_ns =
		1e9 * (double) cfg->quantum / (double) cfg->rate;

	atomic_fetch_add(&s->callback_count, 1);
	atomic_fetch_add(&s->sum_ns, interval_ns);
	atomic_fetch_add(&s->sum_sq_ns, interval_ns * interval_ns);

	if (interval_ns == 0)
		atomic_fetch_add(&s->zero_interval, 1);

	uint64_t old_max;
	do {
		old_max = atomic_load(&s->max_ns);
	} while (interval_ns > old_max &&
		 !atomic_compare_exchange_weak(&s->max_ns, &old_max,
					       interval_ns));

	if (interval_ns > (uint64_t) (expected_period_ns * cfg->threshold_3))
		atomic_fetch_add(&s->over_threshold_3, 1);
	else if (interval_ns >
		 (uint64_t) (expected_period_ns * cfg->threshold_2))
		atomic_fetch_add(&s->over_threshold_2, 1);
	else if (interval_ns >
		 (uint64_t) (expected_period_ns * cfg->threshold_1))
		atomic_fetch_add(&s->over_threshold_1, 1);

	uint64_t bucket = (uint64_t) (interval_ns * 16.0 / expected_period_ns);
	if (bucket >= MAX_HISTOGRAM_BUCKETS)
		bucket = MAX_HISTOGRAM_BUCKETS - 1;
	atomic_fetch_add(&s->histogram[bucket], 1);

	uint64_t idx = atomic_fetch_add(&s->raw_count, 1);
	if (idx < s->raw_capacity)
		s->raw_intervals[idx] = interval_ns;
}

static int cmp_u64(const void *a, const void *b) {
	uint64_t x = *(const uint64_t *) a;
	uint64_t y = *(const uint64_t *) b;
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

static void stats_compute_percentiles(struct rt_stats *s) {
	uint64_t count = atomic_load(&s->raw_count);
	if (count > s->raw_capacity)
		count = s->raw_capacity;

	if (count == 0) {
		s->p95_ns = 0;
		s->p99_ns = 0;
		return;
	}

	qsort(s->raw_intervals, count, sizeof(uint64_t), cmp_u64);

	uint64_t p95_idx = (count - 1) * 95 / 100;
	uint64_t p99_idx = (count - 1) * 99 / 100;
	s->p95_ns = s->raw_intervals[p95_idx];
	s->p99_ns = s->raw_intervals[p99_idx];
}

static void report_results(const struct bench_context *ctx) {
	const struct bench_config *cfg = &ctx->config;
	const struct rt_stats *s = &ctx->stats;

	uint64_t callback_count = atomic_load(&s->callback_count);
	double expected_period_us =
		1e6 * (double) cfg->quantum / (double) cfg->rate;

	uint64_t sum_ns = atomic_load(&s->sum_ns);
	uint64_t max_ns = atomic_load(&s->max_ns);
	uint64_t p95_ns = s->p95_ns;
	uint64_t p99_ns = s->p99_ns;

	uint64_t over_1 = atomic_load(&s->over_threshold_1);
	uint64_t over_2 = atomic_load(&s->over_threshold_2);
	uint64_t over_3 = atomic_load(&s->over_threshold_3);

	uint64_t dequeue_fail = atomic_load(&s->dequeue_fail);
	uint64_t null_buffer = atomic_load(&s->null_buffer);
	uint64_t state_errors = atomic_load(&s->state_errors);

	uint32_t actual_rate = atomic_load(&s->actual_rate);
	uint32_t actual_quantum = atomic_load(&s->actual_quantum);

	if (actual_rate == 0)
		actual_rate = cfg->rate;
	if (actual_quantum == 0)
		actual_quantum = cfg->quantum;

	double avg_us = callback_count > 0
				? (double) sum_ns / callback_count / 1000.0
				: 0.0;

	double pct_1 = callback_count > 0 ? 100.0 * (double) over_1 /
						    (double) callback_count
					  : 0.0;
	double pct_2 = callback_count > 0 ? 100.0 * (double) over_2 /
						    (double) callback_count
					  : 0.0;
	double pct_3 = callback_count > 0 ? 100.0 * (double) over_3 /
						    (double) callback_count
					  : 0.0;

	printf("PipeWire Playback Benchmark Summary\n");
	printf("====================================\n");
	printf("Duration:           %.3f s\n",
	       (double) (s->test_end_ns - s->test_start_ns) / 1e9);
	uint32_t actual_format = atomic_load(&s->actual_format);
	const char *actual_format_name =
		actual_format != 0
			? spa_type_audio_format_to_short_name(actual_format)
			: "UNKNOWN";

	printf("Requested rate:     %u Hz\n", cfg->rate);
	printf("Requested quantum:  %u frames\n", cfg->quantum);
	printf("Requested format:   %s\n",
	       spa_type_audio_format_to_short_name(cfg->format));
	printf("Actual rate:        %u Hz\n", actual_rate);
	printf("Actual quantum:     %u frames\n", actual_quantum);
	printf("Actual format:      %s\n", actual_format_name);
	printf("Theoretical period: %.2f us\n", expected_period_us);
	printf("\n");
	printf("Callback statistics:\n");
	printf("  Total callbacks:  %lu\n", (unsigned long) callback_count);
	printf("  Avg interval:     %.2f us\n", avg_us);
	printf("  p95 interval:     %.2f us\n", (double) p95_ns / 1000.0);
	printf("  p99 interval:     %.2f us\n", (double) p99_ns / 1000.0);
	printf("  Max interval:     %.2f us\n", (double) max_ns / 1000.0);
	printf("\n");
	printf("Threshold violations:\n");
	printf("  >%.1fx period:    %lu  (%.2f%%)\n", cfg->threshold_1,
	       (unsigned long) over_1, pct_1);
	printf("  >%.1fx period:    %lu  (%.2f%%)\n", cfg->threshold_2,
	       (unsigned long) over_2, pct_2);
	printf("  >%.1fx period:    %lu  (%.2f%%)\n", cfg->threshold_3,
	       (unsigned long) over_3, pct_3);
	printf("\n");
	printf("Errors:\n");
	printf("  dequeue failures: %lu\n", (unsigned long) dequeue_fail);
	printf("  null buffers:     %lu\n", (unsigned long) null_buffer);
	printf("  state errors:     %lu\n", (unsigned long) state_errors);
}

static void on_signal(void *data, int signal_number) {
	(void) signal_number;
	struct bench_context *ctx = data;
	atomic_store(&ctx->should_stop, true);
	pw_main_loop_quit(ctx->loop);
}

static void on_duration_timeout(void *data, uint64_t expirations) {
	(void) expirations;
	struct bench_context *ctx = data;
	atomic_store(&ctx->should_stop, true);
	pw_main_loop_quit(ctx->loop);
}

static void on_progress_tick(void *data, uint64_t expirations) {
	(void) expirations;
	struct bench_context *ctx = data;

	uint64_t now = now_ns();
	uint64_t elapsed_ns = now - ctx->run_start_ns;
	uint64_t total_ns = (uint64_t) ctx->config.duration_sec * 1000000000ULL;
	uint64_t remaining_ns =
		elapsed_ns < total_ns ? total_ns - elapsed_ns : 0;
	int remaining_sec = (int) (remaining_ns / 1000000000ULL);

	printf("\rremaining: %*d s", ctx->remaining_width, remaining_sec);
	fflush(stdout);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
				    enum pw_stream_state state,
				    const char *error) {
	(void) old;
	struct bench_context *ctx = data;

	if (state == PW_STREAM_STATE_ERROR) {
		atomic_fetch_add(&ctx->stats.state_errors, 1);
		fprintf(stderr, "stream error: %s\n",
			error ? error : "unknown");
		pw_main_loop_quit(ctx->loop);
	}
}

static void on_param_changed(void *data, uint32_t id,
			     const struct spa_pod *param) {
	struct bench_context *ctx = data;

	if (param == NULL)
		return;

	if (id == SPA_PARAM_Format) {
		struct spa_audio_info_raw info;
		if (spa_format_audio_raw_parse(param, &info) >= 0) {
			atomic_store(&ctx->stats.actual_rate, info.rate);
			atomic_store(&ctx->stats.actual_format, info.format);
		}
	}
}

static void on_process(void *data) {
	struct bench_context *ctx = data;
	struct bench_config *cfg = &ctx->config;
	struct rt_stats *s = &ctx->stats;

	uint64_t now = now_ns();

	struct pw_buffer *b = pw_stream_dequeue_buffer(ctx->stream);
	if (b == NULL) {
		atomic_fetch_add(&s->dequeue_fail, 1);
		goto record;
	}

	struct spa_buffer *buf = b->buffer;
	if (buf->datas[0].data == NULL) {
		atomic_fetch_add(&s->null_buffer, 1);
		pw_stream_queue_buffer(ctx->stream, b);
		goto record;
	}

	uint32_t format = atomic_load(&s->actual_format);
	if (format == 0)
		format = cfg->format;

	uint32_t sample_size = format_sample_size(format);
	if (sample_size == 0)
		sample_size = sizeof(float);

	uint32_t stride = sample_size * cfg->channels;
	uint32_t max_frames = buf->datas[0].maxsize / stride;
	uint32_t n_frames = max_frames;
	if (b->requested > 0 && b->requested < n_frames)
		n_frames = (uint32_t) b->requested;

	switch (format) {
	case SPA_AUDIO_FORMAT_S16_LE:
	case SPA_AUDIO_FORMAT_S16_BE:
		synth_fill_s16(&ctx->synth, buf->datas[0].data, n_frames,
			       cfg->channels);
		break;
	default:
		synth_fill_f32(&ctx->synth, buf->datas[0].data, n_frames,
			       cfg->channels);
		break;
	}

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n_frames * stride;
	pw_stream_queue_buffer(ctx->stream, b);

record: {
	uint64_t last = atomic_exchange(&s->last_callback_ns, now);
	if (last != 0) {
		uint64_t interval = now - last;
		stats_record_interval(s, interval, cfg);
	} else {
		s->test_start_ns = now;
	}
}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static int stream_create(struct bench_context *ctx) {
	char latency_str[64];
	char force_quantum_str[64];
	char force_rate_str[64];

	snprintf(latency_str, sizeof(latency_str), "%u/%u", ctx->config.quantum,
		 ctx->config.rate);
	snprintf(force_quantum_str, sizeof(force_quantum_str), "%u",
		 ctx->config.quantum);
	snprintf(force_rate_str, sizeof(force_rate_str), "%u",
		 ctx->config.rate);

	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, "pipewire-xrun",
		PW_KEY_NODE_LATENCY, latency_str, PW_KEY_NODE_FORCE_QUANTUM,
		force_quantum_str, PW_KEY_NODE_FORCE_RATE, force_rate_str,
		NULL);

	ctx->stream = pw_stream_new_simple(pw_main_loop_get_loop(ctx->loop),
					   "pipewire-xrun", props,
					   &stream_events, ctx);

	if (!ctx->stream)
		return -errno;

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];

	params[0] = spa_format_audio_raw_build(
		&b, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(.format = ctx->config.format,
					 .channels = ctx->config.channels,
					 .rate = ctx->config.rate));

	int res = pw_stream_connect(ctx->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				    PW_STREAM_FLAG_AUTOCONNECT |
					    PW_STREAM_FLAG_MAP_BUFFERS |
					    PW_STREAM_FLAG_RT_PROCESS,
				    params, 1);

	return res;
}

static int bench_init(struct bench_context *ctx) {
	pw_init(NULL, NULL);

	stats_alloc(&ctx->stats, &ctx->config);

	synth_init(&ctx->synth, ctx->config.rate, 440.0);

	ctx->loop = pw_main_loop_new(NULL);
	if (!ctx->loop)
		return -errno;

	pw_loop_add_signal(pw_main_loop_get_loop(ctx->loop), SIGINT, on_signal,
			   ctx);
	pw_loop_add_signal(pw_main_loop_get_loop(ctx->loop), SIGTERM, on_signal,
			   ctx);

	if (stream_create(ctx) < 0)
		return -errno;

	return 0;
}

static void bench_fini(struct bench_context *ctx) {
	if (ctx->stream) {
		pw_stream_destroy(ctx->stream);
		ctx->stream = NULL;
	}

	if (ctx->loop) {
		pw_main_loop_destroy(ctx->loop);
		ctx->loop = NULL;
	}

	pw_deinit();

	stats_free(&ctx->stats);
}

static int bench_run(struct bench_context *ctx) {
	uint64_t timeout_ns =
		(uint64_t) ctx->config.duration_sec * 1000000000ULL;
	ctx->run_start_ns = now_ns();
	ctx->remaining_width =
		snprintf(NULL, 0, "%d", ctx->config.duration_sec);

	struct pw_loop *loop = pw_main_loop_get_loop(ctx->loop);

	struct spa_source *duration_timer =
		pw_loop_add_timer(loop, on_duration_timeout, ctx);
	if (!duration_timer)
		return -errno;

	struct spa_source *progress_timer =
		pw_loop_add_timer(loop, on_progress_tick, ctx);
	if (!progress_timer) {
		pw_loop_destroy_source(loop, duration_timer);
		return -errno;
	}

	struct timespec ts;
	memset(&ts, 0, sizeof(ts));
	ts.tv_sec = (time_t) (timeout_ns / 1000000000ULL);
	ts.tv_nsec = (long) (timeout_ns % 1000000000ULL);
	pw_loop_update_timer(loop, duration_timer, &ts, NULL, false);

	struct timespec interval_ts;
	memset(&interval_ts, 0, sizeof(interval_ts));
	interval_ts.tv_sec = 1;
	pw_loop_update_timer(loop, progress_timer, &interval_ts, &interval_ts,
			     false);

	printf("\rremaining: %*d s", ctx->remaining_width,
	       ctx->config.duration_sec);
	fflush(stdout);

	pw_main_loop_run(ctx->loop);

	ctx->stats.test_end_ns = now_ns();

	pw_loop_destroy_source(loop, progress_timer);
	pw_loop_destroy_source(loop, duration_timer);

	printf("\n");
	return 0;
}

static void print_usage(const char *prog) {
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("Options:\n");
	printf("  -r RATE        Sample rate in Hz (default: 48000)\n");
	printf("  -c CHANNELS    Number of channels (default: 2)\n");
	printf("  -q QUANTUM     Requested quantum in frames (default: "
	       "1024)\n");
	printf("  -d SECONDS     Test duration in seconds (default: 30)\n");
	printf("  -f FORMAT      Sample format: F32, S16LE, S16, etc. "
	       "(default: F32)\n");
	printf("  -h             Show this help message\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s -r 48000 -q 256 -d 60\n", prog);
	printf("  %s -q 64 -d 30\n", prog);
	printf("  %s -f S16LE -q 256 -d 10\n", prog);
}

static bool is_power_of_two(uint32_t x) {
	// 100...0
	// 011...1
	return x != 0 && (x & (x - 1)) == 0;
}

static uint32_t parse_format(const char *name) {
	char upper[32];
	size_t i;
	for (i = 0; i < sizeof(upper) - 1 && name[i]; i++)
		upper[i] = (char) toupper((unsigned char) name[i]);
	upper[i] = '\0';
	return spa_type_audio_format_from_short_name(upper);
}

static int parse_args(struct bench_config *cfg, int argc, char **argv) {
	cfg->rate = 48000;
	cfg->channels = 2;
	cfg->quantum = 1024;
	cfg->duration_sec = 30;
	cfg->format = SPA_AUDIO_FORMAT_F32;
	cfg->threshold_1 = 1.1;
	cfg->threshold_2 = 1.5;
	cfg->threshold_3 = 2.0;

	int opt;
	while ((opt = getopt(argc, argv, "r:c:q:d:f:h")) != -1) {
		switch (opt) {
		case 'r':
			cfg->rate = (uint32_t) atoi(optarg);
			break;
		case 'c':
			cfg->channels = (uint32_t) atoi(optarg);
			break;
		case 'q':
			cfg->quantum = (uint32_t) atoi(optarg);
			break;
		case 'd':
			cfg->duration_sec = (uint32_t) atoi(optarg);
			break;
		case 'f': {
			uint32_t fmt = parse_format(optarg);
			if (fmt == SPA_AUDIO_FORMAT_UNKNOWN) {
				fprintf(stderr, "error: unknown format '%s'\n",
					optarg);
				return -1;
			}
			cfg->format = fmt;
			break;
		}
		case 'h':
			print_usage(argv[0]);
			exit(0);
		default:
			print_usage(argv[0]);
			return -1;
		}
	}

	return 0;
}

static int validate_config(const struct bench_config *cfg) {
	if (cfg->rate == 0 || cfg->channels == 0 || cfg->quantum == 0 ||
	    cfg->duration_sec == 0) {
		fprintf(stderr, "error: rate, channels, quantum and duration "
				"must be non-zero\n");
		return -1;
	}

	if (!is_power_of_two(cfg->quantum)) {
		fprintf(stderr, "error: quantum must be a power of two\n");
		return -1;
	}

	if (format_sample_size(cfg->format) == 0) {
		fprintf(stderr, "error: unsupported sample format '%s'\n",
			spa_type_audio_format_to_short_name(cfg->format));
		return -1;
	}

	return 0;
}

int main(int argc, char **argv) {
	struct bench_context ctx = {0};

	if (parse_args(&ctx.config, argc, argv) < 0)
		return 1;

	if (validate_config(&ctx.config) < 0)
		return 1;

	if (bench_init(&ctx) < 0) {
		fprintf(stderr, "error: failed to initialize benchmark\n");
		return 1;
	}

	if (bench_run(&ctx) < 0) {
		fprintf(stderr, "error: failed to run benchmark\n");
		bench_fini(&ctx);
		return 1;
	}

	stats_compute_percentiles(&ctx.stats);
	report_results(&ctx);

	bench_fini(&ctx);

	uint64_t state_errors = atomic_load(&ctx.stats.state_errors);
	uint64_t over_3 = atomic_load(&ctx.stats.over_threshold_3);

	return (state_errors > 0 || over_3 > 0) ? 1 : 0;
}

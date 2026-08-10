/* Capability probing mirrors the PipeWire alsa plugin sources:
 *
 *   card info + device enumeration -> spa/plugins/alsa/alsa-pcm-device.c
 *       emit_info()        card props
 *       activate_profile() device loop
 *   per-device probe        -> spa/plugins/alsa/test-hw-params.c
 *       main()
 *
 * PipeWire's runtime enumeration (alsa-udev.c) never opens the PCM; the
 * hw-params probe below is the test-hw-params.c diagnostic, not part of
 * the enumeration path.
 */

#include <stdint.h>
#include <stdio.h>

#include <alsa/asoundlib.h>

#include "app-common.h"

static const char *get_class(snd_pcm_class_t c) {
	switch (c) {
	case SND_PCM_CLASS_GENERIC:
		return "generic";
	case SND_PCM_CLASS_MULTI:
		return "multichannel";
	case SND_PCM_CLASS_MODEM:
		return "modem";
	case SND_PCM_CLASS_DIGITIZER:
		return "digitizer";
	default:
		return "unknown";
	}
}

static const char *get_subclass(snd_pcm_subclass_t c) {
	switch (c) {
	case SND_PCM_SUBCLASS_GENERIC_MIX:
		return "generic-mix";
	case SND_PCM_SUBCLASS_MULTI_MIX:
		return "multichannel-mix";
	default:
		return "unknown";
	}
}

static int print_info(snd_pcm_t *pcm, struct report_tab *t) {
	snd_pcm_info_t *info;
	snd_pcm_info_alloca(&info);
	int err = snd_pcm_info(pcm, info);
	if (err < 0)
		return err;

	report_kv(t, "ID", "'%s'", snd_pcm_info_get_id(info));
	report_kv(t, "Name", "'%s'", snd_pcm_info_get_name(info));
	report_kv(t, "Subdevice name", "'%s'",
		  snd_pcm_info_get_subdevice_name(info));
	report_kv(t, "Class", "%s", get_class(snd_pcm_info_get_class(info)));
	report_kv(t, "Subclass", "%s",
		  get_subclass(snd_pcm_info_get_subclass(info)));

	snd_pcm_sync_id_t sync = snd_pcm_info_get_sync(info);
	report_kv(t, "Sync ID", "%08x:%08x:%08x:%08x", sync.id32[0],
		  sync.id32[1], sync.id32[2], sync.id32[3]);
	return 0;
}

static void print_format_mask(struct report_tab *t, snd_pcm_hw_params_t *hw) {
	snd_pcm_format_mask_t *mask;
	snd_pcm_format_mask_alloca(&mask);
	snd_pcm_hw_params_get_format_mask(hw, mask);

	char buf[REPORT_VAL_MAX] = "";
	size_t used = 0;
	for (int fmt = 0; fmt <= SND_PCM_FORMAT_LAST; fmt++) {
		if (!snd_pcm_format_mask_test(mask, (snd_pcm_format_t) fmt))
			continue;
		const char *name = snd_pcm_format_name((snd_pcm_format_t) fmt);
		int n = snprintf(buf + used, sizeof(buf) - used, "%s%s",
				 used ? ", " : "", name);
		if (n < 0 || (size_t) n >= sizeof(buf) - used)
			break;
		used += (size_t) n;
	}
	report_kv(t, "Formats", "%s", buf);
}

static int probe_one(const char *dev, bool capture) {
	snd_pcm_t *pcm;
	snd_pcm_stream_t stream =
		capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
	int err;
	log_debug("Probing %s capabilities for %s",
		  capture ? "capture" : "playback", dev);
	err = snd_pcm_open(&pcm, dev, stream, 0);
	if (err < 0) {
		log_error("Could not open %s PCM %s: %s",
			  capture ? "capture" : "playback", dev,
			  snd_strerror(err));
		report_note("this stream could not be probed");
		return err;
	}

	/* one table per stream */
	struct report_tab *t = report_tab_begin();
	err = print_info(pcm, t);
	if (err < 0) {
		log_error("Could not read information for %s PCM %s: %s",
			  capture ? "capture" : "playback", dev,
			  snd_strerror(err));
		report_note("this stream could not be probed");
		goto error_close;
	}

	/* readable rendering of the snd_pcm_hw_params_dump() output */
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	err = snd_pcm_hw_params_any(pcm, hw);
	if (err < 0) {
		log_error(
			"Could not query hardware parameters for %s PCM %s: %s",
			capture ? "capture" : "playback", dev,
			snd_strerror(err));
		report_note("this stream could not be probed");
		goto error_close;
	}

	unsigned int ch_min, ch_max;
	snd_pcm_hw_params_get_channels_min(hw, &ch_min);
	snd_pcm_hw_params_get_channels_max(hw, &ch_max);

	unsigned int r_min, r_max;
	int dir;
	snd_pcm_hw_params_get_rate_min(hw, &r_min, &dir);
	snd_pcm_hw_params_get_rate_max(hw, &r_max, &dir);

	snd_pcm_uframes_t p_min, p_max, b_min, b_max;
	snd_pcm_hw_params_get_period_size_min(hw, &p_min, &dir);
	snd_pcm_hw_params_get_period_size_max(hw, &p_max, &dir);
	snd_pcm_hw_params_get_buffer_size_min(hw, &b_min);
	snd_pcm_hw_params_get_buffer_size_max(hw, &b_max);

	/* hw params */
	report_kv(t, "Channels", "%u - %u", ch_min, ch_max);
	report_kv(t, "Rate", "%u - %u Hz", r_min, r_max);
	print_format_mask(t, hw);
	report_kv(t, "Period size", "%lu - %lu frames", p_min, p_max);
	report_kv(t, "Buffer size", "%lu - %lu frames", b_min, b_max);
	report_kv(t, "Batch mode", "%s",
		  snd_pcm_hw_params_is_batch(hw) ? "yes" : "no");
	report_kv(t, "Can pause", "%s",
		  snd_pcm_hw_params_can_pause(hw) ? "yes" : "no");
	report_kv(t, "Can resume", "%s",
		  snd_pcm_hw_params_can_resume(hw) ? "yes" : "no");
	report_tab_end(t);

	snd_pcm_close(pcm);
	return 0;

error_close:
	snd_pcm_close(pcm);
	return err;
}

static void caps_usage(const char *prog) {
	usage_header(prog, "[OPTION]...");
	usage_opt("-c CARD", "card number, device 'hw:N' (required)");
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

/* per-stream section header; the subdevices note is only shown when it
 * carries information (multiple subdevices, or some are busy) */
static void device_header(int card, int dev, const char *stream,
			  const char *name, unsigned int count,
			  unsigned int avail) {
	if (count > 1 || avail < count)
		report_section("hw:%d,%d [%s] '%s'  subdevices: %u total, %u "
			       "available",
			       card, dev, stream, name, count, avail);
	else
		report_section("hw:%d,%d [%s] '%s'", card, dev, stream, name);
}

static int caps_run(int argc, char **argv) {
	int card = -1; /* -c is required */
	int verbose = 0;
	int opt;

	while ((opt = getopt(argc, argv, CARD_OPTSTRING COMMON_OPTSTRING)) !=
	       -1) {
		switch (opt) {
		case 'c':
			card = parse_card(optarg);
			if (card < 0) {
				caps_usage(argv[0]);
				return 2;
			}
			break;
		case 'v':
			verbose++;
			break;
		case 'h':
			caps_usage(argv[0]);
			return 0;
		default:
			caps_usage(argv[0]);
			return 2;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		caps_usage(argv[0]);
		return 2;
	}
	if (card < 0) {
		fprintf(stderr, "missing required -c card argument\n");
		caps_usage(argv[0]);
		return 2;
	}
	log_set_verbose(verbose);

	snd_ctl_t *ctl;
	char devname[32];
	snprintf(devname, sizeof(devname), "hw:%d", card);
	int err;
	CHECK(snd_ctl_open(&ctl, devname, 0), "snd_ctl_open");
	log_info("Probing ALSA card %d", card);

	snd_ctl_card_info_t *info;
	snd_ctl_card_info_alloca(&info);
	CHECK(snd_ctl_card_info(ctl, info), "snd_ctl_card_info");

	/* card props */
	report_section("Card hw:%d", card);
	struct report_tab *t = report_tab_begin();
	report_kv(t, "ID", "%s", snd_ctl_card_info_get_id(info));
	report_kv(t, "Driver", "%s", snd_ctl_card_info_get_driver(info));
	report_kv(t, "Name", "%s", snd_ctl_card_info_get_name(info));
	report_kv(t, "Long name", "%s", snd_ctl_card_info_get_longname(info));
	report_kv(t, "Components", "%s",
		  snd_ctl_card_info_get_components(info));
	report_kv(t, "Mixer name", "%s", snd_ctl_card_info_get_mixername(info));
	report_tab_end(t);

	/* device loop */
	int dev = -1;
	int device_count = 0;
	int stream_count = 0;
	int probe_failures = 0;
	snd_pcm_info_t *pcminfo;
	snd_pcm_info_alloca(&pcminfo);

	while ((err = snd_ctl_pcm_next_device(ctl, &dev)) >= 0 && dev >= 0) {
		device_count++;
		char pcmname[32];
		snprintf(pcmname, sizeof(pcmname), "hw:%d,%d", card, dev);
		snd_pcm_info_set_device(pcminfo, dev);
		snd_pcm_info_set_subdevice(pcminfo, 0);

		snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
		if (snd_ctl_pcm_info(ctl, pcminfo) >= 0) {
			stream_count++;
			device_header(
				card, dev, "playback",
				snd_pcm_info_get_name(pcminfo), // Mi Monitor
				snd_pcm_info_get_subdevices_count(pcminfo),
				snd_pcm_info_get_subdevices_avail(pcminfo));
			err = probe_one(pcmname, false);
			if (err < 0)
				probe_failures++;
		}

		snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);
		if (snd_ctl_pcm_info(ctl, pcminfo) >= 0) {
			stream_count++;
			device_header(
				card, dev, "capture",
				snd_pcm_info_get_name(pcminfo),
				snd_pcm_info_get_subdevices_count(pcminfo),
				snd_pcm_info_get_subdevices_avail(pcminfo));
			err = probe_one(pcmname, true);
			if (err < 0)
				probe_failures++;
		}
	}
	if (err < 0) {
		log_error("Could not enumerate PCM devices on card %d: %s",
			  card, snd_strerror(err));
		snd_ctl_close(ctl);
		return err;
	}
	if (device_count == 0)
		report_note("no PCM devices found on %s", devname);
	else if (probe_failures > 0)
		report_warn("%d of %d PCM streams could not be probed",
			    probe_failures, stream_count);
	else
		report_ok("probed %d PCM streams", stream_count);

	snd_ctl_close(ctl);
	return probe_failures > 0 ? 1 : 0;
}

static struct applet caps_applet = {
	.name = "caps",
	.desc = "probe card hardware capabilities",
	.main = caps_run,
	.usage = caps_usage,
	.next = NULL,
};

APPLET_REGISTER(caps_applet);

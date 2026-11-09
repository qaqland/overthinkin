/* HDMI/DP output probing mirrors the PipeWire alsa plugin sources:
 *
 *   ELD parsing          -> spa/plugins/alsa/acp/alsa-util.c
 *       pa_alsa_get_hdmi_eld()
 *   ELD binding          -> spa/plugins/alsa/acp/acp.c
 *       init_eld_ctls(), hdmi_eld_changed()
 *   port availability    -> spa/plugins/alsa/acp/acp.c
 *       report_jack_state(), calc_port_state()
 *
 * PipeWire reaches the ELD control and the port's jack through the ACP
 * port/path structures built from a mixer profile; we find both by name
 * on the raw card control list and rebuild the jack name the way
 * jack_probe() does from the "[Jack HDMI/DP]" section of the
 * hdmi-output paths (alsa-mixer.c).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "app-common.h"

#define MAX_HDMI_OUTPUTS 16
#define ELD_MAX_SIZE 256
#define ELD_NAME_MAX 16
#define ELD_NAME_OFFSET 20

enum availability {
	AVAIL_UNKNOWN,
	AVAIL_YES,
	AVAIL_NO,
};

enum iec958_codec {
	CODEC_UNKNOWN,
	CODEC_PCM,
	CODEC_DTS,
	CODEC_AC3,
	CODEC_MPEG,
	CODEC_MPEG2_AAC,
	CODEC_EAC3,
	CODEC_TRUEHD,
	CODEC_DTSHD,
	CODEC_LAST = CODEC_DTSHD,
};

static const char *const codec_names[] = {
	[CODEC_UNKNOWN] = "UNKNOWN", [CODEC_PCM] = "PCM",
	[CODEC_DTS] = "DTS",	     [CODEC_AC3] = "AC3",
	[CODEC_MPEG] = "MPEG",	     [CODEC_MPEG2_AAC] = "MPEG2-AAC",
	[CODEC_EAC3] = "EAC3",	     [CODEC_TRUEHD] = "TrueHD",
	[CODEC_DTSHD] = "DTS-HD",
};

struct eld_info {
	bool valid;
	char monitor_name[ELD_NAME_MAX + 1];
	unsigned int speakers;
	unsigned int lpcm_channels;
	uint64_t codecs;
};

struct jack_ref {
	unsigned int numid;
	int device;
	char name[64];
	bool plugged;
};

struct hdmi_output {
	int device;
	unsigned int eld_numid;
	struct eld_info eld;
	const struct jack_ref *jack;
	enum availability avail;
};

static const char *avail_str(enum availability avail) {
	switch (avail) {
	case AVAIL_YES:
		return "yes";
	case AVAIL_NO:
		return "no";
	default:
		return "unknown";
	}
}

static bool codec_detected(const struct eld_info *eld,
			   enum iec958_codec codec) {
	return (eld->codecs & (1ULL << codec)) != 0;
}

static void append_str(char *buf, size_t size, size_t *used, const char *str) {
	size_t len = strlen(str);

	if (*used + len >= size)
		return;
	memcpy(buf + *used, str, len);
	*used += len;
	buf[*used] = '\0';
}

/* acp_iec958_codec_mask_to_json() */
static void format_codecs(uint64_t codecs, char *buf, size_t size) {
	size_t used = 0;

	buf[0] = '\0';
	append_str(buf, size, &used, "[");
	for (int c = CODEC_PCM; c <= CODEC_LAST; c++) {
		if (!(codecs & (1ULL << c)))
			continue;
		if (used > 1)
			append_str(buf, size, &used, ",");
		append_str(buf, size, &used, "\"");
		append_str(buf, size, &used, codec_names[c]);
		append_str(buf, size, &used, "\"");
	}
	append_str(buf, size, &used, "]");
}

/* channel order and speaker allocation bits as handled by
 * hdmi_eld_changed() */
static void format_positions(unsigned int speakers, char *buf, size_t size) {
	static const struct {
		unsigned int bit;
		const char *name[2];
	} map[] = {
		{0x01, {"FL", "FR"}}, {0x02, {"LFE", NULL}},
		{0x04, {"FC", NULL}}, {0x08, {"RL", "RR"}},
		{0x40, {"SL", "SR"}}, {0x20, {"RLC", "RRC"}},
		{0x10, {"RC", NULL}},
	};
	size_t used = 0;

	buf[0] = '\0';
	append_str(buf, size, &used, "[");
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (!(speakers & map[i].bit))
			continue;
		for (int j = 0; j < 2 && map[i].name[j] != NULL; j++) {
			if (used > 1)
				append_str(buf, size, &used, ",");
			append_str(buf, size, &used, map[i].name[j]);
		}
	}
	append_str(buf, size, &used, "]");
}

/* pa_alsa_get_hdmi_eld() */
static int eld_parse(const uint8_t *data, unsigned int size,
		     struct eld_info *eld) {
	unsigned int mnl, sad_count;

	memset(eld, 0, sizeof(*eld));

	if (data == NULL || size == 0) {
		log_debug("ELD info empty");
		return -ENODATA;
	}
	if (size < ELD_NAME_OFFSET || size > ELD_MAX_SIZE) {
		log_debug("ELD info has wrong size (%u)", size);
		return -ENODATA;
	}

	mnl = data[4] & 0x1f;
	if (mnl == 0 || mnl > ELD_NAME_MAX || ELD_NAME_OFFSET + mnl > size) {
		log_debug("No monitor name in ELD info");
		mnl = 0;
	}
	memcpy(eld->monitor_name, &data[ELD_NAME_OFFSET], mnl);
	eld->monitor_name[mnl] = '\0';
	if (mnl)
		log_debug("Monitor name in ELD info is '%s'",
			  eld->monitor_name);

	sad_count = (data[5] & 0xf0) >> 4;
	log_debug("SAD count in ELD info is %u", sad_count);
	if (ELD_NAME_OFFSET + mnl + 3 * sad_count > size) {
		log_debug("Invalid SAD count (%u) in ELD info", sad_count);
		sad_count = 0;
	}

	eld->speakers = data[7] & 0x7f;

	for (unsigned int i = 0; i < sad_count; i++) {
		const uint8_t *sad = &data[ELD_NAME_OFFSET + mnl + 3 * i];
		unsigned int lpcm_channels;

		switch ((sad[0] & 0x78) >> 3) {
		case 1:
			eld->codecs |= 1ULL << CODEC_PCM;
			lpcm_channels = (sad[0] & 0x07) + 1;
			if (lpcm_channels > eld->lpcm_channels)
				eld->lpcm_channels = lpcm_channels;
			break;
		case 2:
			eld->codecs |= 1ULL << CODEC_AC3;
			break;
		case 3:
		case 4:
		case 5:
			eld->codecs |= 1ULL << CODEC_MPEG;
			break;
		case 6:
			eld->codecs |= 1ULL << CODEC_MPEG2_AAC;
			break;
		case 7:
			eld->codecs |= 1ULL << CODEC_DTS;
			break;
		case 10:
			eld->codecs |= 1ULL << CODEC_EAC3;
			break;
		case 11:
			eld->codecs |= 1ULL << CODEC_DTSHD;
			break;
		case 12:
			eld->codecs |= 1ULL << CODEC_TRUEHD;
			break;
		default:
			eld->codecs |= 1ULL << CODEC_UNKNOWN;
			break;
		}
	}

	eld->valid = true;
	return 0;
}

static int read_eld_control(snd_ctl_t *ctl, unsigned int numid,
			    struct eld_info *eld) {
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
	int err;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&value);

	snd_ctl_elem_info_set_numid(info, numid);
	CHECK(snd_ctl_elem_info(ctl, info), "snd_ctl_elem_info (ELD)");
	if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BYTES) {
		log_error("The ELD control is not a byte array");
		return -EINVAL;
	}

	snd_ctl_elem_value_set_numid(value, numid);
	CHECK(snd_ctl_elem_read(ctl, value), "snd_ctl_elem_read (ELD)");

	return eld_parse(snd_ctl_elem_value_get_bytes(value),
			 snd_ctl_elem_info_get_count(info), eld);
}

static int read_jack_control(snd_ctl_t *ctl, struct jack_ref *jack) {
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
	int err;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&value);

	snd_ctl_elem_info_set_numid(info, jack->numid);
	CHECK(snd_ctl_elem_info(ctl, info), "snd_ctl_elem_info (Jack)");
	if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BOOLEAN) {
		log_error("Jack control '%s' is not boolean", jack->name);
		return -EINVAL;
	}

	snd_ctl_elem_value_set_numid(value, jack->numid);
	CHECK(snd_ctl_elem_read(ctl, value), "snd_ctl_elem_read (Jack)");

	jack->plugged = snd_ctl_elem_value_get_boolean(value, 0);
	log_debug("Jack '%s' is now %s", jack->name,
		  jack->plugged ? "plugged in" : "unplugged");
	return 0;
}

/* jack_probe(): the configured jack is "[Jack HDMI/DP]" and the control
 * name gets ",pcm=N Jack" appended */
static bool parse_jack_name(const char *name, int *device) {
	const char *p;
	char *end;
	long val;

	if (strncmp(name, "HDMI", 4) != 0)
		return false;
	p = strstr(name, ",pcm=");
	if (p == NULL)
		return false;

	p += 5;
	val = strtol(p, &end, 10);
	if (end == p || strcmp(end, " Jack") != 0)
		return false;
	if (val < 0 || val > 32)
		return false;

	*device = (int) val;
	return true;
}

static int cmp_outputs(const void *a, const void *b) {
	const struct hdmi_output *oa = a;
	const struct hdmi_output *ob = b;
	return oa->device - ob->device;
}

static int scan_controls(snd_ctl_t *ctl, struct hdmi_output *outputs,
			 size_t *n_outputs, struct jack_ref *jacks,
			 size_t *n_jacks) {
	snd_ctl_elem_list_t *list;
	snd_ctl_elem_list_alloca(&list);
	int err;

	CHECK(snd_ctl_elem_list(ctl, list), "snd_ctl_elem_list");

	unsigned int total = snd_ctl_elem_list_get_count(list);

	CHECK(snd_ctl_elem_list_alloc_space(list, total), "alloc_space");

	CHECK(snd_ctl_elem_list(ctl, list), "snd_ctl_elem_list (2nd)");

	bool truncated = false;
	for (unsigned int i = 0; i < total; i++) {
		const char *name = snd_ctl_elem_list_get_name(list, i);
		snd_ctl_elem_iface_t iface =
			snd_ctl_elem_list_get_interface(list, i);
		unsigned int numid = snd_ctl_elem_list_get_numid(list, i);

		if (name == NULL)
			continue;

		if (iface == SND_CTL_ELEM_IFACE_PCM &&
		    strcmp(name, "ELD") == 0) {
			if (*n_outputs >= MAX_HDMI_OUTPUTS) {
				truncated = true;
				continue;
			}
			struct hdmi_output *o = &outputs[(*n_outputs)++];
			memset(o, 0, sizeof(*o));
			o->device = (int) snd_ctl_elem_list_get_device(list, i);
			o->eld_numid = numid;
			o->avail = AVAIL_UNKNOWN;
			log_debug("Found ELD control %u for PCM device %d",
				  numid, o->device);
			continue;
		}

		if (iface != SND_CTL_ELEM_IFACE_CARD)
			continue;

		int device;
		if (!parse_jack_name(name, &device))
			continue;
		if (*n_jacks >= MAX_HDMI_OUTPUTS) {
			truncated = true;
			continue;
		}
		struct jack_ref *j = &jacks[(*n_jacks)++];
		memset(j, 0, sizeof(*j));
		j->numid = numid;
		j->device = device;
		snprintf(j->name, sizeof(j->name), "%s", name);
		log_debug("Found jack control %u '%s' for PCM device %d", numid,
			  name, device);
	}

	snd_ctl_elem_list_free_space(list);

	if (truncated)
		log_warn("More than %d HDMI/DP controls were found; only the "
			 "first %d are examined",
			 MAX_HDMI_OUTPUTS, MAX_HDMI_OUTPUTS);

	qsort(outputs, *n_outputs, sizeof(*outputs), cmp_outputs);
	return 0;
}

/* hdmi_eld_changed(): trailing whitespace is a driver bug */
static void strip_trailing_space(char *str) {
	for (int i = (int) strlen(str) - 1; i >= 0; i--) {
		if (str[i] != '\n' && str[i] != '\r' && str[i] != '\t' &&
		    str[i] != ' ')
			break;
		str[i] = '\0';
	}
}

/* calc_port_state(): a jack without a control leaves availability
 * unknown, an unplugged jack makes the port unavailable */
static enum availability jack_availability(const struct hdmi_output *o) {
	if (o->jack == NULL)
		return AVAIL_UNKNOWN;
	return o->jack->plugged ? AVAIL_YES : AVAIL_NO;
}

static bool output_usable(const struct hdmi_output *o) {
	if (o->avail == AVAIL_NO)
		return false;
	if (o->eld.valid)
		return codec_detected(&o->eld, CODEC_PCM);
	return true;
}

static void pcm_name(snd_ctl_t *ctl, int card, int device, char *buf,
		     size_t size) {
	snd_pcm_info_t *info;
	snd_pcm_info_alloca(&info);

	snd_pcm_info_set_device(info, (unsigned int) device);
	snd_pcm_info_set_subdevice(info, 0);
	snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
	if (snd_ctl_pcm_info(ctl, info) < 0) {
		snprintf(buf, size, "hw:%d,%d", card, device);
		return;
	}
	snprintf(buf, size, "hw:%d,%d '%s'", card, device,
		 snd_pcm_info_get_name(info));
}

static void report_output(snd_ctl_t *ctl, int card,
			  const struct hdmi_output *o) {
	char pcm[128], codecs[256], positions[256];
	char monitor[ELD_NAME_MAX + 1];

	pcm_name(ctl, card, o->device, pcm, sizeof(pcm));
	snprintf(monitor, sizeof(monitor), "%s", o->eld.monitor_name);
	strip_trailing_space(monitor);

	report_section("HDMI/DP PCM Device %d", o->device);
	struct report_tab *t = report_tab_begin();
	report_kv(t, "Playback PCM", "%s", pcm);
	report_kv(t, "ELD control", "numid %u", o->eld_numid);
	if (o->jack != NULL)
		report_kv(t, "Jack control", "%s (numid %u)", o->jack->name,
			  o->jack->numid);
	else
		report_kv(t, "Jack control", "none");
	report_kv(t, "Jack state", "%s",
		  o->jack == NULL
			  ? "n/a"
			  : (o->jack->plugged ? "plugged" : "unplugged"));
	report_kv(t, "Availability", "%s", avail_str(o->avail));
	if (o->eld.valid) {
		if (monitor[0] != '\0')
			report_kv(t, "Monitor name", "%s", monitor);
		report_kv(t, "LPCM channels", "%u", o->eld.lpcm_channels);
		format_codecs(o->eld.codecs, codecs, sizeof(codecs));
		report_kv(t, "IEC958 codecs", "%s", codecs);
		format_positions(o->eld.speakers, positions, sizeof(positions));
		report_kv(t, "Speaker allocation", "%s", positions);
	} else {
		report_kv(t, "ELD content", "unreadable");
	}
	report_tab_end(t);

	if (o->avail == AVAIL_NO) {
		report_note("PCM device %d is unavailable: jack '%s' is "
			    "unplugged",
			    o->device, o->jack->name);
	} else if (!o->eld.valid) {
		report_warn("PCM device %d is %s but the ELD cannot be read; "
			    "channels and passthrough codecs stay undetected",
			    o->device, avail_str(o->avail));
	} else if (!codec_detected(&o->eld, CODEC_PCM)) {
		report_warn("PCM device %d is %s but the ELD advertises no PCM "
			    "support",
			    o->device, avail_str(o->avail));
	} else if (o->avail == AVAIL_UNKNOWN) {
		report_note("PCM device %d has unknown availability; PipeWire "
			    "treats it as usable and detects %u-channel PCM",
			    o->device, o->eld.lpcm_channels);
	} else {
		report_ok("PCM device %d can play %u-channel PCM", o->device,
			  o->eld.lpcm_channels);
	}
}

static void hdmi_usage(const char *prog) {
	usage_header(prog, "[OPTION]...");
	usage_opt("-c CARD", "card number, device 'hw:N' (required)");
	usage_opt("-v", "increase log level (-v info, -vv debug, -vvv trace)");
	usage_opt("-h", "help");
}

static int hdmi_run(int argc, char **argv) {
	int card = -1; /* -c is required */
	int verbose = 0;
	int opt;

	while ((opt = getopt(argc, argv, CARD_OPTSTRING COMMON_OPTSTRING)) !=
	       -1) {
		switch (opt) {
		case 'c':
			card = parse_card(optarg);
			if (card < 0) {
				hdmi_usage(argv[0]);
				return 2;
			}
			break;
		case 'v':
			verbose++;
			break;
		case 'h':
			hdmi_usage(argv[0]);
			return 0;
		default:
			hdmi_usage(argv[0]);
			return 2;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
		hdmi_usage(argv[0]);
		return 2;
	}
	if (card < 0) {
		fprintf(stderr, "missing required -c card argument\n");
		hdmi_usage(argv[0]);
		return 2;
	}
	log_set_verbose(verbose);

	snd_ctl_t *ctl;
	char devname[32];
	snprintf(devname, sizeof(devname), "hw:%d", card);
	int err;
	CHECK(snd_ctl_open(&ctl, devname, 0), "snd_ctl_open");
	log_info("Probing HDMI/DP outputs on card %d", card);

	struct hdmi_output outputs[MAX_HDMI_OUTPUTS] = {0};
	struct jack_ref jacks[MAX_HDMI_OUTPUTS] = {0};
	size_t n_outputs = 0, n_jacks = 0;
	int read_failures = 0;

	err = scan_controls(ctl, outputs, &n_outputs, jacks, &n_jacks);
	if (err < 0)
		goto out;

	if (n_outputs == 0) {
		report_note("no HDMI/DP outputs found on %s (no ELD controls)",
			    devname);
		err = 0;
		goto out;
	}

	for (size_t i = 0; i < n_outputs; i++) {
		struct hdmi_output *o = &outputs[i];

		err = read_eld_control(ctl, o->eld_numid, &o->eld);
		if (err < 0) {
			if (err != -ENODATA)
				read_failures++;
			err = 0;
		}

		for (size_t j = 0; j < n_jacks; j++) {
			if (jacks[j].device != o->device)
				continue;
			err = read_jack_control(ctl, &jacks[j]);
			if (err < 0) {
				read_failures++;
				err = 0;
				break;
			}
			o->jack = &jacks[j];
			break;
		}
		o->avail = jack_availability(o);
	}

	int usable = 0;
	for (size_t i = 0; i < n_outputs; i++) {
		report_output(ctl, card, &outputs[i]);
		if (output_usable(&outputs[i]))
			usable++;
	}

	if (usable == 0)
		report_fail("none of the %zu HDMI/DP outputs is available for "
			    "playback",
			    n_outputs);
	else if (usable < (int) n_outputs)
		report_warn("%d of %zu HDMI/DP outputs are available for "
			    "playback",
			    usable, n_outputs);
	else
		report_ok("%d of %zu HDMI/DP outputs are available for "
			  "playback",
			  usable, n_outputs);

	if (read_failures > 0)
		log_warn("%d HDMI/DP controls could not be read",
			 read_failures);

	err = usable > 0 ? 0 : -ENODEV;

out:
	snd_ctl_close(ctl);
	return err;
}

static struct applet hdmi_applet = {
	.name = "hdmi",
	.desc = "inspect HDMI/DP availability and ELD capabilities",
	.main = hdmi_run,
	.usage = hdmi_usage,
	.next = NULL,
};

APPLET_REGISTER(hdmi_applet);

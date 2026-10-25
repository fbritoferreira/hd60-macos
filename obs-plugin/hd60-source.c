// OBS source plugin for the Elgato Game Capture HD60: runs the driver core (src/device.c)
// in-process and decodes its MPEG-TS with OBS's own FFmpeg. Video and audio, no helper process.
//
// Threads: the USB thread (hd60_run) pushes TS bytes into a ring buffer; the decode thread
// reads them through a custom AVIOContext, demuxes and decodes, and hands frames to OBS.

#include "device.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <pthread.h>
#include <string.h>

OBS_DECLARE_MODULE()

#define RING_SIZE (16 * 1024 * 1024)

struct hd60_source {
	obs_source_t *source;
	char *firmware;
	char *mode;
	int bitrate;

	volatile int stop;
	pthread_t usb_thread, decode_thread;
	bool running;

	// TS ring buffer between the threads.
	pthread_mutex_t mu;
	pthread_cond_t cond;
	uint8_t *ring;
	size_t head, len;
};

// ---------------------------------------------------------------- ring buffer

static void ring_push(const uint8_t *buf, int n, void *ctx_) {
	struct hd60_source *s = ctx_;
	pthread_mutex_lock(&s->mu);
	if ((size_t)n > RING_SIZE - s->len) {
		// Decoder fell behind: drop the backlog rather than add latency.
		s->head = (s->head + s->len) % RING_SIZE;
		s->len = 0;
	}
	size_t tail = (s->head + s->len) % RING_SIZE;
	size_t first = RING_SIZE - tail < (size_t)n ? RING_SIZE - tail : (size_t)n;
	memcpy(s->ring + tail, buf, first);
	memcpy(s->ring, buf + first, n - first);
	s->len += n;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->mu);
}

static int ring_read(void *opaque, uint8_t *buf, int want) {
	struct hd60_source *s = opaque;
	pthread_mutex_lock(&s->mu);
	while (!s->len && !s->stop) pthread_cond_wait(&s->cond, &s->mu);
	if (s->stop) {
		pthread_mutex_unlock(&s->mu);
		return AVERROR_EOF;
	}
	size_t n = s->len < (size_t)want ? s->len : (size_t)want;
	size_t first = RING_SIZE - s->head < n ? RING_SIZE - s->head : n;
	memcpy(buf, s->ring + s->head, first);
	memcpy(buf + first, s->ring, n - first);
	s->head = (s->head + n) % RING_SIZE;
	s->len -= n;
	pthread_mutex_unlock(&s->mu);
	return (int)n;
}

// ---------------------------------------------------------------- USB thread

static void *usb_main(void *arg) {
	struct hd60_source *s = arg;
	struct hd60_config cfg = {s->firmware, s->mode, s->bitrate, 0};
	int last = 1;
	os_set_thread_name("hd60-usb");
	while (!s->stop) {
		int r = hd60_run(&cfg, ring_push, s, &s->stop);
		if (s->stop) break;
		if (r == HD60_ERR_FIRMWARE || r == HD60_ERR_CONFIG) {
			blog(LOG_ERROR, "[hd60] %s", r == HD60_ERR_FIRMWARE ? "firmware missing or corrupt" : "bad mode/bitrate");
			break;
		}
		if (r != last) {
			blog(LOG_INFO, "[hd60] %s", r == HD60_ERR_NOT_FOUND ? "waiting for the HD60" : "lost the HD60, reconnecting");
			last = r;
		}
		os_sleep_ms(1000);
	}
	return NULL;
}

// ---------------------------------------------------------------- decode thread

static enum audio_format obs_audio_format(int f) {
	switch (f) {
	case AV_SAMPLE_FMT_S16: return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S16P: return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32: return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_S32P: return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLT: return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_FLTP: return AUDIO_FORMAT_FLOAT_PLANAR;
	default: return AUDIO_FORMAT_UNKNOWN;
	}
}

static void output_video(struct hd60_source *s, AVFrame *f, uint64_t ts) {
	if (f->format != AV_PIX_FMT_YUV420P) return;
	struct obs_source_frame frame = {0};
	for (int i = 0; i < 3; i++) {
		frame.data[i] = f->data[i];
		frame.linesize[i] = f->linesize[i];
	}
	frame.width = f->width;
	frame.height = f->height;
	frame.format = VIDEO_FORMAT_I420;
	frame.timestamp = ts;
	frame.full_range = false;
	video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420,
					       frame.color_matrix, frame.color_range_min, frame.color_range_max);
	obs_source_output_video(s->source, &frame);
}

static void output_audio(struct hd60_source *s, AVFrame *f, uint64_t ts) {
	struct obs_source_audio a = {0};
	for (int i = 0; i < MAX_AV_PLANES && f->data[i]; i++) a.data[i] = f->data[i];
	a.frames = f->nb_samples;
	a.samples_per_sec = f->sample_rate;
	a.format = obs_audio_format(f->format);
	a.speakers = f->ch_layout.nb_channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
	a.timestamp = ts;
	if (a.format != AUDIO_FORMAT_UNKNOWN) obs_source_output_audio(s->source, &a);
}

static void *decode_main(void *arg) {
	struct hd60_source *s = arg;
	os_set_thread_name("hd60-decode");
	unsigned char *iobuf = av_malloc(188 * 64);
	AVIOContext *io = avio_alloc_context(iobuf, 188 * 64, 0, s, ring_read, NULL, NULL);
	AVFormatContext *fmt = avformat_alloc_context();
	fmt->pb = io;
	fmt->flags |= AVFMT_FLAG_NOBUFFER;
	AVCodecContext *dec[8] = {0};
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	// Shared clock for audio and video: TS PTS (90 kHz) mapped onto OBS's clock at the first packet.
	int64_t base_pts = AV_NOPTS_VALUE;
	uint64_t base_ns = 0;

	if (avformat_open_input(&fmt, NULL, av_find_input_format("mpegts"), NULL) < 0) {
		blog(LOG_ERROR, "[hd60] could not open the TS stream");
		goto out;
	}
	while (!s->stop && av_read_frame(fmt, pkt) >= 0) {
		int i = pkt->stream_index;
		AVStream *st = fmt->streams[i];
		if (i >= 8) goto next;
		if (!dec[i]) {
			const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
			if (!codec || (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO))
				goto next;
			dec[i] = avcodec_alloc_context3(codec);
			avcodec_parameters_to_context(dec[i], st->codecpar);
			dec[i]->flags |= AV_CODEC_FLAG_LOW_DELAY;
			// ponytail: software decode with slice threads (frame threads add a frame of latency per
			// thread); 1080p60 H.264 is light work here. Add VideoToolbox hwaccel if CPU use matters.
			dec[i]->thread_type = FF_THREAD_SLICE;
			if (avcodec_open2(dec[i], codec, NULL) < 0) {
				avcodec_free_context(&dec[i]);
				goto next;
			}
		}
		if (avcodec_send_packet(dec[i], pkt) < 0) goto next;
		while (avcodec_receive_frame(dec[i], frame) >= 0) {
			int64_t pts = frame->best_effort_timestamp;
			if (pts == AV_NOPTS_VALUE) continue;
			pts = av_rescale_q(pts, st->time_base, (AVRational){1, 90000});
			if (base_pts == AV_NOPTS_VALUE) {
				base_pts = pts;
				base_ns = os_gettime_ns();
			}
			uint64_t ts = base_ns + (uint64_t)av_rescale(pts - base_pts, 1000000000, 90000);
			if (dec[i]->codec_type == AVMEDIA_TYPE_VIDEO) output_video(s, frame, ts);
			else output_audio(s, frame, ts);
		}
	next:
		av_packet_unref(pkt);
	}
out:
	for (int i = 0; i < 8; i++) avcodec_free_context(&dec[i]);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avformat_close_input(&fmt);
	if (io) {
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
	return NULL;
}

// ---------------------------------------------------------------- start / stop

static void hd60_stop(struct hd60_source *s) {
	if (!s->running) return;
	pthread_mutex_lock(&s->mu);
	s->stop = 1;
	pthread_cond_broadcast(&s->cond);
	pthread_mutex_unlock(&s->mu);
	pthread_join(s->usb_thread, NULL); // device core shuts the card down before returning
	pthread_join(s->decode_thread, NULL);
	s->running = false;
	obs_source_output_video(s->source, NULL);
}

static void hd60_start(struct hd60_source *s) {
	hd60_stop(s);
	s->stop = 0;
	s->head = s->len = 0;
	pthread_create(&s->usb_thread, NULL, usb_main, s);
	pthread_create(&s->decode_thread, NULL, decode_main, s);
	s->running = true;
}

// ---------------------------------------------------------------- OBS source

static const char *hd60_name(void *unused) {
	UNUSED_PARAMETER(unused);
	return "Elgato HD60";
}

static void hd60_update(void *data, obs_data_t *settings) {
	struct hd60_source *s = data;
	bfree(s->mode);
	s->mode = bstrdup(obs_data_get_string(settings, "mode"));
	s->bitrate = (int)obs_data_get_int(settings, "bitrate");
	hd60_start(s);
}

static void *hd60_create(obs_data_t *settings, obs_source_t *source) {
	struct hd60_source *s = bzalloc(sizeof *s);
	s->source = source;
	s->firmware = obs_module_file("hd60-bridge.bin");
	s->ring = bmalloc(RING_SIZE);
	pthread_mutex_init(&s->mu, NULL);
	pthread_cond_init(&s->cond, NULL);
	if (!s->firmware) blog(LOG_ERROR, "[hd60] hd60-bridge.bin is not installed in the plugin's data folder");
	// Show frames as soon as they are decoded instead of buffering for smooth timing.
	obs_source_set_async_unbuffered(source, true);
	hd60_update(s, settings);
	return s;
}

static void hd60_destroy(void *data) {
	struct hd60_source *s = data;
	hd60_stop(s);
	pthread_mutex_destroy(&s->mu);
	pthread_cond_destroy(&s->cond);
	bfree(s->ring);
	bfree(s->firmware);
	bfree(s->mode);
	bfree(s);
}

static void hd60_defaults(obs_data_t *settings) {
	obs_data_set_default_string(settings, "mode", "1080p60");
	obs_data_set_default_int(settings, "bitrate", 0);
}

static obs_properties_t *hd60_properties(void *unused) {
	UNUSED_PARAMETER(unused);
	obs_properties_t *p = obs_properties_create();
	obs_property_t *m = obs_properties_add_list(p, "mode", "Output mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const char *const *n = hd60_mode_names(); *n; n++) obs_property_list_add_string(m, *n, *n);
	obs_property_t *b = obs_properties_add_int(p, "bitrate", "Average bitrate (kbps, 0 = default)", 0, 40000, 1000);
	obs_property_int_set_suffix(b, " kbps");
	return p;
}

static struct obs_source_info hd60_source_info = {
	.id = "hd60_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = hd60_name,
	.create = hd60_create,
	.destroy = hd60_destroy,
	.update = hd60_update,
	.get_defaults = hd60_defaults,
	.get_properties = hd60_properties,
	.icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
};

bool obs_module_load(void) {
	obs_register_source(&hd60_source_info);
	return true;
}

/*
 * SceMp4: the MP4 demuxer the SDK's avplayer library uses, answered on the host instead of by Sony's libscemp4.
 *
 * A game opens a movie (sceMp4OpenFile), starts streaming (sceMp4StartFileStreaming), asks each stream's
 * parameters (sceMp4GetStreamInfo), enables the ones it wants, then pulls units (sceMp4GetNextUnit /
 * sceMp4GetNextUnitData) until the end. Video units are H.264 in Annex B, as the console decoder takes them;
 * audio units are raw AAC frames.
 *
 * Adapted from ScarletNX's src/vita/hle/mp4.c (GPL-3.0-or-later), which uses FFmpeg's MP4 demuxer and learns
 * each call's meaning from the SDK library's use of it (Vita3K's NID names, GPL-2.0-or-later). Nativ is
 * GPL-2.0-or-later, so the combined work is GPL-3.0-or-later.
 *
 * The reference reads through the game's SceAvPlayerFileReplacement callbacks. This port reads the file by its
 * guest path instead (app0:/ux0: under RECOMP_FS): the file is on the host, and the callback open/read/size
 * only end up in the same place. The callback form is accepted and ignored.
 */
#include "hle.h"
#include <stdint.h>

#ifdef RECOMP_AVFORMAT
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/mathematics.h>
#include <libavutil/dict.h>

#define FILES 4u
#define STREAMS 8u
#define UNITS 1024u
#define MOST (512u << 20)   /* a movie larger than this is refused rather than read into memory */

typedef struct {
	int used;
	uint8_t *data; size_t bytes, at;
	AVFormatContext *fmt;
	AVIOContext *io;
	int streams[STREAMS]; int video[STREAMS]; AVBSFContext *annexb[STREAMS];
	unsigned nstreams;
	AVPacket *unit; int has_unit;
} Mp4;

static Mp4 files[FILES];
static pthread_mutex_t files_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t addr, size; } units[UNITS];
static unsigned units_next;

static int mp4_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_MP4_LOG") != 0; return v; }
static Mp4 *file_of(uint32_t h) { return h >= 1 && h <= FILES && files[h - 1].used ? &files[h - 1] : 0; }

/* the size a unit was copied to, for the AAC decoder (a raw AAC frame does not say how long it is) */
uint32_t mp4_unit_size(uint32_t addr) {
	uint32_t r = 0;
	pthread_mutex_lock(&files_mx);
	for (unsigned i = 0; i < UNITS; i++) {
		unsigned k = (units_next + UNITS - 1 - i) % UNITS;
		if (units[k].addr == addr && units[k].size) { r = units[k].size; break; }
	}
	pthread_mutex_unlock(&files_mx);
	return r;
}

static int guest_path(uint32_t a, char *out, size_t n) {   /* app0:/ux0: under RECOMP_FS; anything else as a host path */
	const char *p = (const char *)G(a);
	const char *fs = getenv("RECOMP_FS") ? getenv("RECOMP_FS") : "fs";
	if (!p) return 0;
	if (!strncmp(p, "app0:", 5)) { const char *q = p + 5; while (*q == '/') q++; snprintf(out, n, "%s/app0/%s", fs, q); return 1; }
	if (!strncmp(p, "ux0:", 4)) { const char *q = p + 4; while (*q == '/') q++; snprintf(out, n, "%s/ux0/%s", fs, q); return 1; }
	snprintf(out, n, "%s", p);
	return 1;
}

/* ---- the file, in memory ---- */
static int io_read(void *o, uint8_t *buf, int n) {
	Mp4 *m = o; size_t left = m->bytes - m->at, k = (size_t)n < left ? (size_t)n : left;
	if (!k) return AVERROR_EOF;
	memcpy(buf, m->data + m->at, k); m->at += k; return (int)k;
}
static int64_t io_seek(void *o, int64_t off, int whence) {
	Mp4 *m = o;
	if (whence & AVSEEK_SIZE) return (int64_t)m->bytes;
	whence &= ~AVSEEK_FORCE;
	int64_t base = whence == SEEK_CUR ? (int64_t)m->at : whence == SEEK_END ? (int64_t)m->bytes : 0, to = base + off;
	if (to < 0 || to > (int64_t)m->bytes) return -1;
	m->at = (size_t)to; return to;
}

static void release(Mp4 *m) {
	for (unsigned i = 0; i < STREAMS; i++) if (m->annexb[i]) av_bsf_free(&m->annexb[i]);
	av_packet_free(&m->unit);
	if (m->fmt) avformat_close_input(&m->fmt);
	if (m->io) { av_freep(&m->io->buffer); avio_context_free(&m->io); }
	free(m->data);
	memset(m, 0, sizeof *m);
}

static int demux(Mp4 *m) {
	uint8_t *buf = av_malloc(0x8000);
	if (!buf || !(m->io = avio_alloc_context(buf, 0x8000, 0, m, io_read, 0, io_seek))) { av_free(buf); return 0; }
	m->fmt = avformat_alloc_context();
	if (!m->fmt) return 0;
	m->fmt->pb = m->io; m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	if (avformat_open_input(&m->fmt, 0, av_find_input_format("mov"), 0) < 0) { m->fmt = 0; return 0; }
	if (avformat_find_stream_info(m->fmt, 0) < 0) return 0;
	/* the console's demuxer numbers a movie's video stream first, then its audio, whatever order the container
	 * stores them in: DOA5+'s avplayer takes stream 0 as the picture */
	for (int pass = 0; pass < 2; pass++) {
		for (unsigned i = 0; i < m->fmt->nb_streams; i++) {
			AVStream *st = m->fmt->streams[i];
			enum AVCodecID id = st->codecpar->codec_id;
			int vid = id == AV_CODEC_ID_H264;
			if ((id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_AAC) || (pass == 0) != vid) {
				if (pass == 1 || !vid) st->discard = AVDISCARD_ALL;
				continue;
			}
			if (m->nstreams == STREAMS) { st->discard = AVDISCARD_ALL; continue; }
			unsigned k = m->nstreams++;
			m->streams[k] = (int)i; m->video[k] = vid;
			if (!vid) continue;
			const AVBitStreamFilter *f = av_bsf_get_by_name("h264_mp4toannexb");
			if (!f || av_bsf_alloc(f, &m->annexb[k]) < 0 || avcodec_parameters_copy(m->annexb[k]->par_in, st->codecpar) < 0) return 0;
			m->annexb[k]->time_base_in = st->time_base;
			if (av_bsf_init(m->annexb[k]) < 0) return 0;
		}
	}
	return m->nstreams && (m->unit = av_packet_alloc()) != 0;
}

/* ---- the calls ---- */
static uint32_t open_file(Cpu *c) {   /* (path, replacement, allocator, work, workSize, priority, logLevel) -> handle, 0 on failure */
	char hp[1024];
	if (!guest_path(A0, hp, sizeof hp)) return 0;
	FILE *f = fopen(hp, "rb");
	if (!f) { if (mp4_log()) fprintf(stderr, "mp4: open %s: not found\n", hp); return 0; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	if (sz <= 0 || (uint64_t)sz > MOST) { fclose(f); return 0; }
	pthread_mutex_lock(&files_mx);
	Mp4 *m = 0;
	for (unsigned i = 0; i < FILES; i++) if (!files[i].used) { m = &files[i]; break; }
	if (!m) { pthread_mutex_unlock(&files_mx); fclose(f); return 0; }
	memset(m, 0, sizeof *m); m->used = 1;
	m->data = malloc((size_t)sz);
	if (!m->data || fread(m->data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(m->data); m->used = 0; pthread_mutex_unlock(&files_mx); return 0; }
	fclose(f); m->bytes = (size_t)sz;
	uint32_t handle = (uint32_t)(m - files) + 1;
	if (!demux(m)) { release(m); handle = 0; }
	pthread_mutex_unlock(&files_mx);
	if (mp4_log()) fprintf(stderr, "mp4: %s: %ld bytes, handle %u, %u streams\n", hp, sz, handle, handle ? m->nstreams : 0);
	return handle;
}

static uint32_t start_streaming(Cpu *c) {   /* (handle, int *streams, u64 *duration in 0.1 ms) */
	Mp4 *m = file_of(A0);
	if (!m) return 0x80000000u;
	if (mp4_log()) fprintf(stderr, "mp4: start streaming handle %u\n", A0);
	if (A1) WR32(A1, m->nstreams);
	int64_t d = m->fmt->duration == AV_NOPTS_VALUE ? 0 : m->fmt->duration / 100;
	if (A2) WR64(A2, (uint64_t)d);
	return 0;
}

/* sceMp4GetStreamInfo(handle, index, info): 0x140 bytes, as the SDK reads it */
static uint32_t stream_info(Cpu *c) {
	Mp4 *m = file_of(A0); uint32_t index = A1, info = A2;
	if (!m || index >= m->nstreams || !info) return 0x80000000u;
	if (mp4_log()) fprintf(stderr, "mp4: stream info %u (video %d) -> %#x\n", index, m->video[index], info);
	const AVStream *st = m->fmt->streams[m->streams[index]];
	const AVCodecParameters *p = st->codecpar;
	uint8_t *b = G(info); memset(b, 0, 0x140);
	if (m->video[index]) {
		WR32(info, 0); WR32(info + 4, 1);
		WR32(info + 8, (uint32_t)p->width); WR32(info + 0xc, (uint32_t)p->height);
		b[0x10] = (uint8_t)p->profile;
		b[0x14] = (uint8_t)p->level;
		b[0x18] = 4; b[0x1c] = 4;
		int64_t most = 0;
		for (int i = 0, n = avformat_index_get_entries_count(st); i < n; i++) {
			const AVIndexEntry *e = avformat_index_get_entry((AVStream *)st, i);
			if (e && e->size > most) most = e->size;
		}
		if (!most) most = (int64_t)p->width * p->height;
		WR32(info + 0x20, (uint32_t)most + (uint32_t)p->extradata_size + 0x100u);
		AVRational sar = p->sample_aspect_ratio.num > 0 ? p->sample_aspect_ratio : (AVRational){1, 1};
		double aspect = p->height ? (double)p->width * sar.num / ((double)p->height * sar.den) : 0;
		WR64(info + 0x30, (uint64_t)(int64_t)(aspect * 1e6 + 0.5));
	} else {
		WR32(info, 1); WR32(info + 4, 2);
		WR32(info + 8, (uint32_t)p->sample_rate);
		b[0x1c] = (uint8_t)p->ch_layout.nb_channels;
	}
	const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", 0, 0);
	if (lang) memcpy(b + 0x38, lang->value, strnlen(lang->value, 3));
	return 0;
}

static uint32_t enable_stream(Cpu *c) { Mp4 *m = file_of(A0); if (mp4_log()) fprintf(stderr, "mp4: enable stream %u\n", A1); return m && A1 < m->nstreams ? 0 : 0x80000000u; }

static int next_unit(Mp4 *m, unsigned *ours) {
	for (;;) {
		av_packet_unref(m->unit);
		int r = av_read_frame(m->fmt, m->unit);
		if (r == AVERROR_EOF) return 1;
		if (r < 0) return r;
		unsigned k = 0;
		while (k < m->nstreams && m->streams[k] != m->unit->stream_index) k++;
		if (k == m->nstreams) continue;
		if (m->video[k]) {
			if (av_bsf_send_packet(m->annexb[k], m->unit) < 0) return -1;
			r = av_bsf_receive_packet(m->annexb[k], m->unit);
			if (r == AVERROR(EAGAIN)) continue;
			if (r < 0) return r;
		}
		*ours = k; return 0;
	}
}
static uint64_t ticks(const AVStream *st, int64_t ts) {
	return ts == AV_NOPTS_VALUE ? 0 : (uint64_t)av_rescale_q(ts, st->time_base, (AVRational){1, 90000});
}
static uint32_t next_unit_hle(Cpu *c) {   /* (handle, SceMp4Unit *): 0, 1 at the end, <0 error */
	Mp4 *m = file_of(A0); uint32_t unit = A1;
	if (!m || !unit) return 0x80000000u;
	unsigned k = 0; int r = next_unit(m, &k);
	m->has_unit = r == 0;
	if (r) return r > 0 ? 1u : 0x80000000u;
	const AVStream *st = m->fmt->streams[m->streams[k]];
	int64_t pts = m->unit->pts != AV_NOPTS_VALUE ? m->unit->pts : m->unit->dts;
	memset(G(unit), 0, 0x38);
	WR32(unit + 8, (uint32_t)m->unit->size);
	WR64(unit + 0x10, ticks(st, pts));
	WR64(unit + 0x18, ticks(st, m->unit->dts));
	WR32(unit + 0x28, 1);
	((uint8_t *)G(unit))[0x2c] = (uint8_t)k;
	((uint8_t *)G(unit))[0x30] = (m->unit->flags & AV_PKT_FLAG_KEY) != 0;
	if (mp4_log()) fprintf(stderr, "mp4: unit stream %u %d bytes pts %llu%s\n", k, m->unit->size, (unsigned long long)ticks(st, pts), (m->unit->flags & AV_PKT_FLAG_KEY) ? " key" : "");
	return 0;
}
/* the 2- and 3-reference forms: one unit a call, the others empty (the SDK's player handles them one at a time) */
static uint32_t next_unit_refs(Cpu *c) { static int n; if (mp4_log() && n++ < 4) fprintf(stderr, "mp4: 2/3-ref form called\n"); return next_unit_hle(c); }

static uint32_t next_unit_data(Cpu *c) {   /* (handle, destination) */
	Mp4 *m = file_of(A0); uint32_t to = A1;
	if (mp4_log()) fprintf(stderr, "mp4: data call handle %u to %#x has %d\n", A0, to, m ? m->has_unit : -1);
	if (!m || !m->has_unit || !to) return 0x80000000u;
	static int copied; if (mp4_log() && copied++ < 6) fprintf(stderr, "mp4: unit data %d bytes -> %#x\n", m->unit->size, to);
	if (m->unit->size) memcpy(G(to), m->unit->data, (size_t)m->unit->size);
	if (m->fmt->streams[m->unit->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
		pthread_mutex_lock(&files_mx);
		units[units_next % UNITS].addr = to; units[units_next % UNITS].size = (uint32_t)m->unit->size; units_next++;
		pthread_mutex_unlock(&files_mx);
	}
	m->has_unit = 0;
	return 0;
}

static void seek(Mp4 *m, int64_t us) {
	av_seek_frame(m->fmt, -1, us, AVSEEK_FLAG_BACKWARD);
	for (unsigned i = 0; i < STREAMS; i++) if (m->annexb[i]) av_bsf_flush(m->annexb[i]);
	m->has_unit = 0;
}
static uint32_t reset(Cpu *c) { Mp4 *m = file_of(A0); if (!m) return 0x80000000u; seek(m, 0); return 0; }
static uint32_t jump_pts(Cpu *c) {   /* (handle, unused, u64 milliseconds) */
	Mp4 *m = file_of(A0); if (!m) return 0x80000000u;
	uint64_t ms = A2 | (uint64_t)A3 << 32; seek(m, (int64_t)ms * 1000); return 0;
}
static uint32_t close_file(Cpu *c) {
	pthread_mutex_lock(&files_mx);
	Mp4 *m = file_of(A0);
	if (m) release(m);
	pthread_mutex_unlock(&files_mx);
	return m ? 0 : 0x80000000u;
}
static uint32_t ok(Cpu *c) { (void)c; return 0; }


/* HleFn is void: the value goes to r0 */
static void w_open_file(Cpu *c) { RET(open_file(c)); }
static void w_next_unit(Cpu *c) { RET(next_unit_hle(c)); }
static void w_start(Cpu *c) { RET(start_streaming(c)); }
static void w_next_refs(Cpu *c) { RET(next_unit_refs(c)); }
static void w_reset(Cpu *c) { RET(reset(c)); }
static void w_enable(Cpu *c) { RET(enable_stream(c)); }
static void w_ok(Cpu *c) { RET(ok(c)); }
static void w_info(Cpu *c) { RET(stream_info(c)); }
static void w_close(Cpu *c) { RET(close_file(c)); }
static void w_jump(Cpu *c) { RET(jump_pts(c)); }
static void w_data(Cpu *c) { RET(next_unit_data(c)); }

/* the NIDs, by the names imports.py gives them (the SDK's own, not a module's) */
const HleEntry hle_mp4[] = {
	{"SceMp4_05474AF0", w_open_file},          /* OpenFile */
	{"SceMp4_20A01112", w_next_unit},      /* GetNextUnit */
	{"SceMp4_30E49E4D", w_start},    /* StartFileStreaming */
	{"SceMp4_32A15788", w_next_refs},     /* GetNextUnit2Ref */
	{"SceMp4_40351E1A", w_reset},              /* Reset */
	{"SceMp4_40C64DC0", w_next_refs},     /* GetNextUnit3Ref */
	{"SceMp4_609E57AD", w_enable},      /* EnableStream */
	{"SceMp4_6B69F900", w_ok},                 /* TimeToPTS */
	{"SceMp4_7B4832FE", w_ok},                 /* ReleaseBuffer */
	{"SceMp4_8BE0E3D3", w_info},        /* GetStreamInfo (other SDK revision) */
	{"SceMp4_920623C8", w_close},         /* CloseFile */
	{"SceMp4_94E1305D", w_ok},
	{"SceMp4_96676BA0", w_jump},           /* JumpPTS */
	{"SceMp4_A7C09746", w_ok},                 /* PTSToTime */
	{"SceMp4_A870587F", w_ok},
	{"SceMp4_C05DFF01", w_ok},
	{"SceMp4_CB91CEBC", w_ok},
	{"SceMp4_D092A066", w_data},     /* GetNextUnitData */
	{"SceMp4_D2871F1C", w_ok},
	{"SceMp4_D5B26179", w_info},        /* GetStreamInfo (the SDK this game uses) */
	{"SceMp4_EFE299C9", w_ok},
	{0, 0},
};

#else
/* built without libavformat (the H.264-only toolchains): no demuxer here, as before */
uint32_t mp4_unit_size(uint32_t a) { (void)a; return 0; }
static void h_mp4_none(Cpu *c) { c->r[0] = 0x80020001u; }
const HleEntry hle_mp4[] = {
	{"SceMp4_05474AF0", h_mp4_none},
	{"SceMp4_20A01112", h_mp4_none},
	{"SceMp4_30E49E4D", h_mp4_none},
	{"SceMp4_32A15788", h_mp4_none},
	{"SceMp4_40351E1A", h_mp4_none},
	{"SceMp4_40C64DC0", h_mp4_none},
	{"SceMp4_609E57AD", h_mp4_none},
	{"SceMp4_6B69F900", h_mp4_none},
	{"SceMp4_7B4832FE", h_mp4_none},
	{"SceMp4_8BE0E3D3", h_mp4_none},
	{"SceMp4_920623C8", h_mp4_none},
	{"SceMp4_94E1305D", h_mp4_none},
	{"SceMp4_96676BA0", h_mp4_none},
	{"SceMp4_A7C09746", h_mp4_none},
	{"SceMp4_A870587F", h_mp4_none},
	{"SceMp4_C05DFF01", h_mp4_none},
	{"SceMp4_CB91CEBC", h_mp4_none},
	{"SceMp4_D092A066", h_mp4_none},
	{"SceMp4_D2871F1C", h_mp4_none},
	{"SceMp4_D5B26179", h_mp4_none},
	{"SceMp4_EFE299C9", h_mp4_none},
	{0, 0},
};
#endif

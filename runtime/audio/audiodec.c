/*
 * SceAudiodecUser: the hardware audio decoders (ATRAC9, AAC, MP3) behind sceAudiodec*.
 *
 * A game initialises the library for a codec, asks how much working memory a decoder needs
 * (sceAudiodecGetContextSize), creates a decoder over a SceAudiodecCtrl and then feeds it elementary
 * stream bytes, taking interleaved 16-bit PCM back. ATRAC9 is LibAtrac9 (the decoder NGS already uses);
 * AAC and MP3 are FFmpeg's, when the build links libavcodec (RECOMP_AVCODEC). Without it those two
 * report the console's "codec can't be set up" error, so a game's own skip/error path runs rather than
 * waiting for frames that never come.
 *
 * The layouts are VitaSDK's psp2/audiodec.h (SceAudiodecCtrl is 0x28 bytes, SceAudiodecInfo 0x1c).
 * guest memory is addressed as 32-bit guest addresses (G/RD32/WR32), never host pointers.
 */
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "third_party/libatrac9/libatrac9.h"
#ifdef RECOMP_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#endif

enum {
	TYPE_AT9 = 0x1003, TYPE_MP3 = 0x1004, TYPE_AAC = 0x1005, TYPE_CELP = 0x1006,
	ERR_API_FAIL = 0x807F0000, ERR_INVALID_TYPE = 0x807F0001, ERR_NOT_INITIALIZED = 0x807F0005,
	ERR_INVALID_PTR = 0x807F0008, ERR_INVALID_HANDLE = 0x807F0009, ERR_NOT_HANDLE_IN_USE = 0x807F000A,
	ERR_INVALID_SIZE = 0x807F000D, ERR_UNSUPPORTED = 0x807F000E, ERR_AT9_CONFIG = 0x807F2000,
};

#define CTRL_SIZE 0x28u
#define INFO_SIZE 0x1Cu

typedef struct {
	int used;
	uint32_t type;
	void *at9;
	int at9_frame_samples;
#ifdef RECOMP_AVCODEC
	AVCodecContext *ctx;
	AVFrame *frm;
	AVPacket *pkt;
	uint8_t *pktbuf; size_t pktcap;
#endif
	int channels, sample_rate, is_adts;
	pthread_mutex_t mx;
} Adec;

#define NDEC 16
static int ad_log(void);
static Adec decs[NDEC];
static pthread_mutex_t decs_mx = PTHREAD_MUTEX_INITIALIZER;
static int initialized[4];   /* by type index: AT9, MP3, AAC, CELP */

static int type_index(uint32_t t) {
	switch (t) { case TYPE_AT9: return 0; case TYPE_MP3: return 1; case TYPE_AAC: return 2; case TYPE_CELP: return 3; default: return -1; }
}
static Adec *dec_of(uint32_t handle) {
	if (handle < 1 || handle > NDEC) return 0;
	Adec *d = &decs[handle - 1];
	return d->used ? d : 0;
}

/* a ctrl's fields (SceAudiodecCtrl): 0 size, 4 handle, 8 es, 12 es used, 16 es max, 20 pcm, 24 pcm used, 28 pcm max, 32 word length, 36 info */
#define C_SIZE(a)  RD32((a) + 0)
#define C_HANDLE(a) RD32((a) + 4)
#define C_ES(a)    RD32((a) + 8)
#define C_ES_USED(a) RD32((a) + 12)
#define C_ES_MAX(a) RD32((a) + 16)
#define C_PCM(a)   RD32((a) + 20)
#define C_PCM_USED(a) RD32((a) + 24)
#define C_PCM_MAX(a) RD32((a) + 28)
#define C_WORD(a)  RD32((a) + 32)
#define C_INFO(a)  RD32((a) + 36)

static int ctrl_ok(uint32_t ctrl, uint32_t *err) {
	if (!ctrl) { *err = ERR_INVALID_PTR; return 0; }
	if (C_SIZE(ctrl) != CTRL_SIZE) { *err = ERR_INVALID_SIZE; return 0; }
	return 1;
}

/* ---- the library ---- */
static void h_init(Cpu *c) {
	uint32_t t = A0; int i = type_index(t);
	if (ad_log()) fprintf(stderr, "audiodec: init type %#x param %#x\n", t, A1);
	if (!A1) { RET(ERR_INVALID_PTR); return; }
	if (i < 0) { RET(ERR_INVALID_TYPE); return; }
	initialized[i] = 1;
	RET(0);
}
static void h_term(Cpu *c) {
	int i = type_index(A0); if (i < 0) { RET(ERR_INVALID_TYPE); return; }
	initialized[i] = 0;
	pthread_mutex_lock(&decs_mx);
	for (int k = 0; k < NDEC; k++) if (decs[k].used && decs[k].type == A0) {
		if (decs[k].at9) { Atrac9ReleaseHandle(decs[k].at9); decs[k].at9 = 0; }
#ifdef RECOMP_AVCODEC
		avcodec_free_context(&decs[k].ctx); av_frame_free(&decs[k].frm); av_packet_free(&decs[k].pkt); free(decs[k].pktbuf);
#endif
		pthread_mutex_destroy(&decs[k].mx);
		memset(&decs[k], 0, sizeof decs[k]);
	}
	pthread_mutex_unlock(&decs_mx);
	RET(0);
}

/* sceAudiodecGetContextSize(ctrl, codec): the working memory CreateDecoderExternal may be given */
static void h_context_size(Cpu *c) {
	uint32_t err;
	if (!ctrl_ok(A0, &err)) { RET(err); return; }
	if (ad_log()) fprintf(stderr, "audiodec: context size type %#x ctrl %#x info %#x\n", A1, A0, C_INFO(A0));
	uint32_t info = C_INFO(A0);
	switch (A1) {
	case TYPE_AT9: {
		uint32_t cfg = info ? RD32(info + 4) : 0;   /* configData[4] as one word */
		unsigned v = (((cfg >> 8) & 0xf) >> 1);     /* configData[1] bits 0-3, >> 1 */
		if (v == 0) RET(0x800);
		else if (v == 1 || v == 2) RET(0xC00);
		else RET(ERR_AT9_CONFIG);
		return;
	}
	case TYPE_AAC: RET(0x18000); return;
	case TYPE_MP3: case TYPE_CELP: RET(0); return;
	default: RET(ERR_INVALID_TYPE); return;
	}
}

static void h_create(Cpu *c) {
	uint32_t ctrl = A0, type = A1, err;
	if (ad_log()) fprintf(stderr, "audiodec: create type %#x ctrl %#x size %#x info %#x\n", type, ctrl, ctrl ? C_SIZE(ctrl) : 0, ctrl ? C_INFO(ctrl) : 0);
	if (!ctrl_ok(ctrl, &err)) { RET(err); return; }
	int ti = type_index(type);
	if (ti < 0) { RET(ERR_INVALID_TYPE); return; }   /* (the library call is a formality, as on the console: a decoder may be made after it, and ScarletNX's HLE accepts either order) */
	uint32_t info = C_INFO(ctrl);
	if (!info) { RET(ERR_INVALID_PTR); return; }

	pthread_mutex_lock(&decs_mx);
	int slot = -1;
	for (int i = 0; i < NDEC; i++) if (!decs[i].used) { slot = i; break; }
	if (slot < 0) { pthread_mutex_unlock(&decs_mx); RET(ERR_API_FAIL); return; }
	Adec *d = &decs[slot];
	memset(d, 0, sizeof *d);
	d->used = 1; d->type = type;
	pthread_mutex_init(&d->mx, 0);
	uint32_t handle = (uint32_t)slot + 1;
	WR32(ctrl + 4, handle);
	pthread_mutex_unlock(&decs_mx);

	switch (type) {
	case TYPE_AT9: {
		uint8_t cfg[4] = { (uint8_t)RD8(info + 4), (uint8_t)RD8(info + 5), (uint8_t)RD8(info + 6), (uint8_t)RD8(info + 7) };
		d->at9 = Atrac9GetHandle();
		if (!d->at9 || Atrac9InitDecoder(d->at9, cfg) < 0) {
			if (d->at9) Atrac9ReleaseHandle(d->at9);
			d->at9 = 0; d->used = 0; pthread_mutex_destroy(&d->mx);
			WR32(ctrl + 4, 0);
			RET(ERR_AT9_CONFIG);
			return;
		}
		Atrac9CodecInfo ci;
		memset(&ci, 0, sizeof ci);
		Atrac9GetCodecInfo(d->at9, &ci);
		d->channels = ci.channels; d->sample_rate = ci.samplingRate; d->at9_frame_samples = ci.frameSamples;
		WR32(info + 8, (uint32_t)ci.channels);
		WR32(info + 12, 0);                         /* bit rate: not reported by LibAtrac9 */
		WR32(info + 16, (uint32_t)ci.samplingRate);
		WR32(info + 20, (uint32_t)ci.superframeSize);
		WR32(info + 24, (uint32_t)ci.framesInSuperframe);
		WR32(ctrl + 16, (uint32_t)(ci.superframeSize < 1024 ? ci.superframeSize : 1024));
		WR32(ctrl + 28, (uint32_t)(ci.frameSamples * ci.channels * 2));
		RET(0);
	}
#ifdef RECOMP_AVCODEC
	case TYPE_AAC: {
		d->is_adts = (int)RD32(info + 4);
		d->channels = (int)RD32(info + 8);
		d->sample_rate = (int)RD32(info + 12);
		int sbr = (int)RD32(info + 16);
		const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
		if (!codec) { d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0); RET(ERR_UNSUPPORTED); return; }
		d->ctx = avcodec_alloc_context3(codec);
		d->frm = av_frame_alloc(); d->pkt = av_packet_alloc();
		if (!d->ctx || !d->frm || !d->pkt) { avcodec_free_context(&d->ctx); av_frame_free(&d->frm); av_packet_free(&d->pkt); d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0); RET(ERR_API_FAIL); return; }
		d->ctx->sample_rate = d->sample_rate;
		av_channel_layout_default(&d->ctx->ch_layout, d->channels > 0 ? d->channels : 2);
		/* a raw AAC frame carries no configuration: the AudioSpecificConfig is what the console's decoder is given
		 * separately. AAC LC (2), the sample rate's index and the channel count, as ScarletNX's Vita HLE builds it */
		{
			static const uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
			uint32_t index = 0; while (index < 12 && rates[index] != (uint32_t)d->sample_rate) index++;
			uint8_t *extra = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
			if (!extra) { avcodec_free_context(&d->ctx); av_frame_free(&d->frm); av_packet_free(&d->pkt); d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0); RET(ERR_API_FAIL); return; }
			extra[0] = (uint8_t)((2 << 3) | (index >> 1));
			extra[1] = (uint8_t)(((index & 1) << 7) | ((d->channels & 15) << 3));
			d->ctx->extradata = extra; d->ctx->extradata_size = 2;
		}
		if (avcodec_open2(d->ctx, codec, NULL) < 0) { if (ad_log()) fprintf(stderr, "audiodec: AAC open failed\n"); avcodec_free_context(&d->ctx); av_frame_free(&d->frm); av_packet_free(&d->pkt); d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0); RET(ERR_UNSUPPORTED); return; }
		WR32(ctrl + 16, (uint32_t)(1536 + (d->is_adts ? 0x100 : 0)));
		WR32(ctrl + 28, (uint32_t)((d->channels > 0 ? d->channels : 2) * 2048 * 2 * (sbr ? 2 : 1)));
		if (ad_log()) fprintf(stderr, "audiodec: AAC ready: ch %d rate %d adts %d sbr %d\n", d->channels, d->sample_rate, d->is_adts, sbr);
		RET(0);
	}
	case TYPE_MP3: {
		d->channels = (int)RD32(info + 4);
		d->sample_rate = 0;
		const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
		if (!codec) { d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0); RET(ERR_UNSUPPORTED); return; }
		d->ctx = avcodec_alloc_context3(codec);
		d->frm = av_frame_alloc(); d->pkt = av_packet_alloc();
		if (!d->ctx || !d->frm || !d->pkt) { avcodec_free_context(&d->ctx); av_frame_free(&d->frm); av_packet_free(&d->pkt); d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0); RET(ERR_API_FAIL); return; }
		av_channel_layout_default(&d->ctx->ch_layout, d->channels > 0 ? d->channels : 2);
		if (avcodec_open2(d->ctx, codec, NULL) < 0) { avcodec_free_context(&d->ctx); av_frame_free(&d->frm); av_packet_free(&d->pkt); d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0); RET(ERR_UNSUPPORTED); return; }
		WR32(ctrl + 16, 1441);
		WR32(ctrl + 28, (uint32_t)((d->channels > 0 ? d->channels : 2) * 1152 * 2));
		RET(0);
	}
#endif
	default:
		d->used = 0; pthread_mutex_destroy(&d->mx); WR32(ctrl + 4, 0);
		RET(ERR_UNSUPPORTED);
	}
}

static void h_delete(Cpu *c) {
	uint32_t ctrl = A0, err;
	if (!ctrl_ok(ctrl, &err)) { RET(err); return; }
	Adec *d = dec_of(C_HANDLE(ctrl));
	if (!d) { RET(ERR_INVALID_HANDLE); return; }
	pthread_mutex_lock(&d->mx);
	if (d->at9) { Atrac9ReleaseHandle(d->at9); d->at9 = 0; }
#ifdef RECOMP_AVCODEC
	avcodec_free_context(&d->ctx); av_frame_free(&d->frm); av_packet_free(&d->pkt); free(d->pktbuf);
#endif
	pthread_mutex_unlock(&d->mx);
	pthread_mutex_destroy(&d->mx);
	memset(d, 0, sizeof *d);
	WR32(ctrl + 4, 0);
	RET(0);
}

static void h_clear(Cpu *c) {
	uint32_t ctrl = A0, err;
	if (!ctrl_ok(ctrl, &err)) { RET(err); return; }
	Adec *d = dec_of(C_HANDLE(ctrl));
	if (!d) { RET(ERR_INVALID_HANDLE); return; }
#ifdef RECOMP_AVCODEC
	if (d->ctx) avcodec_flush_buffers(d->ctx);
#endif
	RET(0);
}

#ifdef RECOMP_AVCODEC
/* FLTP (AAC) or S16/FLT (MP3) into interleaved 16-bit, the Vita's PCM */
static uint32_t pcm_s16(const AVFrame *f, uint8_t *out, uint32_t cap, int *channels) {
	int ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 2;
	int n = f->nb_samples;
	*channels = ch;
	uint32_t need = (uint32_t)(n * ch * 2);
	if (need > cap) { n = (int)(cap / (uint32_t)(ch * 2)); need = (uint32_t)(n * ch * 2); }
	short *o = (short *)out;
	if (f->format == AV_SAMPLE_FMT_S16 || f->format == AV_SAMPLE_FMT_S16P) {
		for (int i = 0; i < n; i++) for (int k = 0; k < ch; k++) {
			const short *p = (const short *)(f->format == AV_SAMPLE_FMT_S16 ? f->data[0] + (size_t)i * ch * 2 : f->extended_data[k] + (size_t)i * 2);
			o[i * ch + k] = *p;
		}
	} else if (f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_FLT) {
		for (int i = 0; i < n; i++) for (int k = 0; k < ch; k++) {
			float v = f->format == AV_SAMPLE_FMT_FLTP ? ((const float *)f->extended_data[k])[i] : ((const float *)f->data[0])[(size_t)i * ch + k];
			int s = (int)lrintf(v * 32767.0f);
			o[i * ch + k] = (short)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
		}
	} else {
		memset(out, 0, need);
	}
	return need;
}
#endif

/* one decode call: ctrl's es bytes in, PCM out; returns the guest error code */
static int ad_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_AUDIODEC_LOG") != 0; return v; }
static uint32_t decode_one(uint32_t ctrl) {
	Adec *d = dec_of(C_HANDLE(ctrl));
	if (!d) return ERR_INVALID_HANDLE;
	uint32_t es = C_ES(ctrl), esmax = C_ES_MAX(ctrl), pcm = C_PCM(ctrl), pcmmax = C_PCM_MAX(ctrl);
	if (ad_log()) fprintf(stderr, "audiodec: type %#x es %#x used %u max %u pcm %#x cap %u\n",
	                      d->type, es, C_ES_USED(ctrl), esmax, pcm, pcmmax);
	if (!es || !pcm) return ERR_INVALID_PTR;
	WR32(ctrl + 12, 0);   /* es used */
	WR32(ctrl + 24, 0);   /* pcm used */

	if (d->type == TYPE_AT9) {
		if (!d->at9) return ERR_INVALID_HANDLE;
		int used = 0;
		int r = Atrac9Decode(d->at9, G(es), (short *)G(pcm), &used);
		if (r < 0) return ERR_API_FAIL;
		WR32(ctrl + 12, (uint32_t)used);
		uint32_t bytes = (uint32_t)(d->at9_frame_samples * d->channels * 2);
		if (bytes > pcmmax) bytes = pcmmax;
		WR32(ctrl + 24, bytes);
		return 0;
	}
#ifdef RECOMP_AVCODEC
	if (!d->ctx) return ERR_INVALID_HANDLE;
	uint32_t n = esmax;
	uint32_t given = C_ES_USED(ctrl);
	if (given && given <= esmax) n = given;   /* the game said how many bytes this unit is */
	if (d->type == TYPE_AAC && d->is_adts && esmax >= 7) {
		uint8_t *p = G(es);
		if (p[0] == 0xFF && (p[1] & 0xF0) == 0xF0) {
			uint32_t l = ((uint32_t)(p[3] & 3) << 11) | ((uint32_t)p[4] << 3) | (p[5] >> 5);
			if (l >= 7 && l <= esmax) n = l;
		}
	}
	size_t need = (size_t)n + AV_INPUT_BUFFER_PADDING_SIZE;
	if (d->pktcap < need) { free(d->pktbuf); d->pktbuf = malloc(need); d->pktcap = d->pktbuf ? need : 0; if (!d->pktbuf) return ERR_API_FAIL; }
	memcpy(d->pktbuf, G(es), n); memset(d->pktbuf + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	d->pkt->data = d->pktbuf; d->pkt->size = (int)n;
	pthread_mutex_lock(&d->mx);
	int r = avcodec_send_packet(d->ctx, d->pkt);	if (r == AVERROR(EAGAIN)) { av_frame_unref(d->frm); avcodec_receive_frame(d->ctx, d->frm); r = avcodec_send_packet(d->ctx, d->pkt); }
	av_frame_unref(d->frm);
	int got = r >= 0 ? avcodec_receive_frame(d->ctx, d->frm) : r;
	pthread_mutex_unlock(&d->mx);
	WR32(ctrl + 12, n);
	if (got < 0) return 0;   /* no frame this time: not a stream error (the decoder may need another unit) */
	int ch = 0;
	uint32_t bytes = pcm_s16(d->frm, G(pcm), pcmmax, &ch);
	WR32(ctrl + 24, bytes);
	if (ad_log()) fprintf(stderr, "audiodec: decoded %u bytes, %d ch, %d samples, fmt %d, es %u\n",
	                      bytes, ch, d->frm->nb_samples, d->frm->format, n);
	(void)ch;
	return 0;
#else
	return ERR_UNSUPPORTED;
#endif
}

static void h_decode(Cpu *c) {
	uint32_t ctrl = A0, err;
	if (!ctrl_ok(ctrl, &err)) { RET(err); return; }
	RET(decode_one(ctrl));
}
static void h_decode_nframes(Cpu *c) {
	uint32_t ctrl = A0, n = A1, err;
	if (!ctrl_ok(ctrl, &err)) { RET(err); return; }
	for (uint32_t i = 0; i < n; i++) {
		uint32_t r = decode_one(ctrl);
		if (r) { RET(r); return; }
	}
	RET(0);
}
static void h_internal_error(Cpu *c) {
	uint32_t ctrl = A0, err;
	if (!ctrl_ok(ctrl, &err)) { RET(err); return; }
	if (A1) WR32(A1, 0);
	RET(0);
}
static void h_create_external(Cpu *c) {   /* (ctrl, type, vaContext, contextSize): the host keeps its own memory */
	uint32_t ctrl = A0, type = A1;
	h_create(c);   /* reads ctrl/type from the same registers */
	(void)ctrl; (void)type;
}
static void h_delete_external(Cpu *c) {
	uint32_t ctrl = A0;
	h_delete(c);
	if (A1) WR32(A1, 0);
}

const HleEntry hle_audiodec[] = {
	{"sceAudiodecInitLibrary", h_init}, {"sceAudiodecTermLibrary", h_term},
	{"sceAudiodecGetContextSize", h_context_size},
	{"sceAudiodecCreateDecoder", h_create}, {"sceAudiodecCreateDecoderExternal", h_create_external},
	{"sceAudiodecDeleteDecoder", h_delete}, {"sceAudiodecDeleteDecoderExternal", h_delete_external},
	{"sceAudiodecDecode", h_decode}, {"sceAudiodecDecodeNFrames", h_decode_nframes},
	{"sceAudiodecClearContext", h_clear}, {"sceAudiodecGetInternalError", h_internal_error},
	{0, 0},
};

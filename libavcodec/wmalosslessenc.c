/*
 * Windows Media Audio Lossless encoder
 * Copyright (c) 2025 Andrew Moe
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_components.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

#include "libavutil/audio_fifo.h"
#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "wma_common.h"

#define ENC_MAX_CHANNELS            8
#define CDLMS_MAX_ORDER             256
#define CDLMS_MAX_FILTERS           8

#define TRANSIENT_INITIAL_COUNT     2048
#define TRANSIENT_MIN_COUNT         1024
#define ENTROPY_SAMPLE_WINDOW       128
#define SEEKABLE_FRAME_INTERVAL     20480

#define WMALOSSLESS_MAX_SAMPLE_RATE  96000
#define WMALOSSLESS_PACKET_SIZE_441  13375
#define WMALOSSLESS_PACKET_SIZE_48   12288
#define WMALOSSLESS_DEFAULT_BIT_RATE 1152000
#define WMALOSSLESS_CODEC_DEFAULT_BIT_RATE 128000

#define WMA_DECODE_FLAG_LEN_PREFIX  0x0040
#define WMA_DECODE_FLAG_DRC         0x0080
#define WMA_DECODE_FLAG_EXT         0x0100
#define WMA_DECODE_FLAG_SUBFRAMES_MASK 0x0038
#define WMA_DECODE_FLAGS_DEFAULT    0x01A1

typedef struct BitWriter {
    uint8_t *buf;
    size_t   capacity;
    size_t   pos;
    uint32_t bitbuf;
    int      bitcnt;
} BitWriter;

static av_cold void bw_init(BitWriter *bw, uint8_t *buf, size_t capacity)
{
    bw->buf      = buf;
    bw->capacity = capacity;
    bw->pos      = 0;
    bw->bitbuf   = 0;
    bw->bitcnt   = 0;
}

static av_always_inline void bw_put_byte(BitWriter *bw, uint8_t b)
{
    if (bw->pos < bw->capacity)
        bw->buf[bw->pos] = b;
    bw->pos++;
}

static void bw_put_bits(BitWriter *bw, unsigned value, int n)
{
    if (n <= 0)
        return;

    if (n < 32)
        value &= (1U << n) - 1U;

    bw->bitbuf = (bw->bitbuf << n) | value;
    bw->bitcnt += n;

    while (bw->bitcnt >= 8) {
        uint8_t out = (uint8_t)(bw->bitbuf >> (bw->bitcnt - 8));
        bw_put_byte(bw, out);
        bw->bitcnt -= 8;
        if (bw->bitcnt)
            bw->bitbuf &= (1U << bw->bitcnt) - 1U;
        else
            bw->bitbuf = 0;
    }
}

static void bw_put_bit(BitWriter *bw, unsigned bit)
{
    bw_put_bits(bw, bit & 1U, 1);
}

static void bw_pad_to_byte(BitWriter *bw)
{
    if (bw->bitcnt)
        bw_put_bits(bw, 0, 8 - bw->bitcnt);
}

static void bw_flush(BitWriter *bw)
{
    bw_pad_to_byte(bw);
}

static size_t bw_bytes(const BitWriter *bw)
{
    return bw->pos;
}

static size_t bw_bits(const BitWriter *bw)
{
    return bw->pos * 8 + bw->bitcnt;
}

static void bw_put_bits_exact(BitWriter *bw, const uint8_t *src, int nbits)
{
    int bytes, rem, i;

    if (nbits <= 0)
        return;

    bytes = nbits >> 3;
    rem   = nbits & 7;

    for (i = 0; i < bytes; i++)
        bw_put_bits(bw, src[i], 8);

    if (rem) {
        uint8_t b = src[bytes];
        b >>= 8 - rem;
        bw_put_bits(bw, b, rem);
    }
}

static void bw_put_bits_slice(BitWriter *bw, const uint8_t *src,
                               int start_bit, int nbits)
{
    int byte, off, take;
    uint8_t b;

    if (nbits <= 0)
        return;

    byte = start_bit >> 3;
    off  = start_bit & 7;

    if (off) {
        take = FFMIN(8 - off, nbits);
        b = src[byte] << off;
        b >>= 8 - take;
        bw_put_bits(bw, b, take);
        nbits -= take;
        byte++;
    }

    while (nbits >= 8) {
        bw_put_bits(bw, src[byte], 8);
        byte++;
        nbits -= 8;
    }

    if (nbits > 0) {
        b = src[byte] >> (8 - nbits);
        bw_put_bits(bw, b, nbits);
    }
}

typedef struct CDLMSFilter {
    int order;
    int scaling;
    int16_t coefs[CDLMS_MAX_ORDER];
    int32_t history[CDLMS_MAX_ORDER * 2];
    int16_t updates[CDLMS_MAX_ORDER * 2];
    int recent;
} CDLMSFilter;

typedef struct CDLMSContext {
    int num_filters;
    CDLMSFilter filters[CDLMS_MAX_FILTERS];
    int update_speed;
} CDLMSContext;

static av_always_inline int wmalossless_sign32(int32_t v)
{
    return (v > 0) - (v < 0);
}

static av_cold void cdlms_init(CDLMSContext *ctx, int num_filters)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->num_filters = num_filters;
    ctx->update_speed = 8;
}

static void cdlms_config_filter(CDLMSContext *ctx, int filter_idx,
                                          int order, int scaling)
{
    if (filter_idx >= ctx->num_filters)
        return;

    CDLMSFilter *f = &ctx->filters[filter_idx];
    memset(f, 0, sizeof(*f));
    f->order  = order;
    f->scaling = scaling;
    f->recent = order;
}

static void cdlms_reset(CDLMSContext *ctx)
{
    int i;
    CDLMSFilter *f;

    for (i = 0; i < ctx->num_filters; i++) {
        f = &ctx->filters[i];
        memset(f->coefs,    0, sizeof(f->coefs));
        memset(f->history,  0, sizeof(f->history));
        memset(f->updates,  0, sizeof(f->updates));
        f->recent = f->order;
    }
    ctx->update_speed = 16;
}

/* Performs adaptive prediction using the filter's coefficient history
 * and updates coefficients based on the prediction error. */
static int32_t cdlms_filter_forward(CDLMSFilter *f, int32_t sample,
                                    int update_speed, int bits_per_sample)
{
    const int order = f->order;
    int recent, sign, i;
    unsigned pred;
    int32_t predicted, residue, clipped;
    const int range = 1 << (bits_per_sample - 1);
    size_t tail;

    if (order <= 0)
        return sample;

    recent = f->recent;
    pred = (1U << f->scaling) >> 1;

    for (i = 0; i < order; i++)
        pred += f->coefs[i] * f->history[recent + i];

    predicted = (int)pred >> f->scaling;
    residue   = sample - predicted;
    sign = wmalossless_sign32(residue);

    if (sign) {
        for (i = 0; i < order; i++)
            f->coefs[i] = (int16_t)(f->coefs[i] +
                                    sign * f->updates[recent + i]);
    }

    if (recent) {
        recent--;
    } else {
        memcpy(f->history + order, f->history, order * sizeof(*f->history));
        memcpy(f->updates + order, f->updates, order * sizeof(*f->updates));
        recent = order - 1;
    }

    clipped = av_clip(sample, -range, range - 1);
    f->history[recent] = clipped;

    f->updates[recent] = (int16_t)(wmalossless_sign32(sample) * update_speed);
    if (order >= 16)
        f->updates[recent + (order >> 4)] >>= 2;
    if (order >= 8)
        f->updates[recent + (order >> 3)] >>= 1;

    tail = (size_t)(order * 2 - (recent + order));
    if (tail)
        memset(f->updates + recent + order, 0, tail * sizeof(*f->updates));

    f->recent = recent;
    return residue;
}

static void cdlms_forward(CDLMSContext *ctx, int32_t *samples,
                                    int num_samples, int bits_per_sample)
{
    int s, i;
    int32_t val;
    CDLMSFilter *f;

    if (!ctx || ctx->num_filters <= 0)
        return;

    for (s = 0; s < num_samples; s++) {
        val = samples[s];
        for (i = 0; i < ctx->num_filters; i++) {
            f = &ctx->filters[i];
            if (f->order <= 0)
                continue;
            val = cdlms_filter_forward(f, val, ctx->update_speed,
                                       bits_per_sample);
        }
        samples[s] = val;
    }
}

typedef struct EntropyContext {
    uint32_t ave_sum;
    int movave_scaling;
    int transient;
    int transient_pos;
    int transient_counter;
} EntropyContext;

static av_cold void entropy_init(EntropyContext *ctx, int movave_scaling)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->movave_scaling = movave_scaling;
    ctx->transient = 1;
    ctx->transient_counter = TRANSIENT_INITIAL_COUNT;
}

static void entropy_reset(EntropyContext *ctx)
{
    ctx->transient = 1;
    ctx->transient_pos = 0;
    ctx->transient_counter = FFMAX(ctx->transient_counter, TRANSIENT_MIN_COUNT);
}

static uint32_t entropy_zigzag_encode(int32_t v)
{
    return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

static int entropy_calc_rice_k(uint32_t ave_mean)
{
    if (ave_mean <= 1)
        return 0;
    return av_log2(ave_mean - 1) + 1;
}

/* Encodes value as q ones followed by a zero. For large values (>=32),
 * uses extended encoding with explicit length field. */
static void entropy_write_unary(BitWriter *bw, uint32_t q)
{
    uint32_t ext;
    int i, cnt;

    if (q < 32) {
        while (q--)
            bw_put_bit(bw, 1);
        bw_put_bit(bw, 0);
        return;
    }

    for (i = 0; i < 32; i++)
        bw_put_bit(bw, 1);
    bw_put_bit(bw, 0);

    ext = q - 32;
    cnt = ext ? av_log2(ext) + 1 : 1;
    cnt = FFMIN(cnt, 25);
    bw_put_bits(bw, cnt - 1, 5);
    if (cnt)
        bw_put_bits(bw, ext, cnt);
}

/* Writes compressed residues using adaptive Rice parameter estimation
 * based on moving average statistics. */
static void entropy_encode_channel(BitWriter *bw, EntropyContext *ctx,
                                   const int32_t *residues, int nb_samples,
                                   int bits_per_sample, int seekable_tile,
                                   int inter_ch_decorr)
{
    int i, k, sample_window, first_bits;
    uint32_t prev_sum, ave_mean, simple_mean, max_reasonable, prev_mean;
    uint32_t max_mean, mask, v, ave, q, r;
    uint64_t sample_sum;

    if (nb_samples <= 0)
        return;

    bw_put_bit(bw, 0); /* transient flag */

    if (seekable_tile) {
        prev_sum = ctx->ave_sum;
        entropy_reset(ctx);

        ave_mean = 0;
        sample_window = FFMIN(nb_samples, ENTROPY_SAMPLE_WINDOW);
        if (sample_window > 0) {
            sample_sum = 0;
            for (i = 0; i < sample_window; i++) {
                v = entropy_zigzag_encode(residues[i]);
                sample_sum += v;
            }

            simple_mean = (uint32_t)(sample_sum / sample_window);
            max_reasonable = (1U << (bits_per_sample - 1));

            if (prev_sum > 0) {
                prev_mean = prev_sum >> (ctx->movave_scaling + 1);
                ave_mean = (simple_mean * 3 + prev_mean) / 4;
            } else {
                ave_mean = simple_mean;
            }

            ave_mean = FFMIN(ave_mean, max_reasonable);
        } else {
            ave_mean = prev_sum >> (ctx->movave_scaling + 1);
        }

        max_mean = bits_per_sample >= 32 ?
                   UINT32_MAX : ((1U << bits_per_sample) - 1U);
        ave_mean = FFMIN(ave_mean, max_mean);
        bw_put_bits(bw, ave_mean, bits_per_sample);
        ctx->ave_sum = ave_mean << (ctx->movave_scaling + 1);


        if (nb_samples > 0) {
            first_bits = bits_per_sample + (inter_ch_decorr ? 1 : 0);
            mask = first_bits >= 32 ?
                   UINT32_MAX : ((1U << first_bits) - 1U);
            bw_put_bits(bw, residues[0] & mask, first_bits);
        }

        for (i = 1; i < nb_samples; i++) {
            v = entropy_zigzag_encode(residues[i]);
            ave = (ctx->ave_sum + (1U << ctx->movave_scaling)) >>
                  (ctx->movave_scaling + 1);
            k = entropy_calc_rice_k(ave);
            q = k ? v >> k : v;
            r = k ? (v & ((1U << k) - 1U)) : 0;
            entropy_write_unary(bw, q);
            if (k)
                bw_put_bits(bw, r, k);
            ctx->ave_sum = v + ctx->ave_sum -
                           (ctx->ave_sum >> ctx->movave_scaling);
        }
        return;
    }

    for (i = 0; i < nb_samples; i++) {
        v = entropy_zigzag_encode(residues[i]);
        ave = (ctx->ave_sum + (1U << ctx->movave_scaling)) >>
              (ctx->movave_scaling + 1);
        k = entropy_calc_rice_k(ave);
        q = k ? v >> k : v;
        r = k ? (v & ((1U << k) - 1U)) : 0;
        entropy_write_unary(bw, q);
        if (k)
            bw_put_bits(bw, r, k);
        ctx->ave_sum = v + ctx->ave_sum -
                       (ctx->ave_sum >> ctx->movave_scaling);
    }
}

typedef struct WMALosslessEncParams {
    int bits_per_sample;                                ///< audio bit depth (16 or 24)
    int channels;                                       ///< number of audio channels
    int sample_rate;                                    ///< audio sample rate
    uint32_t channel_mask;                              ///< channel layout mask
    uint16_t decode_flags;                              ///< WMA decoder flags
    uint32_t packet_size;                               ///< ASF packet size in bytes
    int enable_cdlms;                                   ///< enable CDLMS adaptive filters
    int force_rawpcm;                                   ///< force raw PCM output (no compression)
} WMALosslessEncParams;

typedef struct WMALosslessPacket {
    uint8_t  *data;                                     ///< encoded packet data
    int       size;                                     ///< packet size in bytes
    uint64_t  pts_samples;                              ///< PTS in samples
    int       duration_samples;                         ///< packet duration in samples
} WMALosslessPacket;

typedef struct WMALosslessEncContext {
    AVCodecContext *avctx;
    WMALosslessEncParams par;                           ///< encoder configuration parameters

    /* frame configuration */
    int samples_per_frame;                              ///< number of samples per frame
    int log2_frame_size;                                ///< log2 of samples_per_frame

    /* packet state */
    uint8_t *packet_buf;                                ///< ASF packet buffer
    BitWriter packet_bw;                                ///< bitwriter for current packet
    uint8_t packet_seq;                                 ///< current packet sequence number
    uint64_t total_samples;                             ///< total samples encoded across all packets
    uint64_t written_samples;                           ///< samples written to output
    uint32_t frame_index;                               ///< current frame index
    int first_frame;                                    ///< flag indicating first frame in stream

    /* adaptive coding filter */
    int use_inter_ch;                                   ///< enable inter-channel decorrelation
    int use_ac_filter;                                  ///< enable adaptive coding filter
    int do_mclms;                                       ///< enable multi-channel LMS
    int ac_order;                                       ///< adaptive coding filter order
    int ac_scaling;                                     ///< adaptive coding filter scaling
    int16_t ac_coefs[16];                               ///< adaptive coding filter coefficients
    int32_t ac_prev[ENC_MAX_CHANNELS][16];              ///< AC filter previous values per channel

    /* CDLMS (per-channel adaptive filters) */
    CDLMSContext *cdlms;                                ///< CDLMS filter contexts per channel
    EntropyContext *entropy;                            ///< entropy coding contexts per channel

    /* multi-channel LMS filter */
    int mclms_order;                                    ///< MCLMS filter order
    int mclms_scaling;                                  ///< MCLMS filter scaling factor
    int16_t mclms_coeffs[ENC_MAX_CHANNELS * ENC_MAX_CHANNELS * 32]; ///< MCLMS coefficient history
    int16_t mclms_coeffs_cur[ENC_MAX_CHANNELS * ENC_MAX_CHANNELS];  ///< current MCLMS coefficients
    int32_t mclms_prevvalues[ENC_MAX_CHANNELS * 2 * 32];            ///< MCLMS previous sample values
    int32_t mclms_updates[ENC_MAX_CHANNELS * 2 * 32];               ///< MCLMS coefficient updates
    int mclms_recent;                                   ///< index of most recent MCLMS update

    /* seekable frame placement */
    int seekable_target_interval;                      ///< target interval between seekable frames
    int64_t last_seekable_samples;                     ///< sample position of last seekable frame
    int64_t seekable_accumulated_error;                ///< accumulated error for seekable placement
    uint8_t current_packet_has_seekable;               ///< flag if current packet contains a seekable frame

    /* frame spanning state (when frames cross packet boundaries) */
    int pending_frame_bits;                            ///< total bits in pending frame
    int pending_frame_pos_bits;                        ///< current bit position in pending frame
    uint8_t *pending_frame_buf;                        ///< buffer holding pending frame data
    int pending_frame_seekable;                        ///< flag if pending frame is seekable
    uint16_t pending_frame_samples;                    ///< sample count in pending frame

    /* statistics */
    int stat_frames_in_packet;                         ///< number of frames in current packet
    int stat_packet_bits_used;                         ///< bits used in current packet

    /* input buffering */
    AVAudioFifo *fifo;                                 ///< FIFO for input samples
    int64_t input_samples;                             ///< total input samples received

    /* packet queue for output */
    WMALosslessPacket *packet_queue;                   ///< queue of encoded packets
    int packet_queue_count;                            ///< number of packets in queue
    int packet_queue_index;                            ///< index of next packet to output
} WMALosslessEncContext;

static int wmalossless_ilog2u(uint32_t v)
{
    int n;

    n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

static void wmalossless_clear_packet_queue(WMALosslessEncContext *s)
{
    int i;

    if (!s->packet_queue)
        return;

    for (i = s->packet_queue_index; i < s->packet_queue_count; i++)
        av_freep(&s->packet_queue[i].data);

    av_freep(&s->packet_queue);
    s->packet_queue_count = 0;
    s->packet_queue_index = 0;
}

static int wmalossless_queue_packet(WMALosslessEncContext *s, const uint8_t *data,
                                    int size, uint64_t pts_samples,
                                    int duration_samples)
{
    WMALosslessPacket *tmp, *pkt;

    tmp = av_realloc_array(s->packet_queue,
                           s->packet_queue_count + 1,
                           sizeof(*tmp));
    if (!tmp)
        return AVERROR(ENOMEM);

    s->packet_queue = tmp;
    pkt = &s->packet_queue[s->packet_queue_count++];

    pkt->data = av_malloc(size);
    if (!pkt->data)
        return AVERROR(ENOMEM);
    memcpy(pkt->data, data, size);
    pkt->size = size;
    pkt->pts_samples = pts_samples;
    pkt->duration_samples = duration_samples;

    return 0;
}

static int wmalossless_frame_build_bits(WMALosslessEncContext *s,
                                        int32_t **ch_buf, uint16_t frame_samples,
                                        int seekable, uint8_t **out_buf,
                                        size_t *out_bytes);
static void wmalossless_packet_start(WMALosslessEncContext *s,
                                     unsigned num_bits_prev_frame);
static void wmalossless_packet_write_bits(WMALosslessEncContext *s,
                                          const uint8_t *buf, int start_bit,
                                          int nbits);
static int wmalossless_packet_bits_capacity(WMALosslessEncContext *s);
static int wmalossless_packet_header_bits(WMALosslessEncContext *s);
static int wmalossless_packet_calculate_frame_total_bits(WMALosslessEncContext *s,
                                                         int frame_bits);
static void wmalossless_packet_write_frame(WMALosslessEncContext *s,
                                           uint8_t *frame_buf, int frame_bits,
                                           int will_have_more);
static void wmalossless_packet_finalize(WMALosslessEncContext *s,
                                        int eff_samples);
static int wmalossless_seekable_should_place(WMALosslessEncContext *s,
                                             int64_t current_samples,
                                             int is_first_frame);
static void wmalossless_seekable_update_position(WMALosslessEncContext *s,
                                                  int64_t frame_sample_position);

/* Processes buffered PCM samples, encodes them into WMA Lossless frames,
 * and packs multiple frames into ASF packets. Handles frame splitting
 * across packet boundaries and seekable frame placement. */
static int wmalossless_encode_fifo(WMALosslessEncContext *s)
{
    const int channels = s->par.channels;
    const int fmt_is_s32 = s->avctx->sample_fmt == AV_SAMPLE_FMT_S32P;
    const int use_len_prefix = (s->par.decode_flags &
                                WMA_DECODE_FLAG_LEN_PREFIX) ? 1 : 0;
    int total_samples, ret, consumed, c, d, i;
    int packet_bits_cap, header_bits, payload_cap_bits, pending_remaining;
    int carry_bits, header_payload_bits, packet_bits_used, packet_eff_samples;
    int bits_to_write, write_remain, bits_left_after_trailer;
    int more_samples_pending, will_have_more, take, frame_first, seekable;
    int frame_bits, total, remaining_bits, to_write;
    int32_t **pcm, **ch, v;
    int16_t **tmp16;
    uint16_t frame_samples;
    uint64_t pkt_pts_samples;
    uint8_t *frame_buf;
    size_t frame_bytes;
    int64_t frame_sample_position;
    void **tmp;

    total_samples = av_audio_fifo_size(s->fifo);
    ret = 0;

    if (total_samples <= 0 &&
        s->pending_frame_bits <= s->pending_frame_pos_bits)
        return 0;

    pcm = av_calloc(channels, sizeof(*pcm));
    if (!pcm)
        return AVERROR(ENOMEM);

    for (c = 0; c < channels; c++) {
        pcm[c] = av_calloc(total_samples, sizeof(**pcm));
        if (!pcm[c]) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (total_samples > 0) {
        if (fmt_is_s32) {
            tmp = av_malloc_array(channels, sizeof(*tmp));
            if (!tmp) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            for (c = 0; c < channels; c++)
                tmp[c] = pcm[c];
            ret = av_audio_fifo_read(s->fifo, tmp, total_samples);
            av_free(tmp);
            if (ret < total_samples) {
                ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
                goto fail;
            }
        } else {
            tmp16 = av_malloc_array(channels, sizeof(*tmp16));
            if (!tmp16) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            for (c = 0; c < channels; c++) {
                tmp16[c] = av_malloc_array(total_samples, sizeof(**tmp16));
                if (!tmp16[c]) {
                    ret = AVERROR(ENOMEM);
                    for (d = 0; d < c; d++)
                        av_freep(&tmp16[d]);
                    av_free(tmp16);
                    goto fail;
                }
            }
            ret = av_audio_fifo_read(s->fifo, (void **)tmp16, total_samples);
            if (ret < total_samples) {
                ret = ret < 0 ? ret : AVERROR_INVALIDDATA;
                for (c = 0; c < channels; c++)
                    av_freep(&tmp16[c]);
                av_free(tmp16);
                goto fail;
            }
            for (c = 0; c < channels; c++) {
                for (i = 0; i < total_samples; i++)
                    pcm[c][i] = tmp16[c][i];
                av_freep(&tmp16[c]);
            }
            av_free(tmp16);
        }
    }

    av_audio_fifo_reset(s->fifo);

    consumed = 0;
    while (consumed < total_samples ||
           (s->pending_frame_bits > s->pending_frame_pos_bits)) {
        packet_bits_cap = wmalossless_packet_bits_capacity(s);
        header_bits = wmalossless_packet_header_bits(s);
        payload_cap_bits = packet_bits_cap - header_bits;
        pending_remaining = s->pending_frame_bits -
                            s->pending_frame_pos_bits;
        if (pending_remaining < 0)
            pending_remaining = 0;
        carry_bits = 0;
        if (pending_remaining > 0 && !use_len_prefix && payload_cap_bits > 0) {
            if (pending_remaining + 1 <= payload_cap_bits)
                carry_bits = pending_remaining;
            else
                carry_bits = payload_cap_bits;
        }
        header_payload_bits = FFMIN(carry_bits, pending_remaining);

        /* Check if we have a pending seekable frame that will go into this packet */
        if (s->pending_frame_bits > 0 && s->pending_frame_seekable) {
            s->current_packet_has_seekable = 1;
        }

        /* Check if this is the very first packet - should be seekable */
        if (s->packet_seq == 0 && s->pending_frame_bits == 0 &&
            consumed == 0 && s->written_samples == 0) {
            s->current_packet_has_seekable = 1;
        }

        wmalossless_packet_start(s, header_payload_bits);
        packet_bits_used = header_bits;
        packet_eff_samples = 0;
        pkt_pts_samples = s->written_samples;

        if (carry_bits > 0) {
            bits_to_write = carry_bits;
            write_remain = FFMIN(pending_remaining, bits_to_write);
            if (write_remain > 0) {
                wmalossless_packet_write_bits(s, s->pending_frame_buf,
                                              s->pending_frame_pos_bits,
                                              write_remain);
                s->pending_frame_pos_bits += write_remain;
                packet_bits_used += write_remain;
                bits_to_write -= write_remain;
            }
            if (bits_to_write > 0) {
                bits_left_after_trailer = packet_bits_cap -
                                          packet_bits_used - 1;
                more_samples_pending = (consumed < total_samples);
                will_have_more = (bits_left_after_trailer > 0 &&
                                  more_samples_pending) ? 1 : 0;
                bw_put_bit(&s->packet_bw, will_have_more ? 1 : 0);
                packet_bits_used++;
                bits_to_write--;
            }
            if (s->pending_frame_pos_bits >= s->pending_frame_bits) {
                frame_samples = s->pending_frame_samples ?
                                s->pending_frame_samples :
                                s->samples_per_frame;
                packet_eff_samples += frame_samples;
                if (s->pending_frame_seekable)
                    s->current_packet_has_seekable = 1;
                av_freep(&s->pending_frame_buf);
                s->pending_frame_bits = 0;
                s->pending_frame_pos_bits = 0;
                s->pending_frame_seekable = 0;
                s->pending_frame_samples = 0;
                s->frame_index++;
                s->first_frame = 0;
            }
        }

        while (packet_bits_used < packet_bits_cap && consumed < total_samples) {
            take = FFMIN(s->samples_per_frame, total_samples - consumed);
            frame_samples = take;

            ch = av_calloc(channels, sizeof(*ch));
            if (!ch) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            for (c = 0; c < channels; c++) {
                ch[c] = av_calloc(s->samples_per_frame, sizeof(int32_t));
                if (!ch[c]) {
                    ret = AVERROR(ENOMEM);
                    for (d = 0; d < c; d++)
                        av_freep(&ch[d]);
                    av_free(ch);
                    goto fail;
                }
            }

            for (i = 0; i < s->samples_per_frame; i++) {
                for (c = 0; c < channels; c++) {
                    v = (i < take) ? pcm[c][consumed + i] : 0;
                    /* For 24-bit in S32P, samples are in upper 24 bits, shift right by 8 */
                    if (s->par.bits_per_sample == 24)
                        v >>= 8;
                    ch[c][i] = v;
                }
            }

            frame_sample_position = s->total_samples + consumed;
            frame_first = (s->total_samples == 0 && consumed == 0 &&
                           packet_eff_samples == 0 &&
                           s->pending_frame_bits == 0);
            seekable = wmalossless_seekable_should_place(
                           s, frame_sample_position, frame_first);
            if (seekable) {
                wmalossless_seekable_update_position(s, frame_sample_position);
                s->current_packet_has_seekable = 1;
            }

            frame_buf = NULL;
            frame_bytes = 0;
            frame_bits = wmalossless_frame_build_bits(s, ch, frame_samples,
                                                      seekable, &frame_buf,
                                                      &frame_bytes);

            for (c = 0; c < channels; c++)
                av_freep(&ch[c]);
            av_free(ch);

            if (frame_bits < 0) {
                av_free(frame_buf);
                ret = frame_bits;
                goto fail;
            }

            consumed += take;

            if (use_len_prefix) {
                total = wmalossless_packet_calculate_frame_total_bits(
                            s, frame_bits);
                if (total > packet_bits_cap - packet_bits_used) {
                    s->pending_frame_buf = frame_buf;
                    s->pending_frame_bits = frame_bits + 1;
                    s->pending_frame_pos_bits = 0;
                    s->pending_frame_seekable = seekable;
                    s->pending_frame_samples = frame_samples;
                    frame_buf = NULL;
                    break;
                }
                wmalossless_packet_write_frame(s, frame_buf, frame_bits, 0);
                packet_bits_used += total;
                packet_eff_samples += frame_samples;
                av_free(frame_buf);
                frame_buf = NULL;
                s->frame_index++;
                s->first_frame = 0;
            } else {
                remaining_bits = packet_bits_cap - packet_bits_used;
                if (frame_bits + 1 <= remaining_bits) {
                    wmalossless_packet_write_bits(s, frame_buf, 0, frame_bits);
                    packet_bits_used += frame_bits;
                    bits_left_after_trailer = packet_bits_cap -
                                              packet_bits_used - 1;
                    more_samples_pending = (consumed < total_samples);
                    will_have_more = (bits_left_after_trailer > 0 &&
                                      more_samples_pending) ? 1 : 0;
                    bw_put_bit(&s->packet_bw, will_have_more ? 1 : 0);
                    packet_bits_used++;
                    packet_eff_samples += frame_samples;
                    av_free(frame_buf);
                    frame_buf = NULL;
                    s->frame_index++;
                    s->first_frame = 0;
                } else {
                    to_write = remaining_bits;
                    wmalossless_packet_write_bits(s, frame_buf, 0, to_write);
                    packet_bits_used += to_write;
                    s->pending_frame_buf = frame_buf;
                    s->pending_frame_bits = frame_bits + 1;
                    s->pending_frame_pos_bits = to_write;
                    s->pending_frame_seekable = seekable;
                    s->pending_frame_samples = frame_samples;
                    frame_buf = NULL;
                    break;
                }
            }
        }

        wmalossless_packet_finalize(s, packet_eff_samples);
        s->total_samples += packet_eff_samples;

        ret = wmalossless_queue_packet(s, s->packet_buf, s->par.packet_size,
                                        pkt_pts_samples, packet_eff_samples);
        if (ret < 0)
            goto fail;
    }

    for (c = 0; c < channels; c++)
        av_freep(&pcm[c]);
    av_free(pcm);
    return 0;

fail:
    for (c = 0; c < channels; c++)
        av_freep(&pcm[c]);
    av_free(pcm);
    return ret;
}

static void wmalossless_ac_filter_reset(WMALosslessEncContext *s)
{
    if (!s->use_ac_filter)
        return;
    memset(s->ac_prev, 0, sizeof(s->ac_prev));
}

static int wmalossless_ac_filter_apply(WMALosslessEncContext *s,
                                       int32_t **ch_buf,
                                       uint16_t frame_samples)
{
    int ch, i, j, order, scaling, idx, p;
    int32_t prev, last_in, in, pred_s32, x, *orig;
    int16_t *fc;
    int *pv;
    int64_t pred;

    if (!s->use_ac_filter || !frame_samples)
        return 0;

    for (ch = 0; ch < s->par.channels; ch++) {
        if (s->ac_order == 1 && s->ac_scaling == 0 && s->ac_coefs[0] == 1) {
            prev = s->ac_prev[ch][0];
            last_in = prev;
            for (i = 0; i < frame_samples; i++) {
                in = ch_buf[ch][i];
                pred_s32 = (i == 0) ? prev : last_in;
                ch_buf[ch][i] = in - pred_s32;
                last_in = in;
            }
            if (frame_samples)
                s->ac_prev[ch][0] = last_in;
            continue;
        }

        order = s->ac_order;
        scaling = s->ac_scaling;
        fc = s->ac_coefs;
        pv = s->ac_prev[ch];
        orig = av_malloc_array(frame_samples, sizeof(*orig));
        if (!orig)
            return AVERROR(ENOMEM);

        memcpy(orig, ch_buf[ch], frame_samples * sizeof(*orig));

        for (i = 0; i < frame_samples; i++) {
            pred = 0;
            for (j = 0; j < order; j++) {
                idx = (int)i - 1 - j;
                if (idx >= 0)
                    x = orig[idx];
                else {
                    p = -idx - 1;
                    x = (p < order) ? pv[p] : 0;
                }
                pred += (int64_t)fc[j] * x;
            }
            pred >>= scaling;
            ch_buf[ch][i] = orig[i] - (int32_t)pred;
        }

        for (j = 0; j < order; j++) {
            if ((int)frame_samples > j)
                pv[j] = orig[frame_samples - 1 - j];
        }

        av_free(orig);
    }

    return 0;
}

static void wmalossless_inter_ch_apply(WMALosslessEncContext *s,
                                       int32_t **ch_buf,
                                       uint16_t frame_samples)
{
    int i;
    int32_t L, R, X1, X0;

    if (!s->use_inter_ch || s->par.channels != 2)
        return;

    for (i = 0; i < frame_samples; i++) {
        L = ch_buf[0][i];
        R = ch_buf[1][i];
        X1 = R - L;
        X0 = L + (X1 >> 1);
        ch_buf[0][i] = X0;
        ch_buf[1][i] = X1;
    }
}

static void wmalossless_mclms_clear(WMALosslessEncContext *s)
{
    memset(s->mclms_coeffs, 0, sizeof(s->mclms_coeffs));
    memset(s->mclms_coeffs_cur, 0, sizeof(s->mclms_coeffs_cur));
    memset(s->mclms_prevvalues, 0, sizeof(s->mclms_prevvalues));
    memset(s->mclms_updates, 0, sizeof(s->mclms_updates));
    s->mclms_recent = s->mclms_order * s->par.channels;
}

static void wmalossless_mclms_reset(WMALosslessEncContext *s)
{
    s->mclms_recent = s->mclms_order * s->par.channels;
}

static void wmalossless_mclms_apply_forward(WMALosslessEncContext *s,
                                            int32_t **ch_buf,
                                            int nb_samples)
{
    const int order = s->mclms_order;
    const int channels = s->par.channels;
    const int max = 1 << (s->par.bits_per_sample - 1);
    int i, ch, j, hist_idx, coeff_idx, pos;
    int32_t original, residue;
    int64_t pred;

    if (!s->do_mclms)
        return;

    for (i = 0; i < nb_samples; i++) {
        for (ch = 0; ch < channels; ch++) {
            original = ch_buf[ch][i];
            pred = 0;
            for (j = 0; j < order * channels; j++) {
                hist_idx = j + s->mclms_recent;
                coeff_idx = j + order * channels * ch;
                pred += (int64_t)s->mclms_prevvalues[hist_idx] *
                        s->mclms_coeffs[coeff_idx];
            }
            pred >>= s->mclms_scaling;
            residue = original - (int32_t)pred;
            residue = av_clip(residue, -max, max - 1);
            ch_buf[ch][i] = residue;

            pos = s->mclms_recent - 1;
            if (pos < 0)
                pos = order * channels - 1;
            s->mclms_prevvalues[pos] = original;
            s->mclms_recent = pos;
        }
    }
}

static void wmalossless_cdlms_apply(WMALosslessEncContext *s, int32_t **ch_buf,
                                    uint16_t frame_samples, int seekable)
{
    int ch, il, i, prev, next, count;
    CDLMSFilter *f;

    if (!s->cdlms)
        return;

    for (ch = 0; ch < s->par.channels; ch++) {
        prev = s->cdlms[ch].update_speed;
        next = seekable ? 16 : 8;
        if (prev != next) {
            for (il = 0; il < s->cdlms[ch].num_filters; il++) {
                f = &s->cdlms[ch].filters[il];
                count = f->order * 2;
                if (count > 0) {
                    if (next == 16 && prev == 8) {
                        for (i = 0; i < count; i++)
                            f->updates[i] = (int16_t)(f->updates[i] * 2);
                    } else if (next == 8 && prev == 16) {
                        for (i = 0; i < count; i++)
                            f->updates[i] = (int16_t)(f->updates[i] / 2);
                    }
                }
            }
        }
        s->cdlms[ch].update_speed = next;
    }

    for (ch = 0; ch < s->par.channels; ch++)
        cdlms_forward(&s->cdlms[ch], ch_buf[ch], frame_samples,
                      s->par.bits_per_sample);
}

static int wmalossless_apply_transforms(WMALosslessEncContext *s,
                                        int32_t **ch_buf,
                                        uint16_t frame_samples,
                                        int seekable, int use_rawpcm)
{
    int ret;

    if (use_rawpcm)
        return 0;

    ret = wmalossless_ac_filter_apply(s, ch_buf, frame_samples);
    if (ret < 0)
        return ret;

    wmalossless_inter_ch_apply(s, ch_buf, frame_samples);

    if (s->do_mclms)
        wmalossless_mclms_apply_forward(s, ch_buf, frame_samples);

    wmalossless_cdlms_apply(s, ch_buf, frame_samples, seekable);

    return 0;
}

static void wmalossless_frame_write_tile_header(BitWriter *bw,
                                                WMALosslessEncContext *s,
                                                av_unused uint16_t frame_samples,
                                                av_unused uint16_t end_skip)
{
    int log2_max_num_subframes, max_num_subframes, c, ratio, len_bits;

    log2_max_num_subframes = (s->par.decode_flags &
                              WMA_DECODE_FLAG_SUBFRAMES_MASK) >> 3;
    max_num_subframes = 1 << log2_max_num_subframes;

    if (max_num_subframes > 1) {
        bw_put_bit(bw, 0);
        /* Only write channel bits if we have more than one channel.
         * For mono, the decoder auto-sets contains_subframe=1 without reading bits. */
        if (s->par.channels > 1) {
            for (c = 0; c < s->par.channels; c++)
                bw_put_bit(bw, 1);
        }
        if (max_num_subframes > 1) {
            ratio = max_num_subframes - 1;
            len_bits = wmalossless_ilog2u(max_num_subframes - 1) + 1;
            bw_put_bits(bw, ratio, len_bits);
        }
    } else {
        bw_put_bit(bw, 1);
    }
}

static void wmalossless_frame_write_drc_and_skip(BitWriter *bw,
                                                 WMALosslessEncContext *s,
                                                 uint16_t end_skip)
{
    const int start_skip = 0;
    const int have_end_skip = end_skip > 0;
    const int have_skip = start_skip || have_end_skip;
    int skip_bits;

    if (s->par.decode_flags & WMA_DECODE_FLAG_DRC)
        bw_put_bits(bw, 0, 8);

    bw_put_bit(bw, have_skip ? 1 : 0);
    if (have_skip) {
        skip_bits = wmalossless_ilog2u(
                        (unsigned)(s->samples_per_frame * 2));
        bw_put_bit(bw, start_skip ? 1 : 0);
        if (start_skip)
            bw_put_bits(bw, start_skip, skip_bits);
        bw_put_bit(bw, have_end_skip ? 1 : 0);
        if (have_end_skip)
            bw_put_bits(bw, end_skip, skip_bits);
    }
}

static void wmalossless_frame_write_ac_config(BitWriter *bw,
                                              WMALosslessEncContext *s)
{
    int i;
    unsigned x;

    bw_put_bits(bw, (unsigned)(s->ac_order - 1), 4);
    bw_put_bits(bw, s->ac_scaling, 4);
    for (i = 0; i < s->ac_order; i++) {
        if (s->ac_scaling > 0) {
            x = s->ac_coefs[i] > 0 ?
                (unsigned)(s->ac_coefs[i] - 1) : 0;
            bw_put_bits(bw, x, s->ac_scaling);
        }
    }
}

static void wmalossless_frame_write_mclms_config(BitWriter *bw,
                                                 WMALosslessEncContext *s)
{
    bw_put_bits(bw, (s->mclms_order / 2) - 1, 4);
    bw_put_bits(bw, 0, 4);
    bw_put_bit(bw, 0);
}

static void wmalossless_frame_write_cdlms_config(BitWriter *bw,
                                                 WMALosslessEncContext *s)
{
    int c, i, num_filters, order, scaling;

    bw_put_bit(bw, 0);  /* cdlms_send_coef = 0 */
    for (c = 0; c < s->par.channels; c++) {
        num_filters = s->cdlms ? s->cdlms[c].num_filters : 1;
        if (num_filters < 1) num_filters = 1;
        if (num_filters > 8) num_filters = 8;
        bw_put_bits(bw, num_filters - 1, 3);  /* cdlms_ttl[c] - 1 */

        /* Write order for each filter ((order/8) - 1) */
        for (i = 0; i < num_filters; i++) {
            order = 16;  /* Default order matching reference */
            if (s->cdlms && i < s->cdlms[c].num_filters)
                order = s->cdlms[c].filters[i].order;
            if (order < 8) order = 8;
            if (order > 256) order = 256;
            bw_put_bits(bw, (order / 8) - 1, 7);
        }

        /* Write scaling for each filter */
        for (i = 0; i < num_filters; i++) {
            scaling = 12;  /* Default scaling matching reference */
            if (s->cdlms && i < s->cdlms[c].num_filters)
                scaling = s->cdlms[c].filters[i].scaling;
            if (scaling < 0) scaling = 0;
            if (scaling > 15) scaling = 15;
            bw_put_bits(bw, scaling, 4);
        }
    }
}

static void wmalossless_frame_write_subframe_header(BitWriter *bw,
                                                    WMALosslessEncContext *s,
                                                    int seekable)
{
    int ch;

    bw_put_bit(bw, seekable ? 1 : 0);

    if (seekable) {
        bw_put_bit(bw, 0);
        bw_put_bit(bw, s->use_ac_filter ? 1 : 0);
        bw_put_bit(bw, (s->use_inter_ch && s->par.channels == 2) ? 1 : 0);
        bw_put_bit(bw, s->do_mclms ? 1 : 0);

        if (s->use_ac_filter)
            wmalossless_frame_write_ac_config(bw, s);
        if (s->do_mclms)
            wmalossless_frame_write_mclms_config(bw, s);
        wmalossless_frame_write_cdlms_config(bw, s);

        if (s->cdlms) {
            for (ch = 0; ch < s->par.channels; ch++)
                cdlms_reset(&s->cdlms[ch]);
        }
        wmalossless_ac_filter_reset(s);
        if (s->do_mclms)
            wmalossless_mclms_reset(s);
    }
}

static int wmalossless_frame_choose_movave(WMALosslessEncContext *s,
                                           int32_t **ch_buf,
                                           uint16_t frame_samples)
{
    int best_ms, best_bits, ms, total_bits, ch;
    uint8_t sbuf[64];
    BitWriter sbw;
    EntropyContext tmp;

    if (!s->entropy)
        return 3;

    best_ms = 5;
    best_bits = INT_MAX;

    for (ms = 5; ms <= 5; ms++) {
        bw_init(&sbw, sbuf, sizeof(sbuf));
        total_bits = 0;
        for (ch = 0; ch < s->par.channels; ch++) {
            tmp = s->entropy[ch];
            tmp.movave_scaling = ms;
            entropy_encode_channel(&sbw, &tmp, ch_buf[ch], frame_samples,
                                   s->par.bits_per_sample, 1,
                                   s->use_inter_ch && s->par.channels == 2);
            total_bits += (int)bw_bits(&sbw);
            bw_init(&sbw, sbuf, sizeof(sbuf));
        }
        if (total_bits < best_bits) {
            best_bits = total_bits;
            best_ms = ms;
        }
    }
    return best_ms;
}

static void wmalossless_frame_write_rawpcm(BitWriter *bw,
                                           WMALosslessEncContext *s,
                                           int32_t **ch_buf,
                                           uint16_t frame_samples)
{
    int bits, ch, i;
    int32_t sample;

    bits = s->par.bits_per_sample;
    for (ch = 0; ch < s->par.channels; ch++) {
        for (i = 0; i < s->samples_per_frame; i++) {
            sample = (i < frame_samples) ? ch_buf[ch][i] : 0;
            /* bw_put_bits takes lower N bits of the value, preserving signed representation */
            bw_put_bits(bw, sample, bits);
        }
    }
}

static int wmalossless_frame_write_cdlms_residues(BitWriter *bw,
                                                  WMALosslessEncContext *s,
                                                  int32_t **ch_buf,
                                                  uint16_t frame_samples,
                                                  int seekable)
{
    int ch, i;
    int32_t *channel_residues;

    for (ch = 0; ch < s->par.channels; ch++) {
        channel_residues = av_calloc(s->samples_per_frame,
                                     sizeof(*channel_residues));
        if (!channel_residues)
            return AVERROR(ENOMEM);

        for (i = 0; i < frame_samples; i++)
            channel_residues[i] = ch_buf[ch][i];

        entropy_encode_channel(bw, &s->entropy[ch], channel_residues,
                               s->samples_per_frame,
                               s->par.bits_per_sample, seekable,
                               s->use_inter_ch && s->par.channels == 2);

        av_free(channel_residues);
    }

    return 0;
}

static int wmalossless_frame_build_bits(WMALosslessEncContext *s,
                                        int32_t **ch_buf,
                                        uint16_t frame_samples,
                                        int seekable,
                                        uint8_t **out_buf,
                                        size_t *out_bytes)
{
    const size_t frame_pcm_bytes = (size_t)s->samples_per_frame *
                                   s->par.channels *
                                   ((s->par.bits_per_sample + 7) >> 3);
    const size_t tmp_cap = FFMAX((size_t)s->par.packet_size,
                                 frame_pcm_bytes + 64);
    uint8_t *tmp;
    BitWriter fb;
    uint16_t end_skip;
    int ret, use_rawpcm, saved_ac, saved_ic, saved_mclms, frame_bits;
    int best_ms, ch;

    tmp = av_malloc(tmp_cap);
    if (!tmp)
        return AVERROR(ENOMEM);

    end_skip = frame_samples < s->samples_per_frame ?
               s->samples_per_frame - frame_samples : 0;

    bw_init(&fb, tmp, tmp_cap);

    wmalossless_frame_write_tile_header(&fb, s, frame_samples, end_skip);
    wmalossless_frame_write_drc_and_skip(&fb, s, end_skip);

    use_rawpcm = s->par.force_rawpcm ? 1 : (!s->par.enable_cdlms);
    saved_ac = s->use_ac_filter;
    saved_ic = s->use_inter_ch;
    saved_mclms = s->do_mclms;

    if (use_rawpcm) {
        s->use_ac_filter = 0;
        s->use_inter_ch = 0;
        s->do_mclms = 0;
    }

    wmalossless_frame_write_subframe_header(&fb, s, seekable);

    if (seekable) {
        best_ms = wmalossless_frame_choose_movave(s, ch_buf, frame_samples);
        bw_put_bits(&fb, best_ms, 3);
        if (s->entropy) {
            for (ch = 0; ch < s->par.channels; ch++)
                s->entropy[ch].movave_scaling = best_ms;
        }
        bw_put_bits(&fb, 0, 8);
    }

    ret = wmalossless_apply_transforms(s, ch_buf, frame_samples, seekable,
                                       use_rawpcm);
    if (ret < 0) {
        av_free(tmp);
        s->use_ac_filter = saved_ac;
        s->use_inter_ch = saved_ic;
        s->do_mclms = saved_mclms;
        return ret;
    }

    bw_put_bit(&fb, use_rawpcm ? 1 : 0);
    if (!use_rawpcm) {
        for (ch = 0; ch < s->par.channels; ch++)
            bw_put_bit(&fb, 1);
        if (s->par.decode_flags & WMA_DECODE_FLAG_EXT)
            bw_put_bit(&fb, 0);
    }
    bw_put_bit(&fb, 0);

    if (use_rawpcm)
        wmalossless_frame_write_rawpcm(&fb, s, ch_buf, frame_samples);
    else {
        ret = wmalossless_frame_write_cdlms_residues(&fb, s, ch_buf,
                                                     frame_samples, seekable);
        if (ret < 0) {
            av_free(tmp);
            s->use_ac_filter = saved_ac;
            s->use_inter_ch = saved_ic;
            s->do_mclms = saved_mclms;
            return ret;
        }
    }

    s->use_ac_filter = saved_ac;
    s->use_inter_ch = saved_ic;
    s->do_mclms = saved_mclms;

    frame_bits = (int)bw_bits(&fb);

    if (frame_bits < 0)
        frame_bits = 0;

    bw_flush(&fb);
    *out_bytes = bw_bytes(&fb);
    *out_buf = tmp;

    return frame_bits;
}

static av_cold void wmalossless_packet_init(WMALosslessEncContext *s)
{
    bw_init(&s->packet_bw, s->packet_buf, s->par.packet_size);
}

static void wmalossless_packet_write_header(WMALosslessEncContext *s,
                                            unsigned num_bits_prev_frame,
                                            int has_seekable)
{
    bw_put_bits(&s->packet_bw, s->packet_seq & 0xF, 4);
    bw_put_bit(&s->packet_bw, has_seekable ? 1 : 0);
    bw_put_bit(&s->packet_bw, 0);
    bw_put_bits(&s->packet_bw, num_bits_prev_frame, s->log2_frame_size);
}

static void wmalossless_packet_start(WMALosslessEncContext *s,
                                     unsigned num_bits_prev_frame)
{
    wmalossless_packet_init(s);
    wmalossless_packet_write_header(s, num_bits_prev_frame,
                                    s->current_packet_has_seekable);
}

static void wmalossless_packet_write_bits(WMALosslessEncContext *s,
                                          const uint8_t *buf, int start_bit,
                                          int nbits)
{
    bw_put_bits_slice(&s->packet_bw, buf, start_bit, nbits);
    s->stat_packet_bits_used += nbits;
}

static int wmalossless_packet_bits_capacity(WMALosslessEncContext *s)
{
    return s->par.packet_size * 8;
}

static int wmalossless_packet_header_bits(WMALosslessEncContext *s)
{
    return 6 + s->log2_frame_size;
}

static int wmalossless_packet_calculate_frame_total_bits(WMALosslessEncContext *s,
                                                         int frame_bits)
{
    const int use_len_prefix = !!(s->par.decode_flags &
                                  WMA_DECODE_FLAG_LEN_PREFIX);
    return frame_bits + 1 +
           (use_len_prefix ? (s->log2_frame_size + 1) : 0);
}

static void wmalossless_packet_write_frame(WMALosslessEncContext *s,
                                           uint8_t *frame_buf, int frame_bits,
                                           int will_have_more)
{
    const int use_len_prefix = !!(s->par.decode_flags &
                                  WMA_DECODE_FLAG_LEN_PREFIX);

    if (use_len_prefix) {
        unsigned frame_len_value = s->log2_frame_size + frame_bits + 2;
        bw_put_bits(&s->packet_bw, frame_len_value, s->log2_frame_size);
    }

    bw_put_bits_exact(&s->packet_bw, frame_buf, frame_bits);

    if (use_len_prefix)
        bw_put_bit(&s->packet_bw, 0);
    bw_put_bit(&s->packet_bw, will_have_more ? 1 : 0);

    s->stat_frames_in_packet++;
}

static void wmalossless_packet_finalize(WMALosslessEncContext *s,
                                        int eff_samples)
{
    size_t used;

    bw_flush(&s->packet_bw);
    used = bw_bytes(&s->packet_bw);

    if (used < s->par.packet_size)
        memset(s->packet_buf + used, 0, s->par.packet_size - used);

    s->written_samples += eff_samples;
    s->packet_seq = (s->packet_seq + 1) & 0xF;
    s->current_packet_has_seekable = 0;
    s->stat_frames_in_packet = 0;
    s->stat_packet_bits_used = 0;
}

static int wmalossless_seekable_should_place(WMALosslessEncContext *s,
                                             int64_t current_samples,
                                             int is_first_frame)
{
    if (is_first_frame)
        return 1;
    return current_samples - s->last_seekable_samples >=
           s->seekable_target_interval;
}

static void wmalossless_seekable_update_position(WMALosslessEncContext *s,
                                                  int64_t frame_sample_position)
{
    s->last_seekable_samples = frame_sample_position;
}

static void wmalossless_parse_extradata(WMALosslessEncContext *s)
{
    const AVCodecContext *avctx = s->avctx;
    const uint8_t *ed = avctx->extradata;
    const int size = avctx->extradata_size;

    if (!ed || size < 18)
        return;

    s->par.bits_per_sample = AV_RL16(ed);
    s->par.channel_mask    = AV_RL32(ed + 2);
    s->par.decode_flags    = AV_RL16(ed + 14);
}

static int wmalossless_write_default_extradata(WMALosslessEncContext *s)
{
    AVCodecContext *avctx = s->avctx;
    uint8_t *ed;

    if (avctx->extradata_size >= 18)
        return 0;

    ed = av_mallocz(18 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ed)
        return AVERROR(ENOMEM);

    AV_WL16(ed,     s->par.bits_per_sample);
    AV_WL32(ed + 2, s->par.channel_mask);
    AV_WL16(ed + 14, s->par.decode_flags);

    av_freep(&avctx->extradata);
    avctx->extradata = ed;
    avctx->extradata_size = 18;
    return 0;
}

static av_cold int wmalossless_encode_init(AVCodecContext *avctx)
{
    WMALosslessEncContext *s = avctx->priv_data;
    const int version = 3;
    int frame_len_bits;
    uint64_t mask = 0;

    s->avctx = avctx;

    if (avctx->ch_layout.nb_channels <= 0) {
        av_log(avctx, AV_LOG_ERROR, "channel count not set\n");
        return AVERROR(EINVAL);
    }
    if (avctx->ch_layout.nb_channels > ENC_MAX_CHANNELS) {
        av_log(avctx, AV_LOG_ERROR, "too many channels (%d > %d)\n",
               avctx->ch_layout.nb_channels, ENC_MAX_CHANNELS);
        return AVERROR(EINVAL);
    }

    if (avctx->sample_fmt == AV_SAMPLE_FMT_S32P) {
        avctx->bits_per_raw_sample = 24;
    } else {
        avctx->bits_per_raw_sample = 16;
        avctx->sample_fmt = AV_SAMPLE_FMT_S16P;
    }

    s->par.bits_per_sample = avctx->bits_per_raw_sample;
    s->par.channels        = avctx->ch_layout.nb_channels;
    s->par.sample_rate     = avctx->sample_rate;
    s->par.decode_flags    = WMA_DECODE_FLAGS_DEFAULT;
    s->par.enable_cdlms    = 1;
    s->par.force_rawpcm    = 0;

    if (avctx->sample_rate <= 0 ||
        avctx->sample_rate > WMALOSSLESS_MAX_SAMPLE_RATE) {
        av_log(avctx, AV_LOG_ERROR,
               "unsupported sample rate %d (max %d Hz)\n",
               avctx->sample_rate, WMALOSSLESS_MAX_SAMPLE_RATE);
        return AVERROR(EINVAL);
    }

    mask = avctx->ch_layout.u.mask;
    if (!mask) {
        if (s->par.channels == 2)
            mask = 0x3;
        else if (s->par.channels == 1)
            mask = 0x4;
    }
    s->par.channel_mask = (uint32_t)mask;

    if (avctx->block_align > 0) {
        s->par.packet_size = avctx->block_align;
    } else {
        const int is_48k_family = avctx->sample_rate % 48000 == 0;
        s->par.packet_size = is_48k_family ?
                             WMALOSSLESS_PACKET_SIZE_48 :
                             WMALOSSLESS_PACKET_SIZE_441;
    }
    avctx->block_align = s->par.packet_size;
    avctx->bit_rate = WMALOSSLESS_DEFAULT_BIT_RATE;
    avctx->bits_per_coded_sample = avctx->bits_per_raw_sample;

    wmalossless_parse_extradata(s);

    frame_len_bits = ff_wma_get_frame_len_bits(avctx->sample_rate,
                                               version, s->par.decode_flags);
    s->samples_per_frame = 1 << frame_len_bits;
    s->log2_frame_size   = wmalossless_ilog2u(s->par.packet_size) + 4;

    avctx->frame_size = s->samples_per_frame;

    s->packet_buf = av_mallocz(s->par.packet_size);
    if (!s->packet_buf)
        return AVERROR(ENOMEM);

    bw_init(&s->packet_bw, s->packet_buf, s->par.packet_size);

    s->packet_seq  = 0;
    s->total_samples = 0;
    s->written_samples = 0;
    s->input_samples = 0;
    s->frame_index = 0;
    s->first_frame = 1;
    s->use_inter_ch = (s->par.channels == 2);
    s->use_ac_filter = 1;
    s->do_mclms = 0;
    s->mclms_order = 2;
    s->mclms_scaling = 0;
    wmalossless_mclms_clear(s);
    s->ac_order = 1;
    s->ac_scaling = 6;
    memset(s->ac_coefs, 0, sizeof(s->ac_coefs));
    s->ac_coefs[0] = 61;
    memset(s->ac_prev, 0, sizeof(s->ac_prev));

    if (s->par.enable_cdlms) {
        int ch;

        s->cdlms = av_calloc(s->par.channels, sizeof(*s->cdlms));
        if (!s->cdlms)
            return AVERROR(ENOMEM);
        s->entropy = av_calloc(s->par.channels, sizeof(*s->entropy));
        if (!s->entropy)
            return AVERROR(ENOMEM);

        for (ch = 0; ch < s->par.channels; ch++) {
            cdlms_init(&s->cdlms[ch], 1);
            cdlms_config_filter(&s->cdlms[ch], 0, 16, 12);
            cdlms_reset(&s->cdlms[ch]);
            entropy_init(&s->entropy[ch], 5);
        }
    }

    s->seekable_target_interval = SEEKABLE_FRAME_INTERVAL;
    s->last_seekable_samples = 0;
    s->seekable_accumulated_error = 0;
    s->current_packet_has_seekable = 0;
    s->pending_frame_bits = 0;
    s->pending_frame_pos_bits = 0;
    s->pending_frame_buf = NULL;
    s->pending_frame_seekable = 0;
    s->pending_frame_samples = 0;

    s->fifo = av_audio_fifo_alloc(avctx->sample_fmt, s->par.channels,
                                  s->samples_per_frame);
    if (!s->fifo)
        return AVERROR(ENOMEM);

    s->packet_queue = NULL;
    s->packet_queue_count = 0;
    s->packet_queue_index = 0;

    return wmalossless_write_default_extradata(s);
}

static int wmalossless_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                                    const AVFrame *frame, int *got_packet)
{
    WMALosslessEncContext *s = avctx->priv_data;
    WMALosslessPacket *pkt;
    int ret;

    *got_packet = 0;

    if (frame) {
        int write_samples = frame->duration > 0 ?
                            frame->duration : frame->nb_samples;
        s->input_samples += write_samples;
        ret = av_audio_fifo_write(s->fifo, (void **)frame->extended_data,
                                  write_samples);
        if (ret < write_samples)
            return ret < 0 ? ret : AVERROR(ENOMEM);
        return 0;
    }

    if (s->packet_queue_index >= s->packet_queue_count) {
        wmalossless_clear_packet_queue(s);
        ret = wmalossless_encode_fifo(s);
        if (ret < 0)
            return ret;
    }

    if (s->packet_queue_index >= s->packet_queue_count)
        return AVERROR_EOF;

    pkt = &s->packet_queue[s->packet_queue_index++];

    ret = ff_alloc_packet(avctx, avpkt, pkt->size);
    if (ret < 0)
        return ret;

    memcpy(avpkt->data, pkt->data, pkt->size);
    avpkt->size = pkt->size;
    avpkt->pts = ff_samples_to_time_base(avctx, pkt->pts_samples);
    avpkt->duration = ff_samples_to_time_base(avctx, pkt->duration_samples);

    av_freep(&pkt->data);

    *got_packet = 1;

    if (s->packet_queue_index >= s->packet_queue_count)
        wmalossless_clear_packet_queue(s);

    return 0;
}

static av_cold int wmalossless_encode_close(AVCodecContext *avctx)
{
    WMALosslessEncContext *s = avctx->priv_data;
    int i;

    if (s->packet_buf)
        av_freep(&s->packet_buf);
    if (s->cdlms)
        av_freep(&s->cdlms);

    if (s->entropy)
        av_freep(&s->entropy);
    if (s->pending_frame_buf)
        av_freep(&s->pending_frame_buf);
    if (s->fifo)
        av_audio_fifo_free(s->fifo);
    if (s->packet_queue) {
        for (i = 0; i < s->packet_queue_count; i++)
            av_freep(&s->packet_queue[i].data);
        av_freep(&s->packet_queue);
    }

    return 0;
}

const FFCodec ff_wmalossless_encoder = {
    .p.name         = "wmalossless",
    CODEC_LONG_NAME("Windows Media Audio Lossless"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_WMALOSSLESS,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE |
                      AV_CODEC_CAP_DELAY,
    .priv_data_size = sizeof(WMALosslessEncContext),
    .init           = wmalossless_encode_init,
    FF_CODEC_ENCODE_CB(wmalossless_encode_frame),
    .close          = wmalossless_encode_close,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S32P),
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};

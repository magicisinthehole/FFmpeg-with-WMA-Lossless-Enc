/*
 * ASF muxer
 * Copyright (c) 2000, 2001 Fabrice Bellard
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

#include <time.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavcodec/codec_desc.h"
#include "avformat.h"
#include "avlanguage.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"
#include "riff.h"
#include "asf.h"
#include "version.h"

#define ASF_INDEXED_INTERVAL    10000000
#define ASF_INDEX_BLOCK         (1<<9)
#define ASF_PAYLOADS_PER_PACKET 63

#define ASF_PACKET_ERROR_CORRECTION_DATA_SIZE 0x2
#define ASF_PACKET_ERROR_CORRECTION_FLAGS          \
    (ASF_PACKET_FLAG_ERROR_CORRECTION_PRESENT |    \
     ASF_PACKET_ERROR_CORRECTION_DATA_SIZE)

#if (ASF_PACKET_ERROR_CORRECTION_FLAGS != 0)
#   define ASF_PACKET_ERROR_CORRECTION_FLAGS_FIELD_SIZE 1
#else
#   define ASF_PACKET_ERROR_CORRECTION_FLAGS_FIELD_SIZE 0
#endif

#define ASF_PPI_PROPERTY_FLAGS                                       \
    (ASF_PL_FLAG_REPLICATED_DATA_LENGTH_FIELD_IS_BYTE           |    \
     ASF_PL_FLAG_OFFSET_INTO_MEDIA_OBJECT_LENGTH_FIELD_IS_DWORD |    \
     ASF_PL_FLAG_MEDIA_OBJECT_NUMBER_LENGTH_FIELD_IS_BYTE       |    \
     ASF_PL_FLAG_STREAM_NUMBER_LENGTH_FIELD_IS_BYTE)

#define ASF_PPI_LENGTH_TYPE_FLAGS 0

#define ASF_PAYLOAD_FLAGS ASF_PL_FLAG_PAYLOAD_LENGTH_FIELD_IS_WORD

#if (ASF_PPI_FLAG_SEQUENCE_FIELD_IS_BYTE == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_SEQUENCE_FIELD_SIZE))
#   define ASF_PPI_SEQUENCE_FIELD_SIZE 1
#endif
#if (ASF_PPI_FLAG_SEQUENCE_FIELD_IS_WORD == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_SEQUENCE_FIELD_SIZE))
#   define ASF_PPI_SEQUENCE_FIELD_SIZE 2
#endif
#if (ASF_PPI_FLAG_SEQUENCE_FIELD_IS_DWORD == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_SEQUENCE_FIELD_SIZE))
#   define ASF_PPI_SEQUENCE_FIELD_SIZE 4
#endif
#ifndef ASF_PPI_SEQUENCE_FIELD_SIZE
#   define ASF_PPI_SEQUENCE_FIELD_SIZE 0
#endif

#if (ASF_PPI_FLAG_PACKET_LENGTH_FIELD_IS_BYTE == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_PACKET_LENGTH_FIELD_SIZE))
#   define ASF_PPI_PACKET_LENGTH_FIELD_SIZE 1
#endif
#if (ASF_PPI_FLAG_PACKET_LENGTH_FIELD_IS_WORD == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_PACKET_LENGTH_FIELD_SIZE))
#   define ASF_PPI_PACKET_LENGTH_FIELD_SIZE 2
#endif
#if (ASF_PPI_FLAG_PACKET_LENGTH_FIELD_IS_DWORD == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_PACKET_LENGTH_FIELD_SIZE))
#   define ASF_PPI_PACKET_LENGTH_FIELD_SIZE 4
#endif
#ifndef ASF_PPI_PACKET_LENGTH_FIELD_SIZE
#   define ASF_PPI_PACKET_LENGTH_FIELD_SIZE 0
#endif

#if (ASF_PPI_FLAG_PADDING_LENGTH_FIELD_IS_BYTE == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_PADDING_LENGTH_FIELD_SIZE))
#   define ASF_PPI_PADDING_LENGTH_FIELD_SIZE 1
#endif
#if (ASF_PPI_FLAG_PADDING_LENGTH_FIELD_IS_WORD == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_PADDING_LENGTH_FIELD_SIZE))
#   define ASF_PPI_PADDING_LENGTH_FIELD_SIZE 2
#endif
#if (ASF_PPI_FLAG_PADDING_LENGTH_FIELD_IS_DWORD == (ASF_PPI_LENGTH_TYPE_FLAGS & ASF_PPI_MASK_PADDING_LENGTH_FIELD_SIZE))
#   define ASF_PPI_PADDING_LENGTH_FIELD_SIZE 4
#endif
#ifndef ASF_PPI_PADDING_LENGTH_FIELD_SIZE
#   define ASF_PPI_PADDING_LENGTH_FIELD_SIZE 0
#endif

#if (ASF_PL_FLAG_REPLICATED_DATA_LENGTH_FIELD_IS_BYTE == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_REPLICATED_DATA_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_REPLICATED_DATA_LENGTH_FIELD_SIZE 1
#endif
#if (ASF_PL_FLAG_REPLICATED_DATA_LENGTH_FIELD_IS_WORD == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_REPLICATED_DATA_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_REPLICATED_DATA_LENGTH_FIELD_SIZE 2
#endif
#if (ASF_PL_FLAG_REPLICATED_DATA_LENGTH_FIELD_IS_DWORD == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_REPLICATED_DATA_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_REPLICATED_DATA_LENGTH_FIELD_SIZE 4
#endif
#ifndef ASF_PAYLOAD_REPLICATED_DATA_LENGTH_FIELD_SIZE
#   define ASF_PAYLOAD_REPLICATED_DATA_LENGTH_FIELD_SIZE 0
#endif

#if (ASF_PL_FLAG_OFFSET_INTO_MEDIA_OBJECT_LENGTH_FIELD_IS_BYTE == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_OFFSET_INTO_MEDIA_OBJECT_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_OFFSET_INTO_MEDIA_OBJECT_FIELD_SIZE 1
#endif
#if (ASF_PL_FLAG_OFFSET_INTO_MEDIA_OBJECT_LENGTH_FIELD_IS_WORD == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_OFFSET_INTO_MEDIA_OBJECT_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_OFFSET_INTO_MEDIA_OBJECT_FIELD_SIZE 2
#endif
#if (ASF_PL_FLAG_OFFSET_INTO_MEDIA_OBJECT_LENGTH_FIELD_IS_DWORD == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_OFFSET_INTO_MEDIA_OBJECT_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_OFFSET_INTO_MEDIA_OBJECT_FIELD_SIZE 4
#endif
#ifndef ASF_PAYLOAD_OFFSET_INTO_MEDIA_OBJECT_FIELD_SIZE
#   define ASF_PAYLOAD_OFFSET_INTO_MEDIA_OBJECT_FIELD_SIZE 0
#endif

#if (ASF_PL_FLAG_MEDIA_OBJECT_NUMBER_LENGTH_FIELD_IS_BYTE == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_MEDIA_OBJECT_NUMBER_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_MEDIA_OBJECT_NUMBER_FIELD_SIZE 1
#endif
#if (ASF_PL_FLAG_MEDIA_OBJECT_NUMBER_LENGTH_FIELD_IS_WORD == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_MEDIA_OBJECT_NUMBER_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_MEDIA_OBJECT_NUMBER_FIELD_SIZE 2
#endif
#if (ASF_PL_FLAG_MEDIA_OBJECT_NUMBER_LENGTH_FIELD_IS_DWORD == (ASF_PPI_PROPERTY_FLAGS & ASF_PL_MASK_MEDIA_OBJECT_NUMBER_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_MEDIA_OBJECT_NUMBER_FIELD_SIZE 4
#endif
#ifndef ASF_PAYLOAD_MEDIA_OBJECT_NUMBER_FIELD_SIZE
#   define ASF_PAYLOAD_MEDIA_OBJECT_NUMBER_FIELD_SIZE 0
#endif

#if (ASF_PL_FLAG_PAYLOAD_LENGTH_FIELD_IS_BYTE == (ASF_PAYLOAD_FLAGS & ASF_PL_MASK_PAYLOAD_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_LENGTH_FIELD_SIZE 1
#endif
#if (ASF_PL_FLAG_PAYLOAD_LENGTH_FIELD_IS_WORD == (ASF_PAYLOAD_FLAGS & ASF_PL_MASK_PAYLOAD_LENGTH_FIELD_SIZE))
#   define ASF_PAYLOAD_LENGTH_FIELD_SIZE 2
#endif
#ifndef ASF_PAYLOAD_LENGTH_FIELD_SIZE
#   define ASF_PAYLOAD_LENGTH_FIELD_SIZE 0
#endif

#define PACKET_HEADER_MIN_SIZE \
    (ASF_PACKET_ERROR_CORRECTION_FLAGS_FIELD_SIZE +       \
     ASF_PACKET_ERROR_CORRECTION_DATA_SIZE +              \
     1 +        /* Length Type Flags */                   \
     1 +        /* Property Flags */                      \
     ASF_PPI_PACKET_LENGTH_FIELD_SIZE +                   \
     ASF_PPI_SEQUENCE_FIELD_SIZE +                        \
     ASF_PPI_PADDING_LENGTH_FIELD_SIZE +                  \
     4 +        /* Send Time Field */                     \
     2)         /* Duration Field */

// Replicated Data shall be at least 8 bytes long.
#define ASF_PAYLOAD_REPLICATED_DATA_LENGTH 0x08

#define PAYLOAD_HEADER_SIZE_SINGLE_PAYLOAD                \
    (1 +     /* Stream Number */                          \
     ASF_PAYLOAD_MEDIA_OBJECT_NUMBER_FIELD_SIZE +         \
     ASF_PAYLOAD_OFFSET_INTO_MEDIA_OBJECT_FIELD_SIZE +    \
     ASF_PAYLOAD_REPLICATED_DATA_LENGTH_FIELD_SIZE +      \
     ASF_PAYLOAD_REPLICATED_DATA_LENGTH)

#define PAYLOAD_HEADER_SIZE_MULTIPLE_PAYLOADS             \
    (1 +        /* Stream Number */                       \
     ASF_PAYLOAD_MEDIA_OBJECT_NUMBER_FIELD_SIZE +         \
     ASF_PAYLOAD_OFFSET_INTO_MEDIA_OBJECT_FIELD_SIZE +    \
     ASF_PAYLOAD_REPLICATED_DATA_LENGTH_FIELD_SIZE +      \
     ASF_PAYLOAD_REPLICATED_DATA_LENGTH +                 \
     ASF_PAYLOAD_LENGTH_FIELD_SIZE)

#define SINGLE_PAYLOAD_HEADERS                            \
    (PACKET_HEADER_MIN_SIZE +                             \
     PAYLOAD_HEADER_SIZE_SINGLE_PAYLOAD)

#define MULTI_PAYLOAD_HEADERS                             \
    (PACKET_HEADER_MIN_SIZE +                             \
     1 +         /* Payload Flags */                      \
     2 * PAYLOAD_HEADER_SIZE_MULTIPLE_PAYLOADS)

#define DATA_HEADER_SIZE 50

#define PACKET_SIZE_MAX 65536
#define PACKET_SIZE_MIN 100

typedef struct ASFStream {
    int num;
    unsigned char seq;

    uint16_t stream_language_index;
} ASFStream;

typedef struct ASFContext {
    AVClass *av_class;
    uint32_t seqno;
    int is_streamed;
    ASFStream streams[128];              ///< it's max number and it's not that big
    const char *languages[128];
    int nb_languages;
    int64_t creation_time;
    /* non-streamed additional info */
    uint64_t nb_packets;                 ///< how many packets are there in the file, invalid if broadcasting
    int64_t duration;                    ///< in 100ns units
    /* packet filling */
    unsigned char multi_payloads_present;
    int packet_size_left;
    int64_t packet_timestamp_start;
    int64_t packet_timestamp_end;
    unsigned int packet_nb_payloads;
    uint8_t packet_buf[PACKET_SIZE_MAX];
    FFIOContext pb;
    /* only for reading */
    uint64_t data_offset;                ///< beginning of the first data packet

    ASFIndex *index_ptr;
    uint32_t nb_index_memory_alloc;
    uint16_t maximum_packet;
    uint32_t next_packet_number;
    uint16_t next_packet_count;
    uint64_t next_packet_offset;
    int      next_start_sec;
    int      end_sec;
    int      packet_size;
    int      attached_pic_index;       ///< stream index for Metadata Library WM/Picture, or -1
    int      attached_pic_thumb_index; ///< stream index for ECD thumbnail WM/Picture, or -1
    int64_t *pkt_send_times;           ///< per-packet send_time (ms) for ESP leaky bucket
    uint32_t pkt_stats_alloc;          ///< allocated capacity of pkt_send_times
} ASFContext;

static const ff_asf_guid ff_asf_stream_bitrate_properties = {
    0xce, 0x75, 0xf8, 0x7b, 0x8d, 0x46, 0xd1, 0x11,
    0x8d, 0x82, 0x00, 0x60, 0x97, 0xc9, 0xa2, 0xb2
};

static const struct {
    const char *key;
    ASFDataType type;
} asf_typed_tags[] = {
    { "IsVBR",                      ASF_BOOL  },
    { "WM/WMADRCPeakReference",     ASF_DWORD },
    { "WM/WMADRCPeakTarget",        ASF_DWORD },
    { "WM/WMADRCAverageReference",  ASF_DWORD },
    { "WM/WMADRCAverageTarget",     ASF_DWORD },
    { NULL, 0 }
};

static ASFDataType asf_get_tag_type(const char *key)
{
    for (int i = 0; asf_typed_tags[i].key; i++)
        if (!av_strcasecmp(asf_typed_tags[i].key, key))
            return asf_typed_tags[i].type;
    return ASF_UNICODE;
}

static const AVCodecTag codec_asf_bmp_tags[] = {
    { AV_CODEC_ID_MPEG4,     MKTAG('M', '4', 'S', '2') },
    { AV_CODEC_ID_MPEG4,     MKTAG('M', 'P', '4', 'S') },
    { AV_CODEC_ID_MSMPEG4V3, MKTAG('M', 'P', '4', '3') },
    { AV_CODEC_ID_NONE,      0 },
};

static const AVCodecTag *const asf_codec_tags[] = {
        codec_asf_bmp_tags, ff_codec_bmp_tags, ff_codec_wav_tags, NULL
};

#define PREROLL_TIME 3000

/**
 * Return the MIME type string for an attached picture codec.
 */
static const char *asf_pic_mime_type(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_MJPEG: return "image/jpeg";
    case AV_CODEC_ID_PNG:   return "image/png";
    case AV_CODEC_ID_BMP:   return "image/bmp";
    default:                return "image/jpeg";
    }
}

/**
 * Compute the byte size of a WM/Picture attribute value.
 * Layout: [1B type][4B data_len][MIME UTF-16LE null-term][desc UTF-16LE null-term][data]
 */
static int asf_wm_pic_value_size(const AVPacket *pic, const char *mime)
{
    int mime_utf16_len = ((int)strlen(mime) + 1) * 2; /* including null */
    return 1 + 4 + mime_utf16_len + 2 + pic->size;    /* 2 = empty desc null */
}

/**
 * Write the WM/Picture binary value (shared between ECD and Metadata Library).
 */
static void asf_write_wm_pic_value(AVIOContext *pb, const AVPacket *pic,
                                   const char *mime)
{
    avio_w8(pb, 3);                                  /* picture type: Front Cover */
    avio_wl32(pb, pic->size);                        /* picture data length */
    avio_put_str16le(pb, mime);                      /* MIME type, null-terminated UTF-16LE */
    avio_wl16(pb, 0);                                /* empty description (null terminator) */
    avio_write(pb, pic->data, pic->size);            /* raw image data */
}

static void put_str16(AVIOContext *s, AVIOContext *dyn_buf, const char *tag)
{
    uint8_t *buf;
    int len;

    avio_put_str16le(dyn_buf, tag);
    len = avio_get_dyn_buf(dyn_buf, &buf);
    avio_wl16(s, len);
    avio_write(s, buf, len);
    ffio_reset_dyn_buf(dyn_buf);
}

static int64_t put_header(AVIOContext *pb, const ff_asf_guid *g)
{
    int64_t pos;

    pos = avio_tell(pb);
    ff_put_guid(pb, g);
    avio_wl64(pb, 24);
    return pos;
}

/* update header size */
static void end_header(AVIOContext *pb, int64_t pos)
{
    int64_t pos1;

    pos1 = avio_tell(pb);
    avio_seek(pb, pos + 16, SEEK_SET);
    avio_wl64(pb, pos1 - pos);
    avio_seek(pb, pos1, SEEK_SET);
}

/* write an asf chunk (only used in streaming case) */
static void put_chunk(AVFormatContext *s, int type,
                      int payload_length, int flags)
{
    ASFContext *asf = s->priv_data;
    AVIOContext *pb = s->pb;
    int length;

    length = payload_length + 8;
    avio_wl16(pb, type);
    avio_wl16(pb, length);      // size
    avio_wl32(pb, asf->seqno);  // sequence number
    avio_wl16(pb, flags);       // unknown bytes
    avio_wl16(pb, length);      // size_confirm
    asf->seqno++;
}

/* convert from av time to windows time */
static int64_t unix_to_file_time(int64_t ti)
{
    int64_t t;

    t  = ti * INT64_C(10);
    t += INT64_C(116444736000000000);
    return t;
}

static int32_t get_send_time(ASFContext *asf, int64_t pres_time, uint64_t *offset)
{
    int32_t send_time = 0;
    *offset = asf->data_offset + DATA_HEADER_SIZE;
    for (int i = 0; i < asf->next_start_sec; i++) {
        if (pres_time <= asf->index_ptr[i].send_time)
            break;
        send_time = asf->index_ptr[i].send_time;
        *offset   = asf->index_ptr[i].offset;
    }

    return send_time / 10000;
}

static void asf_write_markers(AVFormatContext *s, AVIOContext *dyn_buf)
{
    ASFContext *asf = s->priv_data;
    AVIOContext *pb = s->pb;
    AVRational scale = {1, 10000000};
    int64_t hpos = put_header(pb, &ff_asf_marker_header);

    ff_put_guid(pb, &ff_asf_reserved_4);// ASF spec mandates this reserved value
    avio_wl32(pb, s->nb_chapters);     // markers count
    avio_wl16(pb, 0);                  // ASF spec mandates 0 for this
    avio_wl16(pb, 0);                  // name length 0, no name given

    for (unsigned i = 0; i < s->nb_chapters; i++) {
        AVChapter *c = s->chapters[i];
        AVDictionaryEntry *t = av_dict_get(c->metadata, "title", NULL, 0);
        int64_t pres_time = av_rescale_q(c->start, c->time_base, scale);
        uint64_t offset;
        int32_t send_time = get_send_time(asf, pres_time, &offset);
        int len = 0;
        uint8_t *buf;
        if (t) {
            avio_put_str16le(dyn_buf, t->value);
            len = avio_get_dyn_buf(dyn_buf, &buf);
        }
        avio_wl64(pb, offset);            // offset of the packet with send_time
        avio_wl64(pb, pres_time + PREROLL_TIME * 10000); // presentation time
        avio_wl16(pb, 12 + len);          // entry length
        avio_wl32(pb, send_time);         // send time
        avio_wl32(pb, 0);                 // flags, should be 0
        avio_wl32(pb, len / 2);           // marker desc length in WCHARS!
        if (t) {
            avio_write(pb, buf, len);     // marker desc
            ffio_reset_dyn_buf(dyn_buf);
        }
    }
    end_header(pb, hpos);
}

/**
 * Compute leaky bucket buffer window for ASFLeakyBucketPairs.
 *
 * Models a receiver draining at @p drain_bps where packet i requires
 * i * pkt_size bytes (zero-indexed: the arriving packet itself is not
 * yet counted as consumed).  The result is clamped to at least one
 * packet's transmission time so that even at very high drain rates the
 * buffer can hold one packet.
 *
 * Matches the convention observed in files produced by the Windows
 * Media Format SDK.
 *
 * Returns buffer window in milliseconds.
 */
static int asf_leaky_bucket_ms(const int64_t *send_times, uint64_t nb_packets,
                                int pkt_size, int32_t drain_bps)
{
    int64_t max_deficit = 0;
    int pkt_time;

    for (uint64_t i = 0; i < nb_packets; i++) {
        int64_t consumed = (int64_t)i * pkt_size;
        int64_t arrived  = (int64_t)drain_bps * send_times[i] / 8000;
        int64_t deficit  = consumed - arrived;
        if (deficit > max_deficit)
            max_deficit = deficit;
    }

    pkt_time = drain_bps > 0 ? (int)((int64_t)pkt_size * 8000 / drain_bps) : 0;

    if (drain_bps > 0) {
        int lb = (int)(max_deficit * 8000 / drain_bps);
        return lb > pkt_time ? lb : pkt_time;
    }
    return 5000;
}

/**
 * Compute the ESP buffer_size using the overflow (encoder-side) leaky bucket.
 *
 * Per the ASF specification, the encoder fills a bucket with payload data
 * at the times it is produced (presentation_time), and the bucket drains
 * at the constant data_bitrate.  The buffer_size is the minimum bucket
 * capacity (in ms) such that the bucket never overflows.
 *
 * This uses a running model where the bucket level is clamped to zero
 * when it would go negative (the bucket cannot drain below empty).
 * The payload per sample is block_align (the compressed frame size,
 * "excluding all ASF Data Packet overhead" per the ASF spec).
 *
 * Returns buffer window in milliseconds.
 */
static int asf_overflow_buffer_ms(const int64_t *send_times, uint64_t nb_packets,
                                  int payload_size, int32_t drain_bps)
{
    int64_t max_level = 0;
    int64_t level = 0;
    int64_t prev_time;

    if (nb_packets < 2 || drain_bps <= 0)
        return 5000;

    prev_time = send_times[0];
    for (uint64_t i = 0; i < nb_packets; i++) {
        int64_t drain = (int64_t)drain_bps * (send_times[i] - prev_time) / 8000;
        level -= drain;
        if (level < 0)
            level = 0;
        level += payload_size;
        if (level > max_level)
            max_level = level;
        prev_time = send_times[i];
    }

    return (int)(max_level * 8000 / drain_bps) + 1;
}

/* write the header (used two times if non streamed) */
static int asf_write_header1(AVFormatContext *s, int64_t file_size,
                             int64_t data_chunk_size)
{
    ASFContext *asf = s->priv_data;
    AVIOContext *pb = s->pb, *dyn_buf;
    AVDictionaryEntry *tags[5];
    int header_size, extra_size, extra_size2, wav_extra_size;
    int has_title, has_aspect_ratio = 0;
    int metadata_count, has_wmalossless = 0;
    int wmalossless_bitrate = 0;
    int64_t header_offset, cur_pos, hpos;
    int bit_rate, ret;
    int64_t duration;
    int audio_language_counts[128] = { 0 };
    int nb_real_streams;
    int32_t wma_avg_bitrate = 0, wma_peak_bitrate = 0;
    int wma_avg_buffer_ms = 5000, wma_peak_buffer_ms = 5000;
    int64_t wma_avg_time = 0;
    int has_pic       = (asf->attached_pic_index >= 0 &&
                         s->streams[asf->attached_pic_index]->attached_pic.data);
    int has_pic_thumb  = (asf->attached_pic_thumb_index >= 0 &&
                          s->streams[asf->attached_pic_thumb_index]->attached_pic.data);

    /* Count non-attached-pic streams and assign sequential stream numbers.
     * Attached_pic streams are excluded from the ASF output, so real
     * streams get 1-based numbering that skips any attached_pic gaps. */
    nb_real_streams = 0;
    for (unsigned n = 0; n < s->nb_streams; n++) {
        if (s->streams[n]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            asf->streams[n].num = 0;
            continue;
        }
        asf->streams[n].num = ++nb_real_streams;
    }

    ff_metadata_conv(&s->metadata, ff_asf_metadata_conv, NULL);

    tags[0] = av_dict_get(s->metadata, "title", NULL, 0);
    tags[1] = av_dict_get(s->metadata, "author", NULL, 0);
    tags[2] = av_dict_get(s->metadata, "copyright", NULL, 0);
    tags[3] = av_dict_get(s->metadata, "comment", NULL, 0);
    tags[4] = av_dict_get(s->metadata, "rating", NULL, 0);

    duration       = asf->duration + PREROLL_TIME * 10000;
    has_title      = tags[0] || tags[1] || tags[2] || tags[3] || tags[4];

    if (!file_size) {
        if (ff_parse_creation_time_metadata(s, &asf->creation_time, 0) != 0)
            av_dict_set(&s->metadata, "creation_time", NULL, 0);
    }

    metadata_count = av_dict_count(s->metadata);

    /* Increment metadata_count for WM/Picture in ECD */
    if (has_pic_thumb)
        metadata_count++;

    /* DRC metadata is written per-stream in Metadata Object, not in ECD */
    {
        static const char *const drc_keys[] = {
            "WM/WMADRCPeakReference", "WM/WMADRCPeakTarget",
            "WM/WMADRCAverageReference", "WM/WMADRCAverageTarget",
        };
        for (int i = 0; i < FF_ARRAY_ELEMS(drc_keys); i++) {
            if (av_dict_get(s->metadata, drc_keys[i], NULL, 0))
                metadata_count--;
        }
    }

    bit_rate = 0;
    for (unsigned n = 0; n < s->nb_streams; n++) {
        AVStream *const st = s->streams[n];
        AVCodecParameters *const par = st->codecpar;
        AVDictionaryEntry *entry;

        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            continue;

        avpriv_set_pts_info(s->streams[n], 32, 1, 1000); /* 32 bit pts in ms */

        bit_rate += par->bit_rate;
        if (   par->codec_type == AVMEDIA_TYPE_VIDEO
            && par->sample_aspect_ratio.num > 0
            && par->sample_aspect_ratio.den > 0)
            has_aspect_ratio++;

        entry = av_dict_get(s->streams[n]->metadata, "language", NULL, 0);
        if (entry) {
            const char *iso6391lang = ff_convert_lang_to(entry->value, AV_LANG_ISO639_1);
            if (iso6391lang) {
                int i;
                for (i = 0; i < asf->nb_languages; i++) {
                    if (!strcmp(asf->languages[i], iso6391lang)) {
                        asf->streams[n].stream_language_index = i;
                        break;
                    }
                }
                if (i >= asf->nb_languages) {
                    asf->languages[asf->nb_languages] = iso6391lang;
                    asf->streams[n].stream_language_index = asf->nb_languages;
                    asf->nb_languages++;
                }
                if (par->codec_type == AVMEDIA_TYPE_AUDIO)
                    audio_language_counts[asf->streams[n].stream_language_index]++;
            }
        } else {
            asf->streams[n].stream_language_index = 128;
        }
    }

    /* detect WMA Lossless — gates Stream Bitrate Properties Object,
     * Extended Stream Properties, and ASFLeakyBucketPairs descriptor */
    for (unsigned n = 0; n < s->nb_streams; n++) {
        if (s->streams[n]->codecpar->codec_id == AV_CODEC_ID_WMALOSSLESS) {
            has_wmalossless = 1;
            wmalossless_bitrate = s->streams[n]->codecpar->bit_rate;
            metadata_count++;  /* for ASFLeakyBucketPairs descriptor */
            break;
        }
    }

    /* Pre-compute WMA Lossless bitrate and buffer stats for trailer rewrite.
     * Used in File Properties, ESP, Stream Bitrate Properties,
     * and ASFLeakyBucketPairs.
     *
     * Average bitrate is computed over send_duration only (excludes preroll).
     * Buffer window uses the encoder-side overflow leaky bucket model: data
     * enters at production time (block_align per sample), drains at constant
     * data_bitrate, and the bucket must never overflow.
     * Alt (peak) buffer window is a fixed 1570 ms; the alt bitrate is the
     * peak over a 5-second sliding window, rounded down to a 5000 bps
     * boundary. */
    if (file_size && has_wmalossless && asf->pkt_send_times
        && asf->nb_packets > 1 && asf->duration > 0) {
        int64_t data_bytes = data_chunk_size - DATA_HEADER_SIZE;
        int pkt_size = s->packet_size;
        int block_align = 0;

        /* Find block_align from the WMA Lossless stream */
        for (unsigned n = 0; n < s->nb_streams; n++) {
            if (s->streams[n]->codecpar->codec_id == AV_CODEC_ID_WMALOSSLESS) {
                block_align = s->streams[n]->codecpar->block_align;
                break;
            }
        }

        /* avg bitrate over send_duration */
        wma_avg_bitrate = (int32_t)(data_bytes * 8 * 10000000LL /
                                    asf->duration);
        wma_avg_time = duration / asf->nb_packets;

        /* Buffer window: overflow leaky bucket at data_bitrate.
         * Uses block_align as the payload per sample (compressed frame size,
         * excluding ASF data packet overhead). */
        if (block_align > 0)
            wma_avg_buffer_ms = asf_overflow_buffer_ms(asf->pkt_send_times,
                                                       asf->nb_packets,
                                                       block_align,
                                                       wma_avg_bitrate);
        else
            wma_avg_buffer_ms = wma_avg_bitrate / 60;
        if (wma_avg_buffer_ms < 1000)
            wma_avg_buffer_ms = 1000;

        /* Peak bitrate: max rate over 5-second sliding windows.
         * A 5-second window captures sustained bursts while filtering
         * single-packet timing jitter. */
        {
            int64_t max_rate = 0;
            uint64_t j = 0;
            for (uint64_t i = 1; i < asf->nb_packets; i++) {
                int64_t dt, n_pkts, rate;
                while (j < i - 1 &&
                       asf->pkt_send_times[i] -
                       asf->pkt_send_times[j + 1] >= 5000)
                    j++;
                dt = asf->pkt_send_times[i] - asf->pkt_send_times[j];
                if (dt > 0) {
                    n_pkts = i - j;
                    rate = n_pkts * (int64_t)pkt_size * 8000 / dt;
                    if (rate > max_rate)
                        max_rate = rate;
                }
            }
            if (max_rate > INT32_MAX)
                max_rate = INT32_MAX;
            wma_peak_bitrate = max_rate > wma_avg_bitrate
                             ? (int32_t)max_rate : wma_avg_bitrate;
        }

        /* Alt buffer: fixed 1570 ms */
        wma_peak_buffer_ms = 1570;

        wmalossless_bitrate = wma_avg_bitrate;
    }

    if (asf->is_streamed) {
        put_chunk(s, 0x4824, 0, 0xc00); /* start of stream (length will be patched later) */
    }

    ff_put_guid(pb, &ff_asf_header);
    avio_wl64(pb, -1); /* header length, will be patched after */
    avio_wl32(pb, 3 + has_wmalossless + has_title + !!metadata_count + nb_real_streams); /* number of chunks in header */
    avio_w8(pb, 1); /* ??? */
    avio_w8(pb, 2); /* ??? */

    /* file header */
    header_offset = avio_tell(pb);
    hpos          = put_header(pb, &ff_asf_file_header);
    ff_put_guid(pb, &ff_asf_my_guid);
    avio_wl64(pb, file_size);
    avio_wl64(pb, unix_to_file_time(asf->creation_time));
    avio_wl64(pb, asf->nb_packets); /* number of packets */
    avio_wl64(pb, duration); /* end time stamp (in 100ns units) */
    avio_wl64(pb, asf->duration); /* duration (in 100ns units) */
    avio_wl64(pb, PREROLL_TIME); /* start time stamp */
    avio_wl32(pb, (asf->is_streamed || !(pb->seekable & AVIO_SEEKABLE_NORMAL)) ? 3 : 2);  /* ??? */
    avio_wl32(pb, s->packet_size); /* packet size */
    avio_wl32(pb, s->packet_size); /* packet size */
    {
        int max_bitrate = bit_rate ? bit_rate : -1;
        if (has_wmalossless && wma_peak_bitrate > 0)
            max_bitrate = wma_peak_bitrate;
        else if (has_wmalossless && wma_avg_bitrate > 0)
            max_bitrate = wma_avg_bitrate;
        avio_wl32(pb, max_bitrate); /* Maximum data rate in bps */
    }
    end_header(pb, hpos);

    /* header_extension */
    hpos = put_header(pb, &ff_asf_head1_guid);
    ff_put_guid(pb, &ff_asf_head2_guid);
    avio_wl16(pb, 6);
    avio_wl32(pb, 0); /* length, to be filled later */
    if (asf->nb_languages) {
        int64_t hpos2;
        int nb_audio_languages = 0;

        hpos2 = put_header(pb, &ff_asf_language_guid);
        avio_wl16(pb, asf->nb_languages);
        for (int i = 0; i < asf->nb_languages; i++) {
            avio_w8(pb, 6);
            avio_put_str16le(pb, asf->languages[i]);
        }
        end_header(pb, hpos2);

        for (int i = 0; i < asf->nb_languages; i++)
            if (audio_language_counts[i])
                nb_audio_languages++;

        if (nb_audio_languages > 1) {
            hpos2 = put_header(pb, &ff_asf_group_mutual_exclusion_object);
            ff_put_guid(pb, &ff_asf_mutex_language);
            avio_wl16(pb, nb_audio_languages);
            for (int i = 0; i < asf->nb_languages; i++) {
                if (audio_language_counts[i]) {
                    avio_wl16(pb, audio_language_counts[i]);
                    for (unsigned n = 0; n < s->nb_streams; n++)
                        if (asf->streams[n].stream_language_index == i && s->streams[n]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                            avio_wl16(pb, asf->streams[n].num);
                }
            }
            end_header(pb, hpos2);
        }

        for (unsigned n = 0; n < s->nb_streams; n++) {
            int64_t es_pos;
            if (s->streams[n]->disposition & AV_DISPOSITION_ATTACHED_PIC)
                continue;
            if (asf->streams[n].stream_language_index > 127)
                continue;
            es_pos = put_header(pb, &ff_asf_extended_stream_properties_object);
            avio_wl64(pb, 0); /* start time */
            avio_wl64(pb, 0); /* end time */
            avio_wl32(pb, s->streams[n]->codecpar->bit_rate); /* data bitrate bps */
            avio_wl32(pb, 5000); /* buffer size ms */
            avio_wl32(pb, 0); /* initial buffer fullness */
            avio_wl32(pb, s->streams[n]->codecpar->bit_rate); /* peak data bitrate */
            avio_wl32(pb, 5000); /* maximum buffer size ms */
            avio_wl32(pb, 0); /* max initial buffer fullness */
            avio_wl32(pb, 0); /* max object size */
            avio_wl32(pb, (!asf->is_streamed && (pb->seekable & AVIO_SEEKABLE_NORMAL)) << 1); /* flags - seekable */
            avio_wl16(pb, asf->streams[n].num); /* stream number */
            avio_wl16(pb, asf->streams[n].stream_language_index); /* language id index */
            avio_wl64(pb, 0); /* avg time per frame */
            avio_wl16(pb, 0); /* stream name count */
            avio_wl16(pb, 0); /* payload extension system count */
            end_header(pb, es_pos);
        }
    }
    /* write Extended Stream Properties for WMA Lossless streams
     * not covered by the language-gated block above */
    for (unsigned n = 0; n < s->nb_streams; n++) {
        AVCodecParameters *par = s->streams[n]->codecpar;
        int64_t es_pos;
        int32_t esp_bitrate, esp_peak_bitrate;
        int esp_buffer_ms, esp_peak_buffer_ms;
        int64_t avg_time;
        if (s->streams[n]->disposition & AV_DISPOSITION_ATTACHED_PIC)
            continue;
        if (par->codec_id != AV_CODEC_ID_WMALOSSLESS)
            continue;
        if (asf->nb_languages && asf->streams[n].stream_language_index <= 127)
            continue;  /* already written in the language block */

        /* Use pre-computed stats during trailer rewrite, defaults otherwise */
        if (wma_avg_bitrate > 0) {
            esp_bitrate      = wma_avg_bitrate;
            esp_peak_bitrate = (wma_peak_bitrate / 5000) * 5000;
            esp_buffer_ms      = wma_avg_buffer_ms;
            esp_peak_buffer_ms = wma_peak_buffer_ms;
            avg_time = wma_avg_time;
        } else {
            esp_bitrate      = par->bit_rate;
            esp_peak_bitrate = par->bit_rate;
            esp_buffer_ms      = 5000;
            esp_peak_buffer_ms = 5000;
            avg_time = 0;
        }

        es_pos = put_header(pb, &ff_asf_extended_stream_properties_object);
        avio_wl64(pb, 0);                  /* start time */
        avio_wl64(pb, 0);                  /* end time */
        avio_wl32(pb, esp_bitrate);        /* data bitrate bps */
        avio_wl32(pb, esp_buffer_ms);      /* buffer size ms */
        avio_wl32(pb, 0);                  /* initial buffer fullness */
        avio_wl32(pb, esp_peak_bitrate);   /* peak data bitrate */
        avio_wl32(pb, esp_peak_buffer_ms); /* max buffer size ms */
        avio_wl32(pb, 0);                  /* max initial buffer fullness */
        avio_wl32(pb, par->block_align);   /* max object size */
        avio_wl32(pb, 0x02);               /* flags: seekable */
        avio_wl16(pb, asf->streams[n].num); /* stream number */
        avio_wl16(pb, 0);                  /* language id index */
        avio_wl64(pb, avg_time);           /* avg time per frame (100ns units) */
        avio_wl16(pb, 0);                  /* stream name count */
        avio_wl16(pb, 0);                  /* payload extension system count */
        end_header(pb, es_pos);
    }
    {
        int metadata_obj_count = 2 * has_aspect_ratio;
        int wmalossless_stream_num = 0;

        if (has_wmalossless) {
            metadata_obj_count += 6; /* IsVBR, DeviceConformanceTemplate, 4x DRC */
            for (unsigned n = 0; n < s->nb_streams; n++) {
                if (s->streams[n]->codecpar->codec_id == AV_CODEC_ID_WMALOSSLESS) {
                    wmalossless_stream_num = asf->streams[n].num;
                    break;
                }
            }
        }

        if (metadata_obj_count) {
            int64_t hpos2;
            const AVDictionaryEntry *e;
            int drc_peak = 0, drc_avg = 0;

            hpos2 = put_header(pb, &ff_asf_metadata_header);
            avio_wl16(pb, metadata_obj_count);

            /* Aspect ratio records (skip attached_pic video streams) */
            for (unsigned n = 0; n < s->nb_streams; n++) {
                AVCodecParameters *const par = s->streams[n]->codecpar;
                if (s->streams[n]->disposition & AV_DISPOSITION_ATTACHED_PIC)
                    continue;
                if (   par->codec_type == AVMEDIA_TYPE_VIDEO
                    && par->sample_aspect_ratio.num > 0
                    && par->sample_aspect_ratio.den > 0) {
                    AVRational sar = par->sample_aspect_ratio;
                    avio_wl16(pb, 0);
                    avio_wl16(pb, asf->streams[n].num);
                    avio_wl16(pb, 26);  /* name_len */
                    avio_wl16(pb,  3);  /* value_type */
                    avio_wl32(pb,  4);  /* value_len */
                    avio_put_str16le(pb, "AspectRatioX");
                    avio_wl32(pb, sar.num);
                    avio_wl16(pb, 0);
                    avio_wl16(pb, asf->streams[n].num);
                    avio_wl16(pb, 26);  /* name_len */
                    avio_wl16(pb,  3);  /* value_type */
                    avio_wl32(pb,  4);  /* value_len */
                    avio_put_str16le(pb, "AspectRatioY");
                    avio_wl32(pb, sar.den);
                }
            }

            /* WMA Lossless per-stream attributes */
            if (has_wmalossless) {
                e = av_dict_get(s->metadata, "WM/WMADRCPeakReference", NULL, 0);
                if (e) drc_peak = strtol(e->value, NULL, 10);
                e = av_dict_get(s->metadata, "WM/WMADRCAverageReference", NULL, 0);
                if (e) drc_avg = strtol(e->value, NULL, 10);

                /* IsVBR (BOOL, WORD-sized) */
                avio_wl16(pb, 0);                              /* lang */
                avio_wl16(pb, wmalossless_stream_num);         /* stream */
                avio_wl16(pb, sizeof("IsVBR") * 2);            /* name_len */
                avio_wl16(pb, 2);                              /* type: BOOL */
                avio_wl32(pb, 2);                              /* val_len */
                avio_put_str16le(pb, "IsVBR");
                avio_wl16(pb, 1);

                /* DeviceConformanceTemplate (Unicode) */
                e = av_dict_get(s->metadata,
                                "DeviceConformanceTemplate",
                                NULL, 0);
                {
                    const char *dct = e ? e->value : "N1";
                    avio_wl16(pb, 0);
                    avio_wl16(pb, wmalossless_stream_num);
                    avio_wl16(pb, sizeof("DeviceConformanceTemplate") * 2);
                    avio_wl16(pb, 0);                          /* type: Unicode */
                    avio_wl32(pb, (strlen(dct) + 1) * 2);
                    avio_put_str16le(pb, "DeviceConformanceTemplate");
                    avio_put_str16le(pb, dct);
                }

                /* WM/WMADRCPeakReference (DWORD) */
                avio_wl16(pb, 0);
                avio_wl16(pb, wmalossless_stream_num);
                avio_wl16(pb, sizeof("WM/WMADRCPeakReference") * 2);
                avio_wl16(pb, 3);                              /* type: DWORD */
                avio_wl32(pb, 4);
                avio_put_str16le(pb, "WM/WMADRCPeakReference");
                avio_wl32(pb, drc_peak);

                /* WM/WMADRCPeakTarget (DWORD) */
                avio_wl16(pb, 0);
                avio_wl16(pb, wmalossless_stream_num);
                avio_wl16(pb, sizeof("WM/WMADRCPeakTarget") * 2);
                avio_wl16(pb, 3);
                avio_wl32(pb, 4);
                avio_put_str16le(pb, "WM/WMADRCPeakTarget");
                avio_wl32(pb, drc_peak);

                /* WM/WMADRCAverageReference (DWORD) */
                avio_wl16(pb, 0);
                avio_wl16(pb, wmalossless_stream_num);
                avio_wl16(pb, sizeof("WM/WMADRCAverageReference") * 2);
                avio_wl16(pb, 3);
                avio_wl32(pb, 4);
                avio_put_str16le(pb, "WM/WMADRCAverageReference");
                avio_wl32(pb, drc_avg);

                /* WM/WMADRCAverageTarget (DWORD) */
                avio_wl16(pb, 0);
                avio_wl16(pb, wmalossless_stream_num);
                avio_wl16(pb, sizeof("WM/WMADRCAverageTarget") * 2);
                avio_wl16(pb, 3);
                avio_wl32(pb, 4);
                avio_put_str16le(pb, "WM/WMADRCAverageTarget");
                avio_wl32(pb, drc_avg);
            }

            end_header(pb, hpos2);
        }
    }
    /* Write Metadata Library Object for full-size WM/Picture inside Header Extension */
    if (has_pic) {
        int64_t ml_pos;
        AVStream *pic_st  = s->streams[asf->attached_pic_index];
        AVPacket *pic_pkt = &pic_st->attached_pic;
        const char *mime  = asf_pic_mime_type(pic_st->codecpar->codec_id);
        int name_utf16_len = ((int)strlen("WM/Picture") + 1) * 2; /* 22 bytes */
        int val_size       = asf_wm_pic_value_size(pic_pkt, mime);

        ml_pos = put_header(pb, &ff_asf_metadata_library_header);
        avio_wl16(pb, 1);                    /* records count */
        /* record fields */
        avio_wl16(pb, 0);                    /* lang_list_index */
        avio_wl16(pb, 0);                    /* stream_number */
        avio_wl16(pb, name_utf16_len);       /* name_len (bytes) */
        avio_wl16(pb, ASF_BYTE_ARRAY);       /* value_type */
        avio_wl32(pb, val_size);             /* value_len (DWORD) */
        avio_put_str16le(pb, "WM/Picture");  /* name */
        asf_write_wm_pic_value(pb, pic_pkt, mime);
        end_header(pb, ml_pos);
    }
    {
        int64_t pos1;
        pos1 = avio_tell(pb);
        avio_seek(pb, hpos + 42, SEEK_SET);
        avio_wl32(pb, pos1 - hpos - 46);
        avio_seek(pb, pos1, SEEK_SET);
    }
    end_header(pb, hpos);

    if ((ret = avio_open_dyn_buf(&dyn_buf)) < 0)
        return ret;

    /* title and other info */
    if (has_title) {
        uint8_t *buf;
        int len;

        hpos = put_header(pb, &ff_asf_comment_header);

        for (size_t n = 0; n < FF_ARRAY_ELEMS(tags); n++) {
            len = tags[n] ? avio_put_str16le(dyn_buf, tags[n]->value) : 0;
            avio_wl16(pb, len);
        }
        len = avio_get_dyn_buf(dyn_buf, &buf);
        avio_write(pb, buf, len);
        ffio_reset_dyn_buf(dyn_buf);
        end_header(pb, hpos);
    }
    if (metadata_count) {
        const AVDictionaryEntry *tag = NULL;
        hpos = put_header(pb, &ff_asf_extended_content_header);
        avio_wl16(pb, metadata_count);
        while ((tag = av_dict_iterate(s->metadata, tag))) {
            ASFDataType tag_type;
            /* DRC metadata belongs in the per-stream Metadata Object.
             * av_dict_get (used for counting) is case-insensitive,
             * so match the same way here. */
            if (!av_strcasecmp(tag->key, "WM/WMADRCPeakReference") ||
                !av_strcasecmp(tag->key, "WM/WMADRCPeakTarget") ||
                !av_strcasecmp(tag->key, "WM/WMADRCAverageReference") ||
                !av_strcasecmp(tag->key, "WM/WMADRCAverageTarget"))
                continue;
            tag_type = asf_get_tag_type(tag->key);
            put_str16(pb, dyn_buf, tag->key);
            switch (tag_type) {
            case ASF_BOOL: {
                int bval = !av_strcasecmp(tag->value, "true") ||
                           !av_strcasecmp(tag->value, "1");
                avio_wl16(pb, ASF_BOOL);
                avio_wl16(pb, 4);
                avio_wl32(pb, bval);
                break;
            }
            case ASF_DWORD:
                avio_wl16(pb, ASF_DWORD);
                avio_wl16(pb, 4);
                avio_wl32(pb, strtol(tag->value, NULL, 10));
                break;
            default:
                avio_wl16(pb, ASF_UNICODE);
                put_str16(pb, dyn_buf, tag->value);
                break;
            }
        }
        /* ASFLeakyBucketPairs: 14 fixed rates matching the WMF SDK schedule.
         * Format: [WORD reserved=0] [DWORD rate, DWORD buffer_ms] * 14 */
        if (has_wmalossless && wmalossless_bitrate > 0) {
            static const int32_t bucket_rates[] = {
                24000, 30000, 45000, 58000, 115200, 240000, 350000,
                500000, 730000, 1000000, 1400000, 2100000, 5000000, 10000000
            };
            int num_pairs = FF_ARRAY_ELEMS(bucket_rates);
            int data_size = 2 + num_pairs * 8;  /* WORD reserved + N * (DWORD rate + DWORD buffer) */

            put_str16(pb, dyn_buf, "ASFLeakyBucketPairs");
            avio_wl16(pb, ASF_BYTE_ARRAY);
            avio_wl16(pb, data_size);
            avio_wl16(pb, 0);  /* reserved */
            for (int i = 0; i < num_pairs; i++) {
                int32_t rate = bucket_rates[i];
                int buffer_ms;
                if (asf->pkt_send_times && asf->nb_packets > 1)
                    buffer_ms = asf_leaky_bucket_ms(asf->pkt_send_times,
                                                     asf->nb_packets,
                                                     s->packet_size, rate);
                else
                    buffer_ms = rate > 0 ? (int)((int64_t)s->packet_size * 8000 / rate) : 5000;
                avio_wl32(pb, rate);
                avio_wl32(pb, buffer_ms);
            }
        }
        /* WM/Picture thumbnail in Extended Content Description */
        if (has_pic_thumb) {
            AVStream *thumb_st  = s->streams[asf->attached_pic_thumb_index];
            AVPacket *thumb_pkt = &thumb_st->attached_pic;
            const char *mime    = asf_pic_mime_type(thumb_st->codecpar->codec_id);
            int val_size        = asf_wm_pic_value_size(thumb_pkt, mime);

            put_str16(pb, dyn_buf, "WM/Picture");
            avio_wl16(pb, ASF_BYTE_ARRAY);
            avio_wl16(pb, val_size);
            asf_write_wm_pic_value(pb, thumb_pkt, mime);
        }
        end_header(pb, hpos);
    }
    /* chapters using ASF markers */
    if (!asf->is_streamed && s->nb_chapters) {
        asf_write_markers(s, dyn_buf);
    }
    /* stream headers */
    for (unsigned n = 0; n < s->nb_streams; n++) {
        AVCodecParameters *const par = s->streams[n]->codecpar;
        int64_t es_pos;

        if (s->streams[n]->disposition & AV_DISPOSITION_ATTACHED_PIC)
            continue;

        asf->streams[n].seq = 1;

        switch (par->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            wav_extra_size = 0;
            extra_size     = 18 + wav_extra_size;
            extra_size2    = 8;
            break;
        default:
        case AVMEDIA_TYPE_VIDEO:
            wav_extra_size = par->extradata_size;
            extra_size     = 0x33 + wav_extra_size;
            extra_size2    = 0;
            break;
        }

        hpos = put_header(pb, &ff_asf_stream_header);
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            ff_put_guid(pb, &ff_asf_audio_stream);
            ff_put_guid(pb, &ff_asf_audio_conceal_spread);
        } else {
            ff_put_guid(pb, &ff_asf_video_stream);
            ff_put_guid(pb, &ff_asf_video_conceal_none);
        }
        avio_wl64(pb, 0); /* ??? */
        es_pos = avio_tell(pb);
        avio_wl32(pb, extra_size); /* wav header len */
        avio_wl32(pb, extra_size2); /* additional data len */
        avio_wl16(pb, asf->streams[n].num); /* stream number */
        avio_wl32(pb, 0); /* ??? */

        if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* WAVEFORMATEX header */
            int wavsize = ff_put_wav_header(s, pb, par, FF_PUT_WAV_HEADER_FORCE_WAVEFORMATEX);

            if (wavsize < 0) {
                ret = wavsize;
                goto fail;
            }
            if (wavsize != extra_size) {
                cur_pos = avio_tell(pb);
                avio_seek(pb, es_pos, SEEK_SET);
                avio_wl32(pb, wavsize); /* wav header len */
                avio_seek(pb, cur_pos, SEEK_SET);
            }
            /* ERROR Correction */
            avio_w8(pb, 0x01);
            if (par->codec_id == AV_CODEC_ID_ADPCM_G726 || !par->block_align) {
                avio_wl16(pb, 0x0190);
                avio_wl16(pb, 0x0190);
            } else {
                avio_wl16(pb, par->block_align);
                avio_wl16(pb, par->block_align);
            }
            avio_wl16(pb, 0x01);
            avio_w8(pb, 0x00);
        } else {
            avio_wl32(pb, par->width);
            avio_wl32(pb, par->height);
            avio_w8(pb, 2); /* ??? */
            avio_wl16(pb, 40 + par->extradata_size); /* size */

            /* BITMAPINFOHEADER header */
            ff_put_bmp_header(pb, par, 1, 0, 0);
        }
        end_header(pb, hpos);
    }

    /* media comments */

    hpos = put_header(pb, &ff_asf_codec_comment_header);
    ff_put_guid(pb, &ff_asf_codec_comment1_header);
    avio_wl32(pb, nb_real_streams);
    for (unsigned n = 0; n < s->nb_streams; n++) {
        AVCodecParameters *const par = s->streams[n]->codecpar;
        const AVCodecDescriptor *const codec_desc = avcodec_descriptor_get(par->codec_id);
        const char *desc;
        const char *codec_params = NULL;
        char wma_params[256];

        if (s->streams[n]->disposition & AV_DISPOSITION_ATTACHED_PIC)
            continue;

        if (par->codec_type == AVMEDIA_TYPE_AUDIO)
            avio_wl16(pb, 2);
        else if (par->codec_type == AVMEDIA_TYPE_VIDEO)
            avio_wl16(pb, 1);
        else
            avio_wl16(pb, -1);

        if (par->codec_id == AV_CODEC_ID_WMAV2) {
            desc = "Windows Media Audio V8";
        } else if (par->codec_id == AV_CODEC_ID_WMALOSSLESS) {
            desc = "Windows Media Audio 9.2 Lossless";
            snprintf(wma_params, sizeof(wma_params),
                     "VBR Quality 100, %d kHz, %d channel %d bit 1-pass VBR",
                     par->sample_rate / 1000,
                     par->ch_layout.nb_channels,
                     par->bits_per_coded_sample);
            codec_params = wma_params;
        } else {
            desc = codec_desc ? codec_desc->name : NULL;
        }

        if (desc) {
            uint8_t *buf;
            int len;

            avio_put_str16le(dyn_buf, desc);
            len = avio_get_dyn_buf(dyn_buf, &buf);
            avio_wl16(pb, len / 2); // "number of characters" = length in bytes / 2

            avio_write(pb, buf, len);
            ffio_reset_dyn_buf(dyn_buf);
        } else
            avio_wl16(pb, 0);

        if (codec_params) {
            uint8_t *buf;
            int len;

            avio_put_str16le(dyn_buf, codec_params);
            len = avio_get_dyn_buf(dyn_buf, &buf);
            avio_wl16(pb, len / 2);
            avio_write(pb, buf, len);
            ffio_reset_dyn_buf(dyn_buf);
        } else {
            avio_wl16(pb, 0); /* no description */
        }

        /* id */
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            avio_wl16(pb, 2);
            avio_wl16(pb, par->codec_tag);
        } else {
            avio_wl16(pb, 4);
            avio_wl32(pb, par->codec_tag);
        }
        if (!par->codec_tag) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }
    end_header(pb, hpos);

    /* stream bitrate properties object (WMA Lossless only —
     * per-stream bitrate; reference encoders use peak bitrate here) */
    if (has_wmalossless) {
        hpos = put_header(pb, &ff_asf_stream_bitrate_properties);
        avio_wl16(pb, nb_real_streams);
        for (unsigned i = 0; i < s->nb_streams; i++) {
            int32_t stream_bitrate;
            if (s->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
                continue;
            if (wma_peak_bitrate > 0)
                stream_bitrate = wma_peak_bitrate;
            else
                stream_bitrate = s->streams[i]->codecpar->bit_rate;
            avio_wl16(pb, asf->streams[i].num);
            avio_wl32(pb, stream_bitrate);
        }
        end_header(pb, hpos);
    }

    /* patch the header size fields */

    cur_pos     = avio_tell(pb);
    header_size = cur_pos - header_offset;
    if (asf->is_streamed) {
        header_size += 8 + 30 + DATA_HEADER_SIZE;

        avio_seek(pb, header_offset - 10 - 30, SEEK_SET);
        avio_wl16(pb, header_size);
        avio_seek(pb, header_offset - 2 - 30, SEEK_SET);
        avio_wl16(pb, header_size);

        header_size -= 8 + 30 + DATA_HEADER_SIZE;
    }
    header_size += 24 + 6;
    avio_seek(pb, header_offset - 14, SEEK_SET);
    avio_wl64(pb, header_size);
    avio_seek(pb, cur_pos, SEEK_SET);

    /* movie chunk, followed by packets of packet_size */
    asf->data_offset = cur_pos;
    ff_put_guid(pb, &ff_asf_data_header);
    avio_wl64(pb, data_chunk_size);
    ff_put_guid(pb, &ff_asf_my_guid);
    avio_wl64(pb, asf->nb_packets); /* nb packets */
    avio_w8(pb, 1); /* ??? */
    avio_w8(pb, 1); /* ??? */
    ret = 0;
fail:
    ffio_free_dyn_buf(&dyn_buf);
    return ret;
}

static int asf_write_header(AVFormatContext *s)
{
    ASFContext *asf = s->priv_data;
    int ret;
    int has_wmalossless = 0;
    int wmalossless_hi_res = 0;

    s->packet_size  = asf->packet_size;
    s->max_interleave_delta = 0;
    asf->nb_packets = 0;

    if (s->nb_streams > 127) {
        av_log(s, AV_LOG_ERROR, "ASF can only handle 127 streams\n");
        return AVERROR(EINVAL);
    }

    /* Classify attached_pic streams for WM/Picture embedding.
     * Two streams: smaller → ECD thumbnail, larger → Metadata Library.
     * One stream:  Metadata Library always; also ECD if value fits in WORD. */
    asf->attached_pic_index       = -1;
    asf->attached_pic_thumb_index = -1;
    {
        int pic_indices[2] = { -1, -1 };
        int pic_count = 0;
        for (unsigned n = 0; n < s->nb_streams; n++) {
            if (s->streams[n]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if (pic_count < 2)
                    pic_indices[pic_count] = n;
                pic_count++;
            }
        }
        if (pic_count == 2) {
            int a = pic_indices[0], b = pic_indices[1];
            int size_a = s->streams[a]->attached_pic.size;
            int size_b = s->streams[b]->attached_pic.size;
            if (size_a <= size_b) {
                asf->attached_pic_thumb_index = a;
                asf->attached_pic_index       = b;
            } else {
                asf->attached_pic_thumb_index = b;
                asf->attached_pic_index       = a;
            }
        } else if (pic_count == 1) {
            int idx = pic_indices[0];
            const char *mime = asf_pic_mime_type(s->streams[idx]->codecpar->codec_id);
            int val_size = asf_wm_pic_value_size(&s->streams[idx]->attached_pic, mime);
            asf->attached_pic_index = idx;
            if (val_size <= 0xFFFF)
                asf->attached_pic_thumb_index = idx;
        }
    }

    for (unsigned n = 0; n < s->nb_streams; n++) {
        AVStream *st = s->streams[n];
        if (st->codecpar->codec_id == AV_CODEC_ID_WMALOSSLESS) {
            has_wmalossless = 1;
            if (st->codecpar->sample_rate > 48000)
                wmalossless_hi_res = 1;
            /* Adjust packet size for WMA Lossless frames if using default size.
             * Each ASF packet carries exactly one WMA Lossless frame (block_align
             * bytes) plus fixed overhead: SINGLE_PAYLOAD_HEADERS (26 bytes of
             * EC + PPI + payload header) + 1 byte padding-length field + 4 bytes
             * of zero padding = 31 bytes total.  This matches the packet layout
             * produced by reference WMA Lossless encoders (e.g. fre:ac / Windows
             * Media Encoder) and is required for reliable Zune device playback. */
            if (st->codecpar->block_align > 0 && asf->packet_size == 3200) {
                int target_size = st->codecpar->block_align
                                + SINGLE_PAYLOAD_HEADERS + 5;
                if (target_size > PACKET_SIZE_MAX)
                    target_size = PACKET_SIZE_MAX;
                if (target_size < PACKET_SIZE_MIN)
                    target_size = PACKET_SIZE_MIN;
                asf->packet_size = target_size;
                s->packet_size = asf->packet_size;
            }
            break;
        }
    }

    if (has_wmalossless) {
        if (!asf->creation_time)
            asf->creation_time = (int64_t)time(NULL) * 1000000LL;

        av_dict_set(&s->metadata, "WMFSDKNeeded", "0.0.0.0000", 0);
        av_dict_set(&s->metadata, "DeviceConformanceTemplate",
                    wmalossless_hi_res ? "N2" : "N1", 0);
        av_dict_set(&s->metadata, "WMFSDKVersion", "12.0.18362.778", 0);
        av_dict_set(&s->metadata, "IsVBR", "1", 0);

        if (!av_dict_get(s->metadata, "WM/ToolName", NULL, 0))
            av_dict_set(&s->metadata, "WM/ToolName", "FFmpeg", 0);
        if (!av_dict_get(s->metadata, "WM/ToolVersion", NULL, 0)) {
            char version[32];
            snprintf(version, sizeof(version), "v%d.%d.%d",
                     LIBAVFORMAT_VERSION_MAJOR,
                     LIBAVFORMAT_VERSION_MINOR,
                     LIBAVFORMAT_VERSION_MICRO);
            av_dict_set(&s->metadata, "WM/ToolVersion", version, 0);
        }

        /* Pre-populate DRC tags so the header size is identical between the
         * initial write and the trailer rewrite. The encoder injects real
         * values via AV_PKT_DATA_STRINGS_METADATA on the last packet, which
         * overwrites these placeholders in s->metadata before the rewrite. */
        av_dict_set(&s->metadata, "WM/WMADRCPeakReference", "0", 0);
        av_dict_set(&s->metadata, "WM/WMADRCPeakTarget", "0", 0);
        av_dict_set(&s->metadata, "WM/WMADRCAverageReference", "0", 0);
        av_dict_set(&s->metadata, "WM/WMADRCAverageTarget", "0", 0);
    }

    asf->index_ptr             = av_malloc(sizeof(ASFIndex) * ASF_INDEX_BLOCK);
    if (!asf->index_ptr)
        return AVERROR(ENOMEM);
    asf->nb_index_memory_alloc = ASF_INDEX_BLOCK;
    asf->maximum_packet        = 0;

    /* the data-chunk-size has to be 50 (DATA_HEADER_SIZE), which is
     * data_size - asf->data_offset at the moment this function is done.
     * It is needed to use asf as a streamable format. */
    if ((ret = asf_write_header1(s, 0, DATA_HEADER_SIZE)) < 0)
        return ret;

    asf->packet_nb_payloads     = 0;
    asf->packet_timestamp_start = -1;
    asf->packet_timestamp_end   = -1;
    ffio_init_write_context(&asf->pb, asf->packet_buf, s->packet_size);

    if (s->avoid_negative_ts < 0)
        s->avoid_negative_ts = 1;

    return 0;
}

static int asf_write_stream_header(AVFormatContext *s)
{
    ASFContext *asf = s->priv_data;

    asf->is_streamed = 1;

    return asf_write_header(s);
}

static int put_payload_parsing_info(AVFormatContext *s,
                                    unsigned sendtime, unsigned duration,
                                    int nb_payloads, int padsize)
{
    ASFContext *asf = s->priv_data;
    AVIOContext *pb = s->pb;
    int ppi_size;
    int64_t start = avio_tell(pb);

    int iLengthTypeFlags = ASF_PPI_LENGTH_TYPE_FLAGS;

    padsize -= PACKET_HEADER_MIN_SIZE;
    if (asf->multi_payloads_present)
        padsize--;
    av_assert0(padsize >= 0);

    avio_w8(pb, ASF_PACKET_ERROR_CORRECTION_FLAGS);
    ffio_fill(pb, 0x0, ASF_PACKET_ERROR_CORRECTION_DATA_SIZE);

    if (asf->multi_payloads_present)
        iLengthTypeFlags |= ASF_PPI_FLAG_MULTIPLE_PAYLOADS_PRESENT;

    if (padsize > 0) {
        if (padsize < 256)
            iLengthTypeFlags |= ASF_PPI_FLAG_PADDING_LENGTH_FIELD_IS_BYTE;
        else
            iLengthTypeFlags |= ASF_PPI_FLAG_PADDING_LENGTH_FIELD_IS_WORD;
    }
    avio_w8(pb, iLengthTypeFlags);

    avio_w8(pb, ASF_PPI_PROPERTY_FLAGS);

    if (iLengthTypeFlags & ASF_PPI_FLAG_PADDING_LENGTH_FIELD_IS_WORD)
        avio_wl16(pb, padsize - 2);
    if (iLengthTypeFlags & ASF_PPI_FLAG_PADDING_LENGTH_FIELD_IS_BYTE)
        avio_w8(pb, padsize - 1);

    avio_wl32(pb, sendtime);
    avio_wl16(pb, duration);
    if (asf->multi_payloads_present)
        avio_w8(pb, nb_payloads | ASF_PAYLOAD_FLAGS);

    ppi_size = avio_tell(pb) - start;

    return ppi_size;
}

static void flush_packet(AVFormatContext *s)
{
    ASFContext *asf = s->priv_data;
    int packet_hdr_size, packet_filled_size;

    av_assert0(asf->packet_timestamp_end >= asf->packet_timestamp_start);

    if (asf->is_streamed)
        put_chunk(s, 0x4424, s->packet_size, 0);

    packet_hdr_size = put_payload_parsing_info(s,
                                               asf->packet_timestamp_start,
                                               asf->packet_timestamp_end - asf->packet_timestamp_start,
                                               asf->packet_nb_payloads,
                                               asf->packet_size_left);

    packet_filled_size = asf->packet_size - asf->packet_size_left;
    av_assert0(packet_hdr_size <= asf->packet_size_left);
    memset(asf->packet_buf + packet_filled_size, 0, asf->packet_size_left);

    avio_write(s->pb, asf->packet_buf, s->packet_size - packet_hdr_size);

    avio_write_marker(s->pb, AV_NOPTS_VALUE, AVIO_DATA_MARKER_FLUSH_POINT);

    asf->nb_packets++;

    /* Track packet timestamps for ESP leaky bucket computation.
     * On allocation failure the array is not grown; ESP computation
     * will fall back to defaults during the trailer rewrite. */
    if (asf->nb_packets > asf->pkt_stats_alloc) {
        uint32_t new_alloc = asf->pkt_stats_alloc <= UINT32_MAX / 2
                           ? FFMAX(asf->pkt_stats_alloc * 2, 1024)
                           : UINT32_MAX;
        int64_t *new_times = av_realloc_array(asf->pkt_send_times, new_alloc,
                                               sizeof(*new_times));
        if (new_times) {
            asf->pkt_send_times  = new_times;
            asf->pkt_stats_alloc = new_alloc;
        } else {
            av_log(s, AV_LOG_WARNING,
                   "Failed to grow packet timestamp array; "
                   "ESP values will use defaults\n");
        }
    }
    if (asf->pkt_send_times && asf->nb_packets <= asf->pkt_stats_alloc)
        asf->pkt_send_times[asf->nb_packets - 1] = asf->packet_timestamp_start;

    asf->packet_nb_payloads     = 0;
    asf->packet_timestamp_start = -1;
    asf->packet_timestamp_end   = -1;
    ffio_init_write_context(&asf->pb, asf->packet_buf, s->packet_size);
}

static void put_payload_header(AVFormatContext *s, ASFStream *stream,
                               int64_t presentation_time, int m_obj_size,
                               int m_obj_offset, int payload_len, int flags)
{
    ASFContext *asf = s->priv_data;
    AVIOContext *const pb = &asf->pb.pub;
    int val;

    val = stream->num;
    if (flags & AV_PKT_FLAG_KEY)
        val |= ASF_PL_FLAG_KEY_FRAME;
    avio_w8(pb, val);

    avio_w8(pb, stream->seq);     // Media object number
    avio_wl32(pb, m_obj_offset);  // Offset Into Media Object

    // Replicated Data shall be at least 8 bytes long.
    // The first 4 bytes of data shall contain the
    // Size of the Media Object that the payload belongs to.
    // The next 4 bytes of data shall contain the
    // Presentation Time for the media object that the payload belongs to.
    avio_w8(pb, ASF_PAYLOAD_REPLICATED_DATA_LENGTH);

    avio_wl32(pb, m_obj_size);        // Replicated Data - Media Object Size
    avio_wl32(pb, (uint32_t) presentation_time); // Replicated Data - Presentation Time

    if (asf->multi_payloads_present) {
        avio_wl16(pb, payload_len);   // payload length
    }
}

static void put_frame(AVFormatContext *s, ASFStream *stream, AVStream *avst,
                      int64_t timestamp, const uint8_t *buf,
                      int m_obj_size, int flags, int64_t pkt_duration)
{
    ASFContext *asf = s->priv_data;
    int m_obj_offset, payload_len, frag_len1;

    m_obj_offset = 0;
    while (m_obj_offset < m_obj_size) {
        payload_len = m_obj_size - m_obj_offset;
        if (asf->packet_timestamp_start == -1) {
            const int multi_payload_constant = (asf->packet_size - MULTI_PAYLOAD_HEADERS);
            asf->multi_payloads_present = (payload_len < multi_payload_constant);

            asf->packet_size_left = asf->packet_size;
            if (asf->multi_payloads_present) {
                frag_len1 = multi_payload_constant - 1;
            } else {
                frag_len1 = asf->packet_size - SINGLE_PAYLOAD_HEADERS;
            }
            asf->packet_timestamp_start = timestamp;
        } else {
            // multi payloads
            frag_len1 = asf->packet_size_left -
                        PAYLOAD_HEADER_SIZE_MULTIPLE_PAYLOADS -
                        PACKET_HEADER_MIN_SIZE - 1;

            if (frag_len1 < payload_len &&
                avst->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                flush_packet(s);
                continue;
            }
            if (asf->packet_timestamp_start > INT64_MAX - UINT16_MAX ||
                timestamp > asf->packet_timestamp_start + UINT16_MAX) {
                flush_packet(s);
                continue;
            }
        }
        if (frag_len1 > 0) {
            if (payload_len > frag_len1)
                payload_len = frag_len1;
            else if (payload_len == (frag_len1 - 1))
                payload_len = frag_len1 - 2;  // additional byte need to put padding length

            put_payload_header(s, stream, timestamp + PREROLL_TIME,
                               m_obj_size, m_obj_offset, payload_len, flags);
            avio_write(&asf->pb.pub, buf, payload_len);

            if (asf->multi_payloads_present)
                asf->packet_size_left -= (payload_len + PAYLOAD_HEADER_SIZE_MULTIPLE_PAYLOADS);
            else
                asf->packet_size_left -= (payload_len + PAYLOAD_HEADER_SIZE_SINGLE_PAYLOAD);
            asf->packet_timestamp_end = timestamp + pkt_duration;

            asf->packet_nb_payloads++;
        } else {
            payload_len = 0;
        }
        m_obj_offset += payload_len;
        buf          += payload_len;

        if (!asf->multi_payloads_present)
            flush_packet(s);
        else if (asf->packet_size_left <= (PAYLOAD_HEADER_SIZE_MULTIPLE_PAYLOADS + PACKET_HEADER_MIN_SIZE + 1))
            flush_packet(s);
        else if (asf->packet_nb_payloads == ASF_PAYLOADS_PER_PACKET)
            flush_packet(s);
    }
    stream->seq++;
}

static int update_index(AVFormatContext *s, int start_sec,
                         uint32_t packet_number, uint16_t packet_count,
                         uint64_t packet_offset)
{
    ASFContext *asf = s->priv_data;

    if (start_sec > asf->next_start_sec) {
        if (!asf->next_start_sec) {
            asf->next_packet_number = packet_number;
            asf->next_packet_count  = packet_count;
            asf->next_packet_offset = packet_offset;
        }

        if (start_sec > asf->nb_index_memory_alloc) {
            int err;
            asf->nb_index_memory_alloc = (start_sec + ASF_INDEX_BLOCK) & ~(ASF_INDEX_BLOCK - 1);
            if ((err = av_reallocp_array(&asf->index_ptr,
                                         asf->nb_index_memory_alloc,
                                         sizeof(*asf->index_ptr))) < 0) {
                asf->nb_index_memory_alloc = 0;
                return err;
            }
        }
        for (int i = asf->next_start_sec; i < start_sec; i++) {
            asf->index_ptr[i].packet_number = asf->next_packet_number;
            asf->index_ptr[i].packet_count  = asf->next_packet_count;
            asf->index_ptr[i].send_time     = asf->next_start_sec * INT64_C(10000000);
            asf->index_ptr[i].offset        = asf->next_packet_offset;

        }
    }
    asf->maximum_packet     = FFMAX(asf->maximum_packet, packet_count);
    asf->next_packet_number = packet_number;
    asf->next_packet_count  = packet_count;
    asf->next_packet_offset = packet_offset;
    asf->next_start_sec     = start_sec;

    return 0;
}

static int asf_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    ASFContext *asf = s->priv_data;
    AVIOContext *pb = s->pb;
    ASFStream *stream;
    AVCodecParameters *par;
    uint32_t packet_number;
    int64_t pts;
    int start_sec;
    int flags = pkt->flags;
    int ret;
    uint64_t offset = avio_tell(pb);

    /* attached_pic data is embedded in the header as WM/Picture metadata */
    if (pkt->stream_index == asf->attached_pic_index ||
        pkt->stream_index == asf->attached_pic_thumb_index)
        return 0;

    par  = s->streams[pkt->stream_index]->codecpar;
    stream = &asf->streams[pkt->stream_index];

    if (par->codec_type == AVMEDIA_TYPE_AUDIO &&
        par->codec_id != AV_CODEC_ID_WMALOSSLESS)
        flags &= ~AV_PKT_FLAG_KEY;

    /* extract DRC metadata from encoder side data */
    {
        size_t side_size;
        const uint8_t *side = av_packet_get_side_data(pkt,
            AV_PKT_DATA_STRINGS_METADATA, &side_size);
        if (side) {
            AVDictionary *dict = NULL;
            if (av_packet_unpack_dictionary(side, side_size, &dict) >= 0) {
                const AVDictionaryEntry *e = NULL;
                while ((e = av_dict_iterate(dict, e)))
                    av_dict_set(&s->metadata, e->key, e->value, 0);
                av_dict_free(&dict);
            }
        }
    }

    pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
    av_assert0(pts != AV_NOPTS_VALUE);
    if (   pts < - PREROLL_TIME
        || pts > (INT_MAX-3)/10000LL * ASF_INDEXED_INTERVAL - PREROLL_TIME) {
        av_log(s, AV_LOG_ERROR, "input pts %"PRId64" is invalid\n", pts);
        return AVERROR(EINVAL);
    }
    pts *= 10000;
    asf->duration = FFMAX(asf->duration, pts + pkt->duration * 10000);

    packet_number = asf->nb_packets;
    put_frame(s, stream, s->streams[pkt->stream_index],
              pkt->dts, pkt->data, pkt->size, flags, pkt->duration);

    start_sec = (int)((PREROLL_TIME * 10000 + pts + ASF_INDEXED_INTERVAL - 1)
              / ASF_INDEXED_INTERVAL);

    /* check index */
    if ((!asf->is_streamed) && (flags & AV_PKT_FLAG_KEY)) {
        uint16_t packet_count = asf->nb_packets - packet_number;
        ret = update_index(s, start_sec, packet_number, packet_count, offset);
        if (ret < 0)
            return ret;
    }
    asf->end_sec = start_sec;

    return 0;
}

static int asf_write_index(AVFormatContext *s, const ASFIndex *index,
                           uint16_t max, uint32_t count)
{
    AVIOContext *pb = s->pb;

    ff_put_guid(pb, &ff_asf_simple_index_header);
    avio_wl64(pb, 24 + 16 + 8 + 4 + 4 + (4 + 2) * count);
    ff_put_guid(pb, &ff_asf_my_guid);
    avio_wl64(pb, ASF_INDEXED_INTERVAL);
    avio_wl32(pb, max);
    avio_wl32(pb, count);
    for (uint32_t i = 0; i < count; i++) {
        avio_wl32(pb, index[i].packet_number);
        avio_wl16(pb, index[i].packet_count);
    }

    return 0;
}

static int asf_write_trailer(AVFormatContext *s)
{
    ASFContext *asf = s->priv_data;
    int64_t file_size, data_size;
    int ret;

    /* flush the current packet */
    if (asf->pb.pub.buf_ptr > asf->pb.pub.buffer)
        flush_packet(s);

    /* write index */
    data_size = avio_tell(s->pb);
    if (!asf->is_streamed && asf->next_start_sec) {
        if ((ret = update_index(s, asf->end_sec + 1, 0, 0, 0)) < 0)
            return ret;
        asf_write_index(s, asf->index_ptr, asf->maximum_packet, asf->next_start_sec);
    }

    if (asf->is_streamed || !(s->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        put_chunk(s, 0x4524, 0, 0); /* end of stream */
    } else {
        /* rewrite an updated header */
        file_size = avio_tell(s->pb);
        avio_seek(s->pb, 0, SEEK_SET);
        asf_write_header1(s, file_size, data_size - asf->data_offset);
    }

    return 0;
}

static void asf_deinit(AVFormatContext *s)
{
    ASFContext *const asf = s->priv_data;

    av_freep(&asf->index_ptr);
    av_freep(&asf->pkt_send_times);
}

static const AVOption asf_options[] = {
    { "packet_size", "Packet size", offsetof(ASFContext, packet_size), AV_OPT_TYPE_INT, {.i64 = 3200}, PACKET_SIZE_MIN, PACKET_SIZE_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL },
};

static const AVClass asf_muxer_class = {
    .class_name     = "ASF (stream) muxer",
    .item_name      = av_default_item_name,
    .option         = asf_options,
    .version        = LIBAVUTIL_VERSION_INT,
};

#if CONFIG_ASF_MUXER
const FFOutputFormat ff_asf_muxer = {
    .p.name         = "asf",
    .p.long_name    = NULL_IF_CONFIG_SMALL("ASF (Advanced / Active Streaming Format)"),
    .p.mime_type    = "video/x-ms-asf",
    .p.extensions   = "asf,wmv,wma",
    .p.audio_codec  = AV_CODEC_ID_WMAV2,
    .p.video_codec  = AV_CODEC_ID_MSMPEG4V3,
    .p.flags        = AVFMT_GLOBALHEADER,
    .p.codec_tag    = asf_codec_tags,
    .p.priv_class   = &asf_muxer_class,
    .priv_data_size = sizeof(ASFContext),
    .write_header   = asf_write_header,
    .write_packet   = asf_write_packet,
    .write_trailer  = asf_write_trailer,
    .deinit         = asf_deinit,
};
#endif /* CONFIG_ASF_MUXER */

#if CONFIG_ASF_STREAM_MUXER
const FFOutputFormat ff_asf_stream_muxer = {
    .p.name         = "asf_stream",
    .p.long_name    = NULL_IF_CONFIG_SMALL("ASF (Advanced / Active Streaming Format)"),
    .p.mime_type    = "video/x-ms-asf",
    .p.extensions   = "asf,wmv,wma",
    .priv_data_size = sizeof(ASFContext),
    .p.audio_codec  = AV_CODEC_ID_WMAV2,
    .p.video_codec  = AV_CODEC_ID_MSMPEG4V3,
    .write_header   = asf_write_stream_header,
    .write_packet   = asf_write_packet,
    .write_trailer  = asf_write_trailer,
    .p.flags        = AVFMT_GLOBALHEADER,
    .p.codec_tag    = asf_codec_tags,
    .p.priv_class   = &asf_muxer_class,
    .deinit         = asf_deinit,
};
#endif /* CONFIG_ASF_STREAM_MUXER */

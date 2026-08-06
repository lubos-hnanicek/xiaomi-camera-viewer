/*
 * xmbridge_types.h - shared type and constant definitions for the bridge ABI.
 *
 * Split out from xmbridge.h so the Go side can include it from its cgo preamble
 * without also pulling in the declarations of the functions it exports, which
 * cgo would reject as duplicates.
 */

#ifndef XMBRIDGE_TYPES_H
#define XMBRIDGE_TYPES_H

#include <stdint.h>

/* --- Status codes ------------------------------------------------------- */

#define XMB_OK                    0
#define XMB_ERR_GENERIC          (-1)
#define XMB_ERR_BUFFER_TOO_SMALL (-2)
#define XMB_ERR_INVALID_ARG      (-3)
#define XMB_ERR_INVALID_HANDLE   (-4)
#define XMB_ERR_EOF             (-20)
#define XMB_ERR_TIMEOUT         (-21)

/* --- Media descriptors -------------------------------------------------- */

#define XMB_KIND_VIDEO 1
#define XMB_KIND_AUDIO 2

#define XMB_CODEC_H264 1
#define XMB_CODEC_H265 2
#define XMB_CODEC_PCMA 3
#define XMB_CODEC_OPUS 4
#define XMB_CODEC_PCM  5
#define XMB_CODEC_PCMU 6

/*
 * Metadata for one access unit. Video payloads are Annex-B byte streams, which
 * is what the camera sends and what libavcodec's parser wants, so no rewriting
 * happens anywhere along the path.
 */
typedef struct XmbFrame {
    int32_t  kind;        /* XMB_KIND_*                                        */
    int32_t  codec;       /* XMB_CODEC_*                                       */
    int32_t  keyframe;    /* 1 if this access unit starts a GOP (video only)   */
    int32_t  sample_rate; /* audio only, in Hz; the camera sends mono          */
    int64_t  pts_ms;      /* presentation timestamp in milliseconds            */
    uint32_t sequence;    /* camera-assigned sequence number                   */
    uint32_t size;        /* bytes written, or bytes required if cap too small */
} XmbFrame;

#endif /* XMBRIDGE_TYPES_H */

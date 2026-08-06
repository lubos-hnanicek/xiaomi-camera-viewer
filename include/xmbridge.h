/*
 * xmbridge.h - C ABI exposed by the Go protocol bridge (xmbridge.dll).
 *
 * The bridge owns everything that talks to Xiaomi: cloud account login, device
 * discovery, MIoT property/action RPC, and the MISS/CS2 peer-to-peer media
 * session with the camera. The application above it only decodes and draws.
 *
 * The surface is deliberately tiny. The whole control plane goes through one
 * JSON-in/JSON-out entry point (xmb_call), so new features need no new exports
 * and the ABI does not churn. Only the media path gets dedicated functions,
 * because it is hot enough that JSON per frame would be silly.
 *
 * Buffer convention, borrowed from snprintf: functions taking (out, cap) return
 * the number of bytes the response needs. If that exceeds cap nothing was
 * written and the caller should retry with a larger buffer. Negative values are
 * XMB_ERR_* codes.
 *
 * Strings are UTF-8 and never NUL-terminated by the bridge; use the returned
 * length. All functions are safe to call from any thread, and calls on distinct
 * stream handles proceed concurrently.
 */

#ifndef XMBRIDGE_H
#define XMBRIDGE_H

#include <stdint.h>

#include "xmbridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Library ------------------------------------------------------------ */

/* Returns a NUL-terminated version string owned by the bridge. */
const char* xmb_version(void);

/*
 * Control plane. `method` selects the operation and `request` is its JSON
 * argument object. The response is always a JSON object, either
 *   {"ok":true, ...}  or  {"ok":false,"error":"..."}
 * so transport failures and protocol failures are reported the same way.
 *
 * Methods:
 *   login.begin    {username, password}
 *                    -> {ok, status:"success"|"captcha"|"verify", ...}
 *                       "captcha" carries captcha_png (base64) and the caller
 *                       must follow up with login.captcha.
 *                       "verify" carries verify_phone / verify_email and the
 *                       caller must follow up with login.verify.
 *                       "success" carries account:{user_id, token}.
 *   login.captcha  {code}   -> same shape as login.begin
 *   login.verify   {ticket} -> same shape as login.begin
 *   login.token    {user_id, token}
 *                    -> {ok} after restoring a saved session
 *   account.forget {user_id}
 *   device.list    {user_id, region}
 *                    -> {ok, devices:[{did,name,model,ip,mac}]}
 *   miot.get       {user_id, region, did, props:[{siid,piid}]}
 *                    -> {ok, result:[{siid,piid,value,code}]}
 *   miot.set       {user_id, region, did, props:[{siid,piid,value}]}
 *   miot.action    {user_id, region, did, siid, aiid, in:[...]}
 */
int xmb_call(const char* method, const char* request, char* out, int cap);

/* --- Media plane -------------------------------------------------------- */

/*
 * Opens a live session with a camera and starts the media flow.
 *
 * `request` is a JSON object:
 *   {user_id, region, did, model, ip, channel, quality, audio}
 * where channel is "" or "0" for the primary lens, quality is one of
 * "auto"/"sd"/"hd" or "0".."5", and audio is 0 or 1.
 *
 * On success returns an opaque non-NULL handle and writes {"ok":true, ...}
 * describing the negotiated session. On failure returns NULL and writes
 * {"ok":false,"error":"..."}.
 */
void* xmb_stream_open(const char* request, char* out, int cap);

/*
 * Blocks until the next access unit is available, then copies it into `buf`.
 *
 * Returns the number of bytes written, or:
 *   XMB_ERR_BUFFER_TOO_SMALL - frame dropped, meta->size holds the size needed
 *   XMB_ERR_EOF              - session ended, no further frames will arrive
 *   XMB_ERR_INVALID_HANDLE   - handle already closed
 *
 * A pull model rather than a callback keeps Go pointers from escaping into C,
 * which cgo forbids, and lets the caller own its own pacing.
 */
int xmb_stream_read(void* handle, unsigned char* buf, int cap, XmbFrame* meta);

/*
 * In-band command on an open session, for things that ride the MISS control
 * channel rather than the cloud.
 *
 * Methods (JSON {"method":..., ...}):
 *   ptz.step  {direction:"up"|"down"|"left"|"right"}
 *   ptz.raw   {body:"<motor payload>"}  diagnostic, for an unknown model
 *   stats     {} -> {ok, frames, bytes, dropped, replies, last_reply,
 *                    audio_asked}
 *
 * The camera moves a fixed step per ptz.step and stops by itself, so holding a
 * direction down means repeating the call and there is no stop to send.
 */
int xmb_stream_command(void* handle, const char* request, char* out, int cap);

/* Tears down the session. Unblocks any in-flight xmb_stream_read with EOF. */
void xmb_stream_close(void* handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XMBRIDGE_H */

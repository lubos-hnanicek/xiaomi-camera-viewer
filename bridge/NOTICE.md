# Third-party code vendored into the bridge

The Xiaomi protocol implementation under `internal/` is derived from
[go2rtc](https://github.com/AlexxIT/go2rtc) by Alexey Khit, used under the MIT
license. A copy of that license is in `internal/LICENSE.go2rtc`.

Upstream revision: `v1.9.14` (commit `b5948cfb25404cc5cb37b166ecaa2dca20b11d4b`).

## What was taken

| Bridge file | Upstream origin |
| --- | --- |
| `internal/crypto/crypto.go` | `pkg/xiaomi/crypto/crypto.go` |
| `internal/cloud/cloud.go` | `pkg/xiaomi/cloud.go` |
| `internal/cloud/api.go` | `internal/xiaomi/xiaomi.go` (cloud request helpers) |
| `internal/miss/client.go` | `pkg/xiaomi/miss/client.go` |
| `internal/miss/cs2/conn.go` | `pkg/xiaomi/miss/cs2/conn.go` |

## What was changed

- `core.RandString` was inlined so the bridge does not pull in `pkg/core`.
- The TUTK and legacy-protocol paths were dropped. They serve pre-2020 cameras;
  the CW400 and CW500 both negotiate the CS2 transport.
- The Dafang/Xiaofang special cases were dropped with TUTK, since their video
  commands are encoded as TUTK ICAM payloads.
- Pan and tilt were added to the MISS client. Upstream declares the command IDs
  `0x112`/`0x113` but never wires them up, because go2rtc has no concept of PTZ
  (see [go2rtc#2162](https://github.com/AlexxIT/go2rtc/issues/2162)), so there
  was no payload to copy and none is published. The accepted payload is
  `{"operation":N}` with 1 and 2 panning and 3 and 4 tilting; each command moves
  a fixed step and stops by itself, so there is no stop command. The
  `{"direction":"left","speed":5}` shape that third-party forks use is silently
  ignored by both the CW400 and the CW500. Found by sending candidates and
  watching the position readback, which `scripts/probe-ptz.ps1` still does.
- The command channel is now drained for as long as a session lives. Upstream
  reads it only during authentication, which is safe there because it never
  sends anything else that the camera answers. Every motor command draws a
  reply, and the channel holds ten messages before the transport treats it as a
  fatal error, so without this the stream dies after about ten button presses.
- `ReadCommand` decodes the command id big-endian, matching `marshalCmd` on the
  way out. Upstream reads it little-endian, which turns the `0x1001` envelope
  every reply arrives in into `0x01100000`. Nothing upstream notices, because the
  only reply it reads is the authentication result and that is recognised by
  searching the body rather than by its command id.
- go2rtc's `core.Producer` pipeline was replaced with a frame queue, since this
  bridge hands access units to a local decoder rather than restreaming RTSP.
- The CS2 LAN search is additionally sent to the directed broadcast of the local
  subnet. Upstream only unicasts it to the camera, which the CW400 family
  ignores completely: those cameras answer the broadcast and nothing else, so
  under upstream they time out with no reply at all. Replies are still accepted
  only from the requested camera.
- A camera the cloud has no address for is located by broadcasting the same
  discovery packet and matching the MAC from the device list against the
  replies. Upstream has nowhere to put this, since a go2rtc stream URL always
  carries an address.
- Quality profile 3 was mapped for `isa.camera.hlc8`, `isa.camera.hlc8a`,
  `isa.camera.500dh` and `mxiang.camera.mod11`. Measured against the CW400 and
  CW500: profile 2 makes the CW400 accept the start command and then send
  nothing, and gives the CW500 only its 640x360 substream, while profile 3 gives
  both 2560x1440. The CW400 half matches
  [go2rtc#2074](https://github.com/AlexxIT/go2rtc/issues/2074) and
  [go2rtc#2313](https://github.com/AlexxIT/go2rtc/pull/2313).
- The handshake reports what it saw when it times out, and compares peer
  addresses with `net.IP.Equal` rather than comparing the raw bytes, which
  differ between the 4-byte and 16-byte forms of the same address.

## When updating go2rtc

Diff the upstream files above against their vendored copies. The parts most
likely to move are the cloud login flow in `cloud.go`, which tracks changes to
Xiaomi's account service, and the model quirk tables in `client.go`.

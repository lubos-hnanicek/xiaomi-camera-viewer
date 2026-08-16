# Third-party notices

Xiaomi Camera Viewer includes and links against the following components.

## go2rtc

The Xiaomi MISS/CS2 protocol implementation under `bridge/internal/` is derived
from [go2rtc](https://github.com/AlexxIT/go2rtc) by Alexey Khit, at revision
`v1.9.14`. Used under the MIT license, a copy of which is at
`bridge/internal/LICENSE.go2rtc`. `bridge/NOTICE.md` records precisely which
files were taken and how they were modified.

## Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, version
`v1.92.9b-docking`, is compiled into the executable. MIT license.

That is the `docking` branch's tag of the same release rather than the one on
`master`. Multi-viewport support exists only on that branch, and it is what lets
the help and log windows be dragged out of the application and become windows of
their own. The docking feature itself is left switched off.

## FFmpeg

Video decoding uses [FFmpeg](https://ffmpeg.org/) 8.1, specifically
`libavcodec`, `libavformat`, `libavutil` and `libswresample`. The shipped
binaries are the LGPL-2.1-or-later configuration from the
[BtbN builds](https://github.com/BtbN/FFmpeg-Builds), linked **dynamically** as
separate DLLs. A copy of the license is included as `LICENSE.LGPL-2.1.txt`.

The exact artefact is `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from
<https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip>,
which is also what `scripts/fetch-deps.ps1` downloads. That release records the
build configuration, and FFmpeg's own source is at
<https://ffmpeg.org/download.html>.

Because the linkage is dynamic and the libraries are shipped unmodified as
replaceable DLLs, the LGPL's relinking requirement is satisfied: you may swap in
your own build of the same FFmpeg version.

No GPL-licensed FFmpeg components are included.

## nlohmann/json

[nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann, version
`v3.12.0`, header-only. MIT license.

## Go standard library and golang.org/x/crypto

The bridge is built with the [Go](https://go.dev/) toolchain and uses
`golang.org/x/crypto` for Curve25519 and ChaCha20. Both are distributed under
the BSD 3-Clause license.

## Windows components

Direct3D 11, DXGI, the Windows Imaging Component and the Data Protection API are
part of Windows and are used through their public APIs.

## Trademarks

Xiaomi, Mi Home and related names are trademarks of Xiaomi Corporation. This
project is an independent work and is not affiliated with, endorsed by, or
supported by Xiaomi.

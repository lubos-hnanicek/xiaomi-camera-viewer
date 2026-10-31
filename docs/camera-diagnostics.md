# Camera diagnostics for remote support

This workflow is for a camera owner helping implement support for a model the
maintainers do not own. It collects evidence rather than trying undocumented
motor commands, changing settings, formatting storage or updating firmware.

## Collect a baseline

1. Download an application package that includes the collector. Run
   XiaomiViewer, sign in, and select the same account region used in Mi Home.
   The unsupported camera need not be added to the grid.
2. Check that live view works in Mi Home. Note the retail model, firmware
   version, lens count and which features work. Close Mi Home live view and
   connect the PC to the camera's subnet.
3. Open **Help -> Camera diagnostics** in XiaomiViewer. It refreshes the account
   camera list independently of configured grid tiles or model support. Select
   the camera and the optional probes, then choose **Collect diagnostics**.
   Stop recordings for this camera and close SD playback first. Its running live
   tiles pause while collecting and resume afterwards; other cameras are left
   running. **Stop all** overrides automatic resumption.
4. Progress appears in the dialog. **Cancel collection** waits for the current
   network request before stopping and preserving partial results. Closing the
   viewer requests cancellation too. **Open bundle folder** and **Open owner
   report** become available when the collector finishes. Reports from the app
   are stored under `%APPDATA%\XiaomiViewer\Diagnostics`.
5. Review every file, fill in `owner-report.txt`, then re-zip the folder to
   include your edits. Send the reviewed ZIP through the support channel agreed
   with the maintainer. A failed connection still produces useful partial data.

## Standalone collection

Close XiaomiViewer as well as Mi Home live view before standalone collection.
Open PowerShell matching the package architecture: x64 for x64; native ARM64
PowerShell for ARM64. No administrator rights, compiler, packet driver or
additional account credentials are needed. From the unpacked folder, run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\collect-camera-diagnostics.ps1
```

With PowerShell 7, use `pwsh` instead of `powershell.exe`. The policy override
applies to this process, not the machine. Choose the camera number when prompted.
Standalone reports appear on the Desktop. Both workflows decrypt the saved
session only in memory, using the Windows user that signed in; credentials are
never passed on the process command line. The app uses the Windows PowerShell
host in its system directory; a missing/incompatible host is reported as a
collection failure, and the standalone architecture-matched command remains
available.

Allow several minutes if public specification/property requests time out. The
live observation itself defaults to eight seconds; SD inspection can take an
additional 45 seconds per storage channel. The collector leaves the saved config
and existing log unchanged; the app saves its current configuration before
launching to make the latest signed-in session available. The collector requests microphone packets to learn
audio behaviour, but never decodes, plays or saves their payloads. Do not run
it during a private conversation.

## What the archive contains

- `summary.json`: bridge version/hash, OS/process architecture, account region,
  model, timings, failures and explicit capture limitations.
- `device.json`: the selected cloud device entry with identifiers removed.
- `miot-spec.json`, when available: the exact public model specification,
  including property IDs, formats, enumerations, actions and access modes.
- `communication.json`: timestamped requests/results for cloud transport
  negotiation, MIoT reads, live-session opening, device-information command
  `0x110`, drained MISS replies, RDT tap observations, raw media headers and
  stream statistics. Failed probes remain alongside successful ones.
- `owner-report.txt`: observations that cannot be inferred from wire data.
- `viewer-log.json` only when requested: at most the last 2,000 redacted lines.

The transport report identifies CS2/TUTK/Agora/MTP (or an unknown vendor code)
without peer IDs, encryption keys or signatures. Media header samples are
bounded to 512 entries and retained even for unknown codecs. They include the
camera's raw header fields, not video/audio access units. The raw packet parser
must still recognise the framing before a header can be captured.
The collector does not consume/decode media, so the stream's bounded viewer
queue can report dropped frames. That counter is not network packet loss.

MIoT reads use the model's specification, not a CW400/CW500 property table.
Only readable properties are requested. Arbitrary string properties are skipped
to avoid collecting names, locations, presets and network credentials; firmware,
hardware, manufacturer and model strings are allowed. No actions or writes are
performed. Known identity/network-credential properties are excluded regardless
of format, including numeric serial numbers. Models without a public MIoT
specification still get the remaining probes.

## Optional probes

Use a device ID already known from your account instead of interactive selection:

```powershell
.\collect-camera-diagnostics.ps1 -Did '<device-id>' -IncludeStorage -IncludeFirmwareInfo
```

`-IncludeStorage` requests catalogue summaries on storage channels **0 and 10**,
not a guessed second-lens channel 1. Those are the known CW500 storage channels,
not a claim that every two-lens camera uses them. No MP4 is requested and playback
is not started. The RDT observations can reveal recording times/activity.

`-IncludeFirmwareInfo` queries the existing read-only version/upgrade-status
endpoints and retains results, including firmware URLs when provided. It never
downloads an image or starts an upgrade. Endpoints may be absent for your model.

`-IncludeAppLog` includes the viewer's log, which can describe other cameras.
Review it especially carefully. The normal collector supplies its own event log
and does not need this switch.

For a silent stream, a maintainer may ask for another run with a specific
profile/lens/transport. Run one variant at a time and let the camera recover
between runs; do not brute-force commands or repeatedly reconnect a battery
camera. For example:

```powershell
.\collect-camera-diagnostics.ps1 -Did '<device-id>' -Quality 3 -Channel 1 -Transport tcp
```

Quality accepts `auto`, `sd`, `hd`, or `0` through `5`; channel accepts the default,
`0`, `1` or `2`. Empty transport lets the camera choose. A forced TCP handshake
can fail on a camera that accepts the default negotiation, so always send the
baseline too. The number of a live lens is not necessarily its storage channel.

Repository users can pass `-DllPath` and `-OutputDirectory`. The collector uses
the packaged DLL beside itself, or a local RelWithDebInfo build as a fallback.
`-SelfTest` exercises redaction, arrays, failure retention and ZIP generation
without an account, DLL, camera or network.

## Privacy and limits

The collector never copies the config, saves session keys, downloads footage,
records audio, uploads data or captures the whole network. Known account/device
identifiers and sensitive JSON fields are redacted before being written. This
is not a guarantee of anonymity: an undocumented binary field, a signed URL,
free-form text or another device's identifier in the optional log can escape
automatic redaction. Inspect every file before sharing, and use a private
channel for anything that still needs to contain personal data.

This is **not a PCAP** and does not observe the Mi Home phone app. Command replies
and RDT observations are bounded samples, not a complete lossless wire dump.
Dropped queues and undecodable framing can hide messages. A missing reply can
mean an incomplete request, a missing card, a sleeping camera, a firewall block
or an unsupported protocol; silence alone cannot distinguish these cases.

## When the baseline is not enough

An unsupported transport cannot be reverse-engineered from a CS2 viewer session
that never opened. Send its vendor result, public MIoT specification and owner
report first. Then agree a focused next experiment with the maintainer:

- Provide the official/public firmware image or download URL for the exact
  model/revision, with its version and SHA-256. Use the optional URL lookup first.
  Do not flash, root, factory-reset or exploit the camera to obtain evidence.
  Firmware handlers were what revealed required SD-playback fields that camera
  silence could not reveal.
- On Android, provide the exact Mi Home/plugin version and, when legitimately
  obtainable, that camera's plugin package for private analysis. Share only
  material you have the right to provide, without app account databases or
  cached credentials. Do not distribute copyrighted packages publicly.
- If needed, capture phone-to-camera traffic on an owner-controlled router/AP,
  filtered to that phone and camera, during one clearly timed operation. On
  Android a user-approved local-VPN capture app may be an alternative, but the
  VPN can change the camera's LAN discovery behaviour. Verify Mi Home still
  works and record the capture method. A PC on a switched LAN normally cannot
  see the phone's unicast packets simply by running Wireshark.
- Separate captures for opening live view, changing quality, selecting a lens,
  microphone listening, one permitted PTZ movement and one SD clip are more
  useful than a long mixed session. List each operation with UTC timestamps.
  Avoid unrelated household traffic and use a non-private scene. Only perform
  movements/actions the owner explicitly agrees to.

Phone captures may contain sensitive identifiers and media; review and share
privately. Encrypted traffic can establish endpoints/framing/timing but not the
decrypted command fields. Do not disable TLS, install interception certificates
or send account/session keys as a workaround. Firmware/plugin analysis plus a
small owner-run confirmation probe is often the useful next step.

## Maintainer triage

Start with `summary.json`, the vendor result and the owner report. For CS2,
distinguish cloud negotiation, LAN discovery, MISS authentication and media
decoding failures. Compare header codec IDs, flags, lens tags and sequences with
existing MISS/stream tests before adding a model special case. Use the model's
MIoT IDs and enum values rather than copying a neighbouring camera's table.

For SD playback, inspect RDT framing and storage channel evidence. Playback
needs a clip's exact catalogue start; an arbitrary rounded timestamp is not a
valid negative test. Agree one narrowly scoped follow-up probe and return a
test build to the owner. Their confirmation of video, audio, control and storage
behaviour is still required before declaring the model supported.
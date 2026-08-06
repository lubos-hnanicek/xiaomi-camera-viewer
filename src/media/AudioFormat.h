#pragma once

#include <cstdint>
#include <vector>

namespace xv::audio {

// What the camera is sending, for a log line or a tooltip.
[[nodiscard]] const char* codecName(int codec);

// The libavcodec id for an XMB_CODEC_* audio constant, returned as an int so
// this header stays free of the FFmpeg headers; callers cast it to AVCodecID.
// Zero, which is AV_CODEC_ID_NONE, for anything that is not audio.
[[nodiscard]] int avCodecId(int codec);

// The rate samples actually come out at, which is not always the rate the
// camera declares: Opus is coded at 48 kHz whatever it was fed, and the rate in
// the packet flags describes the microphone rather than the bitstream.
[[nodiscard]] int outputSampleRate(int codec, int declaredRate);

// The OpusHead identification header, as Ogg, Matroska and libavcodec all
// expect Opus to be described.
//
// The camera sends bare Opus packets with no header of any kind, so this is
// synthesized rather than forwarded. Everything in it except the channel count
// is either fixed by the format or unknowable here: the pre-skip is zero
// because a live stream has no encoder warm-up to trim, and `inputSampleRate`
// is documentation of what the microphone ran at, not something a decoder acts
// on.
[[nodiscard]] std::vector<uint8_t> opusHead(int channels, int inputSampleRate);

} // namespace xv::audio

#include "media/AudioFormat.h"

#include "xmbridge.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace xv::audio {

const char* codecName(int codec) {
    switch (codec) {
    case XMB_CODEC_PCMA: return "G.711 A-law";
    case XMB_CODEC_PCMU: return "G.711 mu-law";
    case XMB_CODEC_PCM: return "PCM";
    case XMB_CODEC_OPUS: return "Opus";
    default: return "unknown";
    }
}

int avCodecId(int codec) {
    switch (codec) {
    case XMB_CODEC_PCMA: return AV_CODEC_ID_PCM_ALAW;
    case XMB_CODEC_PCMU: return AV_CODEC_ID_PCM_MULAW;
    // Little-endian, because the cameras that send raw PCM are the ones that
    // send it the way a Windows WAV file would.
    case XMB_CODEC_PCM: return AV_CODEC_ID_PCM_S16LE;
    case XMB_CODEC_OPUS: return AV_CODEC_ID_OPUS;
    default: return AV_CODEC_ID_NONE;
    }
}

int outputSampleRate(int codec, int declaredRate) {
    if (codec == XMB_CODEC_OPUS) {
        return 48000;
    }
    // A camera that sends no rate at all is taken to mean the narrowband one,
    // which is what G.711 is defined at.
    return declaredRate > 0 ? declaredRate : 8000;
}

std::vector<uint8_t> opusHead(int channels, int inputSampleRate) {
    std::vector<uint8_t> head{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    head.push_back(1); // version
    head.push_back(static_cast<uint8_t>(channels));

    const auto little16 = [&head](uint16_t value) {
        head.push_back(static_cast<uint8_t>(value & 0xFF));
        head.push_back(static_cast<uint8_t>(value >> 8));
    };

    little16(0); // pre-skip
    little16(static_cast<uint16_t>(inputSampleRate & 0xFFFF));
    little16(static_cast<uint16_t>(static_cast<uint32_t>(inputSampleRate) >> 16));
    little16(0); // output gain
    head.push_back(0); // channel mapping family: mono or plain stereo

    return head;
}

} // namespace xv::audio

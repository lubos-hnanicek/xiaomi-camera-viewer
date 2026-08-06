#include "media/Recorder.h"

#include <algorithm>
#include <utility>

#include "app/Log.h"
#include "media/AudioFormat.h"
#include "xmbridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

namespace xv {
namespace {

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

AVCodecID codecIdFor(int codec) {
    return codec == XMB_CODEC_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

// Copies `bytes` into freshly allocated extradata, which FFmpeg frees along
// with the stream. Nothing is written when there is nothing to write, because a
// zero-length extradata block is not the same as none.
bool setExtradata(AVCodecParameters* par, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return true;
    }

    par->extradata =
        static_cast<uint8_t*>(av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (par->extradata == nullptr) {
        return false;
    }

    std::copy(bytes.begin(), bytes.end(), par->extradata);
    par->extradata_size = static_cast<int>(bytes.size());
    return true;
}

// FFmpeg takes paths as UTF-8 and widens them itself on Windows. The native
// narrow encoding that path::string() would give is the wrong one, and a camera
// named in anything but ASCII would land in the wrong place or nowhere.
std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// Collects the parameter set NAL units of an Annex-B access unit: VPS, SPS and
// PPS for H.265, SPS and PPS for H.264.
//
// Parameter sets are what a player needs before it can make sense of a single
// frame, and these cameras send them in band with every keyframe rather than out
// of band, so a recording has to pick them back out.
std::vector<uint8_t> parameterSets(const uint8_t* data, size_t size, int codec) {
    std::vector<uint8_t> out;

    // Offsets of every start code in the access unit, so each unit's extent is
    // known without guessing.
    std::vector<std::pair<size_t, size_t>> units; // payload start, payload end
    size_t previous = 0;
    bool have = false;

    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }

        size_t payload = 0;
        if (data[i + 2] == 1) {
            payload = i + 3;
        } else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
            payload = i + 4;
        } else {
            continue;
        }

        if (have) {
            units.emplace_back(previous, i);
        }
        previous = payload;
        have = true;
        i = payload - 1;
    }
    if (have) {
        units.emplace_back(previous, size);
    }

    for (const auto& [begin, end] : units) {
        if (begin >= end) {
            continue;
        }

        bool wanted = false;
        if (codec == XMB_CODEC_H265) {
            switch ((data[begin] >> 1) & 0x3F) {
            case 32: // VPS
            case 33: // SPS
            case 34: // PPS
                wanted = true;
                break;
            default:
                break;
            }
        } else {
            switch (data[begin] & 0x1F) {
            case 7: // SPS
            case 8: // PPS
                wanted = true;
                break;
            default:
                break;
            }
        }

        if (!wanted) {
            continue;
        }

        // Written back as a start-code stream, which is the form FFmpeg's
        // muxers expect extradata in when it comes from an Annex-B source.
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), data + begin, data + end);
    }

    return out;
}

} // namespace

Recorder::~Recorder() {
    close();
}

bool Recorder::open(const std::filesystem::path& path, int codec, int width, int height,
                    const uint8_t* keyframe, size_t size, const AudioTrack& audio,
                    std::string& error) {
    close();

    if (width <= 0 || height <= 0) {
        error = "the picture size is not known yet";
        return false;
    }

    const std::vector<uint8_t> sets = parameterSets(keyframe, size, codec);
    if (sets.empty()) {
        error = "this keyframe carries no parameter sets";
        return false;
    }

    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);

    const std::string target = utf8Of(path);

    if (const int rc = avformat_alloc_output_context2(&format_, nullptr, "matroska", target.c_str());
        rc < 0) {
        error = "could not set up the Matroska muxer: " + avError(rc);
        format_ = nullptr;
        return false;
    }

    stream_ = avformat_new_stream(format_, nullptr);
    if (stream_ == nullptr) {
        error = "could not add a video track";
        close();
        return false;
    }

    // Milliseconds, because that is the resolution the camera timestamps in.
    // Matroska's own scale is nanoseconds, so nothing is lost by saying so.
    stream_->time_base = AVRational{1, 1000};

    AVCodecParameters* par = stream_->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = codecIdFor(codec);
    par->width = width;
    par->height = height;

    if (!setExtradata(par, sets)) {
        error = "out of memory";
        close();
        return false;
    }

    if (audio.valid() && !addAudioTrack(audio, error)) {
        // A file with a picture and no sound is worth more than no file, so
        // this is reported and carried on from rather than failed.
        XV_WARN("recording without audio: {}", error);
        error.clear();
    }

    if (const int rc = avio_open(&format_->pb, target.c_str(), AVIO_FLAG_WRITE); rc < 0) {
        error = "could not create the file: " + avError(rc);
        close();
        return false;
    }

    if (const int rc = avformat_write_header(format_, nullptr); rc < 0) {
        error = "the muxer rejected the stream: " + avError(rc);
        close();
        return false;
    }

    packet_ = av_packet_alloc();
    if (packet_ == nullptr) {
        error = "out of memory";
        close();
        return false;
    }

    path_ = path;
    bytes_ = 0;
    firstPts_ = -1;
    lastPts_ = -1;
    lastAudioPts_ = -1;

    XV_INFO("recording to {} ({}x{} {}, {})", target, width, height,
            codec == XMB_CODEC_H265 ? "H.265" : "H.264",
            audio_ != nullptr ? audio::codecName(audio.codec) : "no audio");
    return true;
}

bool Recorder::addAudioTrack(const AudioTrack& audio, std::string& error) {
    const auto id = static_cast<AVCodecID>(audio::avCodecId(audio.codec));
    if (id == AV_CODEC_ID_NONE) {
        error = std::format("the camera's audio codec ({}) cannot be stored in Matroska",
                            audio::codecName(audio.codec));
        return false;
    }

    AVStream* stream = avformat_new_stream(format_, nullptr);
    if (stream == nullptr) {
        error = "could not add an audio track";
        return false;
    }

    stream->time_base = AVRational{1, 1000};

    AVCodecParameters* par = stream->codecpar;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->codec_id = id;
    // Opus is always coded at 48 kHz, whatever the microphone in front of it
    // ran at, and a track that claims otherwise plays at the wrong speed.
    par->sample_rate = audio::outputSampleRate(audio.codec, audio.sampleRate);
    av_channel_layout_default(&par->ch_layout, audio.channels);

    switch (id) {
    case AV_CODEC_ID_PCM_ALAW:
    case AV_CODEC_ID_PCM_MULAW:
        // Matroska carries G.711 inside a WAVEFORMATEX block, which the muxer
        // builds itself but cannot do without the sample width.
        par->bits_per_coded_sample = 8;
        break;
    case AV_CODEC_ID_PCM_S16LE:
        par->bits_per_coded_sample = 16;
        break;
    case AV_CODEC_ID_OPUS:
        // The camera sends bare Opus packets, and Matroska refuses an Opus
        // track that cannot say how many channels it has.
        if (!setExtradata(par, audio::opusHead(audio.channels, audio.sampleRate))) {
            error = "out of memory";
            return false;
        }
        break;
    default:
        break;
    }

    audio_ = stream;
    return true;
}

bool Recorder::writeVideo(const uint8_t* data, size_t size, int64_t ptsMs, bool keyframe,
                          std::string& error) {
    if (format_ == nullptr || packet_ == nullptr) {
        return false;
    }

    // The first video frame is what the whole file is timed from, audio
    // included, so the two tracks keep the offset the camera sent them with.
    if (firstPts_ < 0) {
        firstPts_ = ptsMs;
    }

    int64_t pts = ptsMs - firstPts_;

    // Matroska will not take a timestamp that goes backwards, and a camera whose
    // clock steps or wraps would otherwise end the recording. Nudging keeps the
    // file going; the frame lands a millisecond after its neighbour instead of
    // being lost.
    if (pts <= lastPts_) {
        pts = lastPts_ + 1;
    }

    if (!writePacket(stream_, data, size, pts, keyframe, error)) {
        return false;
    }

    lastPts_ = pts;
    return true;
}

bool Recorder::writeAudio(const uint8_t* data, size_t size, int64_t ptsMs, std::string& error) {
    if (format_ == nullptr || packet_ == nullptr || audio_ == nullptr) {
        return true; // a recording without an audio track, which is not a failure
    }

    // Audio that arrived before the opening keyframe belongs to a moment the
    // file does not cover, and would have to be written at a negative time.
    if (firstPts_ < 0) {
        return true;
    }

    int64_t pts = ptsMs - firstPts_;
    if (pts < 0) {
        return true;
    }
    if (pts <= lastAudioPts_) {
        pts = lastAudioPts_ + 1;
    }

    // Every audio packet is independently decodable, so all of them are
    // keyframes as far as the container is concerned.
    if (!writePacket(audio_, data, size, pts, true, error)) {
        return false;
    }

    lastAudioPts_ = pts;
    return true;
}

bool Recorder::writePacket(AVStream* stream, const uint8_t* data, size_t size, int64_t pts,
                           bool keyframe, std::string& error) {
    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    packet_->stream_index = stream->index;
    packet_->pts = pts;
    packet_->dts = pts; // no reordering: these cameras send no B-frames
    packet_->flags = keyframe ? AV_PKT_FLAG_KEY : 0;

    // Interleaved, not direct: Matroska clusters have to come out in timestamp
    // order across both tracks, and only the muxer knows how far ahead one of
    // them has run. It takes a copy of the packet, which is what lets the data
    // keep pointing at the caller's buffer.
    const int rc = av_interleaved_write_frame(format_, packet_);

    // The muxer unreferences the packet on the way out, but it may not have got
    // that far, and the buffer behind it is the caller's to reuse.
    packet_->data = nullptr;
    packet_->size = 0;

    if (rc < 0) {
        error = "could not write to the file: " + avError(rc);
        return false;
    }

    bytes_ += size;
    return true;
}

void Recorder::close() {
    if (format_ != nullptr && format_->pb != nullptr) {
        // Only a file that got its header can get a trailer, and the trailer is
        // what carries the index and the duration. Writing it also flushes
        // whatever the interleaving queue is still holding on to.
        if (stream_ != nullptr && lastPts_ >= 0) {
            if (const int rc = av_write_trailer(format_); rc < 0) {
                XV_WARN("recording {} did not finish cleanly: {}", utf8Of(path_), avError(rc));
            }
        }
        avio_closep(&format_->pb);
    }

    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (format_ != nullptr) {
        avformat_free_context(format_);
        format_ = nullptr;
    }

    stream_ = nullptr;
    audio_ = nullptr;
    firstPts_ = -1;
    lastPts_ = -1;
    lastAudioPts_ = -1;
}

} // namespace xv

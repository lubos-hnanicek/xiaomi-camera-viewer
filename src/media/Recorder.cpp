#include "media/Recorder.h"

#include <algorithm>
#include <utility>

#include "app/Log.h"
#include "xmbridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
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
                    const uint8_t* keyframe, size_t size, std::string& error) {
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

    par->extradata = static_cast<uint8_t*>(
        av_mallocz(sets.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (par->extradata == nullptr) {
        error = "out of memory";
        close();
        return false;
    }
    std::copy(sets.begin(), sets.end(), par->extradata);
    par->extradata_size = static_cast<int>(sets.size());

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

    XV_INFO("recording to {} ({}x{} {})", target, width, height,
            codec == XMB_CODEC_H265 ? "H.265" : "H.264");
    return true;
}

bool Recorder::write(const uint8_t* data, size_t size, int64_t ptsMs, bool keyframe,
                     std::string& error) {
    if (format_ == nullptr || packet_ == nullptr) {
        return false;
    }

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

    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    packet_->stream_index = stream_->index;
    packet_->pts = pts;
    packet_->dts = pts; // no reordering: these cameras send no B-frames
    packet_->flags = keyframe ? AV_PKT_FLAG_KEY : 0;

    const int rc = av_write_frame(format_, packet_);

    // Written or not, the packet must not keep pointing at a caller's buffer
    // that is about to be reused for the next access unit.
    packet_->data = nullptr;
    packet_->size = 0;

    if (rc < 0) {
        error = "could not write to the file: " + avError(rc);
        return false;
    }

    bytes_ += size;
    lastPts_ = pts;
    return true;
}

void Recorder::close() {
    if (format_ != nullptr && format_->pb != nullptr) {
        // Only a file that got its header can get a trailer, and the trailer is
        // what carries the index and the duration.
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
    firstPts_ = -1;
    lastPts_ = -1;
}

} // namespace xv

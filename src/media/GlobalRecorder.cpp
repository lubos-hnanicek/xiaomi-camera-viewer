#include "media/GlobalRecorder.h"

#include <algorithm>
#include <exception>
#include <format>

#include "app/Log.h"
#include "media/AudioFormat.h"

namespace xv {

const char* globalRecordingStateName(GlobalRecorder::State state) {
    switch (state) {
    case GlobalRecorder::State::Idle: return "Idle";
    case GlobalRecorder::State::Preparing: return "Preparing";
    case GlobalRecorder::State::Recording: return "Recording";
    case GlobalRecorder::State::Error: return "Error";
    }
    return "?";
}

GlobalRecorder::~GlobalRecorder() {
    stop();
}

bool GlobalRecorder::start(const std::filesystem::path& directory,
                           std::vector<Participant> participants, std::string& error) {
    stop();

    if (participants.empty()) {
        error = "no live cameras are available to record";
        return false;
    }

    std::unordered_set<std::string> videoIds;
    std::unordered_set<std::string> audioSources;
    for (const Participant& participant : participants) {
        if (participant.videoId.empty() || participant.sourceId.empty()) {
            error = "a recording participant has no stable identity";
            return false;
        }
        if (!videoIds.insert(participant.videoId).second) {
            error = "the same live view was added to the recording twice";
            return false;
        }
        if (participant.audioOwner && !audioSources.insert(participant.sourceId).second) {
            error = "a physical camera was assigned more than one audio track";
            return false;
        }
    }

    startedUtc_ = std::chrono::system_clock::now();
    started_ = std::chrono::steady_clock::now();
    path_ = directory / fileName(startedUtc_);

    videos_.clear();
    sources_.clear();
    videoOrder_.clear();
    for (Participant& participant : participants) {
        videoOrder_.push_back(participant.videoId);

        VideoRuntime runtime;
        runtime.participant = participant;
        runtime.spec.title = participant.videoTitle;
        videos_.emplace(participant.videoId, std::move(runtime));

        SourceRuntime& source = sources_[participant.sourceId];
        if (participant.audioOwner) {
            source.title = participant.audioTitle;
            source.audioOwnerVideoId = participant.videoId;
            source.audioExpected = true;
        }
    }
    latestElapsedMs_ = 0;

    {
        std::scoped_lock lock(membershipMutex_);
        participantIds_ = std::move(videoIds);
        audioOwnerIds_.clear();
        for (const Participant& participant : participants) {
            if (participant.audioOwner) {
                audioOwnerIds_.insert(participant.videoId);
            }
        }
    }

    {
        std::scoped_lock lock(queueMutex_);
        queue_.clear();
        queuedBytes_ = 0;
        stopRequested_ = false;
        pendingError_.clear();
    }
    {
        std::scoped_lock lock(statusMutex_);
        status_ = Status{
            .state = State::Preparing,
            .participants = participants.size(),
            .prepared = 0,
            .path = path_.filename().string(),
        };
    }

    active_.store(true, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
    thread_ = std::thread(&GlobalRecorder::run, this);

    XV_INFO("preparing global recording with {} live view(s)", participants.size());
    return true;
}

void GlobalRecorder::stop() {
    accepting_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(queueMutex_);
        stopRequested_ = true;
    }
    queueSignal_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }

    active_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(membershipMutex_);
        participantIds_.clear();
        audioOwnerIds_.clear();
    }
    {
        std::scoped_lock lock(statusMutex_);
        status_.state = State::Idle;
        status_.participants = 0;
        status_.prepared = 0;
        status_.error.clear();
    }
}

void GlobalRecorder::submitVideo(const std::string& videoId, const uint8_t* data, size_t size,
                                 int codec, int width, int height, int64_t ptsMs, bool keyframe) {
    if (!participates(videoId)) {
        return;
    }

    Event event;
    event.type = EventType::Video;
    event.id = videoId;
    event.data.assign(data, data + size);
    event.codec = codec;
    event.width = width;
    event.height = height;
    event.ptsMs = ptsMs;
    event.elapsedMs = elapsedNow();
    event.keyframe = keyframe;
    enqueue(std::move(event));
}

void GlobalRecorder::submitAudio(const std::string& sourceId, const uint8_t* data, size_t size,
                                 int codec, int sampleRate, int64_t ptsMs) {
    if (!accepting_.load(std::memory_order_acquire)) {
        return;
    }

    Event event;
    event.type = EventType::Audio;
    event.id = sourceId;
    event.data.assign(data, data + size);
    event.codec = codec;
    event.sampleRate = sampleRate;
    event.ptsMs = ptsMs;
    event.elapsedMs = elapsedNow();
    event.keyframe = true;
    enqueue(std::move(event));
}

void GlobalRecorder::declareAudioFormat(const std::string& sourceId, int codec, int sampleRate) {
    if (!accepting_.load(std::memory_order_acquire) || codec == 0 || sampleRate <= 0) {
        return;
    }

    Event event;
    event.type = EventType::AudioFormat;
    event.id = sourceId;
    event.codec = codec;
    event.sampleRate = sampleRate;
    event.elapsedMs = elapsedNow();
    enqueue(std::move(event));
}

void GlobalRecorder::sessionEnded(const std::string& videoId) {
    if (!participates(videoId)) {
        return;
    }

    Event event;
    event.type = EventType::SessionEnded;
    event.id = videoId;
    event.elapsedMs = elapsedNow();
    enqueue(std::move(event));
}

GlobalRecorder::Status GlobalRecorder::status() const {
    std::scoped_lock lock(statusMutex_);
    return status_;
}

bool GlobalRecorder::participates(const std::string& videoId) const {
    if (!active_.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock lock(membershipMutex_);
    return participantIds_.contains(videoId);
}

bool GlobalRecorder::audioOwner(const std::string& videoId) const {
    if (!active_.load(std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock lock(membershipMutex_);
    return audioOwnerIds_.contains(videoId);
}

void GlobalRecorder::enqueue(Event event) {
    if (!accepting_.load(std::memory_order_acquire)) {
        return;
    }

    const size_t bytes = event.data.size();
    {
        std::scoped_lock lock(queueMutex_);
        if (!accepting_.load(std::memory_order_relaxed)) {
            return;
        }
        if (queuedBytes_ + bytes > kMaxQueueBytes) {
            pendingError_ =
                "the global recording queue filled because packets could not be written fast enough";
            accepting_.store(false, std::memory_order_release);
        } else {
            queuedBytes_ += bytes;
            queue_.push_back(std::move(event));
        }
    }
    queueSignal_.notify_one();
}

void GlobalRecorder::run() {
    for (;;) {
        Event event;
        std::string queueError;
        {
            std::unique_lock lock(queueMutex_);
            const auto ready = [this] {
                return !queue_.empty() || stopRequested_ || !pendingError_.empty();
            };
            if (!muxer_.open()) {
                const auto deadline = started_ + std::chrono::milliseconds(kPreparationTimeoutMs);
                if (!queueSignal_.wait_until(lock, deadline, ready)) {
                    lock.unlock();
                    fail(preparationTimeoutError());
                    return;
                }
            } else {
                queueSignal_.wait(lock, ready);
            }

            if (!pendingError_.empty()) {
                queueError = std::move(pendingError_);
                pendingError_.clear();
                queue_.clear();
                queuedBytes_ = 0;
            } else if (queue_.empty()) {
                if (stopRequested_) {
                    break;
                }
                continue;
            } else {
                event = std::move(queue_.front());
                queuedBytes_ -= event.data.size();
                queue_.pop_front();
            }
        }

        if (!queueError.empty()) {
            fail(queueError);
            return;
        }

        std::string error;
        if (!process(std::move(event), error)) {
            fail(error);
            return;
        }
        if (!muxer_.open() && elapsedNow() >= kPreparationTimeoutMs) {
            fail(preparationTimeoutError());
            return;
        }
    }

    muxer_.close();
    publishProgress();
    active_.store(false, std::memory_order_release);
}

bool GlobalRecorder::process(Event event, std::string& error) {
    latestElapsedMs_ = std::max(latestElapsedMs_, event.elapsedMs);

    switch (event.type) {
    case EventType::SessionEnded:
        endSession(event.id);
        return true;

    case EventType::Audio:
    case EventType::AudioFormat: {
        auto source = sources_.find(event.id);
        if (source == sources_.end()) {
            return true;
        }
        if (!muxer_.open()) {
            source->second.audio = AudioTrack{event.codec, event.sampleRate, 1};
            if (event.type == EventType::Audio && event.data.size() <= kMaxAudioPrerollBytes) {
                while (!source->second.preroll.empty() &&
                       source->second.prerollBytes + event.data.size() >
                           kMaxAudioPrerollBytes) {
                    source->second.prerollBytes -= source->second.preroll.front().data.size();
                    source->second.preroll.pop_front();
                }
                source->second.prerollBytes += event.data.size();
                source->second.preroll.push_back(std::move(event));
            }
            const bool allPrepared =
                std::all_of(videos_.begin(), videos_.end(),
                            [](const auto& entry) { return entry.second.prepared; });
            if (allPrepared) {
                return openFile(error);
            }
            return true;
        }
        if (event.type == EventType::AudioFormat) {
            if (!source->second.muxTrack) {
                return true;
            }
            if (event.codec != source->second.audio.codec ||
                event.sampleRate != source->second.audio.sampleRate) {
                error = std::format("{} changed audio format during the global recording",
                                    source->second.title);
                return false;
            }
            return true;
        }
        return writeAudio(event, error);
    }

    case EventType::Video:
        return muxer_.open() ? writeVideo(event, error)
                             : prepareVideo(std::move(event), error);
    }
    return true;
}

bool GlobalRecorder::prepareVideo(Event event, std::string& error) {
    auto found = videos_.find(event.id);
    if (found == videos_.end()) {
        return true;
    }
    VideoRuntime& video = found->second;

    if (event.keyframe) {
        std::vector<uint8_t> extradata =
            videoParameterSets(event.data.data(), event.data.size(), event.codec);
        if (event.width <= 0 || event.height <= 0 || extradata.empty()) {
            return true;
        }

        video.spec.codec = event.codec;
        video.spec.width = event.width;
        video.spec.height = event.height;
        video.spec.extradata = std::move(extradata);
        video.prepared = true;
        video.preroll.clear();
        video.prerollBytes = 0;
    }

    if (!video.prepared) {
        return true;
    }

    if (video.prerollBytes + event.data.size() > kMaxPrerollBytes) {
        // Keep memory bounded without ever retaining inter frames whose keyframe
        // was discarded. The next keyframe starts a fresh bounded GOP.
        video.prepared = false;
        video.preroll.clear();
        video.prerollBytes = 0;
    } else {
        video.prerollBytes += event.data.size();
        video.preroll.push_back(std::move(event));
    }

    const size_t prepared =
        static_cast<size_t>(std::count_if(videos_.begin(), videos_.end(), [](const auto& entry) {
            return entry.second.prepared;
        }));
    {
        std::scoped_lock lock(statusMutex_);
        status_.prepared = prepared;
    }

    if (prepared == videos_.size()) {
        return openFile(error);
    }
    return true;
}

bool GlobalRecorder::openFile(std::string& error) {
    const bool waitingForAudio =
        std::any_of(sources_.begin(), sources_.end(), [](const auto& entry) {
            return entry.second.audioExpected && !entry.second.audio.valid();
        });
    if (waitingForAudio && latestElapsedMs_ < kAudioFormatWaitMs) {
        return true;
    }

    std::vector<MatroskaVideoTrack> videoTracks;
    videoTracks.reserve(videoOrder_.size());
    for (size_t i = 0; i < videoOrder_.size(); ++i) {
        VideoRuntime& video = videos_.at(videoOrder_[i]);
        video.spec.defaultTrack = i == 0;
        video.muxTrack = i;
        videoTracks.push_back(video.spec);
    }

    std::vector<MatroskaAudioTrack> audioTracks;
    std::unordered_set<std::string> addedSources;
    for (const std::string& videoId : videoOrder_) {
        const Participant& participant = videos_.at(videoId).participant;
        if (!participant.audioOwner || !addedSources.insert(participant.sourceId).second) {
            continue;
        }

        SourceRuntime& source = sources_.at(participant.sourceId);
        if (!source.audio.valid()) {
            XV_WARN("{} has sent no audio; its global recording track is video-only",
                    participant.videoTitle);
            continue;
        }
        if (audio::avCodecId(source.audio.codec) == 0) {
            XV_WARN("{} audio ({}) cannot be stored in Matroska", participant.videoTitle,
                    audio::codecName(source.audio.codec));
            continue;
        }

        source.muxTrack = audioTracks.size();
        audioTracks.push_back(
            MatroskaAudioTrack{source.audio, source.title, audioTracks.empty()});
    }

    if (!muxer_.open(path_, videoTracks, audioTracks, startedUtc_, error)) {
        return false;
    }

    {
        std::scoped_lock lock(statusMutex_);
        status_.state = State::Recording;
    }

    std::vector<Event> preroll;
    for (const std::string& videoId : videoOrder_) {
        VideoRuntime& video = videos_.at(videoId);
        video.segmentStarted = false;
        video.timeline.reset();
        while (!video.preroll.empty()) {
            preroll.push_back(std::move(video.preroll.front()));
            video.preroll.pop_front();
        }
        video.prerollBytes = 0;
    }
    for (auto& [id, source] : sources_) {
        while (!source.preroll.empty()) {
            preroll.push_back(std::move(source.preroll.front()));
            source.preroll.pop_front();
        }
        source.prerollBytes = 0;
    }

    std::stable_sort(preroll.begin(), preroll.end(),
                     [](const Event& left, const Event& right) {
                         return left.elapsedMs < right.elapsedMs;
                     });
    for (const Event& event : preroll) {
        const bool written = event.type == EventType::Video ? writeVideo(event, error)
                                                            : writeAudio(event, error);
        if (!written) {
            return false;
        }
    }

    publishProgress();
    XV_INFO("global recording started with {} video and {} audio track(s)", videoTracks.size(),
            audioTracks.size());
    return true;
}

bool GlobalRecorder::writeVideo(const Event& event, std::string& error) {
    auto found = videos_.find(event.id);
    if (found == videos_.end()) {
        return true;
    }
    VideoRuntime& video = found->second;

    if (event.codec != video.spec.codec || event.width != video.spec.width ||
        event.height != video.spec.height) {
        error = std::format("{} changed video format during the global recording",
                            video.participant.videoTitle);
        return false;
    }

    if (!video.segmentStarted) {
        if (!event.keyframe) {
            return true;
        }
        video.segmentStarted = true;
    }

    SourceRuntime& source = sources_.at(video.participant.sourceId);
    if (!video.timeline.anchored()) {
        if (!event.keyframe) {
            return true;
        }
        int64_t anchor = std::max(event.elapsedMs, video.lastOutputMs + 1);
        if (video.participant.audioOwner) {
            anchor = std::max(anchor, source.lastOutputMs + 1);
        }
        video.timeline.anchor(event.ptsMs, anchor);
    }

    std::optional<int64_t> outputPts = video.timeline.map(event.ptsMs);
    if (!outputPts || *outputPts < 0) {
        if (!event.keyframe) {
            return true;
        }
        const int64_t anchor = std::max(event.elapsedMs, video.lastOutputMs + 1);
        video.timeline.anchor(event.ptsMs, anchor);
        outputPts = anchor;
    }
    *outputPts = monotonicRecordingPts(*outputPts, video.lastOutputMs);

    if (!muxer_.writeVideo(video.muxTrack, event.data.data(), event.data.size(), *outputPts,
                           event.keyframe, error)) {
        return false;
    }
    video.lastOutputMs = *outputPts;
    publishProgress();
    return true;
}

bool GlobalRecorder::writeAudio(const Event& event, std::string& error) {
    auto found = sources_.find(event.id);
    if (found == sources_.end() || !found->second.muxTrack) {
        return true;
    }
    SourceRuntime& source = found->second;

    if (event.codec != source.audio.codec || event.sampleRate != source.audio.sampleRate) {
        error = std::format("{} changed audio format during the global recording", source.title);
        return false;
    }

    const auto owner = videos_.find(source.audioOwnerVideoId);
    if (owner == videos_.end()) {
        return true;
    }
    std::optional<int64_t> outputPts = owner->second.timeline.map(event.ptsMs);
    if (!outputPts || *outputPts < 0) {
        return true;
    }
    *outputPts = monotonicRecordingPts(*outputPts, source.lastOutputMs);

    if (!muxer_.writeAudio(*source.muxTrack, event.data.data(), event.data.size(), *outputPts,
                           error)) {
        return false;
    }
    source.lastOutputMs = *outputPts;
    publishProgress();
    return true;
}

void GlobalRecorder::endSession(const std::string& videoId) {
    const auto found = videos_.find(videoId);
    if (found == videos_.end()) {
        return;
    }
    VideoRuntime& video = found->second;
    video.timeline.reset();
    video.segmentStarted = false;
    if (!muxer_.open()) {
        video.prepared = false;
        video.preroll.clear();
        video.prerollBytes = 0;

        if (video.participant.audioOwner) {
            SourceRuntime& source = sources_.at(video.participant.sourceId);
            source.preroll.clear();
            source.prerollBytes = 0;
        }
    }

    if (!muxer_.open()) {
        const size_t prepared = static_cast<size_t>(
            std::count_if(videos_.begin(), videos_.end(),
                          [](const auto& entry) { return entry.second.prepared; }));
        std::scoped_lock lock(statusMutex_);
        status_.prepared = prepared;
    }
}

void GlobalRecorder::fail(const std::string& error) {
    XV_ERROR("global recording stopped: {}", error);
    muxer_.close();

    {
        std::scoped_lock lock(statusMutex_);
        status_.state = State::Error;
        status_.error = error;
        status_.bytes = muxer_.bytesWritten();
        status_.durationMs = muxer_.durationMs();
    }
    accepting_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
}

void GlobalRecorder::publishProgress() {
    std::scoped_lock lock(statusMutex_);
    status_.bytes = muxer_.bytesWritten();
    status_.durationMs = muxer_.durationMs();
}

std::string GlobalRecorder::preparationTimeoutError() const {
    std::string missing;
    for (const std::string& videoId : videoOrder_) {
        const VideoRuntime& video = videos_.at(videoId);
        if (video.prepared) {
            continue;
        }
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += video.participant.videoTitle;
    }
    return missing.empty() ? "global recording preparation timed out"
                           : std::format("timed out waiting for {}", missing);
}

int64_t GlobalRecorder::elapsedNow() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 started_)
        .count();
}

std::string GlobalRecorder::fileName(
    std::chrono::system_clock::time_point recordingStartUtc) const {
    std::string stamp;
    const auto now = std::chrono::floor<std::chrono::seconds>(recordingStartUtc);
    try {
        stamp = std::format("{:%Y-%m-%d %H-%M-%S}",
                            std::chrono::zoned_time{std::chrono::current_zone(), now});
    } catch (const std::exception&) {
        stamp = std::format("{:%Y-%m-%d %H-%M-%S}", now);
    }
    return std::format("All cameras {}.mkv", stamp);
}

} // namespace xv

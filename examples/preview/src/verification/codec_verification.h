#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

#include <huxerui/avkit/codec.h>

namespace avkit_preview::verification {

using huxerui::avkit::AudioDecoderSession;
using huxerui::avkit::AudioEncoderSession;
using huxerui::avkit::CodecNegotiation;
using huxerui::avkit::CodecProvider;
using huxerui::avkit::VideoDecoderSession;
using huxerui::avkit::VideoEncoderSession;
using namespace huxerui::media;

struct DecoderUsage {
  struct Track { std::size_t opened{}, active{}, peak{}; };

  void Open(Track& track) {
    const std::lock_guard lock(mutex);
    ++track.opened;
    ++track.active;
    track.peak = std::max(track.peak, track.active);
    peak = std::max(peak, video.active + audio.active);
  }

  void Close(Track& track) {
    const std::lock_guard lock(mutex);
    --track.active;
  }

  void Reset() {
    const std::lock_guard lock(mutex);
    video = {};
    audio = {};
    peak = 0;
  }

  // Read after the observed work has finished.
  Track video, audio;
  std::size_t peak{};

private:
  std::mutex mutex;
};

struct VideoFaults {
  std::size_t decode_after{}, encode_after{};
};

class ObservedCodecProvider final : public CodecProvider {
public:
  ObservedCodecProvider(std::shared_ptr<CodecProvider> delegate, DecoderUsage* usage = nullptr, VideoFaults faults = {},
                        std::function<void(VideoFrame&)> observe_picture = {})
      : delegate_(std::move(delegate)), usage_(usage), faults_(faults), observe_picture_(std::move(observe_picture)) {}

  std::string_view Name() const noexcept override { return delegate_->Name(); }
  CodecResult<CodecNegotiation> Probe(const VideoDecoderConfig& c) const override { return delegate_->Probe(c); }
  CodecResult<CodecNegotiation> Probe(const AudioDecoderConfig& c) const override { return delegate_->Probe(c); }
  CodecResult<CodecNegotiation> Probe(const VideoEncoderConfig& c) const override { return delegate_->Probe(c); }
  CodecResult<CodecNegotiation> Probe(const AudioEncoderConfig& c) const override { return delegate_->Probe(c); }

  CodecResult<std::unique_ptr<VideoDecoderSession>> Open(const VideoDecoderConfig& c) const override {
    return OpenObserved<VideoDecoderSession, VideoFrame>(c, usage_ ? &usage_->video : nullptr, faults_.decode_after,
                                                         &decode_injected_, observe_picture_);
  }
  CodecResult<std::unique_ptr<AudioDecoderSession>> Open(const AudioDecoderConfig& c) const override {
    return OpenObserved<AudioDecoderSession, AudioFrame>(c, usage_ ? &usage_->audio : nullptr);
  }
  CodecResult<std::unique_ptr<VideoEncoderSession>> Open(const VideoEncoderConfig& c) const override {
    return OpenObserved<VideoEncoderSession, EncodedPacket>(c, nullptr, faults_.encode_after, &encode_injected_);
  }
  CodecResult<std::unique_ptr<AudioEncoderSession>> Open(const AudioEncoderConfig& c) const override { return delegate_->Open(c); }

private:
  template<class Session, class Event> class ObservedSession final : public Session {
  public:
    ObservedSession(std::unique_ptr<Session> delegate, DecoderUsage* usage, DecoderUsage::Track* track,
                    std::size_t remaining, std::atomic_bool* injected,
                    std::function<void(Event&)> observe)
        : delegate_(std::move(delegate)), usage_(usage), track_(track), remaining_(remaining), injected_(injected),
          observe_(std::move(observe)) {}
    ~ObservedSession() override { Close(); }

    CodecNegotiation Negotiated() const noexcept override { return delegate_->Negotiated(); }
    CodecResult<SubmitStatus> TrySubmit(const typename Session::Input& input) override { return delegate_->TrySubmit(input); }
    CodecResult<std::optional<typename Session::Output>> TryRead() override {
      if (injected_ && remaining_ == 0 && !injected_->exchange(true, std::memory_order_relaxed)) {
        return CodecResult<std::optional<typename Session::Output>>::Failure(
            {CodecErrorCode::ResourceExhausted, "Injected failure after VideoToolbox produced output", {}});
      }
      auto event = delegate_->TryRead();
      if (event.Succeeded() && event.Value()) {
        if (auto* value = std::get_if<Event>(&*event.Value())) {
          if (remaining_) --remaining_;
          if (observe_) observe_(*value);
        }
      }
      return event;
    }
    CodecResult<void> FinishInput() override { return delegate_->FinishInput(); }
    void Close() noexcept override {
      if (std::exchange(closed_, true)) return;
      delegate_->Close();
      if (track_) usage_->Close(*track_);
    }

  private:
    std::unique_ptr<Session> delegate_;
    DecoderUsage* usage_;
    DecoderUsage::Track* track_;
    std::size_t remaining_;
    std::atomic_bool* injected_;
    std::function<void(Event&)> observe_;
    bool closed_{};
  };

  template<class Session, class Event, class Config>
  CodecResult<std::unique_ptr<Session>> OpenObserved(const Config& config, DecoderUsage::Track* track,
                                                     std::size_t fail_after = 0, std::atomic_bool* injected = nullptr,
                                                     std::function<void(Event&)> observe = {}) const {
    auto result = delegate_->Open(config);
    const bool arm = fail_after && injected && !injected->load(std::memory_order_relaxed);
    if (!result.Succeeded() || !result.Value() || (!track && !arm && !observe)) return result;
    auto session = std::make_unique<ObservedSession<Session, Event>>(std::move(result).Value(), usage_, track, fail_after,
                                                                    arm ? injected : nullptr, std::move(observe));
    if (track) usage_->Open(*track);
    return CodecResult<std::unique_ptr<Session>>::Success(std::move(session));
  }

  std::shared_ptr<CodecProvider> delegate_;
  DecoderUsage* usage_;
  VideoFaults faults_;
  std::function<void(VideoFrame&)> observe_picture_;
  mutable std::atomic_bool decode_injected_{}, encode_injected_{};
};

} // namespace avkit_preview::verification

#pragma once

#include <algorithm>
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
  Track video, audio;
  std::size_t peak{};
};

struct VideoFaults {
  std::size_t decode_after{}, encode_after{};
};

class ObservedCodecProvider final : public CodecProvider {
public:
  ObservedCodecProvider(std::shared_ptr<CodecProvider> delegate, DecoderUsage* usage = nullptr, VideoFaults faults = {})
      : delegate_(std::move(delegate)), usage_(usage), faults_(faults) {}

  std::string_view Name() const noexcept override { return delegate_->Name(); }
  CodecResult<CodecNegotiation> Probe(const VideoDecoderConfig& c) const override { return delegate_->Probe(c); }
  CodecResult<CodecNegotiation> Probe(const AudioDecoderConfig& c) const override { return delegate_->Probe(c); }
  CodecResult<CodecNegotiation> Probe(const VideoEncoderConfig& c) const override { return delegate_->Probe(c); }
  CodecResult<CodecNegotiation> Probe(const AudioEncoderConfig& c) const override { return delegate_->Probe(c); }

  CodecResult<std::unique_ptr<VideoDecoderSession>> Open(const VideoDecoderConfig& c) const override {
    return OpenObserved<VideoDecoderSession, VideoFrame>(c, usage_ ? &usage_->video : nullptr, faults_.decode_after, &decode_injected_);
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
    ObservedSession(std::unique_ptr<Session> delegate, DecoderUsage::Track* track, std::size_t remaining, bool* injected)
        : delegate_(std::move(delegate)), track_(track), remaining_(remaining), injected_(injected) {}
    ~ObservedSession() override { Close(); }

    CodecNegotiation Negotiated() const noexcept override { return delegate_->Negotiated(); }
    CodecResult<SubmitStatus> TrySubmit(const typename Session::Input& input) override { return delegate_->TrySubmit(input); }
    CodecResult<std::optional<typename Session::Output>> TryRead() override {
      if (injected_ && !*injected_ && remaining_ == 0) {
        *injected_ = true;
        return CodecResult<std::optional<typename Session::Output>>::Failure(
            {CodecErrorCode::ResourceExhausted, "Injected failure after VideoToolbox produced output", {}});
      }
      auto event = delegate_->TryRead();
      if (remaining_ && event.Succeeded() && event.Value() && std::holds_alternative<Event>(*event.Value())) --remaining_;
      return event;
    }
    CodecResult<void> FinishInput() override { return delegate_->FinishInput(); }
    void Close() noexcept override {
      if (std::exchange(closed_, true)) return;
      delegate_->Close();
      if (track_) --track_->active;
    }

  private:
    std::unique_ptr<Session> delegate_;
    DecoderUsage::Track* track_;
    std::size_t remaining_;
    bool* injected_;
    bool closed_{};
  };

  template<class Session, class Event, class Config>
  CodecResult<std::unique_ptr<Session>> OpenObserved(const Config& config, DecoderUsage::Track* track,
                                                     std::size_t fail_after = 0, bool* injected = nullptr) const {
    auto result = delegate_->Open(config);
    const bool arm = injected && !*injected && fail_after;
    if (!result.Succeeded() || !result.Value() || (!track && !arm)) return result;
    auto session = std::make_unique<ObservedSession<Session, Event>>(std::move(result).Value(), track, fail_after,
                                                                    arm ? injected : nullptr);
    if (track) {
      ++track->opened;
      ++track->active;
      track->peak = std::max(track->peak, track->active);
      usage_->peak = std::max(usage_->peak, usage_->video.active + usage_->audio.active);
    }
    return CodecResult<std::unique_ptr<Session>>::Success(std::move(session));
  }

  std::shared_ptr<CodecProvider> delegate_;
  DecoderUsage* usage_;
  VideoFaults faults_;
  mutable bool decode_injected_{}, encode_injected_{};
};

} // namespace avkit_preview::verification

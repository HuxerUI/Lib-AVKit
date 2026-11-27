#include <huxerui/avkit/codec.h>

#include <condition_variable>
#include <mutex>
#include <utility>

namespace huxerui::avkit {
namespace {

using namespace media;

CodecNegotiation Negotiation(const VideoDecoderConfig& config) {
  auto negotiation = CodecNegotiation{CodecImplementation::Unknown, VideoFrameContract{config.output_storage, config.output}};
#if !defined(__EMSCRIPTEN__)
  negotiation.allow_sequence_handoff = true;
#endif
  return negotiation;
}

CodecNegotiation Negotiation(const VideoEncoderConfig& config) {
  return {CodecImplementation::Unknown, VideoFrameContract{config.input_storage, config.input.pixel_format}};
}

CodecNegotiation Negotiation(const AudioDecoderConfig&) { return {CodecImplementation::Unknown, std::nullopt}; }

CodecNegotiation Negotiation(const AudioEncoderConfig&) { return {CodecImplementation::Unknown, std::nullopt}; }

template<class Codec, class Config>
CodecResult<CodecNegotiation> ProbeCodec(const Config& config) {
  struct PendingProbe {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<CodecResult<void>> result;
  };
  auto pending = std::make_shared<PendingProbe>();
  Codec::Probe(config, [pending](CodecResult<void> result) {
    {
      std::lock_guard lock(pending->mutex);
      pending->result.emplace(std::move(result));
    }
    pending->changed.notify_one();
  });
  std::unique_lock lock(pending->mutex);
  pending->changed.wait(lock, [&] { return pending->result.has_value(); });
  if (!pending->result->Succeeded()) return CodecResult<CodecNegotiation>::Failure(pending->result->Error());
  return CodecResult<CodecNegotiation>::Success(Negotiation(config));
}

template<class Codec, class Session> class LibMediaSession final : public Session {
public:
  LibMediaSession(Codec codec, CodecNegotiation negotiation)
      : codec_(std::move(codec)), negotiation_(negotiation) {}

  CodecNegotiation Negotiated() const noexcept override { return negotiation_; }

  CodecResult<SubmitStatus> TrySubmit(const typename Session::Input& input) override { return codec_.TrySubmit(input); }

  CodecResult<std::optional<typename Session::Output>> TryRead() override { return codec_.TryRead(); }

  CodecResult<void> FinishInput() override { return codec_.Finish(); }

  void Close() noexcept override { codec_.Close(); }

private:
  Codec codec_;
  CodecNegotiation negotiation_;
};

template<class Codec, class Session, class Config>
CodecResult<std::unique_ptr<Session>> OpenCodec(const Config& config) {
  using R = CodecResult<std::unique_ptr<Session>>;
  auto codec = Codec::Create(config);
  if (!codec.Succeeded()) return R::Failure(codec.Error());
  return R::Success(std::make_unique<LibMediaSession<Codec, Session>>(std::move(codec).Value(), Negotiation(config)));
}

class LibMediaProvider final : public CodecProvider {
public:
  std::string_view Name() const noexcept override { return "Lib-Media"; }

  CodecResult<CodecNegotiation> Probe(const VideoDecoderConfig& config) const override {
    return ProbeCodec<VideoDecoder>(config);
  }

  CodecResult<CodecNegotiation> Probe(const VideoEncoderConfig& config) const override {
    return ProbeCodec<VideoEncoder>(config);
  }

  CodecResult<CodecNegotiation> Probe(const AudioDecoderConfig& config) const override {
    return ProbeCodec<AudioDecoder>(config);
  }

  CodecResult<CodecNegotiation> Probe(const AudioEncoderConfig& config) const override {
    return ProbeCodec<AudioEncoder>(config);
  }

  CodecResult<std::unique_ptr<VideoDecoderSession>> Open(const VideoDecoderConfig& config) const override {
    return OpenCodec<VideoDecoder, VideoDecoderSession>(config);
  }

  CodecResult<std::unique_ptr<VideoEncoderSession>> Open(const VideoEncoderConfig& config) const override {
    return OpenCodec<VideoEncoder, VideoEncoderSession>(config);
  }

  CodecResult<std::unique_ptr<AudioDecoderSession>> Open(const AudioDecoderConfig& config) const override {
    return OpenCodec<AudioDecoder, AudioDecoderSession>(config);
  }

  CodecResult<std::unique_ptr<AudioEncoderSession>> Open(const AudioEncoderConfig& config) const override {
    return OpenCodec<AudioEncoder, AudioEncoderSession>(config);
  }
};

} // namespace

std::shared_ptr<CodecProvider> CreateLibMediaCodecProvider() { return std::make_shared<LibMediaProvider>(); }

} // namespace huxerui::avkit

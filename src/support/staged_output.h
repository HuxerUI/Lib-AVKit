#pragma once

#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/file.h>

namespace huxerui::avkit::detail {

// Keep incomplete output in a sibling file. The caller closes a taken stream before publishing it.
class StagedOutput {
public:
  explicit StagedOutput(const File& target)
      : file_(target.Path() + ".avkit-" + std::to_string(std::random_device{}()) + ".tmp") {
    auto opened = file_.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew);
    if (!opened.Succeeded()) throw std::runtime_error(opened.Error().message);
    stream_.emplace(std::move(opened).Value());
  }
  ~StagedOutput() {
    if (!published_) {
      stream_.reset();
      (void)file_.Delete();
    }
  }

  RandomAccessStream TakeStream() {
    auto stream = std::move(*stream_);
    stream_.reset();
    return stream;
  }
  void Publish(const File& target, bool overwrite = false) {
    if (!file_.MoveTo(target, overwrite)) throw std::runtime_error("Cannot publish output to " + target.Path());
    published_ = true;
  }

private:
  File file_;
  std::optional<RandomAccessStream> stream_;
  bool published_{};
};

} // namespace huxerui::avkit::detail

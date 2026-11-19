#pragma once

#include "texture_bridge.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

// Include the platform's OpenGL or OpenGL ES declarations before this header.
namespace huxerui::avkit::render::detail {

struct GlTextureName {
  GLuint texture{};
  bool owned{true};
  std::shared_ptr<const void> producer;
};

inline GLuint AllocateRgba(PixelSize size) {
  GLuint name = 0;
  GLint previous = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
  glGenTextures(1, &name);
  glBindTexture(GL_TEXTURE_2D, name);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, size.width, size.height);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
  if (!name || glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &name);
    throw std::runtime_error("Cannot allocate an RGBA output texture");
  }
  return name;
}

class GlStorage final : public SurfaceStorage {
public:
  GlStorage(std::shared_ptr<const int> owner, ImageDesc image, RowOrigin origin, std::shared_ptr<GlTextureName> name)
      : owner(std::move(owner)), image(image), origin(origin), name(std::move(name)) {}

  std::shared_ptr<const int> owner;
  ImageDesc image;
  RowOrigin origin;
  std::shared_ptr<GlTextureName> name;
};

class GlTextureBridge : public BgfxTextureBridge {
public:
  std::shared_ptr<const SurfaceStorage> CreateOutput(ImageDesc image) const override {
    auto record = std::make_shared<GlTextureName>();
    record->texture = AllocateRgba(image.size);
    return Retain(image, RowOrigin::BottomLeft, std::move(record)).storage;
  }

  std::uint64_t GetTexture(const ImageSurface& surface) const override {
    const auto* storage = dynamic_cast<const GlStorage*>(surface.storage.get());
    if (!storage || storage->owner != owner_ || storage->image != surface.image || storage->origin != surface.origin)
      return 0;
    return storage->name->texture;
  }

  void WaitForCompletion() const override {
    glFinish();
    CollectReleasedNames();
  }

  void ReleaseOutputResources() override {
    for (const auto& name : names_) {
      if (name->owned) glDeleteTextures(1, &name->texture);
    }
    names_.clear();
  }

protected:
  ImageSurface Retain(ImageDesc image, RowOrigin origin, std::shared_ptr<GlTextureName> name) const {
    names_.push_back(name);
    return {image, origin, std::make_shared<GlStorage>(owner_, image, origin, std::move(name))};
  }

  void CollectReleasedNames() const {
    std::erase_if(names_, [](const auto& name) {
      if (name.use_count() != 1) return false;
      if (name->owned) glDeleteTextures(1, &name->texture);
      return true;
    });
  }

private:
  // GL names are retired on the compositor sequence after bgfx finishes using them.
  std::shared_ptr<const int> owner_ = std::make_shared<const int>(0);
  mutable std::vector<std::shared_ptr<GlTextureName>> names_;
};

} // namespace huxerui::avkit::render::detail

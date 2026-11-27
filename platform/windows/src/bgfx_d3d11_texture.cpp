#include "texture_bridge.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <stdexcept>
#include <thread>

#include <huxerui/windows/external_texture.h>

namespace huxerui::avkit::render::detail {

namespace {

using Microsoft::WRL::ComPtr;

struct D3DStorage final : TextureBacking {
  ComPtr<ID3D11Texture2D> texture;
};

class D3D11TextureBridge final : public BgfxTextureBridge {
public:
  D3D11TextureBridge() {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
      throw std::runtime_error("Cannot create a D3D11 device for the compositor");
    }
    const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
    if (FAILED(device_->CreateQuery(&description, &complete_))) {
      throw std::runtime_error("Cannot create the D3D11 completion query");
    }
  }

  ID3D11Device* Device() const { return device_.Get(); }

  std::shared_ptr<const TextureBacking> CreateOutput(ImageInfo image) const override {
    return Allocate(image, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
  }

  std::uint64_t GetTexture(const Texture& surface) const override {
    const auto* storage = dynamic_cast<const D3DStorage*>(TextureAccess::Backing(surface).get());
    if (!storage || surface.Origin() != RowOrigin::TopLeft) return 0;
    D3D11_TEXTURE2D_DESC description{};
    storage->texture->GetDesc(&description);
    ComPtr<ID3D11Device> owner;
    storage->texture->GetDevice(&owner);
    if (owner.Get() != device_.Get() || surface.Info().size != PixelSize{description.Width, description.Height} ||
        description.Format != Format(surface.Info().format)) return 0;
    return reinterpret_cast<std::uint64_t>(storage->texture.Get());
  }

  void WaitForCompletion() const override {
    context_->End(complete_.Get());
    context_->Flush();
    HRESULT result;
    while ((result = context_->GetData(complete_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
      std::this_thread::yield();
    if (FAILED(result)) throw std::runtime_error("D3D11 compositing did not complete");
  }

  ImportedTexture Acquire(const ExternalTexture& input) override {
    using R = ImportedTexture;
    const auto* mailbox = dynamic_cast<const windows::D3D11Texture*>(&input);
    auto snapshot = mailbox ? mailbox->AcquireFrame() : nullptr;
    if (!snapshot) return R::Failure({RenderErrorCode::Unsupported, "Expected a published D3D11 frame"});
    try {
      const ImageInfo image{{static_cast<std::uint32_t>(snapshot->PixelWidth()),
                             static_cast<std::uint32_t>(snapshot->PixelHeight())}, PixelFormat::Bgra8Unorm};
      auto storage = Allocate(image, D3D11_BIND_SHADER_RESOURCE);
      if (!snapshot->TryCopyTo(*context_.Get(), *storage->texture.Get())) {
        return R::Failure({RenderErrorCode::Unsupported, "The D3D11 frame is busy or on another adapter"});
      }
      const auto alpha = snapshot->Alpha() == windows::D3D11Texture::Alpha::Opaque
          ? AlphaMode::Opaque : AlphaMode::Premultiplied;
      WaitForCompletion();
      return R::Success(TextureAccess::Make(image, RowOrigin::TopLeft, std::move(storage), alpha));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }

  PublishedTexture Publish(const Texture& surface, media::MediaTimestamp) override {
    using R = PublishedTexture;
    try {
      const auto [width, height] = surface.Info().size;
      auto* source = reinterpret_cast<ID3D11Texture2D*>(GetTexture(surface));
      if (!source) return R::Failure({RenderErrorCode::Unsupported, "Expected a completed D3D11 texture"});
      auto mailbox = std::make_shared<windows::D3D11Texture>(Size{float(width), float(height)});
      mailbox->Publish({.texture = source});
      mailbox->Finish();
      return R::Success(std::move(mailbox));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }

private:
  static DXGI_FORMAT Format(PixelFormat format) {
    switch (format) {
      case PixelFormat::Bgra8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
      case PixelFormat::Rgba8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
      case PixelFormat::R8Unorm: return DXGI_FORMAT_R8_UNORM;
      case PixelFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
  }

  std::shared_ptr<D3DStorage> Allocate(ImageInfo image, UINT flags) const {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.size.width;
    description.Height = image.size.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = Format(image.format);
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = flags;
    auto storage = std::make_shared<D3DStorage>();
    if (FAILED(device_->CreateTexture2D(&description, nullptr, &storage->texture))) {
      throw std::runtime_error("Cannot allocate a D3D11 compositor texture");
    }
    return storage;
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Query> complete_;
};

} // namespace

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageInfo target, bgfx::Init init, std::stop_token stop) {
  init.type = bgfx::RendererType::Direct3D11;
  auto bridge = std::make_unique<D3D11TextureBridge>();
  init.platformData.context = bridge->Device();
  return CreateBgfxBackend(target, init, stop, std::move(bridge));
}

} // namespace huxerui::avkit::render::detail

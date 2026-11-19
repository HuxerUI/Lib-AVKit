# Lib-AVKit

Lib-AVKit is a C++20 audio and video post-processing library for HuxerUI. The initial implementation provides an editable timeline, Flecs scene evaluation, GPU compositing, and optional file codecs. See the [architecture and milestone verification](docs/architecture.md) for the wider design and detailed checks.

## Targets

| Target | Responsibility |
| --- | --- |
| `AVKit::Core` | Projects, edit recipes, Flecs scene evaluation, tracking association, and the render plan contract. |
| `AVKit::Media` | Optional Lib-Media codec adapter, file readers, timeline sampling, thumbnails, waveforms, and MP4 output. |
| `AVKit::FFmpeg` | Optional FFmpeg software codec provider for the file profile. |
| `AVKit::Bgfx` | Shared GPU compositor with platform shader binaries and SDK texture transfer. |
| `AVKit::NCNN` | Optional cross-platform CPU provider for face, body, and hand landmarks and portrait segmentation. |
| `HuxerUI::AVKit` | HuxerUI library integration and application hook. Exposes Core and the bgfx compositor. |

Core and the media orchestration use public HuxerUI and Lib-Media types. The file profile accepts MP4 or MOV with H.264 video and AAC-LC audio. The macOS example integrates Metal textures, file media, and capture. The other preview entry points run the shared compositor with a small generated image, sticker, and mask. [Platform plan](docs/architecture.md#external-video-frames)

## Repository layout

| Path | Contents |
| --- | --- |
| `include/huxerui/` | Public cross-platform C++ headers. |
| `src/` | Shared project, ECS, analysis, media, and rendering code. `render/bgfx/` contains the compositor and shared shader sources. |
| `platform/` | Platform texture bridges, graphics context setup, and packaging files. |
| `cmake/` | Target and dependency setup. `AVKitBgfx.cmake` selects the renderer, shader profile, and platform sources. |
| `models/` | Versioned model assets with checksums and upstream licenses. |
| `examples/preview/` | Shared HuxerUI app, timeline, and fixtures in `src/`, with platform entry points and verification under `platform/`. |

In documentation and identifiers, use *platform* for platform adapters, capabilities, and storage. Use *system* for operating-system services and scheduling or when referring to an ECS system. Reserve *native* for terms such as JNI declarations and handles. Name a concrete type when it is known, such as a Metal texture or an FFmpeg frame.

## Build

Use CMake 3.21 or newer, a HuxerUI SDK or source checkout selected through `HUXERUI_HOME`, and a Lib-Media source checkout next to Lib-AVKit. Set `AVKIT_LIB_MEDIA_SOURCE_DIR` to select another checkout.

Build the headless Core on the current target platform.

```sh
cmake -S . -B build/core
cmake --build build/core --target avkit_core
```

Build the macOS preview with the file workflow. Point `PKG_CONFIG_PATH` at the target-compatible FFmpeg installation described in the [file workflow](docs/architecture.md#macos-file-media-milestone).

```sh
cmake -S examples/preview -B build/preview -DAVKIT_ENABLE_FILE_MEDIA=ON
cmake --build build/preview --target example_av_kit

preview="$PWD/build/preview/example_av_kit.app/Contents/MacOS/example_av_kit"
"$preview" --verify
"$preview" --timeline-verify-tracking "$PWD/build/tracking-check"
```

All supported platforms build the bgfx compositor. Cross builds compile the pinned `shaderc` for the host with the host C++ toolchain. Set `AVKIT_BGFX_SHADERC` to reuse an existing host executable built from the same revision. The compositor uses Metal on macOS and iOS, OpenGL ES 3 on Android, WebGL 2 on Web, and D3D11 for Windows builds hosted on Windows. Other Windows builds use Vulkan. Linux uses OpenGL for a current compatible GDK/EGL context and Vulkan for independent rendering. Linux builds use GTK, epoxy, EGL, X11, and XCB development headers. [Compositor API](include/huxerui/avkit/bgfx.h)

The [video texture API](include/huxerui/avkit/video_texture.h) accepts HuxerUI capture textures, Lib-Media platform decoder textures, and FFmpeg-decoded CPU planes. File preview and export retry a rejected decoder texture in FFmpeg at the same source PTS. Timeline rendering applies the same retry to the affected clip when supplied with an input importer. File and timeline export can restart with a CPU encoder when platform encoder transfer fails.

`AVKIT_ENABLE_FILE_MEDIA` enables the file module and defaults to off. With file media enabled, `AVKIT_ENABLE_FFMPEG=OFF` builds the module with the Lib-Media adapter. The macOS camera and microphone example can be enabled separately with `AVKIT_ENABLE_CAPTURE_PREVIEW`. It uses Lib-Camera and Lib-AudioCapture alongside the bgfx and FFmpeg targets.

Set `AVKIT_ENABLE_NCNN=ON` to build `AVKit::NCNN`. The provider accepts verified [YuNet](models/yunet/2023mar) and [MediaPipe body and hand](models/mediapipe/body-hand) model packs through the [C++ factories](include/huxerui/avkit/ncnn.h). See the [tracking verification](docs/architecture.md#ncnn-body-and-hand-provider) for the macOS preview, seek, and export commands.

Portrait background replacement uses [MediaPipe Selfie Segmentation](models/mediapipe/selfie-segmentation) by default. Clips can select the optional [PP-HumanSegV2-Lite](models/paddle/pp-humansegv2-lite) pack independently. [Model selection and verification](docs/architecture.md#ncnn-portrait-segmentation)

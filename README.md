# Lib-AVKit

Lib-AVKit is a C++20 audio and video post-processing library for HuxerUI. It provides an editable timeline, EntityX scene evaluation, GPU compositing, file codecs, vision analysis, and PAG animation. See the [architecture](docs/architecture.md) for design and integration details.

## Targets

| Target | Responsibility |
| --- | --- |
| `AVKit::Core` | Projects, edit recipes, EntityX scene evaluation, tracking association, and the render plan contract. |
| `AVKit::Media` | Lib-Media codec adapter, file readers, timeline sampling, thumbnails, waveforms, and MP4 output. |
| `AVKit::FFmpeg` | FFmpeg software codec provider for the file profile. |
| `AVKit::Bgfx` | Shared GPU compositor with platform shader binaries and SDK texture transfer. |
| `AVKit::NCNN` | Cross-platform CPU provider for face, body, and hand landmarks and portrait segmentation. |
| `AVKit::PAG` | PAG animation, editable text and image slots, and platform transition interop. |
| `AVKit::Render` | Timeline compositing, versioned effect resource resolution, and texture transfer for preview and export. |
| `AVKit::Capture` | Camera frame processing, synchronized microphone input, and composited recording on macOS and Android. |
| `HuxerUI::AVKit` | HuxerUI application hook and the combined AVKit library targets. |

Core and the media orchestration use public HuxerUI and Lib-Media types. The file profile accepts SDR video in MP4, MOV, Matroska, WebM, MPEG-TS, and AVI. Supported codecs depend on the container and the available decoders. The workbench integrates file editing on macOS and Android. The recording workbench is available on macOS and Android. [File profile](docs/architecture.md#macos-file-media-milestone) [Platform plan](docs/architecture.md#external-video-frames)

AVKit-owned formats and resource manifests use version 1 during pre-release development. The workbench saves and opens the current v1 `.avkit` layout. [Project persistence](docs/architecture.md#macos-timeline-and-edit-template-milestone)

## Repository layout

| Path | Contents |
| --- | --- |
| `include/huxerui/` | Public cross-platform C++ headers. |
| `src/` | Shared project, ECS, analysis, media, and rendering code. `render/bgfx/` contains the compositor and shared shader sources. |
| `platform/` | Platform texture bridges, graphics context setup, and packaging files. |
| `cmake/` | Target and dependency setup. `AVKitBgfx.cmake` selects the renderer, shader profile, and platform sources. |
| `models/` | Versioned model assets with checksums and upstream licenses. |
| `examples/preview/` | HuxerUI workbench in `src/` and platform adapters in `platform/`. |

In documentation and identifiers, use *platform* for platform adapters, capabilities, and storage. Use *system* for operating-system services and scheduling or when referring to an ECS system. Reserve *native* for terms such as JNI declarations and handles. Name a concrete type when it is known, such as a Metal texture or an FFmpeg frame.

## Build

Use CMake 3.21 or newer and a HuxerUI SDK or source checkout selected through `HUXERUI_HOME`. The build uses the pinned Lib-Media Git revision by default. Set `AVKIT_LIB_MEDIA_SOURCE_DIR` to use a local checkout.

Each platform configures the same AVKit targets. Supply FFmpeg and libpag built for the target, including the selected architecture, ABI, or WebAssembly toolchain. Point `PKG_CONFIG_PATH` at the target FFmpeg installation and supply `AVKIT_PAG_INCLUDE_DIR` and `AVKIT_PAG_LIBRARY` for the matching libpag SDK. ncnn and bgfx are built from pinned sources. An application can link an individual AVKit target or `HuxerUI::AVKit` for the full set.

To build just the Core target after configuring the project, run the following commands.

```sh
cmake -S . -B build/core \
  -DAVKIT_PAG_INCLUDE_DIR="/path/to/libpag/include" \
  -DAVKIT_PAG_LIBRARY="/path/to/target/libpag"
cmake --build build/core --target avkit_core
```

Capture on macOS and Android requires Lib-Camera and Lib-AudioCapture. Place the source checkouts next to Lib-AVKit or set `AVKIT_LIB_CAMERA_SOURCE_DIR` to the Camera checkout. The recording page supports JPEG photos, microphone audio, live voice processing, pause and resume, and saving a copy. macOS and Android use the AVKit bgfx sticker compositor and codec selection for recording.

The macOS workbench supports file editing, export, PAG, and ncnn. Point `PKG_CONFIG_PATH` at the target-compatible FFmpeg installation described in the [file workflow](docs/architecture.md#macos-file-media-milestone), and supply matching libpag headers and the macOS library.

```sh
cmake -S examples/preview -B build/preview \
  -DAVKIT_PAG_INCLUDE_DIR="/path/to/libpag/include" \
  -DAVKIT_PAG_LIBRARY="/path/to/libpag.framework/libpag"
cmake --build build/preview --target example_av_kit
open build/preview/example_av_kit.app
```

The Android workbench imports files in the file profile, plays and seeks the timeline, and exports BT.709 H.264/AAC MP4 through the system file picker. Lib-Media handles platform decoding and passes bgfx GLES composites to the MediaCodec AVC encoder surface. On a recoverable video encoder failure, export restarts from the beginning with FFmpeg and publishes the finalized MP4. Android export uses FFmpeg AAC-LC so the MP4 edit list carries exact priming and padding.

Build static, position-independent FFmpeg for each Android ABI you run. Enable the file profile decoders and the AAC and `libx264` encoders. Build static, position-independent x264 for the same ABI and API level and make its pkg-config file available when configuring FFmpeg. The `libx264` encoder requires FFmpeg's GPL build option. Put each install prefix under one directory with its Android ABI name. For example, `build/android-ffmpeg/arm64-v8a` contains `include` and `lib`, with `libavcodec.a`, `libavutil.a`, `libswresample.a`, `libx264.a`, and their pkg-config files under `lib`. [File profile build](docs/architecture.md#macos-file-media-milestone) [FFmpeg x264 configuration](https://ffmpeg.org/general.html#x264)

```sh
AVKIT_ANDROID_FFMPEG_ROOT="$PWD/build/android-ffmpeg" \
AVKIT_PAG_INCLUDE_DIR="/path/to/libpag/include" \
AVKIT_ANDROID_PAG_AAR="/path/to/libpag-4.5.102.aar" \
  huxerui run android --abi arm64-v8a
```

The AAR must contain `libpag.so` and its runtime libraries for every selected ABI. AVKit links the library for the current ABI, and the preview APK packages the same AAR. [libpag release](https://github.com/Tencent/libpag/releases/tag/v4.5.102)

All supported platforms build the bgfx compositor. Cross builds compile the pinned `shaderc` for the host with the host C++ toolchain. Set `AVKIT_BGFX_SHADERC` to reuse an existing host executable built from the same revision. The compositor uses Metal on macOS and iOS, OpenGL ES 3 on Android, WebGL 2 on Web, and D3D11 for Windows builds hosted on Windows. Other Windows builds use Vulkan. Linux uses OpenGL for a current compatible GDK/EGL context and Vulkan for independent rendering. Linux builds use GTK, epoxy, EGL, X11, and XCB development headers. [Render API](include/huxerui/avkit/render.h)

Open a managed `TimelinePreviewSession` to play, seek, and present an immutable project revision. The host applies each prepared frame and its selection geometry together. `ExportProject` executes the same render tree at the requested output size and can borrow the preview's renderer reservation for the duration of the export. Both accept the same scene configuration, including resource catalog, analysis adapter, and ECS module. [Timeline renderer API](include/huxerui/avkit/timeline.h)

For independent processing, sample a `TimelineSession` or build a `TimelineFrame` from a `RenderScene` or a caller-authored `RenderPlan`. Use `TimelineRenderer::OpenTimeline` to bind timeline sampling to the GPU device, or call `ImportTexture` for a camera or other external GPU texture. `Draw` uploads retained CPU pixels as needed and produces a retained GPU result. Publish that image through a reusable `PreviewTexture` or transfer it to the selected encoder with `EncoderFrame`. Platform decoder textures that cannot be shared can be retried through FFmpeg at the same source timestamp. [ECS API](include/huxerui/avkit/scene.h) · [Sampling API](include/huxerui/avkit/timeline.h)

`InspectTimelineVisual` imports movies and PNG or JPEG stills. `ProjectEdit::InsertImage` defaults to three seconds and accepts a duration override. The image remains a still in the project. `OpenEditTemplate` and `CreateTemplateProject` resolve a versioned recipe and produce a normal editable project. [Project API](include/huxerui/avkit/project.h) · [Template API](include/huxerui/avkit/edit_template.h)

Applications with `HuxerUI::Camera` resolved can call `avkit_add_capture_target()` and link `AVKit::Capture` on macOS and Android. `CameraRecording` connects Camera's frame processor, captures a composed still, and records against the AudioCapture clock. The application owns permissions, device selection, preview layout, and destination selection. [Capture API](include/huxerui/avkit/camera_recording.h)

The ncnn provider accepts [YuNet](models/yunet/2023mar) and [MediaPipe body and hand](models/mediapipe/body-hand) model packs through the [C++ factories](include/huxerui/avkit/ncnn.h). See [body and hand tracking](docs/architecture.md#ncnn-body-and-hand-provider) for the provider behavior.

Portrait background replacement uses [MediaPipe Selfie Segmentation](models/mediapipe/selfie-segmentation) by default. Clips can select the optional [PP-HumanSegV2-Lite](models/paddle/pp-humansegv2-lite) pack independently. [Model selection](docs/architecture.md#ncnn-portrait-segmentation)

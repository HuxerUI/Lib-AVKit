# Lib-AVKit

Lib-AVKit is a C++20 audio and video post-processing library for HuxerUI. It provides an editable timeline, Flecs scene evaluation, GPU compositing, file codecs, vision analysis, and PAG animation. See the [architecture and milestone verification](docs/architecture.md) for the wider design and detailed checks.

## Targets

| Target | Responsibility |
| --- | --- |
| `AVKit::Core` | Projects, edit recipes, Flecs scene evaluation, tracking association, and the render plan contract. |
| `AVKit::Media` | Lib-Media codec adapter, file readers, timeline sampling, thumbnails, waveforms, and MP4 output. |
| `AVKit::FFmpeg` | FFmpeg software codec provider for the file profile. |
| `AVKit::Bgfx` | Shared GPU compositor with platform shader binaries and SDK texture transfer. |
| `AVKit::NCNN` | Cross-platform CPU provider for face, body, and hand landmarks and portrait segmentation. |
| `AVKit::PAG` | PAG animation, editable text and image slots, and platform transition interop. |
| `HuxerUI::AVKit` | HuxerUI application hook and the combined AVKit library targets. |

Core and the media orchestration use public HuxerUI and Lib-Media types. The file profile accepts MP4 or MOV with 8-bit SDR H.264 or HEVC video and optional AAC-LC audio. The workbench integrates file editing on macOS and Android. Camera capture is wired on macOS, iOS, Android, and Windows. [Platform plan](docs/architecture.md#external-video-frames)

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

Use CMake 3.21 or newer and a HuxerUI SDK or source checkout selected through `HUXERUI_HOME`. The build uses the pinned Lib-Media Git revision by default. Set `AVKIT_LIB_MEDIA_SOURCE_DIR` to use a local checkout.

Each platform configures the same AVKit targets. Supply FFmpeg and libpag built for the target, including the selected architecture, ABI, or WebAssembly toolchain. Point `PKG_CONFIG_PATH` at the target FFmpeg installation and supply `AVKIT_PAG_INCLUDE_DIR` and `AVKIT_PAG_LIBRARY` for the matching libpag SDK. ncnn and bgfx are built from pinned sources. An application can link an individual AVKit target or `HuxerUI::AVKit` for the full set.

To build just the Core target after configuring the project, run the following commands.

```sh
cmake -S . -B build/core \
  -DAVKIT_PAG_INCLUDE_DIR="/path/to/libpag/include" \
  -DAVKIT_PAG_LIBRARY="/path/to/target/libpag"
cmake --build build/core --target avkit_core
```

Capture on macOS, iOS, Android, and Windows requires Lib-Camera and Lib-AudioCapture. Place the source checkouts next to Lib-AVKit or set `AVKIT_LIB_CAMERA_SOURCE_DIR` to the Camera checkout. The recording page supports JPEG photos, microphone audio, live voice processing, pause and resume, and saving a copy. iOS, Android, and Windows use Camera's H.264/AAC MP4 recording with the AudioCapture PCM processor. The macOS live recording path includes the AVKit bgfx sticker compositor and codec selection.

The macOS workbench also builds file editing, export, PAG, and ncnn. Point `PKG_CONFIG_PATH` at the target-compatible FFmpeg installation described in the [file workflow](docs/architecture.md#macos-file-media-milestone), and supply matching libpag headers and the macOS library.

```sh
cmake -S examples/preview -B build/preview \
  -DAVKIT_PAG_INCLUDE_DIR="/path/to/libpag/include" \
  -DAVKIT_PAG_LIBRARY="/path/to/libpag.framework/libpag"
cmake --build build/preview --target example_av_kit

preview="$PWD/build/preview/example_av_kit.app/Contents/MacOS/example_av_kit"
"$preview" --verify
"$preview" --timeline-verify-tracking "$PWD/build/tracking-check"
```

The Android workbench imports MP4 or MOV files in the file profile, plays and seeks the timeline, and exports BT.709 H.264/AAC MP4 through the system file picker. Lib-Media handles platform decoding and passes bgfx GLES composites to the MediaCodec AVC encoder surface. On a recoverable video encoder failure, export restarts from the beginning with FFmpeg and publishes the finalized MP4. Android export uses FFmpeg AAC-LC so the MP4 edit list carries exact priming and padding.

Build static, position-independent FFmpeg for each Android ABI you run, with the H.264, HEVC, and AAC decoders and the AAC and `libx264` encoders enabled. Build static, position-independent x264 for the same ABI and API level and make its pkg-config file available when configuring FFmpeg. The `libx264` encoder requires FFmpeg's GPL build option. Put each install prefix under one directory with its Android ABI name. For example, `build/android-ffmpeg/arm64-v8a` contains `include` and `lib`, with `libavcodec.a`, `libavutil.a`, `libswresample.a`, `libx264.a`, and their pkg-config files under `lib`. [FFmpeg x264 configuration](https://ffmpeg.org/general.html#x264)

```sh
AVKIT_ANDROID_FFMPEG_ROOT="$PWD/build/android-ffmpeg" \
AVKIT_PAG_INCLUDE_DIR="/path/to/libpag/include" \
AVKIT_ANDROID_PAG_AAR="/path/to/libpag-4.5.102.aar" \
  huxerui run android --abi arm64-v8a
```

The AAR must contain `libpag.so` and its runtime libraries for every selected ABI. AVKit links the library for the current ABI, and the preview APK packages the same AAR. [libpag release](https://github.com/Tencent/libpag/releases/tag/v4.5.102)

All supported platforms build the bgfx compositor. Cross builds compile the pinned `shaderc` for the host with the host C++ toolchain. Set `AVKIT_BGFX_SHADERC` to reuse an existing host executable built from the same revision. The compositor uses Metal on macOS and iOS, OpenGL ES 3 on Android, WebGL 2 on Web, and D3D11 for Windows builds hosted on Windows. Other Windows builds use Vulkan. Linux uses OpenGL for a current compatible GDK/EGL context and Vulkan for independent rendering. Linux builds use GTK, epoxy, EGL, X11, and XCB development headers. [Compositor API](include/huxerui/avkit/bgfx.h)

The [video texture API](include/huxerui/avkit/video_texture.h) accepts HuxerUI capture textures, Lib-Media platform decoder textures, and FFmpeg-decoded CPU planes. File preview and export retry a rejected decoder texture in FFmpeg at the same source PTS. Timeline rendering applies the same retry to the affected clip when supplied with an input importer. File and timeline export can restart with a CPU encoder when platform encoder transfer fails.

The ncnn provider accepts verified [YuNet](models/yunet/2023mar) and [MediaPipe body and hand](models/mediapipe/body-hand) model packs through the [C++ factories](include/huxerui/avkit/ncnn.h). See the [tracking verification](docs/architecture.md#ncnn-body-and-hand-provider) for the macOS preview, seek, and export commands.

Portrait background replacement uses [MediaPipe Selfie Segmentation](models/mediapipe/selfie-segmentation) by default. Clips can select the optional [PP-HumanSegV2-Lite](models/paddle/pp-humansegv2-lite) pack independently. [Model selection and verification](docs/architecture.md#ncnn-portrait-segmentation)

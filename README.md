# Lib-AVKit

Lib-AVKit is a C++20 audio and video post-processing library for HuxerUI. The initial implementation provides an editable timeline, Flecs scene evaluation, render plans, and optional adapters for file codecs and GPU compositing. See the [architecture and milestone verification](docs/architecture.md) for the wider design and detailed checks.

## Targets

| Target | Responsibility |
| --- | --- |
| `AVKit::Core` | Projects, edit recipes, Flecs scene evaluation, tracking association, and the render plan contract. |
| `AVKit::Media` | Optional Lib-Media codec adapter, file readers, timeline sampling, thumbnails, waveforms, and MP4 output. |
| `AVKit::FFmpeg` | Optional FFmpeg software codec provider for the file profile. |
| `AVKit::Bgfx` | Optional bgfx compositor with a macOS Metal texture bridge. |
| `HuxerUI::AVKit` | HuxerUI library integration and application hook. Also exposes Core. |

Core and the media orchestration use public HuxerUI and Lib-Media types. The file profile accepts MP4 or MOV with H.264 video and AAC-LC audio. The GPU preview, capture, and file examples exercise the macOS integration. The preview project also has application entry points for iOS, Android, Web, Linux, and Windows. [Platform plan](docs/architecture.md#external-video-frames)

## Repository layout

| Path | Contents |
| --- | --- |
| `include/huxerui/` | Public C++ headers. The macOS Metal bridge has its own header under `avkit/macos/`. |
| `src/` | Shared project, ECS, analysis, and media code. `media/codec/` contains provider adapters, `render/bgfx/shaders/` contains shared shader sources, and `support/` contains private helpers. |
| `platform/` | Platform integration and packaging files. The Metal implementation is under `macos/src/`. |
| `cmake/` | Target and dependency setup. `platform/` selects adapter sources, frameworks, and shader profiles. |
| `examples/preview/` | Shared HuxerUI app, timeline, and fixtures in `src/`, with platform entry points and verification under `platform/`. |

In documentation and identifiers, use *platform* for platform adapters, capabilities, and storage. Use *system* for operating-system services and scheduling or when referring to an ECS system. Reserve *native* for terms such as JNI declarations and handles. Name a concrete type when it is known, such as a Metal texture or an FFmpeg frame.

## Build

Use a HuxerUI SDK or source checkout selected through `HUXERUI_HOME` and a Lib-Media source checkout next to Lib-AVKit. Set `AVKIT_LIB_MEDIA_SOURCE_DIR` to select another checkout.

Build the headless Core on the current target platform.

```sh
cmake -S . -B build/core -DAVKIT_ENABLE_BGFX=OFF
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

`AVKIT_ENABLE_BGFX` defaults to on for macOS and off for other target platforms. `AVKIT_ENABLE_FILE_MEDIA` enables the file module and defaults to off. With file media enabled, `AVKIT_ENABLE_FFMPEG=OFF` builds the module with the Lib-Media adapter. The macOS camera and microphone example can be enabled separately with `AVKIT_ENABLE_CAPTURE_PREVIEW`. It uses Lib-Camera and Lib-AudioCapture alongside the macOS bgfx and FFmpeg targets.

# ScreenStreamRTSPPlugin

**Version:** 1.0.0  
**Engine Version:** Unreal Engine 5.5+ (developed on 5.7)  
**Platforms:** Win64, Linux  
**License:** MIT License

## Description

ScreenStreamRTSPPlugin captures an Unreal `SceneCapture2D` render, H.264-encodes it,
and serves it as an **RTSP** stream from an embedded GStreamer `rtsp-server` running
inside the engine — no external media server (MediaMTX, etc.) required. It can
alpha-composite one or more UMG `UUserWidget` overlays onto every frame (HUD labels,
telemetry, annotations) and can change the stream resolution at runtime without
dropping the connection.

It is the RTSP/H.264 sibling of
[ScreenStreamMJPEGPlugin](https://github.com/NikkittaP/ScreenStreamMJPEGPlugin) and
reuses the same SceneCapture front-end.

## Features

- Real-time `SceneCapture2D` capture → H.264 → **RTSP** (`rtsp://host:8554/cam0`)
- **Embedded RTSP server** (GStreamer `gst-rtsp-server`) — self-contained, no proxy
- Hardware (**NVENC** `nvh264enc`) or software (`x264enc`) encoding, selectable
- **Multiple UMG/Slate overlays** composited into the stream (stacked, add-order = z-order)
- **Runtime resolution change** via in-place caps renegotiation (the session stays up;
  the player recovers automatically after a brief glitch)
- Encodes only while a client is connected (no GPU/CPU cost when nobody is watching)
- Blueprint and C++ API
- Low-latency `zerolatency` tuning, regular SPS/PPS so late joiners decode quickly
- Win64 and Linux

## Requirements — GStreamer

This plugin links GStreamer (core, `app`, `rtsp-server`, `rtp`). Install the
**development** runtime before building.

### Windows (MSVC)

1. Install the **GStreamer 1.x MSVC 64-bit *development*** runtime from
   <https://gstreamer.freedesktop.org/download/> (the "complete" / development
   installer, not the MinGW or runtime-only build) — e.g. to `C:\gstreamer`.
2. Ensure the installer set the environment variable
   `GSTREAMER_1_0_ROOT_MSVC_X86_64` (e.g. `C:\gstreamer\1.0\msvc_x86_64\`) and added
   `…\msvc_x86_64\bin` to `PATH`. The `Build.cs` reads that variable; the editor
   needs `bin` on `PATH` at runtime. Restart the IDE/editor after changing them.

### Ubuntu 24.04 / Linux

```bash
sudo apt install -y \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav pkg-config
# NVENC (nvh264enc) lives in gstreamer1.0-plugins-bad (nvcodec) and needs the NVIDIA driver.
```
`Build.cs` resolves include/lib paths via `pkg-config` on Linux (with a hardcoded
fallback for the stock `/usr/lib/x86_64-linux-gnu` layout).

> Verify the encoders are present: `gst-inspect-1.0 x264enc` and `gst-inspect-1.0 nvh264enc`.

## Installation

1. Copy the `ScreenStreamRTSPPlugin` folder into your project's `Plugins` directory.
2. Install GStreamer (see **Requirements** above).
3. Enable the plugin (Edit → Plugins → "ScreenStreamRTSPPlugin") and restart the editor.
4. Regenerate project files and rebuild (a new module is added).

If you use the overlay feature, add `"UMG"` to your module's `PrivateDependencyModuleNames`.

## Usage

### Quick Start

1. Place a **Scene Capture 2D** actor in your level (position/aim it).
2. Place a **StreamManagerRTSP** actor (Place Actors → All Classes).
3. Select it and set, in Details:
   - `Capture Component` → your Scene Capture 2D
   - `Frame Width` / `Frame Height` (default 1920×1080)
   - `Server Port` (default 8554), `Mount Point` (default `/cam0`)
   - `Target FPS` (30), `Target Bitrate Kbps` (8000)
   - `Use Hardware Encoder` (NVENC; uncheck for `x264enc`)
4. Press **Play**. Capture/encode starts automatically the moment a client connects
   (you do **not** need to call capture each tick — the actor self-drives while a
   client is watching).
5. Open `rtsp://localhost:8554/cam0`.

### Accessing the stream

- **GStreamer (low latency):**
  `gst-launch-1.0 rtspsrc location=rtsp://localhost:8554/cam0 latency=50 ! decodebin ! autovideosink`
- **ffplay:** `ffplay -fflags nobuffer -flags low_delay rtsp://localhost:8554/cam0`
- **VLC:** Open Network Stream → `rtsp://localhost:8554/cam0` (raise/lower the network
  caching value to trade latency for smoothness)

> RTSP clients default to a ~2 s jitter buffer. The producer is already low-latency;
> set a small client-side latency (e.g. `latency=50`) for near-real-time playback.
> For remote access, replace `localhost` with the machine's reachable IP and open
> TCP `8554` (and the UDP RTP range if you use UDP transport).

### Overlays (multiple)

The stream composites any number of `UUserWidget` overlays (stacked into one
`SOverlay`; add order = z-order). The widgets are rendered off-screen into the frame —
do **not** add them to the viewport.

| Function | Description |
|----------|-------------|
| `AddOverlayWidget(UUserWidget*)` | Add an overlay (composited into the stream). |
| `RemoveOverlayWidget(UUserWidget*)` | Remove a previously added overlay. |
| `ClearOverlayWidgets()` | Remove all overlays. |
| `SetOverlayWidget(UUserWidget*)` | Back-compat convenience: clear, then add one. |

```cpp
// Use a DEDICATED instance per stream — a widget can only have one parent, so don't
// reuse one you AddToViewport. Design it as a full-screen canvas at the stream size.
UUserWidget* Hud = CreateWidget<UUserWidget>(GetWorld(), MyHudClass);
StreamManagerRTSP->AddOverlayWidget(Hud);
```
- The widget background must be transparent (alpha 0) so only its elements appear.
- `OverlayRefreshInterval` (default 1) controls overlay GPU-readback frequency in frames.

### Runtime resolution change

`SetStreamResolution(NewWidth, NewHeight)` resizes the capture + stream in place: it
renegotiates the live appsrc caps, the encoder emits a fresh keyframe, and connected
players recover automatically after a short glitch (no reconnect). Dimensions are
clamped and rounded to even (H.264 requirement). No-op when unchanged.

### C++ quick reference

```cpp
#include "StreamManagerRTSP.h"

AStreamManagerRTSP* S = GetWorld()->SpawnActor<AStreamManagerRTSP>();
S->CaptureComponent     = MySceneCapture2D;
S->FrameWidth           = 1920;
S->FrameHeight          = 1080;
S->ServerPort           = 8554;
S->MountPoint           = TEXT("/cam0");
S->TargetFPS            = 30;
S->TargetBitrateKbps    = 8000;
S->bUseHardwareEncoder  = true;
// (the actor starts its RTSP server in BeginPlay and self-drives capture)
```

## Configuration reference

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `ServerPort` | int32 | 8554 | RTSP server TCP port |
| `MountPoint` | FString | `/cam0` | RTSP path; URL = `rtsp://host:Port+MountPoint` |
| `FrameWidth` / `FrameHeight` | int32 | 1920 / 1080 | Capture + stream resolution |
| `TargetFPS` | int32 | 30 | Capture/stream frame rate |
| `TargetBitrateKbps` | int32 | 8000 | Encoder target bitrate (kbps) |
| `bUseHardwareEncoder` | bool | true | NVENC (`nvh264enc`) vs `x264enc` |
| `CaptureComponent` | ASceneCapture2D* | null | Scene Capture 2D to stream |
| `OverlayRefreshInterval` | int32 | 1 | Overlay GPU readback frequency (frames) |
| `VerboseLogging` | bool | false | Verbose log output |

## Troubleshooting

- **Build error: "GStreamer not found … GSTREAMER_1_0_ROOT_MSVC_X86_64"** — install the
  MSVC *development* runtime and restart the IDE so it inherits the variable.
- **Editor can't load `gst*-1.0-0.dll`** — add `…\msvc_x86_64\bin` to `PATH`, restart.
- **Log: "no element x264enc/nvh264enc"** — set `GST_PLUGIN_PATH` to
  `…\msvc_x86_64\lib\gstreamer-1.0` and restart (usually auto-resolved).
- **`gst_rtsp_server_attach failed (port in use)`** — change `ServerPort`.
- **Black / no frames** — ensure the `SceneCapture2D` renders each frame
  (`bCaptureEveryFrame`); the plugin sets it, but a custom capture setup may override it.
- **NVENC errors / no NVIDIA GPU** — set `bUseHardwareEncoder = false` (`x264enc`).
- **Logs** — `Log LogStreamRTSP Verbose` in the console.

## Packaged builds

`Build.cs` stages the GStreamer `bin\*.dll` next to the executable on Windows, but the
GStreamer **plugins** (`lib/gstreamer-1.0`) are not yet bundled. For a packaged build,
install the GStreamer runtime on the target machine (same version, `bin` on `PATH`), or
extend the staging to include the plugin DLLs and set `GST_PLUGIN_PATH` at runtime.

## Based on

- [UnrealImageCapture](https://github.com/TimmHess/UnrealImageCapture) — SceneCapture2D
  capture/readback pattern (shared with the MJPEG sibling plugin).
- [GStreamer](https://gstreamer.freedesktop.org/) — H.264 encoding and `rtsp-server`.

## License

This project is licensed under the [MIT License](LICENSE).

## Support

For support or inquiries, please contact [Nikita Petrov](mailto:nikitapetroff@gmail.com).

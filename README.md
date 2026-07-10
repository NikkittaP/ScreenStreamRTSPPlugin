# ScreenStreamRTSPPlugin

**Version:** 1.2.0  
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

Optionally, the same frames can also be published over **WebRTC** using **WHIP**
(WebRTC-HTTP Ingestion Protocol) to any WHIP ingest endpoint (e.g. a LiveKit
Ingress). This is additive and **off by default**, so RTSP is unaffected unless you
turn it on — see [WebRTC / WHIP publishing](#webrtc--whip-publishing-optional).

## Features

- Real-time `SceneCapture2D` capture → H.264 → **RTSP** (`rtsp://host:8554/cam0`)
- **Embedded RTSP server** (GStreamer `gst-rtsp-server`) — self-contained, no proxy
- **Optional WebRTC publishing** over **WHIP** (additive — RTSP keeps working): the same
  frames go to any WHIP ingest (e.g. a LiveKit Ingress) via `whipclientsink`, which
  auto-encodes H.264 and runs congestion control for adaptive bitrate
- Hardware (**NVENC**) or software (`x264enc`) encoding, selectable; the NVENC variant
  is auto-picked (`nvautogpuh264enc` → `nvcudah264enc` → `nvh264enc`) with automatic
  fallback to `x264enc` when no NVENC factory is available (e.g. GPU-less host)
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

### Ubuntu 24.04 / Linux (native build)

```bash
sudo apt install -y \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav pkg-config
# NVENC (nvh264enc) lives in gstreamer1.0-plugins-bad (nvcodec) and needs the NVIDIA driver.
```
When building **on** Linux, `Build.cs` resolves include/lib paths via `pkg-config`
(with a hardcoded fallback for the stock `/usr/lib/x86_64-linux-gnu` layout) — these
`-dev` packages are all it needs.

> Verify the encoders are present: `gst-inspect-1.0 x264enc` and `gst-inspect-1.0 nvh264enc`.

### Cross-compiling for Linux from a Windows host

A Windows machine cannot use `pkg-config` or `/usr` to find the Linux GStreamer, so
for a cross-compiled Linux target the plugin links against a **prebuilt GStreamer tree
bundled inside the plugin** at `ThirdParty/GStreamerLinux/`. `Build.cs` auto-detects it
(priority 1, ahead of `pkg-config`); if you cross-compile without it the build fails
loudly with the path it expected.

GStreamer/GLib are **pure C** — their public ABI is C (glibc), not C++ — so the stock
Ubuntu `.so` files link cleanly from Unreal's clang/libc++ toolchain. No libc++ rebuild
and no custom toolchain image are needed: just extract the dev libs from an Ubuntu 24.04
install. Expected layout:

```
ThirdParty/GStreamerLinux/
  VERSION                                  # e.g. 1.24.2-ubuntu24.04 (informational)
  include/
    gstreamer-1.0/                         # gst/*.h
    glib-2.0/                              # glib.h, gobject/*, ...
  lib/
    libgstreamer-1.0.so  libgstapp-1.0.so  libgstrtspserver-1.0.so
    libgstrtp-1.0.so     libgobject-2.0.so libglib-2.0.so
    glib-2.0/include/glibconfig.h          # generated GLib config header
```

> The bundle is **not** committed — `ThirdParty/` is git-ignored (third-party binaries
> are not vendored). Each developer/CI produces it once per GStreamer revision.

**Producing the bundle from Ubuntu 24.04** (run on a Linux box, in WSL, or in a
throwaway `ubuntu:24.04` Docker container — no NVIDIA/GPU needed, dev headers only):

```bash
apt-get update
apt-get install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
                   libgstrtspserver-1.0-dev libglib2.0-dev

mkdir -p out/include out/lib/glib-2.0/include
# Headers
cp -r /usr/include/gstreamer-1.0 out/include/
cp -r /usr/include/glib-2.0      out/include/
cp /usr/lib/x86_64-linux-gnu/glib-2.0/include/glibconfig.h out/lib/glib-2.0/include/
# Core libraries — copy the real object (follow the symlink) as lib<name>.so.
# The linker records each .so's embedded SONAME (libgstreamer-1.0.so.0, ...) as the
# runtime DT_NEEDED regardless of the on-disk name, so lib<name>.so is enough to link.
for n in gstreamer-1.0 gstapp-1.0 gstrtspserver-1.0 gstrtp-1.0 gobject-2.0 glib-2.0; do
  cp -L "/usr/lib/x86_64-linux-gnu/lib$n.so" "out/lib/lib$n.so"
done
echo "$(gst-inspect-1.0 --version | awk '/version/{print $NF}')-ubuntu24.04" > out/VERSION
```

Then move `out/{include,lib,VERSION}` into `ThirdParty/GStreamerLinux/` on the Windows
host (e.g. copy the folder out of WSL/Docker, or `tar -xzf` it there — `tar` ships with
Windows 10+). `Build.cs` picks it up automatically on the next cross-build.

> The bundle only satisfies **link time**. The deployed Ubuntu box still needs the real
> GStreamer libraries and element plugins installed at runtime — see
> [Packaged / deployed builds](#packaged--deployed-builds).

### WebRTC / WHIP (optional — only if you use `bEnableWebRtc`)

WHIP publishing additionally needs the **`whipclientsink`** element (from
[`gst-plugins-rs`](https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs)) and the
**`webrtcbin`** element it builds on (from `gst-plugins-bad`). Plain RTSP does **not**
need either — if they are missing, WHIP start fails (logged) and RTSP is unaffected.

- **Windows:** the official GStreamer 1.22+ MSVC *development* installer ships both.
  Verify: `gst-inspect-1.0 whipclientsink` and `gst-inspect-1.0 webrtcbin`.
- **Ubuntu/Linux:** `webrtcbin` comes with `gstreamer1.0-plugins-bad`; `whipclientsink`
  ships in the Rust plugin set (`gstreamer1.0-plugins-rs` where packaged, or built from
  `gst-plugins-rs`). Verify with the same two `gst-inspect-1.0` commands.

> `webrtcbin` needs `libnice >= 0.1.23`. Some older/stripped distributions build
> GStreamer's webrtc without a usable `nice` backend, in which case `webrtcbin` is absent
> even though `whipclientsink` is present — verify **both** elements resolve.

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
clamped and rounded to even (H.264 requirement). No-op when unchanged. The change
applies to the WebRTC/WHIP session too, if enabled.

### WebRTC / WHIP publishing (optional)

The same captured frames can be published over **WebRTC** using **WHIP**
(WebRTC-HTTP Ingestion Protocol), *in addition* to RTSP. This is fully additive: RTSP
keeps working exactly as before, and WebRTC stays off unless you enable it.

Enable it on the `StreamManagerRTSP` actor:

- `bEnableWebRtc` (default **false**) — turn on the WHIP publish.
- `WhipUrl` — the full WHIP ingest URL **including the stream key**, e.g.
  `http://host:8080/w/<STREAM_KEY>`. Empty disables the publish even when
  `bEnableWebRtc` is `true`.

```cpp
S->bEnableWebRtc = true;
S->WhipUrl       = TEXT("http://host:8080/w/<STREAM_KEY>");
```

Point `WhipUrl` at any WHIP-compatible ingest endpoint — for example a
[LiveKit](https://livekit.io/) Ingress (its `provision.py` / `up.sh` prints the exact
`http://host:8080/w/<KEY>` URL), or another WHIP server. Viewers subscribe on the
media-server / SFU side; the plugin only publishes.

Behaviour notes:

- Unlike RTSP — which encodes only while a client is connected — the WHIP pipeline
  publishes **continuously** while enabled (a WHIP ingest has no "client connected"
  signal to gate on). `IsStreaming()` is `true` when an RTSP client is connected **or**
  WHIP is publishing, and it drives the capture cadence and overlay refresh.
- If `whipclientsink` / `webrtcbin` are missing, or the ingest rejects the connection,
  WHIP fails and logs it — **RTSP is never affected**.
- Pipeline: `appsrc(BGRA) → queue → videoconvert → whipclientsink`. `whipclientsink`
  auto-encodes to H.264 and runs Google Congestion Control (adaptive bitrate), so the
  plugin feeds it raw frames and never pre-encodes. See
  [Requirements → WebRTC / WHIP](#webrtc--whip-optional--only-if-you-use-benablewebrtc).

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
| `bUseHardwareEncoder` | bool | true | NVENC (auto-picked, see Features) vs `x264enc` |
| `bEnableWebRtc` | bool | false | Also publish over WebRTC/WHIP (additive; RTSP untouched) |
| `WhipUrl` | FString | (empty) | Full WHIP ingest URL incl. stream key, e.g. `http://host:8080/w/<KEY>` |
| `CaptureComponent` | ASceneCapture2D* | null | Scene Capture 2D to stream |
| `OverlayRefreshInterval` | int32 | 1 | Overlay GPU readback frequency (frames) |
| `VerboseLogging` | bool | false | Verbose log output |

## Troubleshooting

- **Build error: "GStreamer not found … GSTREAMER_1_0_ROOT_MSVC_X86_64"** — install the
  MSVC *development* runtime and restart the IDE so it inherits the variable.
- **Editor can't load `gst*-1.0-0.dll`** — add `…\msvc_x86_64\bin` to `PATH`, restart.
- **Log: "no element x264enc/nvh264enc" or "appsrc 'src' not found"** — the element
  plugins (`lib/gstreamer-1.0`) aren't on the search path. The plugin resolves this
  automatically (staged folder, then `GSTREAMER_1_0_ROOT_MSVC_X86_64`); if it still
  fails, set `GST_PLUGIN_PATH` to `…\msvc_x86_64\lib\gstreamer-1.0` and restart.
- **`gst_rtsp_server_attach failed (port in use)`** — change `ServerPort`.
- **Black / no frames** — ensure the `SceneCapture2D` renders each frame
  (`bCaptureEveryFrame`); the plugin sets it, but a custom capture setup may override it.
- **NVENC errors / no NVIDIA GPU** — the producer auto-falls back to `x264enc` when no
  NVENC factory is found; force software with `bUseHardwareEncoder = false`. Legacy
  `nvh264enc` may fail at runtime on recent drivers (5xx+) with *"Selected preset not
  supported"* — the encoder auto-selection prefers `nvautogpuh264enc`/`nvcudah264enc`
  to avoid that.
- **Logs** — `Log LogStreamRTSP Verbose` in the console (WebRTC lines are tagged `[WHIP]`).
- **WebRTC: "pipeline missing appsrc 'src' or 'whip' sink — is whipclientsink installed?"**
  — `whipclientsink` (`gst-plugins-rs`) or `webrtcbin` (`gst-plugins-bad`) isn't installed
  or isn't on the plugin search path (see [Requirements → WebRTC](#webrtc--whip-optional--only-if-you-use-benablewebrtc)). RTSP is unaffected.
- **WebRTC: nothing appears at the viewer / "WHIP pipeline error … 503 … ICE"** — the WHIP
  ingest isn't reachable or isn't provisioned: check `WhipUrl`, the ingest server, and that
  the media UDP ports are open end-to-end. RTSP is unaffected.

## Packaged / deployed builds

The linkable core libraries (`bin\*.dll` / `lib*.so`) are **not** the same files as the
GStreamer *element factories* the pipeline needs at runtime (`appsrc`, `videoconvert`,
`x264enc`, `nvh264enc`, `h264parse`, `rtph264pay`, ...). Those live in separate plugin
modules under `lib/gstreamer-1.0`. If they are missing, the server registers zero plugins
and media setup fails with `appsrc 'src' not found in pipeline`.

### Windows (packaged game/server target)

`Build.cs` stages, next to the executable:
- the GStreamer `bin\*.dll` core libraries, **and**
- the element plugins from `lib/gstreamer-1.0` into a sibling `gstreamer-1.0\` folder
  (non-Editor targets only — the Editor falls back to the dev install via
  `GSTREAMER_1_0_ROOT_MSVC_X86_64`, so it skips the ~110 MB per-build copy).

At runtime `RTSPStreamerImpl.cpp` points `GST_PLUGIN_SYSTEM_PATH_1_0` at that staged
`gstreamer-1.0\` folder (falling back to the dev install), so a packaged Windows build is
self-contained — no GStreamer install required on the target.

### Linux (deployed Ubuntu 24.04 target)

A cross-compiled binary links against the bundle stubs only; the real `.so` files **and**
the element plugins must be installed on the deploy box, or it won't even start
(`error while loading shared libraries: libgstrtspserver-1.0.so.0`). Install the
**runtime** packages (no `-dev` needed at runtime):

```bash
sudo apt update
sudo apt install -y \
  libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libgstrtspserver-1.0-0 \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-tools
```

> `libgstrtspserver-1.0-0` is **not** pulled in by the plugin metapackages — list it
> explicitly. On Linux `libgstreamer` loads from `/usr/lib`, so plugin discovery is
> native — the Windows `GST_PLUGIN_PATH` staging is a Windows-only concern. On a GPU-less
> host the `nvh264enc` factory is absent and the producer auto-falls back to `x264enc`.

## Based on

- [UnrealImageCapture](https://github.com/TimmHess/UnrealImageCapture) — SceneCapture2D
  capture/readback pattern (shared with the MJPEG sibling plugin).
- [GStreamer](https://gstreamer.freedesktop.org/) — H.264 encoding and `rtsp-server`.

## License

This project is licensed under the [MIT License](LICENSE).

## Support

For support or inquiries, please contact [Nikita Petrov](mailto:nikitapetroff@gmail.com).

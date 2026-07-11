// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT
//
// Pimpl wrapper around an embedded GStreamer WHIP-publish pipeline:
//
//   appsrc(BGRA) -> queue(leaky) -> videoconvert -> whipclientsink
//
// whipclientsink (gst-plugins-rs) auto-encodes to H.264 and runs Google
// Congestion Control (rtpgccbwe) for adaptive bitrate, so we feed it RAW BGRA
// and never pre-encode. Unlike the RTSP streamer — whose appsrc only exists
// while a client is connected — this pipeline is built and set to PLAYING as
// soon as a WHIP endpoint is known and then publishes CONTINUOUSLY: a LiveKit
// Ingress has no "client connected" signal (viewers subscribe on the SFU side),
// so there is nothing to gate on.
//
// Frames are pushed in from the UE game thread via PushFrame(); a worker thread
// owns the pipeline and polls its bus for async errors.
//
// IMPORTANT: this header and its .cpp must stay **free of Unreal Engine headers**
// for the same reason as RTSPStreamerImpl.* — GLib's `typedef struct _GError
// GError;` collides with UE's global `FOutputDeviceError* GError`, so the
// GStreamer translation unit cannot include CoreMinimal/UObject. Logging is
// routed back to UE via SetLogCallback().

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

struct FWhipGstState;   // opaque holder for the GStreamer objects (in the .cpp)

class FWhipStreamerImpl
{
public:
	struct FSettings
	{
		// Full WHIP ingest URL INCLUDING the stream key, e.g.
		// http://host:8080/w/<STREAM_KEY> — exactly what livekit-whip's
		// provision.py / up.sh prints. Passed verbatim to whipclientsink's
		// signaller::whip-endpoint. Required (empty → Start() fails).
		std::string WhipEndpoint;
		// Optional Bearer token (signaller::auth-token) for deployments that pass
		// the stream key out-of-band instead of in the URL path. Empty = unused.
		std::string AuthToken;
		int         Width = 1920;
		int         Height = 1080;
		int         Fps = 30;
	};

	// Level: 0=Log, 1=Warning, 2=Error, 3=Verbose. Msg is UTF-8.
	using FLogCallback = void (*)(int Level, const char* Msg);
	// Set a process-wide sink so this UE-free TU can surface logs in the UE log.
	static void SetLogCallback(FLogCallback Callback);

	FWhipStreamerImpl();
	~FWhipStreamerImpl();

	// Build the pipeline and start publishing. Returns false (and logs) if the
	// endpoint is empty, the pipeline cannot be created, or it fails to reach
	// PLAYING. On failure RTSP is unaffected. Thread-safe w.r.t. PushFrame.
	bool Start(const FSettings& InSettings);
	void Stop();

	// Push one BGRA8 frame. `SizeBytes` must equal Width*Height*4. Thread-safe.
	// A no-op once the pipeline has torn down (e.g. after a fatal WHIP error).
	void PushFrame(const uint8_t* Bgra, int32_t SizeBytes);

	// Zero-copy variant: wraps the caller's BGRA memory into the GstBuffer
	// instead of allocating + memcpy. `ReleaseOwner(Owner)` is invoked exactly
	// once when this pipeline is done with the memory — including every
	// early-drop path (pipeline down, push failure). Thread-safe.
	void PushFrameZeroCopy(const uint8_t* Bgra, int32_t SizeBytes,
	                       void* Owner, void (*ReleaseOwner)(void*));

	// Change the publish resolution at runtime WITHOUT tearing down the WHIP
	// session: renegotiates the live appsrc caps (the auto-encoder reconfigures
	// and emits a fresh keyframe — a brief glitch, the player auto-recovers).
	// Thread-safe. The caller must start pushing Width*Height*4 frames once this
	// returns.
	void SetResolution(int Width, int Height);

	// True while the pipeline is publishing. Flips to false on a fatal bus error
	// or EOS (so the frame producer can stop feeding it). Thread-safe.
	bool IsRunning() const { return bRunning.load(); }

private:
	FWhipGstState* P = nullptr;   // GStreamer objects; owned by this instance
	std::atomic<bool> bRunning{false};
};

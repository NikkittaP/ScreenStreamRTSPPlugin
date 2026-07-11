// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT
//
// Pimpl wrapper around an embedded GStreamer rtsp-server pipeline:
//
//   appsrc(BGRA) -> queue(leaky) -> videoconvert -> H.264 encoder
//                -> h264parse -> rtph264pay(name=pay0)  ──>  GstRTSPServer
//
// Frames are pushed in from the UE game thread via PushFrame(); the GLib main
// loop that drives the RTSP server runs on its own worker thread.
//
// IMPORTANT: this header and its .cpp must stay **free of Unreal Engine headers**.
// GLib's `typedef struct _GError GError;` collides with UE's global
// `FOutputDeviceError* GError`, so the GStreamer translation unit cannot include
// CoreMinimal/UObject. Logging is routed back to UE via SetLogCallback().

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

struct FRtspGstState;   // opaque holder for the GStreamer objects (in the .cpp)

class FRTSPStreamerImpl
{
public:
	struct FSettings
	{
		int         Port = 8554;
		std::string MountPoint = "/cam0";   // RTSP path, e.g. rtsp://host:8554/cam0
		int         Width = 1920;
		int         Height = 1080;
		int         Fps = 30;
		int         BitrateKbps = 8000;
		bool        bUseHardwareEncoder = true;  // NVENC (nvh264enc) when available
	};

	// Level: 0=Log, 1=Warning, 2=Error, 3=Verbose. Msg is UTF-8.
	using FLogCallback = void (*)(int Level, const char* Msg);
	// Set a process-wide sink so this UE-free TU can surface logs in the UE log.
	static void SetLogCallback(FLogCallback Callback);

	FRTSPStreamerImpl();
	~FRTSPStreamerImpl();

	bool Start(const FSettings& InSettings);
	void Stop();

	// Push one BGRA8 frame. `SizeBytes` must equal Width*Height*4. Thread-safe.
	// Dropped cheaply when no RTSP client is connected.
	void PushFrame(const uint8_t* Bgra, int32_t SizeBytes);

	// Zero-copy variant: wraps the caller's BGRA memory into the GstBuffer
	// instead of allocating + memcpy. `ReleaseOwner(Owner)` is invoked exactly
	// once when this pipeline is done with the memory — including every
	// early-drop path (no client, push failure). Thread-safe.
	void PushFrameZeroCopy(const uint8_t* Bgra, int32_t SizeBytes,
	                       void* Owner, void (*ReleaseOwner)(void*));

	// Change the stream resolution at runtime WITHOUT tearing down the RTSP
	// session: updates the live appsrc caps (the H.264 encoder renegotiates and
	// emits a fresh SPS/PPS keyframe — a brief glitch, the player auto-recovers)
	// and the caps used for future client connections. Thread-safe. The caller
	// must start pushing Width*Height*4 frames once this returns.
	void SetResolution(int Width, int Height);

	bool IsRunning() const { return bRunning.load(); }

	// True while at least one RTSP client is connected (appsrc built). Thread-safe.
	bool HasClient() const;

private:
	FRtspGstState* P = nullptr;   // GStreamer objects; owned by this instance
	std::atomic<bool> bRunning{false};
};

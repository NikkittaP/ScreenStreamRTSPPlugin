// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT
//
// Pure GStreamer translation unit — NO Unreal Engine headers (see WhipStreamerImpl.h
// for the GError-clash rationale). Logs are forwarded via the SetLogCallback sink.
//
// Mirrors RTSPStreamerImpl.cpp deliberately: same GStreamer-init / plugin-path
// handling, same appsrc PTS scheme, same SetResolution renegotiation. The
// difference is the sink (whipclientsink instead of rtph264pay + GstRTSPServer)
// and the lifecycle (the pipeline runs for the whole session, not per client).

// MSVC flags std::getenv (and friends) as "unsafe" (C4996). This TU uses plain
// std::getenv deliberately (read-only, no buffer); opt out for the whole file.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#  define _CRT_SECURE_NO_WARNINGS
#endif

#include "WhipStreamerImpl.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

// GStreamer/GLib headers use bare `#if __GNUC__` / `__clang_major__` etc. and
// other constructs MSVC flags (C4668 "undefined macro → 0", conversions, ...).
// UE normally hides these via THIRD_PARTY_INCLUDES_START, but this TU is kept
// UE-header-free, so silence them with a raw MSVC warning push (level 0).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable: 4668)   // undefined preprocessor macro replaced with 0
#pragma warning(disable: 4005)   // macro redefinition
#endif
// Win32 (GetModuleHandle/GetModuleFileName) to locate the loaded GStreamer core
// DLL. Pulled in before <gst/gst.h> (glib includes windows.h on Win) with
// NOMINMAX so the min/max macros never shadow the std::max used below.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ── Logging sink ─────────────────────────────────────────────────────────────
namespace
{
	FWhipStreamerImpl::FLogCallback GLogCb = nullptr;

	void LogMsg(int Level, const char* Fmt, ...)
	{
		char Buf[1024];
		va_list Args;
		va_start(Args, Fmt);
		vsnprintf(Buf, sizeof(Buf), Fmt, Args);
		va_end(Args);
		if (GLogCb)
		{
			GLogCb(Level, Buf);
		}
		else
		{
			std::fprintf(stderr, "[WHIP] %s\n", Buf);
		}
	}

	// On Windows the element factories (appsrc, videoconvert, whipclientsink, ...)
	// live in plugin modules under lib/gstreamer-1.0, which is NOT on GStreamer's
	// default search path once the core DLL is loaded from Binaries/Win64. Point
	// GStreamer at the real plugin directory before gst_init. (No-op on Linux,
	// where the system plugin dir is found by default — see the handoff doc.)
	// This duplicates RTSPStreamerImpl.cpp's logic so each GStreamer TU stays
	// self-contained and UE-header-free; gst_init is idempotent, so two callers
	// are safe.

	// Directory of the loaded gstreamer-1.0-0.dll (empty if not found).
	static std::string FindGstDllDir()
	{
#if defined(_WIN32)
		if (HMODULE Mod = GetModuleHandleA("gstreamer-1.0-0.dll"))
		{
			char Path[MAX_PATH] = {};
			const DWORD N = GetModuleFileNameA(Mod, Path, static_cast<DWORD>(sizeof(Path)));
			if (N > 0 && N < sizeof(Path))
			{
				const std::string Full(Path, N);
				const size_t Slash = Full.find_last_of("\\/");
				return (Slash == std::string::npos) ? std::string() : Full.substr(0, Slash);
			}
		}
#endif
		return std::string();
	}

	static void EnsureGstPluginPath()
	{
#if defined(_WIN32)
		namespace fs = std::filesystem;

		std::vector<std::string> Candidates;

		const std::string DllDir = FindGstDllDir();
		if (!DllDir.empty())
		{
			Candidates.push_back(DllDir + "\\gstreamer-1.0");          // flat staged layout
			Candidates.push_back(DllDir + "\\..\\lib\\gstreamer-1.0"); // DLL loaded from <root>\bin
		}

		if (const char* Root = std::getenv("GSTREAMER_1_0_ROOT_MSVC_X86_64"))
		{
			std::string R(Root);
			while (!R.empty() && (R.back() == '\\' || R.back() == '/'))
			{
				R.pop_back();
			}
			if (!R.empty())
			{
				Candidates.push_back(R + "\\lib\\gstreamer-1.0");
			}
		}

		for (const std::string& Cand : Candidates)
		{
			std::error_code Ec;
			if (fs::is_directory(Cand, Ec))
			{
				_putenv_s("GST_PLUGIN_PATH", Cand.c_str());
				_putenv_s("GST_PLUGIN_SYSTEM_PATH_1_0", Cand.c_str());
				LogMsg(0, "GStreamer plugin path: %s", Cand.c_str());
				return;
			}
		}

		LogMsg(2, "No GStreamer plugin directory found (looked next to the loaded DLL "
			"and under GSTREAMER_1_0_ROOT_MSVC_X86_64); 'whipclientsink' will be missing.");
#endif
	}

	void EnsureGstInit()
	{
		static std::once_flag Once;
		std::call_once(Once, []()
		{
			EnsureGstPluginPath();

			GError* Err = nullptr;
			if (!gst_init_check(nullptr, nullptr, &Err))
			{
				LogMsg(2, "gst_init_check failed: %s", Err ? Err->message : "unknown");
				if (Err) g_error_free(Err);
			}
			else
			{
				guint Major = 0, Minor = 0, Micro = 0, Nano = 0;
				gst_version(&Major, &Minor, &Micro, &Nano);
				LogMsg(0, "GStreamer initialised (%u.%u.%u)", Major, Minor, Micro);
			}
		});
	}
}

void FWhipStreamerImpl::SetLogCallback(FLogCallback Callback)
{
	GLogCb = Callback;
}

// ── Internal state (all GStreamer objects live here) ─────────────────────────
struct FWhipGstState
{
	FWhipStreamerImpl::FSettings Settings;

	std::thread       LoopThread;
	GstElement*       Pipeline = nullptr;
	std::atomic<bool> bStopThread{false};   // set by Stop() to break the bus poll

	std::mutex   AppSrcMutex;
	GstElement*  AppSrc = nullptr;       // appsrc of the live pipeline (ref-held)
	uint64_t     FrameCount = 0;
};

FWhipStreamerImpl::FWhipStreamerImpl() = default;

FWhipStreamerImpl::~FWhipStreamerImpl()
{
	Stop();
}

static std::string BuildLaunchString(const FWhipStreamerImpl::FSettings& S)
{
	const int Fps = std::max(1, S.Fps);

	// The WHIP endpoint URL (which carries the stream key, with '=' / '/' / ':')
	// is set on the sink's signaller PROGRAMMATICALLY after parse — never inside
	// this launch string — so gst_parse_launch never has to quote it.
	char Launch[512];
	std::snprintf(Launch, sizeof(Launch),
		"appsrc name=src is-live=true do-timestamp=false format=time "
		"caps=video/x-raw,format=BGRA,width=%d,height=%d,framerate=%d/1 "
		"! queue max-size-buffers=3 leaky=downstream ! videoconvert n-threads=2 "
		"! whipclientsink name=whip",
		S.Width, S.Height, Fps);

	return std::string(Launch);
}

bool FWhipStreamerImpl::Start(const FSettings& InSettings)
{
	if (P)
	{
		Stop();
	}

	if (InSettings.WhipEndpoint.empty())
	{
		LogMsg(2, "WHIP start aborted: empty WHIP endpoint URL.");
		return false;
	}

	EnsureGstInit();

	P = new FWhipGstState();
	P->Settings = InSettings;

	std::promise<bool> InitPromise;
	std::future<bool>  InitFuture = InitPromise.get_future();

	P->LoopThread = std::thread([this, Prom = std::move(InitPromise)]() mutable
	{
		FWhipGstState* I = P;

		GError* Err = nullptr;
		const std::string Launch = BuildLaunchString(I->Settings);
		LogMsg(0, "WHIP launch pipeline: %s", Launch.c_str());
		LogMsg(0, "WHIP endpoint: %s", I->Settings.WhipEndpoint.c_str());

		I->Pipeline = gst_parse_launch(Launch.c_str(), &Err);
		if (Err)
		{
			LogMsg(2, "WHIP gst_parse_launch error: %s", Err->message);
			g_error_free(Err);
			Err = nullptr;
		}

		bool bOk = (I->Pipeline != nullptr);

		if (bOk)
		{
			GstElement* AppSrc = gst_bin_get_by_name(GST_BIN(I->Pipeline), "src");   // transfer full
			GstElement* Whip   = gst_bin_get_by_name(GST_BIN(I->Pipeline), "whip");  // transfer full

			if (AppSrc && Whip)
			{
				// Set the WHIP endpoint (and optional Bearer token) on the sink's
				// signaller. signaller::whip-endpoint is the GstChildProxy path that
				// the working gst-launch recipe used.
				gst_child_proxy_set(GST_CHILD_PROXY(Whip),
					"signaller::whip-endpoint", I->Settings.WhipEndpoint.c_str(), nullptr);
				if (!I->Settings.AuthToken.empty())
				{
					gst_child_proxy_set(GST_CHILD_PROXY(Whip),
						"signaller::auth-token", I->Settings.AuthToken.c_str(), nullptr);
				}

				// Congestion-control ramp: webrtcsink's GCC estimator starts at a
				// low default bitrate and takes several seconds to ramp — the first
				// seconds of every stream were visibly blocky/stuttery, worst right
				// when the operator grabs PTZ on a fresh stream. Seed the estimator
				// near the steady-state rate and keep a sane floor so it can't drop
				// into slideshow territory on transient loss. Properties are looked
				// up first so older gst-plugins-rs builds without them still work.
				auto SetUintIfPresent = [](GstElement* E, const char* Name, guint Val)
				{
					if (g_object_class_find_property(G_OBJECT_GET_CLASS(E), Name))
					{
						g_object_set(E, Name, Val, nullptr);
					}
				};
				SetUintIfPresent(Whip, "min-bitrate",   1000000u);   // 1 Mbps floor
				SetUintIfPresent(Whip, "start-bitrate", 3000000u);   // skip the slow ramp
				SetUintIfPresent(Whip, "max-bitrate",   6000000u);

				std::lock_guard<std::mutex> Lock(I->AppSrcMutex);
				I->AppSrc = AppSrc;   // keep the ref returned by get_by_name
				I->FrameCount = 0;
			}
			else
			{
				LogMsg(2, "WHIP pipeline missing appsrc 'src' or 'whip' sink — "
					"is whipclientsink installed? (gst-inspect-1.0 whipclientsink)");
				if (AppSrc) { gst_object_unref(AppSrc); }
				bOk = false;
			}

			if (Whip) { gst_object_unref(Whip); }
		}

		if (bOk)
		{
			const GstStateChangeReturn Ret = gst_element_set_state(I->Pipeline, GST_STATE_PLAYING);
			// Live pipelines return ASYNC (negotiation continues in the background);
			// only an outright FAILURE here is fatal. A rejected WHIP POST or
			// ICE/DTLS failure surfaces later as a bus ERROR (handled below).
			if (Ret == GST_STATE_CHANGE_FAILURE)
			{
				LogMsg(2, "WHIP set_state(PLAYING) failed.");
				bOk = false;
			}
		}

		if (bOk)
		{
			bRunning.store(true);
			LogMsg(0, "WHIP publishing started.");
		}

		Prom.set_value(bOk);

		// Bus poll loop: surface async ERROR/EOS/WARNING (e.g. WHIP POST rejected,
		// ICE/DTLS failure, missing rtpgccbwe) and exit promptly when Stop() flips
		// bStopThread. A plain pipeline needs no GMainLoop — whipclientsink drives
		// its own async runtime internally; we only watch the bus.
		if (bOk)
		{
			GstBus* Bus = gst_element_get_bus(I->Pipeline);
			while (!I->bStopThread.load())
			{
				GstMessage* Msg = gst_bus_timed_pop_filtered(Bus, 100 * GST_MSECOND,
					(GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING));
				if (!Msg)
				{
					continue;
				}

				const GstMessageType Type = GST_MESSAGE_TYPE(Msg);
				if (Type == GST_MESSAGE_ERROR)
				{
					GError* MErr = nullptr;
					gchar*  Dbg  = nullptr;
					gst_message_parse_error(Msg, &MErr, &Dbg);
					LogMsg(2, "WHIP pipeline error from %s: %s (%s)",
						GST_OBJECT_NAME(GST_MESSAGE_SRC(Msg)),
						MErr ? MErr->message : "unknown", Dbg ? Dbg : "");
					if (MErr) { g_error_free(MErr); }
					g_free(Dbg);
					gst_message_unref(Msg);
					bRunning.store(false);   // RTSP unaffected; no auto-retry in v1
					break;
				}
				else if (Type == GST_MESSAGE_WARNING)
				{
					GError* MErr = nullptr;
					gchar*  Dbg  = nullptr;
					gst_message_parse_warning(Msg, &MErr, &Dbg);
					LogMsg(1, "WHIP pipeline warning from %s: %s (%s)",
						GST_OBJECT_NAME(GST_MESSAGE_SRC(Msg)),
						MErr ? MErr->message : "unknown", Dbg ? Dbg : "");
					if (MErr) { g_error_free(MErr); }
					g_free(Dbg);
					gst_message_unref(Msg);
				}
				else // GST_MESSAGE_EOS
				{
					LogMsg(1, "WHIP pipeline reached EOS; stopping publish.");
					gst_message_unref(Msg);
					bRunning.store(false);
					break;
				}
			}
			gst_object_unref(Bus);
		}

		// Teardown (runs on both the success and the failure path).
		if (I->Pipeline)
		{
			gst_element_set_state(I->Pipeline, GST_STATE_NULL);
		}
		{
			std::lock_guard<std::mutex> Lock(I->AppSrcMutex);
			if (I->AppSrc) { gst_object_unref(I->AppSrc); I->AppSrc = nullptr; }
		}
		if (I->Pipeline) { gst_object_unref(I->Pipeline); I->Pipeline = nullptr; }
	});

	const bool bStarted = InitFuture.get();
	if (!bStarted)
	{
		Stop();
		return false;
	}

	return true;
}

void FWhipStreamerImpl::Stop()
{
	if (!P)
	{
		return;
	}

	bRunning.store(false);
	P->bStopThread.store(true);   // breaks the bus poll within ~100 ms

	if (P->LoopThread.joinable())
	{
		P->LoopThread.join();
	}

	delete P;
	P = nullptr;
}

void FWhipStreamerImpl::SetResolution(int Width, int Height)
{
	if (!P || Width <= 0 || Height <= 0)
	{
		return;
	}

	std::lock_guard<std::mutex> Lock(P->AppSrcMutex);
	P->Settings.Width  = Width;
	P->Settings.Height = Height;

	if (P->AppSrc)
	{
		// Renegotiate the live appsrc → downstream auto-encoder reconfigures in place.
		GstCaps* Caps = gst_caps_new_simple("video/x-raw",
			"format",    G_TYPE_STRING, "BGRA",
			"width",     G_TYPE_INT,    Width,
			"height",    G_TYPE_INT,    Height,
			"framerate", GST_TYPE_FRACTION, std::max(1, P->Settings.Fps), 1,
			nullptr);
		gst_app_src_set_caps(GST_APP_SRC(P->AppSrc), Caps);
		gst_caps_unref(Caps);

		// Force an IDR at the new resolution immediately (same downstream
		// "GstForceKeyUnit" event the RTSP impl sends) so the encoder stops
		// referencing the old-resolution picture and the viewer recovers fast.
		GstStructure* Fku = gst_structure_new("GstForceKeyUnit",
			"timestamp",    G_TYPE_UINT64,  (guint64)GST_CLOCK_TIME_NONE,
			"stream-time",  G_TYPE_UINT64,  (guint64)GST_CLOCK_TIME_NONE,
			"running-time", G_TYPE_UINT64,  (guint64)GST_CLOCK_TIME_NONE,
			"all-headers",  G_TYPE_BOOLEAN, TRUE,
			"count",        G_TYPE_UINT,    (guint)0,
			nullptr);
		gst_element_send_event(P->AppSrc, gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, Fku));

		LogMsg(0, "WHIP resolution changed to %dx%d (live renegotiation + forced keyframe)", Width, Height);
	}
}

void FWhipStreamerImpl::PushFrame(const uint8_t* Bgra, int32_t SizeBytes)
{
	if (!P || !Bgra || SizeBytes <= 0)
	{
		return;
	}

	std::lock_guard<std::mutex> Lock(P->AppSrcMutex);
	if (!P->AppSrc)
	{
		return;   // pipeline not built yet, or torn down after a fatal error
	}

	GstBuffer* Buffer = gst_buffer_new_allocate(nullptr, SizeBytes, nullptr);
	if (!Buffer)
	{
		return;
	}
	gst_buffer_fill(Buffer, 0, Bgra, SizeBytes);

	const int Fps = std::max(1, P->Settings.Fps);
	GST_BUFFER_PTS(Buffer)      = gst_util_uint64_scale(P->FrameCount, GST_SECOND, Fps);
	GST_BUFFER_DURATION(Buffer) = gst_util_uint64_scale(1, GST_SECOND, Fps);
	P->FrameCount++;

	const GstFlowReturn Ret = gst_app_src_push_buffer(GST_APP_SRC(P->AppSrc), Buffer);
	if (Ret != GST_FLOW_OK && Ret != GST_FLOW_FLUSHING)
	{
		LogMsg(3, "WHIP appsrc push-buffer returned %d", (int)Ret);
	}
}

void FWhipStreamerImpl::PushFrameZeroCopy(const uint8_t* Bgra, int32_t SizeBytes,
                                          void* Owner, void (*ReleaseOwner)(void*))
{
	if (!P || !Bgra || SizeBytes <= 0)
	{
		if (Owner && ReleaseOwner) { ReleaseOwner(Owner); }
		return;
	}

	std::lock_guard<std::mutex> Lock(P->AppSrcMutex);
	if (!P->AppSrc)
	{
		if (Owner && ReleaseOwner) { ReleaseOwner(Owner); }
		return;   // pipeline not built yet, or torn down after a fatal error
	}

	// Wrap the caller's memory read-only; GStreamer calls ReleaseOwner when the
	// last ref of this buffer is dropped (downstream copies on convert anyway).
	GstBuffer* Buffer = gst_buffer_new_wrapped_full(
		GST_MEMORY_FLAG_READONLY,
		const_cast<uint8_t*>(Bgra), SizeBytes, 0, SizeBytes,
		Owner, ReleaseOwner);
	if (!Buffer)
	{
		if (Owner && ReleaseOwner) { ReleaseOwner(Owner); }
		return;
	}

	const int Fps = std::max(1, P->Settings.Fps);
	GST_BUFFER_PTS(Buffer)      = gst_util_uint64_scale(P->FrameCount, GST_SECOND, Fps);
	GST_BUFFER_DURATION(Buffer) = gst_util_uint64_scale(1, GST_SECOND, Fps);
	P->FrameCount++;

	const GstFlowReturn Ret = gst_app_src_push_buffer(GST_APP_SRC(P->AppSrc), Buffer);
	if (Ret != GST_FLOW_OK && Ret != GST_FLOW_FLUSHING)
	{
		LogMsg(3, "WHIP appsrc push-buffer returned %d", (int)Ret);
	}
}

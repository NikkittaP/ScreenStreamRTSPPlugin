// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT
//
// Pure GStreamer translation unit — NO Unreal Engine headers (see RTSPStreamerImpl.h
// for the GError-clash rationale). Logs are forwarded via the SetLogCallback sink.

// MSVC flags std::getenv (and friends) as "unsafe" (C4996). This TU uses plain
// std::getenv deliberately (read-only, no buffer); opt out for the whole file.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#  define _CRT_SECURE_NO_WARNINGS
#endif

#include "RTSPStreamerImpl.h"

#include <algorithm>
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
#include <gst/rtsp-server/rtsp-server.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ── Logging sink ─────────────────────────────────────────────────────────────
namespace
{
	FRTSPStreamerImpl::FLogCallback GLogCb = nullptr;

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
			std::fprintf(stderr, "[RTSP] %s\n", Buf);
		}
	}

	// UE stages the GStreamer *core* libraries (bin/*.dll) next to the binary,
	// but the *element factories* (appsrc, x264enc, rtph264pay, ...) live in the
	// separate plugin modules under lib/gstreamer-1.0 — e.g. `appsrc` is
	// registered by lib/gstreamer-1.0/gstapp.dll, NOT by the linkable
	// gstapp-1.0-0.dll in bin/. GStreamer derives its default plugin path
	// relative to the loaded gstreamer-1.0-0.dll, so once that DLL is loaded from
	// Binaries/Win64 the default (Binaries/Win64/../lib/gstreamer-1.0) does not
	// exist, zero plugins register, gst_parse_launch cannot create appsrc, and
	// media-configure fails with "appsrc 'src' not found". Point GStreamer at the
	// real plugin directory before gst_init.

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

		// 1) Plugins staged by Build.cs next to the loaded core DLL — the editor
		//    or a packaged build is then self-contained (no env var required).
		const std::string DllDir = FindGstDllDir();
		if (!DllDir.empty())
		{
			Candidates.push_back(DllDir + "\\gstreamer-1.0");          // flat staged layout
			Candidates.push_back(DllDir + "\\..\\lib\\gstreamer-1.0"); // DLL loaded from <root>\bin
		}

		// 2) Dev fallback: the GStreamer MSVC development install.
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
				// GST_PLUGIN_PATH adds to the (broken) default search;
				// GST_PLUGIN_SYSTEM_PATH_1_0 replaces it. Set both. The external
				// gst-plugin-scanner is intentionally left unset — Windows scans
				// in-process, which suits an embedded host.
				_putenv_s("GST_PLUGIN_PATH", Cand.c_str());
				_putenv_s("GST_PLUGIN_SYSTEM_PATH_1_0", Cand.c_str());
				LogMsg(0, "GStreamer plugin path: %s", Cand.c_str());
				return;
			}
		}

		LogMsg(2, "No GStreamer plugin directory found (looked next to the loaded DLL "
			"and under GSTREAMER_1_0_ROOT_MSVC_X86_64); 'appsrc' will be missing.");
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

void FRTSPStreamerImpl::SetLogCallback(FLogCallback Callback)
{
	GLogCb = Callback;
}

// ── Internal state (all GStreamer objects live here) ─────────────────────────
struct FRtspGstState
{
	FRTSPStreamerImpl::FSettings Settings;

	std::thread        LoopThread;
	GMainContext*      Context = nullptr;
	GMainLoop*         Loop = nullptr;
	GstRTSPServer*     Server = nullptr;
	guint              ServerSourceId = 0;

	std::mutex   AppSrcMutex;
	GstElement*  AppSrc = nullptr;       // appsrc of the shared media (ref-held)
	uint64_t     FrameCount = 0;
};

FRTSPStreamerImpl::FRTSPStreamerImpl() = default;

FRTSPStreamerImpl::~FRTSPStreamerImpl()
{
	Stop();
}

// media-configure: grab the appsrc out of the freshly built media bin and
// configure it for live pushing. `UserData` is the FRtspGstState*.
static void OnMediaConfigure(GstRTSPMediaFactory* /*Factory*/, GstRTSPMedia* Media, gpointer UserData);
static void OnMediaUnprepared(GstRTSPMedia* /*Media*/, gpointer UserData);

static std::string BuildLaunchString(const FRTSPStreamerImpl::FSettings& S)
{
	const int Fps  = std::max(1, S.Fps);
	const int Kbps = std::max(200, S.BitrateKbps);
	// Keyframe every ~1s. An RTSP client joining a shared, already-running media
	// shows green until the next IDR, so keep the GOP short to bound that wait
	// (and to recover quickly after a live resolution change). Costs some bitrate.
	const int Gop  = Fps;

	// Pick an H.264 encoder. The legacy device-mode `nvh264enc` maps the old
	// NVENC preset GUIDs that recent NVIDIA drivers (5xx+) dropped, so it fails
	// to prepare the media at runtime with "Selected preset not supported"
	// (gst_nv_base_enc_set_format) — and it wants a GL display, awkward for a
	// headless server. The modern CUDA-mode encoders (nvautogpuh264enc /
	// nvcudah264enc) use the new preset API and the CUDA memory path, so prefer
	// them; fall back to legacy nvh264enc, then software x264enc. `bitrate`
	// (kbps) + `gop-size` are the common subset every NVENC variant accepts — we
	// deliberately set no `preset` so the element uses its own supported default.
	const char* HwEnc = nullptr;
	if (S.bUseHardwareEncoder)
	{
		static const char* const HwCandidates[] = {
			"nvautogpuh264enc", "nvcudah264enc", "nvh264enc"
		};
		for (const char* Cand : HwCandidates)
		{
			if (GstElementFactory* F = gst_element_factory_find(Cand))
			{
				gst_object_unref(F);
				HwEnc = Cand;
				break;
			}
		}
		if (!HwEnc)
		{
			LogMsg(1, "No NVENC encoder factory found — falling back to x264enc (software).");
		}
	}

	char Encoder[256];
	if (HwEnc)
	{
		std::snprintf(Encoder, sizeof(Encoder),
			"%s bitrate=%d gop-size=%d ! h264parse", HwEnc, Kbps, Gop);
		LogMsg(0, "RTSP hardware encoder: %s (bitrate=%d kbps, gop=%d)", HwEnc, Kbps, Gop);
	}
	else
	{
		std::snprintf(Encoder, sizeof(Encoder),
			"x264enc tune=zerolatency speed-preset=superfast bitrate=%d key-int-max=%d ! h264parse",
			Kbps, Gop);
	}

	char Launch[768];
	std::snprintf(Launch, sizeof(Launch),
		"( appsrc name=src is-live=true do-timestamp=false format=time "
		"caps=video/x-raw,format=BGRA,width=%d,height=%d,framerate=%d/1 "
		"! queue max-size-buffers=3 leaky=downstream ! videoconvert ! %s "
		"! rtph264pay name=pay0 pt=96 config-interval=1 )",
		S.Width, S.Height, Fps, Encoder);

	return std::string(Launch);
}

bool FRTSPStreamerImpl::Start(const FSettings& InSettings)
{
	if (P)
	{
		Stop();
	}

	EnsureGstInit();

	P = new FRtspGstState();
	P->Settings = InSettings;

	std::promise<bool> InitPromise;
	std::future<bool>  InitFuture = InitPromise.get_future();

	P->LoopThread = std::thread([this, Prom = std::move(InitPromise)]() mutable
	{
		FRtspGstState* I = P;

		I->Context = g_main_context_new();
		g_main_context_push_thread_default(I->Context);

		I->Server = gst_rtsp_server_new();
		const std::string PortStr = std::to_string(I->Settings.Port);
		gst_rtsp_server_set_service(I->Server, PortStr.c_str());

		const std::string Launch = BuildLaunchString(I->Settings);
		const std::string Mount  = I->Settings.MountPoint.empty() ? std::string("/cam0") : I->Settings.MountPoint;
		LogMsg(0, "RTSP launch pipeline: %s", Launch.c_str());

		GstRTSPMountPoints*  Mounts  = gst_rtsp_server_get_mount_points(I->Server);
		GstRTSPMediaFactory* Factory = gst_rtsp_media_factory_new();
		gst_rtsp_media_factory_set_launch(Factory, Launch.c_str());
		gst_rtsp_media_factory_set_shared(Factory, TRUE);
		gst_rtsp_media_factory_set_suspend_mode(Factory, GST_RTSP_SUSPEND_MODE_NONE);
		gst_rtsp_media_factory_set_latency(Factory, 0);
		g_signal_connect(Factory, "media-configure", G_CALLBACK(OnMediaConfigure), I);
		gst_rtsp_mount_points_add_factory(Mounts, Mount.c_str(), Factory);
		g_object_unref(Mounts);

		I->ServerSourceId = gst_rtsp_server_attach(I->Server, I->Context);
		const bool bAttached = (I->ServerSourceId != 0);

		if (bAttached)
		{
			LogMsg(0, "RTSP server listening on rtsp://0.0.0.0:%d%s", I->Settings.Port, Mount.c_str());
			I->Loop = g_main_loop_new(I->Context, FALSE);
		}
		else
		{
			LogMsg(2, "gst_rtsp_server_attach failed (port %d in use?)", I->Settings.Port);
		}

		Prom.set_value(bAttached);

		if (bAttached)
		{
			g_main_loop_run(I->Loop);
		}

		// Teardown (after g_main_loop_quit).
		{
			std::lock_guard<std::mutex> Lock(I->AppSrcMutex);
			if (I->AppSrc) { gst_object_unref(I->AppSrc); I->AppSrc = nullptr; }
		}
		if (I->Loop)           { g_main_loop_unref(I->Loop); I->Loop = nullptr; }
		if (I->ServerSourceId) { g_source_remove(I->ServerSourceId); I->ServerSourceId = 0; }
		if (I->Server)         { g_object_unref(I->Server); I->Server = nullptr; }
		g_main_context_pop_thread_default(I->Context);
		if (I->Context)        { g_main_context_unref(I->Context); I->Context = nullptr; }
	});

	const bool bOk = InitFuture.get();
	if (!bOk)
	{
		Stop();
		return false;
	}

	bRunning.store(true);
	return true;
}

void FRTSPStreamerImpl::Stop()
{
	if (!P)
	{
		return;
	}

	bRunning.store(false);

	if (P->Loop)
	{
		g_main_loop_quit(P->Loop);   // thread-safe; wakes g_main_loop_run
	}
	if (P->LoopThread.joinable())
	{
		P->LoopThread.join();
	}

	delete P;
	P = nullptr;
}

void FRTSPStreamerImpl::SetResolution(int Width, int Height)
{
	if (!P || Width <= 0 || Height <= 0)
	{
		return;
	}

	std::lock_guard<std::mutex> Lock(P->AppSrcMutex);
	P->Settings.Width  = Width;   // used by media-configure for future connections
	P->Settings.Height = Height;

	if (P->AppSrc)
	{
		// Renegotiate the live appsrc → downstream encoder reconfigures in place.
		GstCaps* Caps = gst_caps_new_simple("video/x-raw",
			"format",    G_TYPE_STRING, "BGRA",
			"width",     G_TYPE_INT,    Width,
			"height",    G_TYPE_INT,    Height,
			"framerate", GST_TYPE_FRACTION, std::max(1, P->Settings.Fps), 1,
			nullptr);
		gst_app_src_set_caps(GST_APP_SRC(P->AppSrc), Caps);
		gst_caps_unref(Caps);

		// Force the encoder to emit an IDR at the new resolution immediately.
		// Otherwise it keeps producing P-frames that reference the old-resolution
		// picture, so the client decodes garbage (green/smeared) until the next
		// periodic IDR. This is the standard "GstForceKeyUnit" downstream event
		// (same fields gst_video_event_new_downstream_force_key_unit sets);
		// GstVideoEncoder-derived encoders (nvenc, x264enc) honour it. Sent via
		// the appsrc, GstBaseSrc queues it and pushes it ahead of the next buffer
		// from the streaming thread, so it is safe to send from here.
		GstStructure* Fku = gst_structure_new("GstForceKeyUnit",
			"timestamp",    G_TYPE_UINT64,  (guint64)GST_CLOCK_TIME_NONE,
			"stream-time",  G_TYPE_UINT64,  (guint64)GST_CLOCK_TIME_NONE,
			"running-time", G_TYPE_UINT64,  (guint64)GST_CLOCK_TIME_NONE,
			"all-headers",  G_TYPE_BOOLEAN, TRUE,
			"count",        G_TYPE_UINT,    (guint)0,
			nullptr);
		gst_element_send_event(P->AppSrc, gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, Fku));

		LogMsg(0, "RTSP resolution changed to %dx%d (live renegotiation + forced keyframe)", Width, Height);
	}
}

bool FRTSPStreamerImpl::HasClient() const
{
	if (!P)
	{
		return false;
	}
	std::lock_guard<std::mutex> Lock(P->AppSrcMutex);
	return P->AppSrc != nullptr;
}

void FRTSPStreamerImpl::PushFrame(const uint8_t* Bgra, int32_t SizeBytes)
{
	if (!P || !Bgra || SizeBytes <= 0)
	{
		return;
	}

	std::lock_guard<std::mutex> Lock(P->AppSrcMutex);
	if (!P->AppSrc)
	{
		return;   // no client connected yet → media (and appsrc) not built
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
		LogMsg(3, "appsrc push-buffer returned %d", (int)Ret);
	}
}

// ── Static GStreamer signal handlers ─────────────────────────────────────────

static void OnMediaConfigure(GstRTSPMediaFactory* /*Factory*/, GstRTSPMedia* Media, gpointer UserData)
{
	FRtspGstState* I = static_cast<FRtspGstState*>(UserData);
	if (!I)
	{
		return;
	}

	GstElement* Element = gst_rtsp_media_get_element(Media);                       // transfer full
	GstElement* AppSrc  = gst_bin_get_by_name_recurse_up(GST_BIN(Element), "src"); // transfer full

	if (AppSrc)
	{
		g_object_set(G_OBJECT(AppSrc),
			"stream-type", 0 /* GST_APP_STREAM_TYPE_STREAM */,
			"format", GST_FORMAT_TIME,
			"is-live", TRUE,
			"do-timestamp", FALSE,
			nullptr);

		GstCaps* Caps = gst_caps_new_simple("video/x-raw",
			"format",    G_TYPE_STRING, "BGRA",
			"width",     G_TYPE_INT,    I->Settings.Width,
			"height",    G_TYPE_INT,    I->Settings.Height,
			"framerate", GST_TYPE_FRACTION, std::max(1, I->Settings.Fps), 1,
			nullptr);
		gst_app_src_set_caps(GST_APP_SRC(AppSrc), Caps);
		gst_caps_unref(Caps);

		{
			std::lock_guard<std::mutex> Lock(I->AppSrcMutex);
			if (I->AppSrc) { gst_object_unref(I->AppSrc); }
			I->AppSrc = AppSrc;   // keep the ref returned by get_by_name
			I->FrameCount = 0;
		}

		g_signal_connect(Media, "unprepared", G_CALLBACK(OnMediaUnprepared), I);
		LogMsg(0, "RTSP media configured — appsrc ready, client streaming.");
	}
	else
	{
		LogMsg(2, "media-configure: appsrc 'src' not found in pipeline "
			"(GStreamer plugins likely failed to load — check GSTREAMER_1_0_ROOT_MSVC_X86_64 "
			"and that %s\\lib\\gstreamer-1.0 exists).",
			std::getenv("GSTREAMER_1_0_ROOT_MSVC_X86_64") ? std::getenv("GSTREAMER_1_0_ROOT_MSVC_X86_64") : "(unset)");
		if (Element && GST_IS_BIN(Element))
		{
			GstIterator* It = gst_bin_iterate_elements(GST_BIN(Element));
			GValue Item = G_VALUE_INIT;
			while (gst_iterator_next(It, &Item) == GST_ITERATOR_OK)
			{
				GstElement* Child = GST_ELEMENT(g_value_get_object(&Item));
				LogMsg(2, "  pipeline child: %s (%s)", GST_OBJECT_NAME(Child),
					G_OBJECT_TYPE_NAME(Child));
				g_value_reset(&Item);
			}
			gst_iterator_free(It);
		}
	}

	if (Element)
	{
		gst_object_unref(Element);
	}
}

static void OnMediaUnprepared(GstRTSPMedia* /*Media*/, gpointer UserData)
{
	FRtspGstState* I = static_cast<FRtspGstState*>(UserData);
	if (!I)
	{
		return;
	}
	std::lock_guard<std::mutex> Lock(I->AppSrcMutex);
	if (I->AppSrc)
	{
		gst_object_unref(I->AppSrc);
		I->AppSrc = nullptr;
	}
	LogMsg(0, "RTSP media unprepared — last client disconnected.");
}

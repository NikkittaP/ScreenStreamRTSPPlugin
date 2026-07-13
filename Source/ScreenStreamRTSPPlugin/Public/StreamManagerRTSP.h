// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT
//
// RTSP sibling of AStreamManagerMJPEG. Captures a SceneCapture2D render target
// (with optional Slate overlay compositing), H.264-encodes the frames and
// serves them over RTSP via an embedded GStreamer rtsp-server.

#pragma once

class ASceneCapture2D;
class UUserWidget;
class UTextureRenderTarget2D;
class FWidgetRenderer;
class SWidget;
class FRTSPStreamerImpl;
class FWhipStreamerImpl;

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Containers/Queue.h"
#include "Widgets/SWidget.h"
#include <atomic>

DECLARE_LOG_CATEGORY_EXTERN(LogStreamRTSP, Log, All);

#include "StreamManagerRTSP.generated.h"

USTRUCT()
struct FRenderRequestStreamRTSPStruct
{
	GENERATED_BODY()

	TArray<FColor>      Image;
	TArray<FColor>      OverlayImage;
	bool                bHasOverlay = false;
	/** True when OverlayImage was freshly read back for this request (not cached). */
	bool                bFreshOverlay = false;
	/** Overlay pixels shared from the manager's cache — a ref, not a per-frame copy. */
	TSharedPtr<const TArray<FColor>, ESPMode::ThreadSafe> CachedOverlay;
	FRenderCommandFence RenderFence;

	FRenderRequestStreamRTSPStruct() {}
};

UCLASS(Blueprintable)
class SCREENSTREAMRTSPPLUGIN_API AStreamManagerRTSP : public AActor
{
	GENERATED_BODY()

public:
	AStreamManagerRTSP();

	// ── RTSP / encoder settings ──────────────────────────────────────────────
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	int32 ServerPort = 8554;

	/** Start the embedded RTSP server. Disable it to run a WHIP-only publisher
	 *  (e.g. per-sensor SensorStreamRigs): multiple managers in one process would
	 *  otherwise all try to bind the same ServerPort. Default true keeps the
	 *  original single-manager RTSP behaviour untouched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	bool bEnableRtsp = true;

	/** RTSP mount path; full URL is rtsp://<host>:<ServerPort><MountPoint>. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	FString MountPoint = TEXT("/cam0");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	int32 FrameWidth = 1920;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	int32 FrameHeight = 1080;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	int32 TargetFPS = 30;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	int32 TargetBitrateKbps = 8000;

	/** Use NVENC (nvh264enc) when available; falls back to x264enc if false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	bool bUseHardwareEncoder = true;

	/** Also publish the same frames over WebRTC/WHIP (additive; RTSP is untouched).
	 *  Requires a non-empty WhipUrl. Unlike RTSP, the WHIP pipeline publishes
	 *  continuously while enabled — there is no "client connected" gate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream|WebRTC")
	bool bEnableWebRtc = false;

	/** Full WHIP ingest URL INCLUDING the stream key, e.g.
	 *  http://host:8080/w/<STREAM_KEY> (from livekit-whip provision.py). Empty
	 *  disables the WebRTC publish even when bEnableWebRtc is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream|WebRTC")
	FString WhipUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream")
	ASceneCapture2D* CaptureComponent = nullptr;

	UPROPERTY(EditAnywhere, Category = "Logging")
	bool VerboseLogging = false;

	// ── HUD Overlay Compositing (mirrors the MJPEG plugin) ────────────────────
	UPROPERTY(BlueprintReadWrite, Category = "Stream|Overlay")
	UTextureRenderTarget2D* OverlayRenderTarget = nullptr;

	/** Replace all overlays with a single widget (back-compat convenience). */
	UFUNCTION(BlueprintCallable, Category = "Stream|Overlay")
	void SetOverlayWidget(UUserWidget* InWidget);

	/** Add an overlay widget. Multiple overlays are composited (stacked, add order = z-order). */
	UFUNCTION(BlueprintCallable, Category = "Stream|Overlay")
	void AddOverlayWidget(UUserWidget* InWidget);

	/** Remove a previously added overlay widget. */
	UFUNCTION(BlueprintCallable, Category = "Stream|Overlay")
	void RemoveOverlayWidget(UUserWidget* InWidget);

	/** Remove all overlay widgets. */
	UFUNCTION(BlueprintCallable, Category = "Stream|Overlay")
	void ClearOverlayWidgets();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stream|Overlay", meta = (ClampMin = "1", ClampMax = "30"))
	int32 OverlayRefreshInterval = 1;

	UFUNCTION(BlueprintCallable, Category = "Stream")
	void UpdateRenderTargetAfterFrameSizeChanged();

	/** Change the capture + RTSP resolution at runtime (e.g. to match the active
	 *  sensor). Resizes the render target, discards stale readbacks, and renegotiates
	 *  the live RTSP caps so the session stays connected and the player auto-recovers.
	 *  Dimensions are clamped and rounded to even (H.264). No-op if unchanged. */
	UFUNCTION(BlueprintCallable, Category = "Stream")
	void SetStreamResolution(int32 NewWidth, int32 NewHeight);

	/** True while at least one RTSP client is connected. */
	UFUNCTION(BlueprintPure, Category = "Stream")
	bool HasClient() const;

	/** True while frames are needed by *any* consumer: an RTSP client is connected
	 *  OR the WebRTC/WHIP pipeline is publishing (which has no per-client gate).
	 *  Drives capture cadence and overlay refresh. */
	UFUNCTION(BlueprintPure, Category = "Stream")
	bool IsStreaming() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void SetupCaptureComponent();

	/** Wait out and discard all pending readbacks (used on resolution change). */
	void DrainRenderQueue();

	// Pimpl hiding the GStreamer streamer. Raw pointer (not TUniquePtr) so the
	// UCLASS's generated destructor / vtable-helper ctor never needs the complete
	// type. Created in BeginPlay, deleted in EndPlay.
	FRTSPStreamerImpl* StreamerImpl = nullptr;

	// Optional WebRTC/WHIP sibling streamer fed from the same captured frames.
	// Same incomplete-type / raw-pointer reasoning as StreamerImpl. Created in
	// BeginPlay only when bEnableWebRtc && !WhipUrl.IsEmpty(), deleted in EndPlay.
	FWhipStreamerImpl* WhipStreamerImpl = nullptr;

	// Completed-readback queue (newest frame wins; stale frames are dropped).
	TQueue<FRenderRequestStreamRTSPStruct*> RenderRequestQueue;
	std::atomic<int32> QueueSize{0};

	int64 FrameCounter = 0;

	// Self-driven capture cadence accumulator.
	float CaptureAccumulator = 0.0f;

	// ── Overlay rendering state ───────────────────────────────────────────────
	// Raw pointer for the same incomplete-type reason as StreamerImpl.
	FWidgetRenderer* WidgetRenderer = nullptr;
	// Every overlay widget, composited (stacked) into CompositeOverlaySlate.
	UPROPERTY()
	TArray<UUserWidget*> OverlayWidgets;
	TSharedPtr<SWidget>  CompositeOverlaySlate;   // SOverlay wrapping OverlayWidgets
	// Last fresh overlay readback, shared with in-flight render requests
	// (TSharedPtr: compositing may consume it a few frames later).
	TSharedPtr<const TArray<FColor>, ESPMode::ThreadSafe> CachedOverlayShared;
	int32 OverlayFrameCounter = 0;
	int32 OverlayDrawCount = 0;

	// (Re)build CompositeOverlaySlate from OverlayWidgets; lazily create resources.
	void RebuildCompositeOverlay();
	void EnsureOverlayResources();

public:
	virtual void Tick(float DeltaTime) override;

	/** Enqueue a GPU readback of the capture (and overlay) for this frame. */
	UFUNCTION(BlueprintCallable, Category = "ImageCapture")
	void CaptureNonBlocking();
};

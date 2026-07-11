// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT

#include "StreamManagerRTSP.h"
#include "RTSPStreamerImpl.h"
#include "WhipStreamerImpl.h"

DEFINE_LOG_CATEGORY(LogStreamRTSP);

#include "Runtime/Engine/Classes/Engine/Engine.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "ShowFlags.h"

#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Async/ParallelFor.h"

#include <atomic>

// Refcounted owner of one finished BGRA frame. The pixel array is MOVED in and
// wrapped zero-copy into one GstBuffer per pipeline (RTSP / WHIP); each pipeline
// releases its reference from its own streaming thread when the buffer is freed.
struct FSharedFrameOwner
{
	TArray<FColor>     Pixels;
	std::atomic<int32> RefCount;

	FSharedFrameOwner(TArray<FColor>&& InPixels, int32 InRefs)
		: Pixels(MoveTemp(InPixels)), RefCount(InRefs) {}

	static void Release(void* Opaque)
	{
		FSharedFrameOwner* Owner = static_cast<FSharedFrameOwner*>(Opaque);
		if (Owner->RefCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			delete Owner;
		}
	}
};

#include "Slate/WidgetRenderer.h"
#include "Blueprint/UserWidget.h"
#include "Widgets/SOverlay.h"

// Bridge FRTSPStreamerImpl logs (a UE-header-free TU) into the UE log category.
static void RtspLogToUE(int Level, const char* Msg)
{
	const FString S = UTF8_TO_TCHAR(Msg);
	switch (Level)
	{
		case 2:  UE_LOG(LogStreamRTSP, Error,   TEXT("%s"), *S); break;
		case 1:  UE_LOG(LogStreamRTSP, Warning, TEXT("%s"), *S); break;
		case 3:  UE_LOG(LogStreamRTSP, Verbose, TEXT("%s"), *S); break;
		default: UE_LOG(LogStreamRTSP, Log,     TEXT("%s"), *S); break;
	}
}

// Same bridge for the WHIP streamer TU, tagged so the two are distinguishable.
static void WhipLogToUE(int Level, const char* Msg)
{
	const FString S = UTF8_TO_TCHAR(Msg);
	switch (Level)
	{
		case 2:  UE_LOG(LogStreamRTSP, Error,   TEXT("[WHIP] %s"), *S); break;
		case 1:  UE_LOG(LogStreamRTSP, Warning, TEXT("[WHIP] %s"), *S); break;
		case 3:  UE_LOG(LogStreamRTSP, Verbose, TEXT("[WHIP] %s"), *S); break;
		default: UE_LOG(LogStreamRTSP, Log,     TEXT("[WHIP] %s"), *S); break;
	}
}

AStreamManagerRTSP::AStreamManagerRTSP()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AStreamManagerRTSP::BeginPlay()
{
	Super::BeginPlay();

	if (!CaptureComponent)
	{
		UE_LOG(LogStreamRTSP, Error, TEXT("No CaptureComponent set! RTSP stream will not start."));
		return;
	}

	SetupCaptureComponent();

	FRTSPStreamerImpl::SetLogCallback(&RtspLogToUE);
	StreamerImpl = new FRTSPStreamerImpl();

	FRTSPStreamerImpl::FSettings Settings;
	Settings.Port               = ServerPort;
	Settings.MountPoint         = TCHAR_TO_UTF8(*MountPoint);
	Settings.Width              = FrameWidth;
	Settings.Height             = FrameHeight;
	Settings.Fps                = TargetFPS;
	Settings.BitrateKbps        = TargetBitrateKbps;
	Settings.bUseHardwareEncoder = bUseHardwareEncoder;

	if (!StreamerImpl->Start(Settings))
	{
		UE_LOG(LogStreamRTSP, Error, TEXT("FRTSPStreamerImpl::Start failed; RTSP stream unavailable."));
		delete StreamerImpl;
		StreamerImpl = nullptr;
	}

	// ── Optional WebRTC/WHIP publish (additive; independent of RTSP clients) ──
	if (bEnableWebRtc && !WhipUrl.IsEmpty())
	{
		FWhipStreamerImpl::SetLogCallback(&WhipLogToUE);
		WhipStreamerImpl = new FWhipStreamerImpl();

		FWhipStreamerImpl::FSettings WhipSettings;
		WhipSettings.WhipEndpoint = TCHAR_TO_UTF8(*WhipUrl);
		WhipSettings.Width        = FrameWidth;
		WhipSettings.Height       = FrameHeight;
		WhipSettings.Fps          = TargetFPS;

		if (!WhipStreamerImpl->Start(WhipSettings))
		{
			UE_LOG(LogStreamRTSP, Error, TEXT("FWhipStreamerImpl::Start failed; WebRTC publish unavailable (RTSP still works)."));
			delete WhipStreamerImpl;
			WhipStreamerImpl = nullptr;
		}
	}
	else if (bEnableWebRtc)
	{
		UE_LOG(LogStreamRTSP, Warning, TEXT("bEnableWebRtc is true but WhipUrl is empty; WebRTC publish skipped."));
	}
}

void AStreamManagerRTSP::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Drain pending render requests so their fences complete before teardown.
	while (!RenderRequestQueue.IsEmpty())
	{
		FRenderRequestStreamRTSPStruct* Request = nullptr;
		RenderRequestQueue.Dequeue(Request);
		if (Request)
		{
			Request->RenderFence.Wait();
			delete Request;
			QueueSize--;
		}
	}

	CachedOverlayShared.Reset();
	if (WidgetRenderer)
	{
		delete WidgetRenderer;
		WidgetRenderer = nullptr;
	}
	OverlayWidgets.Empty();
	CompositeOverlaySlate.Reset();

	if (StreamerImpl)
	{
		StreamerImpl->Stop();
		delete StreamerImpl;
		StreamerImpl = nullptr;
	}

	if (WhipStreamerImpl)
	{
		WhipStreamerImpl->Stop();
		delete WhipStreamerImpl;
		WhipStreamerImpl = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

bool AStreamManagerRTSP::HasClient() const
{
	return StreamerImpl != nullptr && StreamerImpl->HasClient();
}

bool AStreamManagerRTSP::IsStreaming() const
{
	return HasClient() || (WhipStreamerImpl != nullptr && WhipStreamerImpl->IsRunning());
}

void AStreamManagerRTSP::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// ── Self-driven capture cadence ──────────────────────────────────────────
	// Only burn GPU on readback while someone needs the frames: an RTSP client is
	// watching, OR the WHIP pipeline is publishing (it has no per-client gate).
	const bool bNeedFrames = IsStreaming();

	// Gate the SceneCapture render itself, not just the readback: with
	// bCaptureEveryFrame stuck on, the capture costs a full extra scene render
	// per engine frame even with zero consumers (measured: "idle" ~= "rtsp").
	if (IsValid(CaptureComponent))
	{
		if (USceneCaptureComponent2D* Capture = CaptureComponent->GetCaptureComponent2D())
		{
			if (Capture->bCaptureEveryFrame != bNeedFrames)
			{
				Capture->bCaptureEveryFrame = bNeedFrames;
			}
		}
	}

	const float Interval = 1.0f / FMath::Max(1, TargetFPS);
	if (bNeedFrames)
	{
		CaptureAccumulator += DeltaTime;
		// Cap to avoid a capture storm after a long hitch.
		if (CaptureAccumulator > Interval * 4.0f)
		{
			CaptureAccumulator = Interval;
		}
		if (CaptureAccumulator >= Interval)
		{
			CaptureAccumulator -= Interval;
			CaptureNonBlocking();
		}
	}
	else
	{
		CaptureAccumulator = 0.0f;
	}

	// ── Queue overflow guard (memory-leak protection) ─────────────────────────
	if (QueueSize.load() > 10)
	{
		UE_LOG(LogStreamRTSP, Error, TEXT("RenderRequestQueue overflow (size=%d); draining."), QueueSize.load());
		while (!RenderRequestQueue.IsEmpty())
		{
			FRenderRequestStreamRTSPStruct* Request = nullptr;
			RenderRequestQueue.Dequeue(Request);
			if (Request) { delete Request; QueueSize--; }
		}
		return;
	}

	if (RenderRequestQueue.IsEmpty())
	{
		return;
	}

	// ── Keep only the newest completed readback; discard stale ones ───────────
	FRenderRequestStreamRTSPStruct* LatestReady = nullptr;
	while (!RenderRequestQueue.IsEmpty())
	{
		FRenderRequestStreamRTSPStruct* Candidate = nullptr;
		RenderRequestQueue.Peek(Candidate);
		if (!Candidate) break;
		if (!Candidate->RenderFence.IsFenceComplete()) break;  // still GPU-pending

		RenderRequestQueue.Pop();
		QueueSize--;

		if (LatestReady) { delete LatestReady; }
		LatestReady = Candidate;
	}

	if (!LatestReady)
	{
		return;
	}

	// ── Alpha-composite overlay onto the scene image ──────────────────────────
	const TArray<FColor>* OverlaySrc = nullptr;
	if (LatestReady->bHasOverlay)
	{
		OverlaySrc = LatestReady->bFreshOverlay ? &LatestReady->OverlayImage
		                                        : LatestReady->CachedOverlay.Get();
	}
	if (OverlaySrc && OverlaySrc->Num() == LatestReady->Image.Num())
	{
		const int32   PixelCount  = LatestReady->Image.Num();
		FColor*       SceneData   = LatestReady->Image.GetData();
		const FColor* OverlayData = OverlaySrc->GetData();

		// 2M pixels per 1080p frame — blend in parallel chunks instead of a
		// single game-thread loop.
		const int32 ChunkSize = 64 * 1024;
		const int32 NumChunks = FMath::DivideAndRoundUp(PixelCount, ChunkSize);
		ParallelFor(NumChunks, [SceneData, OverlayData, PixelCount, ChunkSize](int32 Chunk)
		{
			const int32 End = FMath::Min((Chunk + 1) * ChunkSize, PixelCount);
			for (int32 i = Chunk * ChunkSize; i < End; ++i)
			{
				const uint8 A = OverlayData[i].A;
				if (A == 0) continue;
				if (A == 255)
				{
					SceneData[i] = OverlayData[i];
				}
				else
				{
					const uint32 InvA = 255 - A;
					SceneData[i].R = (uint8)((OverlayData[i].R * A + SceneData[i].R * InvA + 127) / 255);
					SceneData[i].G = (uint8)((OverlayData[i].G * A + SceneData[i].G * InvA + 127) / 255);
					SceneData[i].B = (uint8)((OverlayData[i].B * A + SceneData[i].B * InvA + 127) / 255);
					SceneData[i].A = 255;
				}
			}
		});

		// A fresh overlay readback becomes the new shared cache (moved, not copied).
		if (LatestReady->bFreshOverlay)
		{
			CachedOverlayShared = MakeShared<TArray<FColor>, ESPMode::ThreadSafe>(MoveTemp(LatestReady->OverlayImage));
		}
	}

	// ── Push the BGRA frame into the GStreamer appsrc(s) ──────────────────────
	// FColor is laid out B,G,R,A in memory → matches the appsrc "BGRA" caps.
	// The pixels are MOVED into a refcounted owner and wrapped zero-copy into a
	// GstBuffer per pipeline (each keeps its own PTS) — no per-sink memcpy.
	if (LatestReady->Image.Num() > 0)
	{
		const int32 SizeBytes = LatestReady->Image.Num() * (int32)sizeof(FColor);
		const int32 NumSinks  = (StreamerImpl ? 1 : 0) + (WhipStreamerImpl ? 1 : 0);
		if (NumSinks > 0)
		{
			FSharedFrameOwner* Owner = new FSharedFrameOwner(MoveTemp(LatestReady->Image), NumSinks);
			const uint8_t* const Pixels = reinterpret_cast<const uint8_t*>(Owner->Pixels.GetData());

			if (StreamerImpl)
			{
				StreamerImpl->PushFrameZeroCopy(Pixels, SizeBytes, Owner, &FSharedFrameOwner::Release);
			}
			if (WhipStreamerImpl)
			{
				WhipStreamerImpl->PushFrameZeroCopy(Pixels, SizeBytes, Owner, &FSharedFrameOwner::Release);
			}
			FrameCounter++;
		}
	}

	delete LatestReady;
}

void AStreamManagerRTSP::SetupCaptureComponent()
{
	if (!IsValid(CaptureComponent))
	{
		UE_LOG(LogStreamRTSP, Error, TEXT("SetupCaptureComponent: CaptureComponent is not valid!"));
		return;
	}

	UTextureRenderTarget2D* RenderTarget2D = NewObject<UTextureRenderTarget2D>(this);
	RenderTarget2D->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	RenderTarget2D->InitCustomFormat(FrameWidth, FrameHeight, PF_B8G8R8A8, true); // LDR, no HDR overhead
	RenderTarget2D->bGPUSharedFlag = true;

	USceneCaptureComponent2D* Capture = CaptureComponent->GetCaptureComponent2D();
	Capture->TextureTarget = RenderTarget2D;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->TextureTarget->TargetGamma = GEngine->GetDisplayGamma();
	Capture->ShowFlags.SetTemporalAA(true);
	// The capture render is expensive (a second full scene render). Keep it OFF
	// until a consumer appears — Tick() toggles bCaptureEveryFrame with
	// IsStreaming(). bCaptureOnMovement would sneak captures back in while idle.
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;

	UE_LOG(LogStreamRTSP, Verbose, TEXT("RTSP capture render target initialised (%dx%d)"), FrameWidth, FrameHeight);
}

void AStreamManagerRTSP::CaptureNonBlocking()
{
	if (!IsValid(CaptureComponent))
	{
		UE_LOG(LogStreamRTSP, Error, TEXT("CaptureNonBlocking: CaptureComponent was not valid!"));
		return;
	}

	if (QueueSize.load() > 5)
	{
		UE_LOG(LogStreamRTSP, Verbose, TEXT("CaptureNonBlocking: skipping, queue size %d"), QueueSize.load());
		return;
	}

	USceneCaptureComponent2D* Capture = CaptureComponent->GetCaptureComponent2D();
	Capture->TextureTarget->TargetGamma = GEngine->GetDisplayGamma();

	FTextureRenderTargetResource* RenderTargetResource = Capture->TextureTarget->GameThread_GetRenderTargetResource();

	struct FReadSurfaceContext
	{
		FRenderTarget*          SrcRenderTarget;
		TArray<FColor>*         OutData;
		FIntRect                Rect;
		FReadSurfaceDataFlags   Flags;
	};

	FRenderRequestStreamRTSPStruct* RenderRequest = new FRenderRequestStreamRTSPStruct();

	// ── Overlay: draw every frame (cheap), throttle GPU readback ──────────────
	bool bFreshOverlayRender = false;
	if (CompositeOverlaySlate.IsValid() && WidgetRenderer != nullptr && OverlayRenderTarget)
	{
		FTextureRenderTargetResource* OverlayResource = OverlayRenderTarget->GameThread_GetRenderTargetResource();
		if (OverlayResource)
		{
			WidgetRenderer->DrawWidget(OverlayResource, CompositeOverlaySlate.ToSharedRef(),
				FVector2D(FrameWidth, FrameHeight), GetWorld()->GetDeltaSeconds());

			OverlayDrawCount++;
			OverlayFrameCounter++;
			if (OverlayFrameCounter >= OverlayRefreshInterval)
			{
				OverlayFrameCounter = 0;
				bFreshOverlayRender = true;
				RenderRequest->bHasOverlay = true;
				RenderRequest->bFreshOverlay = true;
			}
			else if (CachedOverlayShared.IsValid() && CachedOverlayShared->Num() > 0)
			{
				RenderRequest->CachedOverlay = CachedOverlayShared;   // ref only, no pixel copy
				RenderRequest->bHasOverlay = true;
			}
		}
	}

	FReadSurfaceContext ReadSurfaceContext = {
		RenderTargetResource,
		&(RenderRequest->Image),
		FIntRect(0, 0, RenderTargetResource->GetSizeXY().X, RenderTargetResource->GetSizeXY().Y),
		FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX)
	};

	ENQUEUE_RENDER_COMMAND(RTSPSceneReadback)(
		[ReadSurfaceContext](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.ReadSurfaceData(
				ReadSurfaceContext.SrcRenderTarget->GetRenderTargetTexture(),
				ReadSurfaceContext.Rect,
				*ReadSurfaceContext.OutData,
				ReadSurfaceContext.Flags);
		});

	if (bFreshOverlayRender)
	{
		FTextureRenderTargetResource* OverlayResource = OverlayRenderTarget->GameThread_GetRenderTargetResource();
		FReadSurfaceContext OverlayContext = {
			OverlayResource,
			&(RenderRequest->OverlayImage),
			FIntRect(0, 0, OverlayResource->GetSizeXY().X, OverlayResource->GetSizeXY().Y),
			FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX)
		};

		ENQUEUE_RENDER_COMMAND(RTSPOverlayReadback)(
			[OverlayContext](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.ReadSurfaceData(
					OverlayContext.SrcRenderTarget->GetRenderTargetTexture(),
					OverlayContext.Rect,
					*OverlayContext.OutData,
					OverlayContext.Flags);
			});
	}

	RenderRequestQueue.Enqueue(RenderRequest);
	QueueSize++;
	RenderRequest->RenderFence.BeginFence();
}

void AStreamManagerRTSP::SetStreamResolution(int32 NewWidth, int32 NewHeight)
{
	// H.264 requires even dimensions; clamp to sane bounds.
	NewWidth  = FMath::Clamp(NewWidth  & ~1, 16, 7680);
	NewHeight = FMath::Clamp(NewHeight & ~1, 16, 4320);

	if (NewWidth == FrameWidth && NewHeight == FrameHeight)
	{
		return;
	}

	UE_LOG(LogStreamRTSP, Log, TEXT("Stream resolution %dx%d -> %dx%d"),
		FrameWidth, FrameHeight, NewWidth, NewHeight);

	// 1) Discard in-flight readbacks while the render target is still the old size
	//    (their FIntRect matches the current target, so no out-of-bounds read).
	DrainRenderQueue();

	// 2) Resize the capture (and overlay) render targets to the new size.
	FrameWidth  = NewWidth;
	FrameHeight = NewHeight;
	UpdateRenderTargetAfterFrameSizeChanged();

	// 3) Renegotiate the live caps on both pipelines (each session stays alive;
	//    the player/viewer recovers on the forced keyframe).
	if (StreamerImpl)
	{
		StreamerImpl->SetResolution(NewWidth, NewHeight);
	}
	if (WhipStreamerImpl)
	{
		WhipStreamerImpl->SetResolution(NewWidth, NewHeight);
	}
}

void AStreamManagerRTSP::DrainRenderQueue()
{
	while (!RenderRequestQueue.IsEmpty())
	{
		FRenderRequestStreamRTSPStruct* Request = nullptr;
		RenderRequestQueue.Dequeue(Request);
		if (Request)
		{
			Request->RenderFence.Wait();
			delete Request;
			QueueSize--;
		}
	}
	CachedOverlayShared.Reset();   // old-size overlay cache is no longer valid
}

void AStreamManagerRTSP::UpdateRenderTargetAfterFrameSizeChanged()
{
	if (!IsValid(CaptureComponent))
	{
		UE_LOG(LogStreamRTSP, Error, TEXT("UpdateRenderTargetAfterFrameSizeChanged: CaptureComponent is not valid!"));
		return;
	}

	CaptureComponent->GetCaptureComponent2D()->TextureTarget->InitCustomFormat(FrameWidth, FrameHeight, PF_B8G8R8A8, true);

	if (CompositeOverlaySlate.IsValid())
	{
		if (!OverlayRenderTarget)
		{
			OverlayRenderTarget = NewObject<UTextureRenderTarget2D>(this);
		}
		OverlayRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		OverlayRenderTarget->ClearColor = FLinearColor::Transparent;
		OverlayRenderTarget->bGPUSharedFlag = true;
		OverlayRenderTarget->InitCustomFormat(FrameWidth, FrameHeight, PF_B8G8R8A8, true);
	}
}

void AStreamManagerRTSP::SetOverlayWidget(UUserWidget* InWidget)
{
	// Back-compat single-overlay convenience: replace whatever is there.
	ClearOverlayWidgets();
	if (InWidget)
	{
		AddOverlayWidget(InWidget);
	}
}

void AStreamManagerRTSP::AddOverlayWidget(UUserWidget* InWidget)
{
	if (!InWidget || OverlayWidgets.Contains(InWidget))
	{
		return;
	}
	OverlayWidgets.Add(InWidget);
	EnsureOverlayResources();
	RebuildCompositeOverlay();
	UE_LOG(LogStreamRTSP, Verbose, TEXT("Overlay added (%d total)"), OverlayWidgets.Num());
}

void AStreamManagerRTSP::RemoveOverlayWidget(UUserWidget* InWidget)
{
	if (InWidget && OverlayWidgets.Remove(InWidget) > 0)
	{
		RebuildCompositeOverlay();
		UE_LOG(LogStreamRTSP, Verbose, TEXT("Overlay removed (%d left)"), OverlayWidgets.Num());
	}
}

void AStreamManagerRTSP::ClearOverlayWidgets()
{
	OverlayWidgets.Empty();
	CompositeOverlaySlate.Reset();
}

void AStreamManagerRTSP::EnsureOverlayResources()
{
	if (!WidgetRenderer)
	{
		WidgetRenderer = new FWidgetRenderer(/*bUseGammaCorrection=*/true);
	}
	if (!OverlayRenderTarget)
	{
		OverlayRenderTarget = NewObject<UTextureRenderTarget2D>(this);
		OverlayRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		OverlayRenderTarget->ClearColor = FLinearColor::Transparent;
		OverlayRenderTarget->bGPUSharedFlag = true;
		OverlayRenderTarget->InitCustomFormat(FrameWidth, FrameHeight, PF_B8G8R8A8, true);
	}
}

void AStreamManagerRTSP::RebuildCompositeOverlay()
{
	// Drop any widgets that were GC'd.
	OverlayWidgets.RemoveAll([](UUserWidget* W) { return !IsValid(W); });

	if (OverlayWidgets.Num() == 0)
	{
		CompositeOverlaySlate.Reset();
		return;
	}

	// Stack every overlay into one SOverlay so a single DrawWidget pass
	// composites them all (add order = z-order; later widgets draw on top).
	TSharedRef<SOverlay> Composite = SNew(SOverlay);
	for (UUserWidget* W : OverlayWidgets)
	{
		Composite->AddSlot()
		[
			W->TakeWidget()
		];
	}
	CompositeOverlaySlate = Composite;
}

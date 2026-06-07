// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT

#include "ScreenStreamRTSPPlugin.h"

#define LOCTEXT_NAMESPACE "FScreenStreamRTSPPluginModule"

void FScreenStreamRTSPPluginModule::StartupModule()
{
	// GStreamer is initialised lazily on first AStreamManagerRTSP::BeginPlay
	// (see FRTSPStreamerImpl), so there is nothing to do here.
}

void FScreenStreamRTSPPluginModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FScreenStreamRTSPPluginModule, ScreenStreamRTSPPlugin)

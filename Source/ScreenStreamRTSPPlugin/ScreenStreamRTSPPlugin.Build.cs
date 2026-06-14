// Copyright (c) 2026 Nikita Petrov (https://github.com/NikkittaP)
// SPDX-License-Identifier: MIT

using UnrealBuildTool;
using System;
using System.IO;
using System.Diagnostics;
using System.Collections.Generic;

public class ScreenStreamRTSPPlugin : ModuleRules
{
	// GStreamer modules we need: core, app (appsrc) and the RTSP server.
	// gstrtp is pulled in transitively but we link it explicitly to be safe.
	private static readonly string[] GstPackages =
	{
		"gstreamer-1.0",
		"gstreamer-app-1.0",
		"gstreamer-rtsp-server-1.0",
		"gstreamer-rtp-1.0",
		"glib-2.0",
		"gobject-2.0"
	};

	public ScreenStreamRTSPPlugin(ReadOnlyTargetRules Target) : base(Target)
	{
		// C++20 to match the rest of the project (UE 5.5+ requirement).
		CppStandard = CppStandardVersion.Cpp20;

		// RTSPStreamerImpl.cpp is pure GStreamer/GLib and MUST NOT see UE Core
		// headers — GLib's `GError` typedef clashes with UE's global
		// `FOutputDeviceError* GError`. Two safeguards:
		//   • NoPCHs        → UE does not force-include the shared PCH (CoreMinimal)
		//   • bUseUnity=false → the GStreamer TU is never merged with UE-including files
		PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;
		bUseUnity = false;

		// GStreamer is a C library; no UE exceptions required, but keep parity
		// with the SDK plugin which enables them.
		bEnableExceptions = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"RenderCore",
			"Renderer",
			"RHI"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"UMG"
		});

		if (Target.Platform == UnrealTargetPlatform.Linux ||
		    Target.Platform == UnrealTargetPlatform.LinuxArm64)
		{
			ConfigureLinux();
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			ConfigureWindows();
		}
		else
		{
			throw new BuildException("ScreenStreamRTSPPlugin supports only Linux and Win64.");
		}
	}

	// ── Linux: bundled ThirdParty (cross-compile) → pkg-config → /usr fallback ───
	private void ConfigureLinux()
	{
		// Priority 1: a pre-built GStreamer tree bundled in the plugin. REQUIRED
		// when cross-compiling for Linux from a Windows host (no pkg-config / /usr
		// there); on a native Linux host it is used if present. Layout mirrors
		// GatewaySDKPlugin/ThirdParty:
		//   ThirdParty/GStreamerLinux/include/{gstreamer-1.0,glib-2.0}
		//   ThirdParty/GStreamerLinux/lib/{libgstreamer-1.0.so,...,glib-2.0/include/glibconfig.h}
		// GStreamer/GLib are pure C, so the stock Ubuntu .so link fine from UE's
		// clang/libc++ target — the ABI boundary is C (glibc), not C++. Hence,
		// unlike the gateway-cpp SDK, no libc++ rebuild is needed; extract the
		// dev libs straight from an Ubuntu 24.04 install.
		string tpDir = Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "GStreamerLinux");
		string tpInc = Path.Combine(tpDir, "include");
		string tpLib = Path.Combine(tpDir, "lib");
		if (Directory.Exists(tpInc) && Directory.Exists(tpLib))
		{
			Console.WriteLine("ScreenStreamRTSPPlugin: using bundled GStreamer at " + tpDir);
			PublicIncludePaths.AddRange(new string[]
			{
				Path.Combine(tpInc, "gstreamer-1.0"),
				Path.Combine(tpInc, "glib-2.0"),
				Path.Combine(tpLib, "glib-2.0", "include")   // glibconfig.h
			});
			foreach (string so in new string[]
			{
				"libgstreamer-1.0.so", "libgstapp-1.0.so", "libgstrtspserver-1.0.so",
				"libgstrtp-1.0.so", "libgobject-2.0.so", "libglib-2.0.so"
			})
			{
				PublicAdditionalLibraries.Add(Path.Combine(tpLib, so));
			}
			return;
		}

		// Priority 2: native Linux host — resolve flags via pkg-config.
		bool ok = true;
		foreach (string pkg in GstPackages)
		{
			ok &= ApplyPkgConfig(pkg);
		}
		if (ok)
		{
			return;
		}

		// Cross-compiling from Windows with no bundle present → fail loudly; the
		// /usr fallback below would point the linker at paths that don't exist.
		if (BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64)
		{
			throw new BuildException(
				"Cross-compiling for Linux requires GStreamer prebuilt libs bundled at:\n  " +
				tpDir + "\n(extract include/ + lib/ from an Ubuntu 24.04 install — see " +
				"worldsim/Docs/RTSP_SETUP_AND_RUN.md §C).");
		}

		// Priority 3: native Linux without pkg-config — stock Ubuntu 24.04 paths.
		Console.WriteLine("ScreenStreamRTSPPlugin: pkg-config failed; using hardcoded Ubuntu paths.");
		string archDir = "/usr/lib/x86_64-linux-gnu";
		PublicIncludePaths.AddRange(new string[]
		{
			"/usr/include/gstreamer-1.0",
			"/usr/include/glib-2.0",
			Path.Combine(archDir, "glib-2.0/include")
		});
		foreach (string lib in new string[]
		{
			"gstreamer-1.0", "gstapp-1.0", "gstrtspserver-1.0",
			"gstrtp-1.0", "gobject-2.0", "glib-2.0"
		})
		{
			PublicSystemLibraries.Add(lib);
		}
	}

	// Runs `pkg-config --cflags/--libs <pkg>` and feeds the result into UBT.
	// Returns false if the tool or package is missing.
	private bool ApplyPkgConfig(string pkg)
	{
		string cflags = RunPkgConfig("--cflags " + pkg);
		string libs = RunPkgConfig("--libs " + pkg);
		if (cflags == null || libs == null)
		{
			return false;
		}

		foreach (string tok in cflags.Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
		{
			if (tok.StartsWith("-I"))
			{
				PublicIncludePaths.Add(tok.Substring(2));
			}
			else if (tok.StartsWith("-D"))
			{
				PublicDefinitions.Add(tok.Substring(2));
			}
		}

		foreach (string tok in libs.Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
		{
			if (tok.StartsWith("-l"))
			{
				PublicSystemLibraries.Add(tok.Substring(2));
			}
			else if (tok.StartsWith("-L"))
			{
				PublicSystemLibraryPaths.Add(tok.Substring(2));
			}
		}
		return true;
	}

	private static string RunPkgConfig(string args)
	{
		try
		{
			var psi = new ProcessStartInfo("pkg-config", args)
			{
				RedirectStandardOutput = true,
				RedirectStandardError = true,
				UseShellExecute = false,
				CreateNoWindow = true
			};
			using (var p = Process.Start(psi))
			{
				string outp = p.StandardOutput.ReadToEnd();
				p.WaitForExit();
				return p.ExitCode == 0 ? outp.Trim() : null;
			}
		}
		catch (Exception)
		{
			return null;
		}
	}

	// ── Windows: GStreamer MSVC dev install via GSTREAMER_1_0_ROOT_MSVC_X86_64 ──
	private void ConfigureWindows()
	{
		string root = Environment.GetEnvironmentVariable("GSTREAMER_1_0_ROOT_MSVC_X86_64");
		if (string.IsNullOrEmpty(root) || !Directory.Exists(root))
		{
			throw new BuildException(
				"GStreamer not found. Install the GStreamer MSVC 64-bit *development* runtime " +
				"and ensure GSTREAMER_1_0_ROOT_MSVC_X86_64 points at e.g. C:\\gstreamer\\1.0\\msvc_x86_64\\.");
		}

		string inc = Path.Combine(root, "include");
		string lib = Path.Combine(root, "lib");
		string bin = Path.Combine(root, "bin");

		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(inc, "gstreamer-1.0"),
			Path.Combine(inc, "glib-2.0"),
			Path.Combine(lib, "glib-2.0", "include")
		});

		foreach (string l in new string[]
		{
			"gstreamer-1.0.lib", "gstapp-1.0.lib", "gstrtspserver-1.0.lib",
			"gstrtp-1.0.lib", "gobject-2.0.lib", "glib-2.0.lib"
		})
		{
			PublicAdditionalLibraries.Add(Path.Combine(lib, l));
		}

		// Stage the GStreamer runtime DLLs next to the executable so the
		// packaged build can find them (same trick GatewaySDKPlugin uses for
		// OpenSSL). We stage the whole bin/ directory's DLLs to cover the
		// transitive plugin dependencies (libav, x264, nvcodec, ...).
		if (Directory.Exists(bin))
		{
			foreach (string dll in Directory.GetFiles(bin, "*.dll"))
			{
				RuntimeDependencies.Add("$(TargetOutputDir)/" + Path.GetFileName(dll), dll, StagedFileType.NonUFS);
			}
		}

		// The bin/*.dll above are the linkable core *libraries*. The element
		// *factories* the launch string needs (appsrc, videoconvert, x264enc,
		// nvh264enc, h264parse, rtph264pay, ...) live in separate plugin modules
		// under lib/gstreamer-1.0 (e.g. appsrc is in gstapp.dll there, not in
		// bin/gstapp-1.0-0.dll). A packaged build must carry them or it registers
		// zero plugins and media-configure fails with "appsrc 'src' not found".
		// Stage them into a sibling "gstreamer-1.0" folder; RTSPStreamerImpl.cpp
		// points GST_PLUGIN_SYSTEM_PATH_1_0 there at runtime.
		//
		// Skip this for the Editor: the whole plugin set is ~110 MB, and in the
		// editor RTSPStreamerImpl.cpp's path resolution falls back to the dev
		// install via GSTREAMER_1_0_ROOT_MSVC_X86_64 — no per-build copy needed.
		if (Target.Type != TargetType.Editor)
		{
			string plugins = Path.Combine(lib, "gstreamer-1.0");
			if (Directory.Exists(plugins))
			{
				foreach (string dll in Directory.GetFiles(plugins, "*.dll"))
				{
					RuntimeDependencies.Add("$(TargetOutputDir)/gstreamer-1.0/" + Path.GetFileName(dll), dll, StagedFileType.NonUFS);
				}
			}
		}
	}
}

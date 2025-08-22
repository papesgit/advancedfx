AGENTS.md – Half‑Life Advanced Effects (HLAE)
Overview

Half‑Life Advanced Effects (HLAE) is a toolkit that augments Source‑engine titles (primarily CS:GO/CS2) for machinima and demo work

.
The repository contains native hooks, Rust components, shared utilities and a C# GUI that together provide recording, camera, shader and automation features.
Directory Map
Path	Purpose
AfxHookGoldSrc/	DLL hook for GoldSrc (CS 1.6, etc.)
AfxHookSource/	DLL hook for Source engine (CS:GO, TF2…)
AfxHookSource2/	DLL hook for Source 2 (CS2) that bridges to the Rust crate AfxHookSource2Rs
AfxHookSource2Rs/	Rust crate compiled via Corrosion and exposed to C++ through FFI
AfxCppCli/	C++/CLI utilities (Color LUT tools, managed helpers).
hlae/	WinForms GUI and launcher (HLAE.exe)
ShaderBuilder/	C# console tool for compiling shader source to bytecode
ShaderDisassembler/	C# console tool for disassembling DirectX shaders
shared/	Cross‑module C++ utilities (command line parsing, camera paths, OpenEXR output, threading) reused by all hooks
deps/	Bundled third‑party libraries (Detours, OpenEXR, protobuf, injector, localization tools).
installer/	WiX scripts and bundles to build the Windows installer.
doc/	Assorted design notes and build documentation.
resources/, shaders/	Assets shipped with HLAE (localization, icons, shader source).
tests/	Small sample projects and regression tests.
Component Entrypoints
Component	Language	Entrypoint
GoldSrc hook	C++	AfxHookGoldSrc/main.cpp → DllMain
Source hook	C++	AfxHookSource/main.cpp → DllMain
Source 2 hook	C++	AfxHookSource2/main.cpp → DllMain
Source 2 Rust module	Rust	AfxHookSource2Rs/src/lib.rs (exports afx_hook_source2_rs_new, *_run_jobs, *_destroy)
GUI launcher	C# (.NET)	hlae/Program.cs → Main()
Shader compiler	C#	ShaderBuilder/Program.cs → Main(string[] args)
Shader disassembler	C#	ShaderDisassembler/Program.cs → Main(string[] args)
Build System & Toolchain

    CMake MultiBuild orchestrates Win32/x64 builds and installs binaries; Rust integration uses the Corrosion module to pull in AfxHookSource2Rs

.

Dependencies / Tools
– Git, Node 22 LTS, Visual Studio 2022 with C++ and .NET workloads, Python 3.8+, GNU Gettext, WiX Toolset, Rust (targets i686-pc-windows-msvc & x86_64-pc-windows-msvc)

– Build commands: configure with cmake -G "Visual Studio 17 2022" -A Win32, then cmake --build and cmake --install

Cross‑language integration
– C++ hooks are compiled as DLLs and load shared utilities (../shared/*) at build time

    .
    – Rust crate exports C ABI functions consumed by AfxHookSource2 (see FFI functions above).
    – AfxCppCli produces a managed DLL used by the C# GUI for LUT and tooling.
    – WiX projects in installer/ bundle native DLLs and managed executables into an MSI.

Shared Utilities & Interaction Notes

    The shared/ directory holds reusable systems: command parser, camera paths, threading, OpenEXR recording, etc. Hooks include these headers directly (e.g., ../shared/AfxCommandLine.h)

    .

    Hooks expose command interfaces that the GUI manipulates via the injector and configuration files.

    The Rust module provides asynchronous job scheduling and JS embedding for Source 2 features, invoked from the C++ hook via FFI.

    Shader tools (ShaderBuilder, ShaderDisassembler) assist the hook projects by generating and validating HLSL bytecode used at runtime.

Example Codex CLI Queries

# Discover engine hook entrypoints
rg -n "DllMain" AfxHookGoldSrc AfxHookSource AfxHookSource2

# Explore shared command system
rg -n "CommandSystem" shared

# Inspect Rust FFI exports
rg -n "no_mangle" AfxHookSource2Rs/src/lib.rs

# Trace CMake project wiring
rg -n "add_subdirectory" -g CMakeLists.txt

# Locate GUI forms
rg -l "Form.cs" hlae

Suggested Learning Path for New Contributors

    Engine Hooks – Start with AfxHookSource2/main.cpp for modern Source 2 hooking, then review AfxHookSource and AfxHookGoldSrc for legacy engines.

    Shared Utilities – Understand common helpers in shared/, especially command parsing, camera and output modules.

    GUI (hlae/) – Learn how the WinForms launcher interacts with hooks and exposes features to users.

    Documentation – Read doc/ notes and the wiki for design rationale and feature descriptions.

    Build Pipeline – Reproduce the steps in how_to_build.txt to compile and package HLAE, including Rust, WiX and installer tasks.

**Overlay GUI (Source 2)**
- **Location:** `shared/overlay/` (core + adapters), integrated into `AfxHookSource2`.
- **Purpose:** Minimal in-game overlay (Dear ImGui) for CS2 to expose quick controls and diagnostics without impacting game input when hidden.
- **Core:**
  - `shared/overlay/Overlay.h/.cpp`: Visibility state, per-frame Begin/Render/End, device/resize hooks, logging.
  - `shared/overlay/InputRouter.h/.cpp`: Win32 `WndProc` hook, hotkey toggle (default `VK_F10`), input pass-through when hidden, fallback polling if messages are not received.
  - `shared/overlay/IOverlayRenderer.h`: Backend-agnostic renderer interface.
- **Backends:**
  - `shared/overlay/OverlayDx11.h/.cpp`: DX11 adapter using Dear ImGui Win32 + DX11 backends. Creates/binds a backbuffer RTV and handles resize/device-loss.
  - `shared/overlay/OverlayVk.h/.cpp`: Vulkan stub placeholder (to be implemented when Vulkan hook path is ready).
- **Dear ImGui Sources:**
  - Vendored under `shared/overlay/third_party/imgui/`.
  - Recommended to use official Dear ImGui core files: `imgui.cpp`, `imgui_draw.cpp`, `imgui_widgets.cpp`, `imgui_tables.cpp` and backends `backends/imgui_impl_win32.*`, `backends/imgui_impl_dx11.*` (drop-in).
  - CMake conditionally builds with official sources if present; otherwise a stub compiles but will not render.
- **Integration (DX11):**
  - `AfxHookSource2/RenderSystemDX11Hooks.cpp`:
    - In `New_CreateSwapChain`: lazily initialize the overlay renderer (DX11) by querying device/context from the swapchain; hooks swapchain `Present` once.
    - In `New_Present`: render overlay if visible; also contains a robust lazy init fallback if the renderer wasn’t created yet. Logs once if no supported renderer is detected.
- **Runtime Behavior:**
  - Diagnostic watermark drawn every frame when visible: `HLAE Overlay (diagnostic)` in the top-left.
  - Minimal window titled `HLAE Overlay` with example controls (stub checkbox/button) and FPS text.
  - Hotkey toggle (default F10). When hidden, overlay does not intercept input; when visible, it captures mouse/keyboard via `WndProc`.
  - Fallback toggle polling via `GetAsyncKeyState` activates if no `WM_KEYDOWN` events are seen for several frames.
- **Logging (prefixed "Overlay:")**
  - Renderer: `renderer=DX11`
  - WndProc hook: `WndProc hook installed hwnd=..., tid=...`
  - First keydown seen: `first WM_KEYDOWN vk=0x..`
  - Visibility changes: `visible=true/false` (startup forces `visible=true` for diagnostics)
  - Fallback notice if no backend matched: `no supported renderer detected`
- **Build Notes:**
  - `shared/overlay` builds a static library `hlae_overlay` and links into `AfxHookSource2` (x64).
  - Include path adds `shared/overlay/third_party/imgui` so backends can `#include "imgui.h"`.
  - DX11 only at the moment; run CS2 with `-dx11` until Vulkan adapter is implemented.

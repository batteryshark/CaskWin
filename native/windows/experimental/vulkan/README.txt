Native Windows backend for Wine Vulkan
=====================================

This backend keeps Wine's public Vulkan loader, generated dispatch/conversion,
client handles, command pools, command buffers and manual compatibility code.
The upstream vulkan-1.dll forwards to WineVulkan. It is not a native-loader
alias. This is an opt-in builtin backend in the native catalog. The directory
name is historical; production builds use the reviewed sources and lower-level
build scripts here. Vulkan support is limited to the version/extension subset
described below, and the default WineD3D renderer remains OpenGL.

Implementation and limits
-------------------------
The DLL compiles reviewed checkout sources directly, guarded by
WINEDOWS_NATIVE_VULKAN. WINEDOWS_VULKAN_HOST exposes the host object definitions
without WINE_UNIX_LIB, preserving Win32 calling conventions and x86 alignment.
prepare.ps1 emits driver-entry metadata and an unmodified standalone-probe
header only; it performs no source substitutions. make_vulkan preserves the
native guard when the generated conversion source is regenerated.

backend.c implements Wine's vulkan_funcs ABI using the architecture-matching
installed Windows Vulkan loader and ICD. Under a Winedows shim it requires
WinedowsLoadSystemVulkan(), the authoritative fixed-system-file loader with
identity checks and prefix/provider bypass. Missing or failed helper calls
fail initialization; they do not retry a less trusted path. Without a shim,
the standalone backend uses a fixed architecture-specific system path and
file identity check. Ordinary API commands still pass through Wine's dispatch
and conversion code before the Windows ICD.

The advertised core version is Vulkan 1.0. Extensions currently enabled, only
when supplied by the host, are surface, win32_surface,
get_physical_device_properties2, swapchain, maintenance1,
shader_draw_parameters and vertex_attribute_divisor. Driver entries requiring
unimplemented adapter operations are unavailable through procedure lookup.
Newer Vulkan core versions, external Win32 memory/synchronization, debug
callbacks and unlisted extensions are not advertised or validated. This is
not full modern Vulkan/DXVK compatibility.

Resources, registration and loader lifetime
-------------------------------------------
Both native DLLs include the upstream version resources, including the
forwarder's installer-compatible 1.0.0.0 version. The native frontend is not
an independent ICD: it calls the installed Windows Vulkan loader itself.
Its build therefore excludes the upstream 1.4 ICD manifest, and
DllRegisterServer/DllUnregisterServer return ERROR_NOT_SUPPORTED before any
file or registry writes. Ordinary Wine retains its original ICD registration.

The host loader reference is retained for the process lifetime. Repeatedly
unloading/reloading WineVulkan can retain additional references to that same
host module. The implementation deliberately avoids driver teardown from
DllMain while the Windows loader lock is held; explicit unload/reload reference
cleanup remains a limitation. Vulkan resources are still destroyed by their
normal API calls and the lifecycle tests exercise those calls.

Win32 WSI and WineD3D
--------------------
Direct Vulkan retains native surface capabilities and native acquire/present
results. Empty and minimized surfaces do not receive invented nonzero extents;
applications must defer swapchain creation until native capabilities allow it.
The WSI probe does that, checks the same surface through resize/minimize/
restore, and handles SUBOPTIMAL/OUT_OF_DATE when reusing the old swapchain.

WineD3D has a different obligation: DXGI render buffers must remain usable
while the app window is empty. On native Windows its Vulkan swapchain path now
keeps those buffers, submits rendering without WSI presentation while empty
or iconic, and creates presentation resources on the first nonempty present.
Destroy/recreate clears owned arrays and handles, preventing stale-pointer
cleanup after a failed recreation. The Wine-owned hidden DXGI device window
is a nonempty 1x1 popup on native Windows. Application HWNDs are never resized
by that workaround. Ordinary Wine retains its existing hidden-window behavior.

WineD3D upstream currently chooses the first Vulkan physical device. The
backend exposes the actual device list, and the direct probe supports
--vendor 0x8086 or --vendor 0x10de (as separate arguments), but this does not add
multi-adapter selection to WineD3D. Keep those coverage claims separate.

Build and tests
---------------
Prerequisites: existing native/windows/build/wine-x86 and wine-x64 Wine tools
and import libraries; C:\msys64\mingw32 and mingw64 GCC toolchains; installed
Windows Vulkan loaders and hardware ICDs. MSYS2 is a build tool only; no POSIX
runtime DLL, WSL, driver install or security setting change is used.

The standard build-wine.ps1 now builds winevulkan and vulkan-1 through
build-vulkan.ps1. To select the builtin for an application use the Wine override
vulkan-1=b (or b,n for permissive fallback). WineD3D Vulkan additionally needs
WINE_D3D_CONFIG=renderer=vulkan with its Wine D3D/DXGI provider selected.
The provider catalog preserves independent explicit native choices. Fallback
orders apply to module availability; rejection of an unsupported Vulkan API
version by an already loaded builtin does not automatically retry native.
Choose vulkan-1=n for applications requiring the host's broader Vulkan API.

For standalone development from the repository in PowerShell:

  & native/windows/experimental/vulkan/build-all.ps1 -StageDirectory <stage>
  & native/windows/experimental/vulkan/run-standalone.ps1 -StageDirectory <stage>
  & native/windows/experimental/vulkan/run-standalone.ps1 -StageDirectory <stage> -Vendor 0x10de

All scripts accept -StageDirectory;
the default is native/windows/test-output/vulkan-experimental. The lower-level
prepare.ps1, build.ps1 -Arch x86/x64 and build-forwarder.ps1 -Arch x86/x64 can
be used by the central release builder without building fixtures. These scripts
write only their selected stage and read the shared Wine tool builds.

Each direct probe runs three full create/draw/present/destroy cycles and checks
64 clear/readback pixels plus 64 rasterized vertex/fragment pixels per cycle.
It validates actual Win32 swapchain acquisition/presentation, empty-window
recovery, Wine object identity, native-provider identity, extension uniqueness,
bounded enumeration, unknown extension rejection and the advertised core limit.
The shader colors and pixels are independently known. Native runs are comparison
baselines; production override policy is verified by a separate test.

With a stable runtime stage, use prepare-prefix.ps1 then run-prefix.ps1 and
run-wined3d.ps1 with -RuntimeDirectory and -StageDirectory. These tests keep
app-local Wine Vulkan DLLs in their own prefix for pre-catalog integration.
Use test-catalog.ps1 with the same arguments for production catalog checks: it
uses no app-local Vulkan DLLs, tests static/dynamic builtin/native orders and
missing-builtin/missing-dependency fallback versus strict rejection. The static
fixture has a real vulkan-1!vkGetInstanceProcAddr PE import.

Run test-resources.ps1 against the built runtime to check both version
resources and explicit unsupported registration inside a private prefix. It
verifies sentinel-file/ICD-registry preservation and compares read-only host
manifest/registry snapshots. It never performs host registration.

The independent native/windows/test-wined3d-vulkan.ps1 suite compares native
and Wine D3D11 device-only creation, actual shader pixels, initially-empty HWNDs,
resize, minimize, restore and repeated destruction. Its instrumentation counts
real WineVulkan submits and presents, avoiding an OpenGL-fallback false positive.

Evidence and release conditions
-------------------------------
The 0.5 retained prototype is in test-output/vulkan-native-prototype and
vulkan-experimental. It failed at the private 120x0 DXGI window. The initial 0.6
fixes are recorded in test-output/vulkan-06: 6 standalone and 4 prefix direct
cases pass, and WineD3D shader/readback/present/resize passes both architectures.
Fresh direct-source binaries are in test-output/vulkan-06-production. Its
standalone-result-0x8086.json and standalone-result-0x10de.json pass all 12
native/Wine/forwarder cases across both architectures, with Intel UHD 770 and
NVIDIA RTX 4090 selected and logged explicitly. The independent lifecycle
fixture passes 8 Wine cases across ordinary/virtual desktops and normal/empty
initial windows, counting actual Vulkan submits/presents. The final independent
20-case native/Wine lifecycle and 4-case OpenGL regression results are in
test-output/wined3d-vulkan-b777dc74239c4212a5b4a4b340c87c59/result.json and
opengl-result.json: 60 lifecycle cycles, 8,640 checked shader pixels, and
348 WineVulkan submissions / 132 presentations.

The hardened backend passes 36 catalog cases in
test-output/vulkan-06-production/catalog-c4bccede3625492fa5f8936b19c99644/result.json:
static/dynamic imports, builtin/native orders, actual RTX 4090 builtin draws,
missing builtin and missing WineVulkan dependency fallback, and intended strict
rejections. Each successful case includes three shader/WSI lifecycles. The
backend initialization stack frame is about 2.2KiB; standalone fixed-system
path checks use bounded buffers with explicit truncation rejection.

After adding production resources, the exact central runtime build passed
resource/registration checks on both architectures and all 36 catalog cases.
Final evidence is under test-output/vulkan-06-resources/:
resources-cd2d3e6a496c4363bc9bad7c2324e78c/result.json and
catalog-fe8197baf9d842329cd08d447547baf0/result.json. The private and central
resource builds have identical text, read-only data, import, resource and
x64 unwind sections; host file/ICD registry snapshots remained unchanged.

Never mutate a release stage while fixtures read it. Preserve native fallback,
strict failure behavior and physical display modes. WineD3D's first-device
limit, the Vulkan 1.0/extension subset and untested cross-vendor edge cases remain
explicit limitations; this backend does not establish game-wide parity.

Wine sources and backend are LGPL-2.1-or-later; generated declarations retain
their Khronos notices. The checkout sources are the authoritative build inputs.

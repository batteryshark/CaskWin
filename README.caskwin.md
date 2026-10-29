# CaskWin: native Windows host backend for Wine

CaskWin is a standalone native Windows backend for selected Wine implementations.
This repository holds the backend in `native/windows`, a pristine Wine submodule
in `vendor/wine`, and an explicit patch profile. The validated default is Wine 11.0
at `db11d0fe6a169c457e23d007e20404643d067aa8`. Applications run as native Windows
processes. There is no WSL, Linux VM, Cygwin runtime, or Unix Wine server. Windows
provides the host drivers, window system, and NT kernel APIs.

The goal is to preserve useful Wine behavior as Windows compatibility changes,
and make it convenient to compare Wine and native DLLs on the same Windows
machine. The source separation also makes trying another Wine revision or fork
possible without turning the backend repository into a full Wine fork.

**This is an early implementation, not compatibility parity with Linux Wine.**
The upstream Wine sources remain available through the submodule. The native build reuses Wine's
registry string codec from `server/unicode.c` and now builds Wine's real
DirectDraw, Direct3D 8/9/10/11, DXGI, WineD3D, D3DX9/10/11, DirectInput 7/8, XInput, DirectSound, CD audio,
XAudio, XACT 3.7, GDI+, GLU, Vulkan, and Media Foundation source-reader implementations for Windows. These carry
the compatibility behavior in those implementations. Wine's version tables,
version queries, and comparison routines are compiled from upstream as well.
Windows' ntdll, kernel32,
user32, graphics drivers, WASAPI endpoints, and physical input remain host implementations. Prefix
redirection alone does not inherit the rest of Wine's compatibility behavior.

The current **0.11 alpha** builds 41 Wine modules for each architecture,
82 DLLs in total. It adds independent launches joining an active prefix,
including shared registry, named objects, media and virtual-desktop state.

CaskWin was previously named Winedows. Existing prefixes and scripts keep working:
the `.winedows` metadata directory, `WINEDOWS_*` environment variables, and
`winedows32.dll` / `winedows64.dll` runtime names are retained. Keep using these
names as documented below; no prefix migration or DLL renaming is required.
Historical test paths and previously released `winedows-*` archives retain their
original names.

## Build

Run these commands in PowerShell 7 from the CaskWin repository root on Windows
10/11 x64 with Git, Visual Studio C++ Build Tools and a Windows SDK:

```powershell
git submodule update --init vendor/wine
.\scripts\prepare-wine.ps1
.\native\windows\build.ps1
# Optional Wine builtins: MSYS2 MinGW x86/x64 GCC, make, flex, and bison required.
.\native\windows\build-wine.ps1
```

`wine-source.json` pins the upstream commit and ordered, SHA-256 checked patches
in `patches/wine-11.0`. Preparation creates a detached local copy at
`build/wine-source` and applies those patches there. It leaves `vendor/wine`
pristine, validates an existing prepared copy before reusing it, and refuses to
overwrite an unrelated occupied output directory. Builds and fixture/package
helpers read the prepared tree; they do not prepare or modify the submodule
implicitly. Wine paths such as `dlls/...`, `server/...`, `include/...` and
`libs/...` elsewhere in this guide refer to that selected Wine source tree.

Both x86 and x64 executables/DLLs are built in `native/windows/bin`. The build
uses Microsoft's Detours 4.0.1 source, vendored with its MIT license. MSVC is
used for Detours' supported build; C/C++ runtimes are statically linked.
Wine-derived code remains LGPL-2.1-or-later; see `COPYING.LIB` and original
source copyright notices. No game payload is included in this repository.
Use `build.ps1 -OutputDirectory <directory>` to stage a native runtime without
overwriting binaries used by an open game. `build-wine.ps1` also accepts that
option for its DLL output. The object-build directories remain shared, so builds
must run sequentially. Wine object caches record their selected source/profile
identity and reject reuse with a different selection. Wine builds accept
`-Modules d3dx9_43,d3dcompiler_43,d3dxof` for targeted module updates.
MSYS2 supplies build tools only. The produced DLLs import Windows libraries;
they have no MSYS2, Cygwin, or Linux runtime dependency. The Wine build includes
a Windows response-file fix in `tools/winebuild/import.c` for large archives.
`tools/wrc/utils.c` uses Wine's locale table on Windows to retain distinct
translated resources. DirectSound falls back to the device period when native
WASAPI reports zero stream latency, keeping the Wine mixer moving.
The bundled shader compiler legalizes pixel-shader 2 texture coordinates for
native D3D9, allowing Wine D3DX helpers to work with either graphics provider.

### Try another Wine revision or fork

Only the default Wine 11.0 profile has been validated. Another version may need
patch rebasing, ABI adjustments, different build targets and new compatibility
tests. Selecting a source directory alone does not establish support for it.

Create a separate profile JSON with `schemaVersion: 1`, a `name`, `upstreamUrl`,
an exact 40-character `commit`, and an ordered `patches` array. Each patch has a
`path` relative to that profile and a 64-character `sha256`. Use patches reviewed
for the selected commit; an empty array is appropriate only for a fork that
already contains the required adaptations. `-Ref` can name the pinned commit
but cannot bypass the profile's commit check.

For an existing local Wine checkout and its custom profile:

```powershell
.\scripts\prepare-wine.ps1 -Profile '.\profiles\my-fork.json' `
    -SourceDirectory 'D:\src\my-wine-fork' -OutputDirectory '.\build\wine-my-fork'
$wineSource = Join-Path $PWD 'build\wine-my-fork'
$runtime = Join-Path $PWD 'native\windows\test-output\my-fork-runtime'
.\native\windows\build.ps1 -WineSourceDirectory $wineSource -OutputDirectory $runtime
.\native\windows\build-wine.ps1 -WineSourceDirectory $wineSource -OutputDirectory $runtime
```

Preparation leaves the local source checkout unchanged. `-SourceDirectory` is
its input; `-WineSourceDirectory` on builds and source-dependent fixtures refers
to the resulting prepared copy, with its `.caskwin-source.json` provenance.
Resolution is an explicit `-WineSourceDirectory`, then `CASKWIN_WINE_SOURCE`, then
the default `build/wine-source`. Python inspection helpers use
`--wine-source-directory` with the same environment variable and default.

Use a separate CaskWin checkout for parallel source experiments, or archive the
old `native/windows/build/wine-x86` and `wine-x64` object directories before
switching profiles in one checkout. A new runtime `-OutputDirectory` protects
the old binaries but does not create separate Wine object caches. Pass the same
selected source to XACT/XAudio fixture builders, GDI+ font tests and packaging:

```powershell
.\native\windows\build-xact-probe.ps1 -WineSourceDirectory $wineSource `
    -OutputDirectory '.\native\windows\test-output\my-fork-fixtures'
.\native\windows\package.ps1 -Name 'caskwin-my-fork' `
    -RuntimeDirectory $runtime -WineSourceDirectory $wineSource
```

The package uses the selected tree's FAudio license; retain its Wine revision,
patch profile and corresponding sources when redistributing it. Run the relevant
behavioral fixtures and game controls before treating that build as compatible.
Historical results below describe their recorded Wine 11.0 builds and do not
automatically apply to a different source selection.

## Use

```powershell
$wine = '.\native\windows\bin\wine64.exe'
$prefix = "$PWD\native\windows\prefixes\my-game"
& $wine --prefix $prefix init
# Copy/install game files into $prefix\drive_c, then:
& $wine --prefix $prefix run 'C:\Games\Example\game.exe'
& $wine --prefix $prefix drive map d 'D:\ExtractedDisc' cdrom GAME_DISC
& $wine --prefix $prefix drive mount d 'D:\Images\Game.iso'
& $wine --prefix $prefix drive mount d 'D:\Images\MixedModeGame.cue'
& $wine --prefix $prefix drive list
& $wine --prefix $prefix reg import '.\game-settings.reg'
& $wine --prefix $prefix reg import '.\legacy-32bit-settings.reg' /reg:32
& $wine --prefix $prefix reg export
& $wine gpu
```

`WINEPREFIX` selects the prefix when `--prefix` is omitted. `wine64` can launch
x86 and x64 children, using the corresponding `winedows32.dll` or
`winedows64.dll`. Keep both DLLs beside the loader.

The first loader starts and owns a prefix session. Independent x86/x64 loaders
can join it using the same prefix path and runtime directory, under the same
Windows logon/session and integrity level. They share the live private registry
hive, named-object namespace and virtual media state. A differently elevated
loader or an alternate filesystem alias path is rejected. The session checks the loader/shim file
identities and holds those files against replacement while it is active.

The owner waits for every application process in the session, including joined
launches and their descendants. Its first application may exit while a joined
application continues running. Each joining loader waits only for its own nested
process tree. Joiners neither initialize the prefix nor import/export registry
text; the owner exports registry state after all session applications exit.
Terminating the owner loader ends the entire session. Terminating a joining
loader ends only the process tree that loader started. Launches from an
incompatible inherited Windows job hierarchy fail with a diagnostic.

From another terminal, use the same loader directory and prefix to start a
second application already installed there:

```powershell
& $wine --prefix $prefix run 'C:\Games\Example\utility.exe'
```

An active prefix has one desktop configuration. A join with no desktop option
uses the current configuration, including when the session has no virtual
desktop. Explicit matching options are accepted. A different name or size, or a
switch between a virtual desktop and `--desktop off`, is rejected during that
session. Cross-architecture tests verify actual shared window ancestry and
preservation of the current virtual mode after a game changes it. An explicit
match refers to the session's starting configuration; joining does not reset
the current display mode.

`init`, `wineboot`, `reg import/export`, and `drive list/map/mount/audio` require
an idle prefix. Live `drive eject`, `drive load` and `drive swap` remain supported
through the session owner. Wait for the session to end before editing its Wine
text hives or persistent drive configuration.

Legacy WinExec launches use the same prefix-aware child-process path, including
unquoted executable names containing spaces. Legacy OpenFile can reopen a file
whose short guest path is backed by a long physical prefix directory; its
128-byte OFSTRUCT limit still applies to the guest name.
Managed AnyCPU executables select their effective CLR architecture, including
required/preferred 32-bit flags, instead of treating every PE32 image as x86.
The Detours adapter checks actual process bitness before modifying a child;
this avoids an address-dependent AnyCPU failure in the 32-bit import updater.

Application and Wine named mutexes, events, semaphores, shared-memory sections,
and timers use a session namespace shared by that prefix's independently launched
applications and their children. This includes CLR-created objects,
cross-architecture children, Local/Global names,
duplicate handles and reopening queried physical names. Different prefixes and
ordinary host objects remain separate. Native Windows backend callers keep their
host namespace using bounded caller classification. This covers ordinary
BaseNamedObjects objects; it does not isolate every IPC mechanism or hide the
physical object names.

The virtual C: drive is `drive_c`. Additional drives use
`dosdevices/drives.ini`: NTFS cannot represent Wine's Unix symlink names such as
`c:` as ordinary filenames. Mappings inside the prefix can be writable; external
directory mappings and CD-ROM mappings are read-only. This is a Windows layout
adapter, not a byte-for-byte interchangeable Linux prefix.

ISO9660/Joliet mounts retain a private image snapshot and extract a read-only
filesystem view under `.winedows/media`. Raw `\\.\D:` reads return sectors from
that snapshot. Labels, Wine-derived serials, CDFS type, and 2048-byte sector
geometry are exposed without mounting a host drive. Mounting another image
while the prefix is idle replaces the mapping only after validation succeeds.
Storage is approximately the image plus its extracted files; old caches are
currently retained. The original image can be moved offline after mounting.

Mixed-mode BIN/CUE mounting supports one BINARY file with a MODE1/2352 data
track followed by AUDIO tracks. The prefix retains private source copies,
extracts the data filesystem, and prepares PCM audio without FFmpeg. Stored
INDEX 00 pregaps and inserted PREGAP silence retain their positions; TOC track
starts use INDEX 01. Data reads expose cooked 2048-byte sectors and audio reads
expose 2352-byte PCM. EDC/ECC correction, raw data-sector reads, multiple FILE
entries, audio-only CUEs, MODE2, and subchannel data are not implemented.

Games can eject and reinsert the same virtual medium through storage IOCTLs.
Presence, removal locks, and media-change counters are shared by all applications
in the prefix session, including independent joins and other-architecture children.
Empty drives retain their letter and device identity; file reads, directory queries,
new mappings, and media-dependent queries return not-ready. Reloading restores
the private snapshot and resets the CD-audio read cursor. Each new prefix session
begins with its configured medium present; joining an existing session does not
reset its media state. Already mapped pages and in-flight I/O are not revoked.
Ambiguous overlapping or redirected directory
mappings cannot be ejected.

While the loader is running, a second terminal can control its virtual drive:

```powershell
& $wine --prefix $prefix drive swap d 'D:\Images\Disc2.iso'
& $wine --prefix $prefix drive swap d 'D:\Images\Disc3.cue' DISC_THREE
& $wine --prefix $prefix drive eject d
& $wine --prefix $prefix drive load d
```

The session owner prepares and publishes each new immutable ISO/CUE snapshot.
Session applications see the new label, serial, geometry, files, TOC and audio
without restarting. Persistent drive configuration is unchanged, so the next
prefix session starts with the originally configured disc. Removal locks reject swaps. Old
file/directory/raw data handles report media-changed and must be reopened;
old raw device handles still support verification, drive identity and media
controls. Existing mapped pages and in-flight reads remain valid. The session
retains at most 127 media descriptors; cached snapshots are not yet collected.
Live replacement currently requires an unaliased private ISO/CUE CD drive.
The owner sends media arrival/removal `WM_DEVICECHANGE` volume notifications to
windows in the shared session job, including children inside the virtual desktop.
Host windows and other prefixes are excluded. Initial media is silent; swaps
deliver removal then arrival. Delivery is bounded, so hung recipients can miss
events and an excessive backlog is diagnosed and resynchronized. Query-remove
vetoes, service/device-interface subscriptions, and writable folder-media swaps
remain unimplemented.

Direct `NtDeviceIoControlFile` calls support the same emulated controls with
synchronous ALERT/NONALERT handles and null event/APC/context. Access rights,
buffers, NT status and completion byte counts are checked. Unsupported async
forms fail before changing media state; unknown controls and nonvirtual handles
retain native dispatch. This does not implement asynchronous CD device I/O.

`system.reg`, `user.reg`, and `userdef.reg` use Wine registry text syntax.
Runtime registry data is persisted in `.winedows/registry.dat`, loaded as a
private Windows application hive. Windows `.reg` import supports HKLM, HKCU,
HKCR, strings, DWORDs, hex types, and value deletions. Existing Wine text hives
are imported on initial prefix creation. Text edits made while the prefix is
idle are detected and imported before the next session. Malformed input is
rejected without overwriting the text or the existing live roots. Independent
launches joining an active session use the live hive without reimporting text.
Windows `.reg` imports default to the 64-bit view; `/reg:32` and `/reg:64`
select the intended view. Wine text hives retain their physical registry paths.
Inherited and duplicated key handles retain their native 32/64-bit view, including
cross-process and cross-architecture transfers. Routing uses native handle tags
and canonical key names rather than a process-local handle table.

Set `WINEDOWS_TRACE=1` for per-process failed file/key operation logs under
`.winedows`. Set `WINEDOWS_DEBUG=1` for a debugger loop that captures a minidump
on an unhandled exception. Debug mode currently targets native x64 debugging.
These diagnostics are written locally and are not uploaded.

## CD audio tracks

Wine's actual `mcicda` driver can play prepared virtual CD tracks through
DirectSound. Native Windows `winmm` keeps its MCI command/string parser; the
prefix supplies the TOC, track types, and real 2352-byte PCM sector reads.
Tracks are prepared once with FFmpeg and remain inside `.winedows/media`.
No FFmpeg process or runtime decoder is needed while the game runs.

For an audio-only collection numbered `Track01.ogg`, `Track02.ogg`, and so on:

```powershell
.\native\windows\prepare-cdda.ps1 -Prefix $prefix -Drive D -TrackDirectory 'D:\MusicTracks'
$env:WINEDLLOVERRIDES='mcicda,dsound=b,n'
& $wine --prefix $prefix run 'C:\Games\Example\game.exe'
```

WAV, FLAC, and MP3 are also accepted. A collection beginning with Track02 needs
an explicitly declared data track. For extracted game discs without measured
data-track geometry, `-DataSource synthetic -DataSectors 150` reserves a two-second
placeholder track01; it does not recover original disc offsets. With a mounted
ISO, `-DataSource iso -DataSectors N` checks N against that image's exact data
sector count. The existing data files and volume label remain mapped.

This adapter covers digital playback, seek, pause/resume/stop, and MCI notifications
supported by Wine. Q-channel current position follows the reader, including
prefetch, rather than an exact playback clock. Upstream Wine currently returns
numeric `1088`/`1089` for string track-type queries; natural-completion WAIT/NOTIFY
behavior also retains Wine's limits. Physical eject, analog CD playback, and
asynchronous raw IOCTLs are not implemented. Source tracks are never modified.

## Wine builtins and native fallback

The current selectable builtins are `ddraw`, `d3d8`, `d3d9`, `dxgi`, `d3d11`,
`d3d10`, `d3d10_1`, `d3d10core`, `d3dcompiler_39`, `dinput`, `dinput8`,
`xinput1_1`, `xinput1_2`, `xinput1_3`, `xinput1_4`, `xinput9_1_0`,
`dsound`, `mcicda`, `d3dx9_36`, `d3dx9_37`, `d3dx9_41`, `d3dx9_42`, `d3dx9_43`,
`d3dcompiler_42`, `d3dcompiler_43`, `d3dcompiler_47`, `d3dxof`, `d3dx11_43`,
`xaudio2_7`, `xaudio2_8`, `xaudio2_9`, `x3daudio1_7`, `xapofx1_5`, `mfreadwrite`,
`d3dx10_43`, `gdiplus`, `xactengine3_7`, `glu32`, `winevulkan`, and `vulkan-1`.
Their shared `wined3d` dependency is also selectable. Graphics and shader
compilation use bundled WineD3D; audio uses native Windows endpoints.
Choose Wine first, then native if its
builtin is unavailable:

```powershell
$env:WINEDLLOVERRIDES='ddraw,d3d8,d3d9,dinput,dinput8,dsound,mcicda=b,n'
& $wine --prefix $prefix run 'C:\Games\Example\game.exe'
# Or native first; individual modules can use different providers:
$env:WINEDLLOVERRIDES='ddraw=b;d3d9=n,b'
# Wine helpers can also be used with native D3D9:
$env:WINEDLLOVERRIDES='d3dx9_43=b,n;d3d9=n'
# Wine D3D11 with Wine XInput, retaining native availability fallback:
$env:WINEDLLOVERRIDES='d3d11,xinput1_4=b,n'
# Wine GLU helpers with the native Windows OpenGL driver:
$env:WINEDLLOVERRIDES='glu32=b,n'
```

`b` means Wine builtin only, `n` means native only, and `n,b` reverses the order.
Long forms `builtin,native` also work. Native means the application's own DLL
or Windows' normal DLL search. Missing strict builtins are reported instead of
silently falling back. With no settings, the current default remains native.
Required Wine helper dependencies are resolved automatically, with explicit
dependency overrides taking precedence. Selecting D3DX does not force the
game's D3D9 device to use Wine.
Versions 37, 41 and 42 preserve their upstream export sets. Their tests exercise
include callbacks, shader semantics/constants, DDS and RGB565 textures, and
rendered pixels with both native and Wine D3D9. This does not establish behavior
for every D3DX export or upstream stub.
Wine D3DX10_43 supplies shader compilation, texture helpers, matrix math through
D3DX9_36, state effects, and precompiled shader effects. Its dependencies are
selected automatically; the game's D3D10 provider remains independently selectable.
Version checks match native HRESULT behavior. Tests cover both architectures,
native and Wine graphics, and rendered pixel readback. Sprite/font drawing,
Effect::IsValid, and FX source with embedded CompileShader retain upstream
limitations; native helpers remain available for applications needing them.

Wine GLU is the unmodified upstream `glu32` implementation, built for x86/x64.
It supplies projection math, scaling/mipmaps, tessellation and quadrics while
`opengl32`, WGL and the GPU driver remain native. Integrated tests pass 32 cases
covering static/dynamic clients, native/Wine providers, both architectures,
actual Intel UHD 770 and NVIDIA RTX 4090 rendering, missing-builtin fallback and
strict-builtin rejection. They check mapped module identity, native OpenGL import
targets and independently expected geometry, texels and pixels. NURBS remains
unsupported in Wine GLU; applications needing it must select `glu32=n`.
The `b,n` order falls back when a module is unavailable; it does not retry an
unsupported GLU function through native code. See
[`glu32-compatibility.txt`](native/windows/glu32-compatibility.txt) for the bounded
API coverage and [`glu32-notices.txt`](native/windows/glu32-notices.txt) for its
additional SGI and other source notices.

Wine D3DX11_43 supplies version checks, shader compilation and texture loading.
Its helper can use Wine or native D3D11 independently; its shader compiler
dependency is Wine d3dcompiler_47. Tests cover SM4/SM5 compilation, includes and
diagnostics, RGBA/BC1 DDS textures and rendered pixels. The version-check return
contract was corrected against Microsoft's SDK and native DLLs. Thread-pump
creation and texture saving retain upstream stubs; this is not every D3DX11 API.
DXGI and D3D10/11 share Wine-private interfaces and are selected as one provider
family. A strict native/builtin conflict is rejected; permissive fallback
selects a coherent family. Windows graphics-driver factory and D3D11 constructor
calls retain native backends to avoid recursion through Wine's OpenGL renderer.
Application calls still create Wine objects. Native display queries retain their
host view and API buffer contract; application queries retain virtual modes.
Native-first selection checks private application/system DLLs before their host
counterparts and respects an application's custom DLL directory. Builtin
preflight rejects mismatched architecture or invalid PE headers and tries the
next permitted provider. Dynamic loads preserve Windows' x86 System32/SysWOW64/Sysnative
views, including thread redirection disable/revert and exempt system folders.
Private system DLLs now work for native startup imports, including transitive
dependencies and cross-architecture children. The loader configures each
suspended child's initial DLL directory before it runs; original executables
and DLLs are unchanged. This depends on validated Windows process-parameter
layouts and rejects unsupported layouts. An application's existing custom
`SetDllDirectory` retains precedence. For supported import graphs the loader
follows app/custom/private dependencies and preloads the actual imported
providers before startup. Native DLLs need no artificial ordinal-1 export.
Logical prefix custom directories use their private overlay before host
readthrough; the exact logical System32 root selects SysWOW64 for a 32-bit child.
The application still sees its original inherited DLL-directory value.

Unsupported preplanning (such as ambiguous manifests, dependency cycles or
bounded-parser limits) reports a diagnostic and keeps the native search path.
A malformed selected private provider or a failed suspended-child binding
stops that launch. Dependencies reachable only through host or SxS-resolved
branches are not traversed for private fallback. Complete Windows/Wine loader
semantics, including every WOW64 custom-directory variant, remain incomplete.

Persistent settings use Wine's normal private registry locations:
`HKCU\Software\Wine\DllOverrides` and
`HKCU\Software\Wine\AppDefaults\game.exe\DllOverrides`.
The environment wins over application settings, which win over prefix settings.
Registry values are module names (for example `ddraw`) with string data `b,n`.
Import these through the loader's `reg import` command, or edit an idle prefix's
Wine text hives. Prefix files and registry state are shared between providers.

The loader preloads selected Wine DLLs before Windows binds the executable's
imports. Direct EXE imports also bind to the exact selected Wine path in the
suspended process, allowing Wine GDI+ to take precedence over a native side-by-side
assembly. The original game files remain unchanged. Known transitive side-by-side
conflicts are reported; imports reached only through unresolved native or
side-by-side branches still need separate support. Child hooks carry
the choice across architectures and custom environments. Current fallback is
based on provider availability. It does not retry a failed program, recover from
DLL initialization/dependency errors, or implement complete Wine loader behavior.
Selected Wine providers also handle explicit system/application DLL paths and
module-handle lookups. Resource/datafile loads retain the requested file.
An empty override disables a DLL, for example `WINEDLLOVERRIDES='example='`.
This covers direct and transitive executable loads, full paths, startup imports,
and children. Data/resource loads and reuse of an already loaded module remain
available, as in Wine. DLL aliases opened through hard links or symbolic links
retain the requested name through local handle duplication and image mapping.
Startup imports and export forwarders are checked before the child runs; unknown
or ambiguous graphs fail with a diagnostic when a disabled policy is active.
Registry and environment precedence also apply to empty overrides.

Core DLLs, KnownDLLs and API-set contracts cannot be disabled. Path-qualified
override keys and conflicting exact/wildcard orders are rejected explicitly.
The native Windows backend remains available internally. Alias tracking is
bounded to 4,096 handles per process and does not transfer requested-name metadata
through inherited or cross-process file handles. These controls are compatibility
settings, not a security boundary. General KnownDLL replacement and unsupported
process-launch mechanisms still need work.

`use-wine-graphics.ps1` is an optional application-local installation/restore
utility. Ordinary loader overrides do not need it.

The build and package include Wine's `winevulkan` and `vulkan-1` providers.
They compile Wine's actual dispatch and wrappers with a native Windows driver
backend. To try WineD3D's Vulkan renderer:

```powershell
$env:WINEDLLOVERRIDES='d3d11=b,n;vulkan-1=b,n'
$env:WINE_D3D_CONFIG='renderer=vulkan'
& $wine --prefix $prefix --desktop 'Game,1280x720' run 'C:\Games\Example\game.exe'
Remove-Item Env:WINE_D3D_CONFIG
```

For a Vulkan application, selecting `vulkan-1=b,n` alone uses Wine's Vulkan
provider. This backend exposes Vulkan 1.0 and a limited extension set, including
Win32 surfaces and swapchains. Newer core versions, external-memory sharing,
and debug callbacks are not supported. Availability fallback does not retry a
builtin that rejects an application's Vulkan requirements. Select `vulkan-1=n`
to use the native loader instead.
The native backend is not an independently installable Vulkan ICD; its
registration exports return unsupported to avoid registering it with the same
Windows loader that it uses for driver access.

Real shader readback and presentation pass on Intel UHD 770 and RTX 4090 with
both architectures. WineD3D currently selects its first Vulkan device upstream;
direct Vulkan tests can select either GPU. Empty or minimized windows defer
presentation and resume after restoration; resize and repeated swapchain
creation are exercised in ordinary and virtual desktops. The default graphics
policy is unchanged. Implementation details and evidence are in
`native/windows/experimental/vulkan/README.txt`.

## Hardware video decoding

Wine's `mfreadwrite` source reader can use the native Windows Media Foundation
platform, MP4 source, H.264/AAC decoders, video processor and D3D11 hardware device.
Select it with:

```powershell
$env:WINEDLLOVERRIDES='mfreadwrite=b,n;dxgi,d3d11=n'
& $wine --prefix $prefix run 'C:\Games\Example\game.exe'
```

New prefixes receive narrow, validated defaults for the required native MF
classes, file/MP4 handlers, H.264/AAC transforms and color processor. Existing private registrations,
including empty or broken keys, take precedence. Missing optional Windows MF
components do not prevent general prefix creation, but decoding needs them.

The source-reader adaptation pulls output on demand so it does not exhaust a
native decoder's surface pool while holding its consumer lock. Tests use a
generated H.264 clip, check decoded pixels and real DXVA decoder submissions,
and present decoded frames. Lifecycle coverage includes synchronous and
asynchronous reads, end-of-stream, seek, flush and drain. The audiovisual fixture
also checks decoded stereo tones and timestamps, RGB conversion through the
native video processor, multiple outstanding reads, and stream selection while
source startup is pending. Wine coalesces pending starts and reconciles changed
selection after acknowledgement; the native source is not repeatedly restarted
for each queued read. Fresh-prefix tests preserve existing private codec keys,
including intentionally broken registrations.

Queued responses remain assigned to their original read callbacks, and stale
work after a flush cannot consume a new response. A controlled work-item test
reproduces both failures in the previous reader and verifies the correction on
x86/x64, including queued end-of-stream, drain and reentrant reads. This test
uses generated PCM data without audio playback or GPU access.

Hardware decoding currently requires **native D3D11**. Wine D3D11 exposes no
video decoder profiles on the tested host. This is not a Wine codec stack or
complete Media Foundation port: other codecs, playback-clock audio/video synchronization,
Media Engine, DRM and sink-writer parity need separate validation.

## Virtual desktops and display modes

Run a game inside a desktop window, with its own resolution:

```powershell
$env:WINEDLLOVERRIDES='ddraw,d3d8,d3d9=b,n'
& $wine --prefix $prefix --desktop 'Game,800x600' run 'C:\Games\Example\game.exe'
# Wine's familiar explorer command syntax also works:
& $wine --prefix $prefix explorer /desktop=Game,640x480 'C:\Games\Example\game.exe'
```

The desktop advertises virtual resolutions including 320x240, 640x480, 800x600,
1024x768, and larger modes, with 16-bit and 32-bit color. A game can switch
between them even when the physical monitor does not offer those modes.
The container changes size; the host monitor mode stays unchanged. WineD3D
renders and converts the low-color surfaces. Window, monitor, cursor, and
coordinate APIs provide a virtual desktop view, shared by intercepted child
processes across architectures. Independent joins attach to that same container;
native USER32 ancestry checks verify both architecture directions. Closing the
desktop terminates all application trees in the prefix session.
Monitor selection, work-area queries and DPI-aware screen metrics report the
virtual bounds. On Windows with mixed-DPI hosting support, the container preserves
the game's DPI awareness when adopting its windows. Tests cover unaware and
per-monitor-aware applications at the host's 96 DPI; higher display scaling still
needs validation. An application can request a window larger than the desktop.
For a resizable game such as Castlevania Advance Collection, double-clicking its
inner title bar maximizes it within the container and lets the game resize its view.
For sizing diagnostics, set `WINEDOWS_TRACE_DESKTOP=1` before launch. Up to 128
geometry records per process are appended to the prefix's
`.winedows/desktop-<PID>.log`, including GUI programs without standard error.
Tracing is off by default and records no window titles, pixels or input.
Ordinary application callbacks during window creation, showing and resizing now
retain the virtual bounds, including nested window operations. A callback that
tail-jumps directly to a hooked API can lose its caller identity and still see
native values. Mapping directly between a hidden unadopted window and a contained
window also retains native HWND-to-HWND behavior; it can differ from composing
their virtual screen conversions. These are remaining window-manager limits.

Persistent settings use Wine's existing keys: set the `Desktop` string under
`HKCU\Software\Wine\Explorer` to a name, then set that name's string under
`HKCU\Software\Wine\Explorer\Desktops` to `800x600` (or another size).
`HKCU\Software\Wine\AppDefaults\game.exe\Explorer\Desktop` selects a per-game
desktop. For a new session, `--desktop off` overrides persistent settings. Joining
an active session uses its existing desktop unless an explicit matching option
is supplied; an application-specific setting cannot create a second desktop in
that session.

Native resource and indirect dialogs, modal/modeless dialogs, and MessageBox
variants now stay inside the desktop, retaining logical owners, focus and
modal disabling. Nested dialogs and cross-architecture child dialogs are tested.
Application `FindWindow`/`FindWindowEx` searches see the logical desktop's windows,
including its hidden roots and windows created by children of either architecture.
Other prefixes and the host taskbar are excluded from those desktop searches.
Native backend, explicit child-window and message-only searches retain their
native behavior.

Menu-bearing top-level windows created hidden now retain their native menu and
frame style when adopted before their first show. This narrow path preserves
the client size and allows native menu commands inside the container. It does
not cover every creation sequence: visible-at-creation menu windows and menus
added after ordinary child conversion still need separate validation. Space
Cadet's New Game and Launch Ball commands work through this path, but its black
menu appearance after returning to the game and complete focus-return behavior
remain unresolved. The cause of that visual issue has not been established.

Wine DirectDraw/D3D9 and native **windowed** DirectDraw/D3D9 pass rendering
fixtures in the container. This is an experimental container, not a complete
port of Wine's desktop/window manager: general popup-menu behavior, service notifications, all DPI transitions,
multiple monitors, native DXGI exclusive fullscreen, raw input, and all
window-management APIs are not yet covered. There is no 8-bit palette desktop
emulation or scaling of a fixed virtual resolution to a resizable container yet.
Applications using uncovered APIs can create windows outside the container.

Outside a virtual desktop, selecting Wine graphics adds emulated 16-bit modes
at host-supported resolutions. Display resets restore the mode captured at
session startup. Persistent host display changes are rejected by this adapter.
Choosing native graphics without a virtual desktop retains the host display API.

## Wine Windows-version settings

The `Version` string under `HKCU\Software\Wine` or
`HKCU\Software\Wine\AppDefaults\game.exe` selects Wine's Windows version
(for example `win98`, `winxp`, `win7`, or `win10`). New prefixes default to
`win10`; `native` selects the host's normal version responses. Version queries,
verification APIs, and the Windows 9x missing-DLL error rule use Wine behavior.
With a Windows 9x version selected, ANSI RegSetValueExA also uses Wine's rule
of measuring REG_SZ strings instead of trusting the supplied byte count. This
supports older programs that otherwise save truncated paths on NT. Unicode,
other value types, and native Windows callers retain their normal behavior.
Host Windows and driver modules retain native version responses so modern
drivers do not receive the game's emulated Windows version.
This does not recreate every behavior of an older OS: direct PEB reads and
other version-sensitive APIs still require further work.

Validation:

```powershell
.\native\windows\test.ps1 -SkipBuild
.\native\windows\test-wine-graphics.ps1 -SkipBuild
.\native\windows\test-dll-overrides.ps1 -SkipBuild
.\native\windows\test-override-disabled.ps1
.\native\windows\test-disabled-startup.ps1
.\native\windows\test-disabled-alias.ps1 -RuntimeDirectory '.\native\windows\bin' -MarkerDirectory '.\native\windows\bin'
.\native\windows\test-compat.ps1 -SkipBuild -RuntimeDirectory '.\native\windows\bin'
.\native\windows\test-display.ps1 -SkipBuild
.\native\windows\test-desktop.ps1 -SkipBuild
.\native\windows\build-desktop-menu-probe.ps1 -OutputDirectory '.\native\windows\bin'
.\native\windows\test-desktop-menu.ps1 -SkipBuild -RuntimeDirectory '.\native\windows\bin' -FixtureDirectory '.\native\windows\bin'
# Additional MinGW-built fixtures (build these before using -SkipBuild):
.\native\windows\build-input-probe.ps1
.\native\windows\build-d3d8-probe.ps1
.\native\windows\build-media-probe.ps1
.\native\windows\build-registry-probe.ps1
.\native\windows\build-loader-paths-probe.ps1
.\native\windows\test-input.ps1 -SkipBuild
.\native\windows\test-d3d8.ps1 -SkipBuild
.\native\windows\test-media.ps1 -SkipBuild
.\native\windows\test-registry.ps1 -SkipBuild
.\native\windows\test-loader-paths.ps1 -SkipBuild
.\native\windows\test-audio.ps1 -SkipBuild
.\native\windows\build-cdda-probe.ps1
.\native\windows\build-cdda-media-probe.ps1
.\native\windows\test-cdda-media.ps1 -SkipBuild
.\native\windows\test-cdda.ps1 -SkipBuild
.\native\windows\test-registry-import.ps1
.\native\windows\test-pe-architecture.ps1
.\native\windows\build-d3dx9-probe.ps1
.\native\windows\test-d3dx9.ps1 -SkipBuild
.\native\windows\build-d3dx10-probe.ps1 -OutputDirectory '.\native\windows\bin'
.\native\windows\test-d3dx10.ps1 -FixtureDirectory '.\native\windows\bin'
.\native\windows\build-d3dx11-probe.ps1 -OutputDirectory '.\native\windows\bin'
.\native\windows\test-d3dx11.ps1 -FixtureDirectory '.\native\windows\bin'
.\native\windows\build-glu32-probe.ps1 -OutputDirectory '.\native\windows\bin'
.\native\windows\test-glu32.ps1 -RuntimeDirectory '.\native\windows\bin' -FixtureDirectory '.\native\windows\bin'
.\native\windows\build-xaudio-probe.ps1
.\native\windows\test-xaudio.ps1 -SkipBuild
.\native\windows\test-d3d11.ps1
.\native\windows\test-xinput.ps1
.\native\windows\test-cue.ps1
.\native\windows\test-com-marshaling.ps1
.\native\windows\test-imaging.ps1 -RequireWicCom
.\native\windows\test-imaging-defaults.ps1
.\native\windows\test-native-startup.ps1
.\native\windows\test-startup-imports.ps1
.\native\windows\test-managed-startup.ps1
.\native\windows\test-compiler47.ps1
.\native\windows\test-media-state.ps1
.\native\windows\test-media-swap.ps1
.\native\windows\test-desktop-dialogs.ps1
.\native\windows\test-video.ps1
.\native\windows\test-video-fallback.ps1
.\native\windows\test-av.ps1
.\native\windows\test-reader-queue.ps1 -ReaderDirectory '.\native\windows\bin'
.\native\windows\test-named-objects.ps1
.\native\windows\test-d3dx9-versions.ps1
.\native\windows\test-nt-media.ps1 -RuntimeDirectory '.\native\windows\bin'
.\native\windows\test-media-notifications.ps1
.\native\windows\test-registry-transfers.ps1 -RuntimeDirectory '.\native\windows\bin' -FixtureDirectory '.\native\windows\bin'
.\native\windows\build-wined3d-vulkan-probe.ps1 -OutputDirectory '.\native\windows\bin'
.\native\windows\test-wined3d-vulkan.ps1 -SkipBuild -RuntimeDirectory '.\native\windows\bin'
# Package and verify the actual extracted archive:
# Include AV, D3DX9/10/11, GDI+, XACT, GLU, disabled-DLL and WineD3D Vulkan probes
# and their fixture assets (including d3dx10-draw.fxo) in FixtureDirectory.
.\native\windows\package.ps1 -Name 'caskwin-0.11-alpha'
.\native\windows\test-package.ps1 -Archive '.\native\windows\dist\caskwin-0.11-alpha.zip'
```

The independent-launch fixtures use disposable prefixes and original test
programs. Point them at a complete staged runtime:

```powershell
.\native\windows\test-prefix-session.ps1 -RuntimeDirectory $runtime
.\native\windows\test-prefix-session-lifecycle.ps1 -RuntimeDirectory $runtime
.\native\windows\test-prefix-session-desktop.ps1 -RuntimeDirectory $runtime
```

Both Wine and native DirectDraw/Direct3D 9 pass surface/device creation and pixel
readback in x86 and x64. WineD3D uses the Intel UHD 770's OpenGL 4.6 driver on this
machine; its emulated Direct3D adapter identifier differs from the actual GPU.
Wine Direct3D 8 now passes triangle drawing, exact pixel readback, and presentation
on x86/x64 in ordinary and virtual desktops, including startup imports. Native
x86 also passes; this host lacks native x64 D3D8, where `n,b` falls back to Wine.

## Implemented and exercised

- NT file create/open/query/rename/delete redirection into the prefix.
- Private user/machine registry create/open/value/delete operations.
- Canonical NT key names, private HKCR machine/user merging, and private
  in-process COM registration/activation with user overrides.
- Separate 32/64-bit registry views from x86 and x64 callers, including relative
  child opens, shared Classes roots, inherited and cross-process duplicated
  handles, plus explicit registry import views.
- Per-prefix user profile, AppData, Documents, and known-folder paths.
- Process-local drive enumeration/type/label/serial reporting; mapped CD-ROM
  content can be read and writes are rejected.
- Child-process DLL propagation, including x86/x64 transitions and custom
  environment preservation.
- Independent x86/x64 launches sharing a live prefix hive, named objects and media,
  owner lifetime through the last process, and separate joining-loader shutdown.
  Core, lifecycle and shared-desktop fixtures pass, including preservation of
  live virtual modes and the physical host display mode.
- Wine/native DLL selection for supported modules, including startup
  imports, basename/full-path loads, and cross-bitness child processes.
- Wine DirectInput 7/8 keyboard/mouse enumeration, configuration, acquisition,
  state polling, buffered reads, and event registration on x86/x64, with native
  controls, startup/dynamic imports and native fallback. These are keyboard/mouse
  API tests; fresh physical key/mouse event delivery, controllers, and force
  feedback have not been validated with real-device input.
- Wine XInput 1.1/1.2/1.3/1.4/9.1.0 frontend with Windows XInput hardware
  transport, including startup imports and native fallback on both architectures.
  Query/error/export tests and native backend identity checks pass with no attached
  controllers. Physical button/axis transitions, hotplug, vibration, and controller
  audio remain untested; Winebus mappings for generic HID controllers are not
  supplied by this transport.
- Wine/native DirectSound software mixing through actual WASAPI render endpoints,
  COM activation, buffer controls, advancing playback positions, and playback/stop
  notifications on x86/x64, including virtual desktops. Fixtures use muted silence.
- Wine D3DX9 math, shader compilation/reflection, DDS textures, RGB565 upload,
  and rendered shader readback with Wine or native D3D9, both architectures,
  startup/dynamic imports, and ordinary/virtual desktops.
- Wine GLU projection, image scaling/mipmaps, tessellation and quadric rendering
  over native OpenGL, including both architectures, static/dynamic imports and
  native availability fallback. NURBS requires native GLU.
- Wine d3dcompiler_47 SM4/SM5 compilation, macros/includes and reflection,
  rendered pixels and typed-UAV compute with Wine or native D3D11 on both
  architectures. Structured-buffer stores remain unimplemented upstream;
  these selected cases do not establish complete Shader Model 5 coverage.
- PNG/JPEG metadata, texture loading, and rendered readback with Wine D3DX9
  and native WIC on both graphics providers. Three exact native WIC class
  defaults are added only when their private user/machine keys are absent;
  existing registrations, including deliberately empty keys, remain authoritative.
- Standard IStream/ISequentialStream marshaling between STA/MTA apartments,
  including Global Interface Table use and private interface registrations.
  These narrow Wine interface defaults do not import the host COM catalog.
- Wine GDI+ (`gdiplus=b,n`), including static imports that would otherwise bind
  to Windows' side-by-side copy, native fallback, exact ARGB/PNG readback,
  installed fonts and private memory fonts. The private-font fallback validates
  a single SFNT face and rejects host font substitution; collections need further
  work. A minimal empty private WIC encoder catalog permits native PNG encoding
  without importing host codec registrations. Existing private keys take precedence.
- Wine XACT 3.7 (`xactengine3_7=b,n`) COM activation and native fallback, PCM,
  ADPCM and xWMA playback, streaming banks, pause/resume, wave notifications and
  managed/caller-owned lifetimes on both architectures. Tests use silent or muted
  fixtures. This does not redirect games that call a bundled FAudio/FACT directly.
- Wine/native XAudio2.7/2.8/2.9 streaming, refill callbacks, sample progression,
  resampling, pause/resume, startup/dynamic imports, and virtual desktops on
  both architectures. Legacy Wine XAudio2.7 COM activation, X3DAudio matrices,
  and volume-meter processing also pass. Streaming fixtures use muted silence;
  this does not establish audible quality for a particular game.
- Wine version APIs, per-game overrides, and native version opt-out.
- A virtual desktop container, 16/32-bit display modes, RGB565 hardware render
  and readback, coordinate/window containment fixtures, and shared child desktops.
- Wine/native D3D11 shader rendering, readback, resize/presentation, dynamic and
  static imports; D3D10/10.1 clear/readback/presentation. Both architectures,
  ordinary/virtual desktops, and coherent native fallback pass. D3D11 fixtures
  use native d3dcompiler_47; the additional compiler47 matrix exercises Wine's
  compiler too. Wine D3D11 still exposes zero video decoder profiles on this
  machine; the hardware-video backend requires a native D3D11 device.

The fixture currently passes in both bitnesses, including cross-bitness child
launches. Xeno Crisis 1.0.4 from the user's local GameRepo has reached its intro,
main menu, and character selection; its log identifies the NVIDIA RTX 4090
through Direct3D 11. Full gameplay and broad game compatibility are still under
test. Earlier failures and fixes must not be mistaken for proof that every
Wine-compatible game works.

Castle Crashers Remastered 3.0 reaches its menu and character selection with
Wine D3D9/D3DX9 and native DInput8/XAudio2.7; the user reports that it runs
perfectly fine. Castlevania Advance Collection reaches its title with native
graphics and either native or Wine GDI+. Read-only import inspection confirms
all 53 main-executable GDI+ function pointers belong to Wine in the latter run.
Maximizing its inner window fixes the initially oversized view. Astalon 1.1.0
(Unity 2019.4/IL2CPP) now reaches its full setup and title screens at 1280x720
with native D3D11. Its earlier oversized view came from host bounds leaking into
resize callbacks; actual game traces verify the corrected client size. Its
Wine D3D11/OpenGL run now passes setup, the animated intro, title/menu, new-save
creation, opening dialogue, and a brief gameplay check of directional input and
jumping. NVIDIA's internal D3D11 calls required
native driver routing, and the display adapter needed the native legacy buffer
contract. Game rendering remains Wine; its new save files are stored in the
prefix's private LocalLow directory. These observations do not establish complete
gameplay compatibility.

Full Tilt! Pinball's original launcher now opens Space Cadet without the missing
executable or data-path errors. Its table appears inside the virtual desktop
after correcting the host-taskbar lookup leak. In the 0.10 candidate, the menu
adaptation allows New Game and Launch Ball inside the desktop; a visible ball,
mission text and simulation progress were observed, and the user confirmed that
it plays there. The user still reports a black or hard-to-see menu bar after
returning. Focus-return behavior and full gameplay/audio quality remain incompletely
validated; the visual issue is recorded without attributing it to an upstream
Wine bug. The earlier `--desktop off` checkpoint also remains valid: the user
confirmed New Game, plunger launch and gameplay from the same private prefix
with Wine DirectSound. Spelunky reaches character
selection with Wine D3D9/D3DX9 in windowed mode, and the user confirmed that holding
Z selects a character. Gameplay is not yet verified, and its fullscreen
Alt-Tab/restore behavior remains problematic.

Oceanhorn: Monster of Uncharted Seas reaches its animated title and save-selection
screen through Wine D3D10/DXGI/WineD3D, with native
`d3dx10_43` and `d3dcompiler_43`. This combination preserves the native helper
features needed by its source effects, sprites and fonts. Brief automated input
did not start a new save, so gameplay remains unverified. The game changed the
virtual mode beyond the initial 1280x720 and its container exceeded screen bounds.
Demise B153 reached its evaluation reminder with Wine GLU, then exited during
startup before its renderer was observed. A native-GLU control also crashed
during startup. The cause remains unresolved; GLU fixture success does not
establish Demise compatibility or attribute this failure to upstream Wine.
Both games' original GameRepo archives and staged source payloads remain unchanged.

La-Mulana (executable 1.6.6.1, archive version label 2.1.0.12) renders its opening
and in-game configuration with Wine D3D9/D3DX9_43. The user confirmed gameplay
and controls work. This trial used native DirectInput/XInput, so it does not
establish Wine input hardware coverage. Deer Hunter II Extended Season reached
cropped title artwork with its supplied native cnc-ddraw wrapper, but crashed
at startup with Wine DirectDraw. Its failure has not been attributed to either
upstream Wine or this backend. These additional trials used the preserved 0.10
runtime before the standalone source-layout migration.

Additional shared-session backend trials rendered Dungeons of Infinity 1.2.1's
animated title and credits with Wine D3D11/DXGI/WineD3D. Gameplay and reliable
menu input were not established. A second loader launched from a separately
supervised host process group was rejected with Windows error 5 while the game
continued; this is the incompatible caller-job hierarchy limitation described
above, not successful joining evidence. Cursed Castilla showed blank output in
both Wine-graphics and native-graphics virtual-desktop trials; its ordinary
window remains unverified. Sonic 4 Episode 1's supplied launcher requested a
Java runtime unavailable in the private prefix. These are bounded observations,
with no upstream Wine bug attribution or game-specific workaround applied.

Nightmare Creatures reaches gameplay inside its virtual desktop. The user
confirmed music with the GameRepo-provided app-local OGG CD-audio wrapper.
Generic Wine MCICDA playback has separate lifecycle/notification fixtures;
audibility through that provider in Nightmare remains unverified.
Blossom Tales II is FNA/.NET, not Unity: the staged game now reaches its title
screen with its bundled FAudio/SDL2 and native D3D11 on the RTX 4090. Managed
startup required AnyCPU architecture selection and native .NET/OLE/RPC backend
discovery. A diagnostic using its exact bundled FAudio/SDL2 and title-music
bank found normal callback progression with WASAPI and native/Wine DirectSound.
WinMM progressed at only about half real time on both host and prefix. The
diagnostic is muted and excludes the full game/CLR workload. On October 29,
2026, the user confirmed that the actual game's title music sounds smooth
after handling its Windows Firewall prompt. This confirms audible title-screen
playback in that session; extended gameplay audio still needs validation.
Earlier read-only inspection found the game using WASAPI with 44.1 kHz mixing
converted to a 48 kHz device. Its source remained queued and advanced at normal
speed over ten seconds. A separate native waveOut control reproduces the WinMM
slowdown with two 512-frame buffers and recovers normal speed with four; this
does not explain or fix the game's WASAPI audio by itself.
Further muted diagnostics observe actual WASAPI submissions and the normalized
device clock using the copied game libraries and title cue. Host/prefix runs at
44.1/48 kHz, both idle and with bounded 60 Hz D3D11 work, submit 15 seconds of
audio in 15 seconds with maximum submission gaps of 10.40–11.56 ms. The roughly
20 ms application callback gaps at 44.1 kHz coexist with regular device writes
in these fixtures. A deliberate 120 ms stall is detected, and controls with the
observer disabled show similar timing. These fixtures do not reproduce the full
game or independently establish audible quality; no runtime buffer adjustment
is justified by their results.

## Limits that matter

This is game-state separation, **not a security sandbox**. Raw syscalls,
unhandled NT APIs, device/UNC namespaces, services, elevated processes,
out-of-process COM, and launch paths not yet covered by the child hooks can
escape interception. Named application objects have the namespace separation
described above, but general IPC, network access, clipboard, accounts, and
hardware are not isolated. Use trusted applications only. The runtime reads selected
host Windows/device/cryptography configuration and system DLLs; normal
application writes go to private state.

Native .NET, OLE/RPC, and audio/device configuration
can be read through selected host branches; application Classes registrations
remain private. General COM marshaling and out-of-process activation are incomplete.
Audio activation covers playback DirectSound/DirectSound8, native endpoint
classes, Wine/native XAudio2.7 engine/meter/reverb, and XACT 3.7 through CoCreateInstance/Ex.
Native XAudio registration is consulted first; absent registrations can use the
selected native DLL factory. Explicit Wine selection uses Wine's factory.
Capture,
full-duplex, CoGetClassObject-only activation, and OpenAL need separate work.
Bundled FAudio's FAPOFX echo, EQ, reverb, and mastering limiter DSP are upstream
stubs; successful factory creation does not mean those effects process audio.
Native providers remain available. D3DX validation covers DDS/PNG/JPEG textures
and selected helpers, not the entire D3DX API.
XACT retains upstream gaps in shared-XAudio parameters, xWMA duration metadata,
simple-wave cue EOF notifications and some filtering/effects behavior.
One x86 Wine D3D11 helper run crashed during teardown after successful rendering.
Bounded prior-runtime and candidate controls and a repeated full matrix passed;
the cause remains unresolved. See `native/windows/STATUS.md` for preserved evidence.

CD-ROM support maps directories, ISO9660/Joliet images with contiguous extents,
and the mixed-mode BIN/CUE subset described above. UDF-only images, other CUE
layouts, raw data-sector reads, and multi-extent/interleaved files,
multisession, SCSI pass-through, and raw disc authentication remain unsupported.
Live replacement is limited to the private ISO/CUE subset above. ISO/Joliet/CUE
directory-record times and hidden flags are preserved; unspecified dates use
1970 UTC, invalid dates are rejected, and access times follow host filesystem
policy. Extended attributes and other filesystem metadata are not reproduced.
Anti-cheat and kernel drivers are outside this backend.
Linux `.so` Wine modules, winetricks shell scripts, DOS/Win16 applications,
and complete Wine DLL override semantics are not
implemented. Builtin selection currently covers the modules
listed above; it is not general Wine compatibility parity.

The implementation and tests are actively evolving. Do not treat this alpha
as a replacement for a fully validated Wine installation.

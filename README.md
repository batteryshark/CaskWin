# CaskWin

An experimental native Windows backend for Wine. Run Windows applications
with private Wine-style prefixes and selectable Wine DLLs on Windows itself,
without WSL, a Linux VM, or a Unix Wine server.

**Status: 0.10 alpha. Full compatibility parity with Wine on Linux is not implemented.**

CaskWin is a standalone project containing the Windows backend, build tooling,
tests and compatibility patches. Wine is a pinned vendored source dependency,
prepared separately before building. The currently validated source profile is
Wine 11.0; other Wine versions or forks need a matching patch profile and testing.

## Why run Wine on Windows?

Windows changes over time, and older applications can depend on behavior that
has changed along the way. Wine has accumulated compatibility implementations
that can be useful to compare with, or sometimes substitute for, the APIs supplied
by a newer Windows installation.

CaskWin makes those experiments convenient on a Windows machine. You can try
Wine components, compare their behavior with native DLLs, and keep a game's files
and registry settings in a private prefix without setting up WSL or a Linux VM.
This can help investigate a compatibility problem or provide a workaround when a
supported Wine component behaves better for that application.

It is also a way to test Wine-related code against native Windows drivers and
APIs. The separate source profiles make it possible to evaluate different Wine
versions or forks without turning the CaskWin repository into a copy of each
upstream tree. Only the documented profiles and features are validated.

## What works today

CaskWin currently provides:

- Private drive mappings, file and registry redirection, child-process handling,
  and prefix-local named synchronization objects.
- Native Windows builds of Wine DirectDraw, Direct3D 8/9/10/11, WineD3D, D3DX,
  DirectInput, XInput, DirectSound, XAudio, XACT, GDI+, GLU, and selected Vulkan
  and Media Foundation functionality.
- ISO/CUE media, virtual CD-ROM operations, prepared CD audio, and live disc swaps.
- An experimental virtual desktop and virtual display modes.
- Wine/native DLL selection, including availability fallback.

Windows supplies the NT kernel APIs, core system DLLs, window system and hardware
drivers. This port inherits the behavior of the Wine components it actually
builds; it does not automatically inherit every upstream Wine compatibility fix.
Prefix isolation is intended for compatibility and configuration separation;
it is not a security sandbox.

## Try it

Use Windows 10/11 x64 and extract a prebuilt package from this repository's
GitHub Releases. The package contains both x86 and x64 runtime components.

```powershell
.\wine.exe --prefix .\prefixes\game init
```

Place your game's files in `prefixes\game\drive_c\Games\Example`, then choose
the Wine components appropriate to it. For a Direct3D 9 game:

```powershell
$env:WINEDLLOVERRIDES='d3d9,dsound=b,n'
.\wine.exe --prefix .\prefixes\game --desktop Game,1280x720 run 'C:\Games\Example\game.exe'
```

`b` selects Wine, `n` selects native Windows/application DLLs, and `b,n` falls
back to native when the Wine module is unavailable. It does not retry individual
unsupported API calls. With no overrides, the current default is native.
Use `--desktop off` to run without the virtual desktop.

See [the Windows backend guide](README.caskwin.md) for installation, drives,
registry management, per-application settings, provider combinations, tests,
and the limits of each feature. [Development status](native/windows/STATUS.md)
records the tested checkpoints and outstanding failures. Game observations
are bounded trials, not guarantees of complete compatibility.

## Build and contribute

Use PowerShell 7 for the build and test commands. The native loader requires
Visual Studio C++ Build Tools and a Windows SDK.
Wine DLL builds additionally require MSYS2 with MinGW-w64 x86/x64 toolchains,
make, flex and bison. MSYS2 is used at build time only.

```powershell
git clone --recurse-submodules https://github.com/Batteryshark/CaskWin.git
cd CaskWin
.\scripts\prepare-wine.ps1
.\native\windows\build.ps1
.\native\windows\build-wine.ps1
.\native\windows\package.ps1
```

Keep native builds sequential: their intermediate files are shared. Use the
guide's staging options before rebuilding a runtime that a game is using.

The focus is missing Wine functionality and defects in this Windows backend.
The next substantial parity gaps are joining an already-running prefix from an
independent loader, isolating application named pipes while preserving native
Windows service access, and completing virtual-desktop window/menu handling.
These remain unsupported; they are not implied by the features listed above.
For a compatibility report, include the game/version, application architecture,
CaskWin version, DLL overrides, desktop setting, and whether switching to native
components changes the result. Do not include game payloads, saves, credentials
or an entire private prefix in a public report.

## Source and licensing

The default [vendored Wine dependency](vendor/wine) is pinned to Wine 11.0,
commit `db11d0fe6a169c457e23d007e20404643d067aa8`. Our Windows backend lives in
`native/windows`; changes to Wine are recorded separately in `patches/wine-11.0`.
The preparation script checks and applies the selected patches to a separate
build source tree, leaving the vendored checkout pristine. See `wine-source.json`
and the backend guide for source selection and reproducible preparation.

Existing `.winedows` prefix metadata, `WINEDOWS_*` settings and internal
`winedows32.dll`/`winedows64.dll` names remain supported after the rename.

Wine-derived code is LGPL-2.1-or-later; see [LICENSE](LICENSE),
[COPYING.LIB](COPYING.LIB), and the original source notices. Vendored Detours is
MIT-licensed and FAudio is zlib-licensed. GLU includes additional
[SGI and other notices](native/windows/glu32-notices.txt). Preserve the applicable
notices and corresponding sources when redistributing. No game payloads or
native Microsoft redistributable DLLs are included in this repository.

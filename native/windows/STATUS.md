# Development state

## CaskWin standalone 0.10 checkpoint verified (2026-10-29)

CaskWin is a standalone repository containing our Windows backend, tooling,
tests and patches, with selectable Wine source versions/forks. Public source:
https://github.com/batteryshark/CaskWin. This checkpoint establishes the new
repository layout and CaskWin branding; older checkpoints below describe the
preserved legacy Wine checkout and its local artifacts.

vendor/wine is pristine and pinned to Wine 11.0 db11d0fe6a169c457e23d007e20404643d067aa8.
wine-source.json records the upstream URL, full commit, ordered patch paths and
hashes. scripts/prepare-wine.ps1 creates a separate patched build/wine-source.
All 45 patched upstream files match the tested legacy sources byte-for-byte.
Checksum, patch applicability, occupied-output and mismatched-tree rejection
checks pass; a matching preparation can be reused without modifying vendor/wine.
Builds and packages now select that prepared source explicitly or via
CASKWIN_WINE_SOURCE. Other Wine versions/forks still require matching patches
and validation; changing the repository layout does not establish their support.

Both architectures were rebuilt in the standalone layout, including all 82 Wine
DLLs (41 per architecture) and the native loader/backend. No legacy binary or
object cache was reused. The runtime audit passes architecture, required ordinals,
Windows-only imports and wine.exe alias checks. Desktop 9 and menu 12 regression
cases pass, with owned windows closed and the host display preserved. Source
preparation, build identity and package provenance rejection checks also pass.
Evidence: test-output/standalone-validation/result.json and its runtime-audit.json.

Verified archive: dist/caskwin-0.10-alpha.zip, 63,612,395 bytes, SHA256
4B44B17AD5D99CC710D0E442581241158ECA6997FA414694BE3345ADD6914FA0.
Extracted suite package-bae97534366940879c39fdf279cab8fd/result.json passes all
95 manifest hashes and 42 x86/x64 smoke cases. XACT/XAudio probes were rebuilt
against the selected prepared source. The archive includes portable Wine source
provenance in caskwin-build-source.json; it contains no local build paths.
Prepared patched tree: 8049109c9dd0d074dc741207c9cf156d8622c657.
The source profile and patch are stored byte-for-byte in Git so their checked
SHA256 values remain stable across Windows and Unix checkouts.

Visible product branding becomes CaskWin while .winedows metadata, WINEDOWS_*
settings, internal DLL names and existing prefix behavior remain compatible.
The current parity priorities are independent loader/session joining, private
application named pipes with native service access preserved, and incomplete
desktop window/menu sequences. Cosmetic pinball investigation remains deferred.

Additional game evidence used the immutable legacy 0.10 package, not the
new-layout build. La-Mulana (GameRepo label 2.1.0.12, executable 1.6.6.1) renders
its opening and in-game configuration with Wine D3D9/D3DX9_43; the user confirms
gameplay and controls work. Input DLLs were native, so this does not establish
Wine input hardware coverage. Deer Hunter II Extended Season displayed cropped
title art with its supplied native cnc-ddraw wrapper, then crashed at startup
with Wine DirectDraw (0xc0000005). Cause remains unresolved, with no upstream
or backend attribution. Both prefixes are closed and idle; source games remain
read-only. Detailed artifacts remain in the legacy checkout's games-11 directory.

## 0.10 checkpoint verified (2026-10-29)

Source base 2156f87, branch winedows-native. User redirected scope toward bringing
Wine features to Windows: preserve functional port fixes, document the black
pinball menu, and avoid making an unproven upstream/cosmetic issue a blocker.
No Linux control has established the source of the black menu. Full Wine/Linux
parity is still incomplete. No remote publication or PR.

Final immutable runtime: test-output/runtime10-glu, version 0.10-alpha. Its audit
passes 82 Wine DLLs (41 per architecture), five native files, architecture,
Windows-only normal/delay import closure, required ordinals and wine.exe alias.
All 80 Wine DLLs from 0.9 are unchanged; only the two unmodified Wine GLU DLLs
are added. runtime10-desktop is the earlier menu-only stage. All 0.9 stages and
archives remain frozen. Game readers have exited and their prefix locks are idle.
Verified archive: dist/winedows-0.10-alpha.zip, 63,589,105 bytes, SHA256
974B40D070E8AFDEAFEBA377A24027FDA2D6604AD16342817B9C15072E623332.
Extracted suite package-010055d3cec843c69cde62ba9d8ded4a/result.json passes all
94 manifest hashes and 42 x86/x64 smoke cases, including both new GLU GPU/import
checks. Archive and extracted evidence are frozen.

Changes and evidence (relative to native/windows/test-output):

- Wine GLU projection, image scaling/mipmaps, tessellation and quadrics now use
  the native Windows OpenGL driver. No Wine GLU source patches or new shim hooks
  were needed. Catalog, build, package and test module lists agree; notices are
  preserved in glu32-notices.txt / packaged LICENSE-GLU.txt. NURBS is unsupported:
  select glu32=n for it. b,n only falls back when a module is unavailable; it
  does not retry unsupported API calls. Independent fixture review passed.
  Integrated 32/32 cases pass across x86/x64, static/dynamic imports, native/Wine,
  Intel UHD 770/NVIDIA RTX 4090 and missing-builtin fallback/strict behavior:
  helpers-10/glu32-442d2a3698294665bce93daaa334742e/result.json.
  Desktop comparison 3/3 passes at
  helpers-10/glu32-6b6842efea884444bff3e8179725f6a2/result.json.
  These are bounded API/render tests, not a claim of general game compatibility.

- The Windows desktop adapter preserves the menu handle, native frame style
  and client dimensions for existing menu-bearing roots created hidden and
  adopted at ShowWindow. It marks that root kind before reentrant SetParent,
  bounds actual-parent ancestry and avoids duplicate activation forwarding.
  Existing menu-less roots/dialogs retain their path. Visible-at-creation menus
  and menus added after child conversion remain separate gaps. Final runtime
  regressions pass: desktop 9, dialogs 16, find 8, callbacks 16, new menus 12,
  with no timeouts and all owned processes exited. See
  display-10/final-regressions-68a9f7ebc2fb405b8098461c188c6adf/result.json.
  Independent source review passed. Earlier cross-process activation controls
  did not prove a foreground cycle; do not conflate them with these assertions.

- Actual Space Cadet inside FullTilt desktop: Alt+G/New Game/Launch Ball work,
  visible ball and simulation progress observed, and user confirms play. Client
  remains 752x520. User reports focus/menus break after return and clarifies the
  menu is black/hard to see. Full focus-return and audio/gameplay quality are not
  fully validated; cosmetic investigation is deferred by user scope. Earlier
  menu recovery after leaving/returning referred to the ordinary window.
  games-10/pinball-menu10-observation.json records the bounded trial.

- Oceanhorn renders its animated island/title and save-selection screen with
  Wine D3D10/DXGI/WineD3D and native D3DX10_43/compiler43. Loaded-module evidence
  confirms those providers. Brief automated input did not start a new save;
  gameplay and audio quality remain unverified. Its mode request enlarged the
  container beyond the initial 1280x720 and screen bounds. Closed with Alt+F4;
  games-10/oceanhorn-wine10-observation.json and -modules.json preserve evidence.

- Demise B153's 11 supplied OCXs registered successfully through the private
  prefix. Thirty relevant host class-server values were unchanged. Eight game
  settings were imported privately, with GraphOGL selected; archived Windows
  files and blanket registry/license snapshots were not installed. Wine GLU
  loaded, but the game crashed after its evaluation reminder before GraphOGL
  rendering was observed. Native GLU also crashed (0xc0000005); failure cause
  remains unresolved and is not attributed to GLU or upstream Wine. See
  games-10/demise-registration10.json, demise-*-observation.json and
  demise-startup-events10.json. Source archives/payloads are unchanged for both
  newly staged games. Reusable launchers are games-10/launch-oceanhorn.ps1 and
  launch-demise.ps1.

Package fixtures: package10-fixtures preserves 134 prior fixtures and adds two
final reviewed GLU static-import probes. Independent integration review confirms
both hashes match helpers-10/fixture-manifest.json. Package scripts parse cleanly.

## 0.9 desktop discovery evidence (superseded by 0.10 above)

At that checkpoint production retained 0.9 behavior. Native diagnostic sources,
binaries and logs are isolated under test-output/display-09. The same-thread
menu-adoption-native-result.json shows why pure WS_CHILD adoption is insufficient:
it loses HMENU, adds 20 pixels to the client area, allows a disabled menu accelerator,
and suppresses initial focus. Original-style SetParent preserves menu commands,
client size, focus, clipping and parent-relative movement. WS_CHILD|WS_POPUP is
not a valid shortcut: it also executes the disabled accelerator.

Cross-process menu-cross-*-forward*.log controls support menu/clipping/maximize/
restore behavior with original styles, but do not establish correct external
activation or WM_ACTIVATEAPP delivery. Some foreground requests are denied, and
the target remains focused during the attempted deactivation. Do not report these
as a proven activation cycle or start a style-only production patch.
The attempted matrix stopped after five logs (through 64-to-32, forwarding off),
without a completed aggregate result. Root verified no owned cross-process fixture
executables remained. Retain this incomplete outcome; do not label it a pass.

The proposed adapter change needed an explicit registered-root kind, bounded actual
GA_PARENT ancestry, corresponding style-write handling, and a visible-creation
path that preserves callback/menu/show behavior. Review host activation forwarding,
SetForegroundWindow, display-change recipients, owner mutation and dialog focus
together. The current native-container tests are discovery evidence, not a shipped
fix. The ordinary-window Space Cadet control below is the playable path meanwhile.

## 0.9 checkpoint verified (2026-10-29)

Astalon 1.1.0 now passes a brief gameplay check with actual Wine D3D11/OpenGL:
setup, animated intro, title/menu, a new File1 save, opening dialogue, directional
input and jumping. This is not a long-session or audio-quality result. Full Tilt's
missing-executable/data-path failures and offscreen table placement are fixed;
its missing menu and black display after input remain unresolved. Spelunky reaches
character selection using Wine D3D9/D3DX9 with its private Windowed setting enabled;
the user confirmed character-selection input. Fullscreen Alt-Tab restoration and
actual Spelunky gameplay remain unverified. After packaging, the user confirmed
Space Cadet launches the ball and plays with `--desktop off`, using the same
private prefix and Wine DirectSound. Its native menu bar is present. The session
was closed normally and its prefix lock confirmed idle; see
games-09/cadet-native-window09-observation.json and spelunky-user-confirmation09.json.
Reusable local scripts are games-09/play-astalon09.ps1 and play-pinball09.ps1.

Source base cf4a808, branch winedows-native. Overall Wine/Linux parity remains
incomplete. All 0.8 stages and archives are frozen, and original GameRepo files
are unchanged. No remote publication or PR.
Verified archive: dist/winedows-0.9-alpha.zip, 63,102,865 bytes, SHA256
97F95B78A038758F027597BCD4ECCDBE18D293C68B0B3C1D7F73160C8624CCBB.
Extracted suite package-87785e3468dd42cd8f94afac091553d4/result.json passes
91 manifest hashes and 40 x86/x64 smoke cases, including graphics, Wine Vulkan,
audio, audiovisual readers, GDI+, XACT, D3DX10 and disabled startup imports.
Final combined stage is test-output/runtime09-desktop, version 0.9-alpha:
40 Wine modules per architecture, 80 Wine DLLs and five native files. Its PE audit
passes architecture, Windows-only normal/delay import closure, required ordinal
and wine.exe alias checks. Wine delta from 0.8: 72 unchanged, four changed
(D3D11/WineD3D), four added (D3DX10_43/D3DX9_36). See runtime09-desktop-audit.json.
All stages are immutable. package09-fixtures contains 129 preserved 0.8 fixtures
plus five D3DX10 artifacts; package09-fixtures-audit.json records provenance.

Changes and evidence (paths below are relative to native/windows/test-output):

- Wine D3DX10_43 and D3DX9_36 are selectable with native fallback. CheckVersion
  HRESULT corrections cover versions 33 through 43 (33 uses SDK 1001). Native
  controls and 264 comparisons across 44 cases pass. Integrated shader, texture,
  math and effect checks pass 24 cases and 1,280 GPU pixels at
  helpers-09/d3dx10-f1a1b5aa537c49908ee4771619929a6a/result.json. Embedded FX
  CompileShader, sprite/font drawing and Effect::IsValid remain limited; see
  d3dx10-compatibility.txt. Package scripts include the two modules and new smokes.

- Three distinct Astalon fixes: synchronous WineD3D releases the WGL context at
  its outermost boundary so another thread can share it; genuine Windows/driver
  D3D11 constructor calls route to the native backend while game calls remain
  Wine; display queries preserve native simple-A/W legacy buffer contracts and
  bound writes to guest DEVMODE buffers. The native display backend still sees
  physical modes, while application callbacks regain the virtual view. Mode-change
  safeguards are unchanged. Independent source reviews passed.
  The original NVIDIA worker called Wine D3D11CreateDevice while Wine's command
  thread was inside glClientWaitSync. The dump does not establish mutex ownership;
  do not overstate the complete wait cycle. The driver bridge exposed a separate
  EnumDisplaySettingsExA buffer fault, then the display fix resolved it.
  See graphics-09/astalon-48476-call-proof.json, astalon-nvidia-callsite.txt and
  native-bridge-display-crash-proof.json; d3d11-thread-compatibility.txt records
  detailed source hashes, controls and retained failures.

- Final combined graphics checks pass: priority NVIDIA x64 foreign-context case,
  24 ordinary cases, 12 foreign-context cases, two fresh-prefix backend contracts,
  and eight existing static/dynamic shader controls. Both x86/x64 and Intel/NVIDIA,
  CSMT on/off, virtual desktops, exact pixels/queries/deferred work and teardown
  are covered. Evidence graphics-09/final-{priority,ordinary,interop,contract}-results.json
  and dpi-08/native-helpers-b7a38741d9794ed2afe2f23c3b748a7c/result.json.
  Display API guard matrix on runtime09-display passes 600/600 at
  display-api-09/run-56e23791a8154c7da0a7036395b80829/result.json; old failures retained.
  Combined-stage display guards also pass 600/600 at
  display-api-09/run-2e8db774db904e91908ca9b0c601cb0f/result.json; desktop checks
  pass 9/9 at desktop-c94d337110a74109b114ffabf49b9482/result.json and dialogs
  pass 16/16 at desktop-dialogs-02be1b318058452c86d0b517a052af8b/result.json.
  Host display state and runtime hashes are unchanged.

- Actual Astalon used runtime09-display, which contains the graphics/display
  fixes but deliberately retains the immutable cf4a808 desktop.cpp. Provenance:
  graphics-09/display-source-snapshot/provenance.json. Actual launch/observations:
  games-09/astalon-driverfix09-{launch,observation,prefix-writes}.json. Player log:
  games-08/astalon-1.1.0/drive_c/GOG Games/Astalon/winedows-player-driverfix09.log.
  Selected Wine DLLs and Wine GLSL compilation were verified; new 0.dat and
  GameData.dat are inside private LocalLow. Session closed, prefix lock idle.
  GUI inherited-pipe capture helper passes 16 cases at
  debug-09/capture-d18bceab26c64f248e182c8781b57045/result.json. Its 16 MiB limit
  does not bound Unity's separate Player log; the latter was monitored separately.

- Legacy WinExec now uses the prefix-aware child backend; unquoted paths with
  spaces follow progressive executable search; OpenFile projects fixed-prefix
  physical paths to short logical paths before its 128-byte native path parser.
  Native parser/reopen/read-only/media behavior is preserved. Focused tests pass
  526 cases at games-09/legacy-test-1bb9db65/result.json. Native startup 80 cases
  pass at native-startup-a62d7b03487341aab512091e8b65b80d; managed startup 18 at
  managed-startup-9f0c72c2b75e42c3947ec8fabbb82645; version compatibility passes at
  compat-8f972d3d283e46239f12e35840563d4c (runtime09-driver).
  Full Tilt's ANSI REG_SZ writes supply strlen(data+1). Wine's scoped Win9x rule
  measures the full string; NT/default and native Windows callers keep NT rules.
  It passes 144 cases at games-09/legacy-registry-c4e68141/result.json. Original
  registry was backed up to games-09/fulltilt-artifacts/registry-backup-68a2dc263c864be9ac0f85a6bc081ea6
  and restored privately, with win98 set only for the four game executables.
  Original EXEs/DATs remain unchanged.

- FindWindow/FindWindowEx now searches the logical desktop's contained and hidden
  roots, excluding other prefixes and the host taskbar. Native matching, atoms,
  message-only/explicit child searches and continuation/error rules remain native.
  All eight cases pass at display-09/find-425756d2e2044745b777f8c6216ac9c1/result.json,
  including all four cross-architecture pairs. Actual Full Tilt retry on the
  combined stage launches Space Cadet from its original menu and shows the table
  at x=21, replacing x=-378 caused by the leaked Shell_TrayWnd. Evidence:
  games-09/fulltilt-desktop09-{launch,observation}.json and
  games-09/fulltilt-prefix/.winedows/desktop-9320.log. After F2 targeting the outer
  desktop, the display stays black. Adoption also loses GetMenu and adds 20 px to
  client height. Neither is fixed by lookup scoping; focused investigation pending.
  Session closed, exclusive prefix lock idle.

- Spelunky Wine D3D9/D3DX9/input reaches animated intro/title/menu/character selection
  with Windowed1 in private Data/config.txt. Original settings retained at
  games-09/spelunky-fullscreen09-config.txt. Eight D3DX9 IAT slots verified Wine in
  games-09/spelunky-d3dx09-iat.json. Fullscreen Alt-Tab left the inner window minimized;
  WCT showed no cycle and main-thread progress, so it was not a proven deadlock.
  games-09/spelunky-windowed09-launch.json records the later control. Session closed
  and prefix idle. User subsequently confirmed holding Z selects a character;
  gameplay has not been checked.

## 0.8 checkpoint verified (2026-10-29)

The overall Wine/Linux compatibility goal remains incomplete. Source base was
2dd5648. No remote publication or PR. bin, runtime-07 and previous archives are
frozen; original D:\GameRepo archives remain read-only. Final candidate is
test-output/runtime08-readerfix, version0.8-alpha. Its five native files match
the reviewed runtime08-callback stage. It contains38 Wine modules per architecture,
76 DLLs total. All were rebuilt, including Vulkan and every FAudio consumer.
Final runtime08-readerfix-audit.json checks architecture/Windows-only imports,
74 unchanged Wine DLLs and the two corrected mfreadwrite DLLs. Native builds and
source reviews pass.
Verified archive: dist/winedows-0.8-alpha.zip,59,325,102 bytes, SHA256
3717CC33C59CE31A51555FDE096372C71DA0AA829379C960F161A0BCB242ECC6.
Extracted suite package-082a3a35e7b6481096d7ee3d6ab573ef/result.json passes87 hashes
and38 x86/x64 smoke cases, including original AV, GDI+, XACT and Wine Vulkan.

Preserved failed archive dist/winedows-0.8-alpha-attempt1.zip (59,312,267 bytes, SHA256
D5FF1ABEE1ACE5FD513BE4E8AD9147A120251716FB12D5BC66C20F6AFEC7D7BC) failed verification.
First extracted suite package-837d51d19cd94454b78e25473b14deb5 stopped on
av-64.log: queued audiovisual read callback timed out after correct decoded data.
The new GDI+/XACT checks and x86 AV passed before this. Packaged mfreadwrite.dll
is byte-identical to0.7. Unhooked Wine and0.7 controls reproduce7/8 queued
callbacks while native passes. The response-reservation race is now corrected,
independently reviewed and verified with deterministic controls and unchanged AV
fixtures. The corrected archive passes the full extracted suite. Preserve the
first failed archive and logs as regression evidence.

Changes:
- Real Wine GDI+ with native fallback, direct static import binding despite native
  WinSxS, installed/private single-SFNT font fixes and private WIC encoder catalog.
  No host codec registrations are copied. Existing private registrations win.
- Real Wine XACT3.7 COM provider on native endpoints, PCM/ADPCM/xWMA and streaming
  banks, wave notifications and managed/caller-owned lifetimes. Existing shared
  XAudio, duration metadata, simple-cue EOF and effects/filtering limits remain.
  This does not replace Blossom's direct bundled FAudio/FACT path.
- Direct EXE builtin imports are bound to exact selected paths in suspended
  processes. Original files/native imports stay intact. Native bound imports,
  DLL-owned manifests and32-to64 System32 checks have old-failure/new-pass controls.
  Only main EXE descriptors are rewritten; transitive SxS limits are explicit.
- Virtual monitor/work-area APIs, DPI metrics and mixed-DPI parenting. Ordinary
  application callbacks retain virtual bounds during creation/showing/resizing,
  including nested operations. Hidden-window point conversion agrees with native
  rectangles; copied SDL centering is correct. Retained system image ranges keep
  native internals physical without loader calls on callback paths.
- Opt-in WINEDOWS_TRACE_DESKTOP=1 writes at most128 geometry records per process
  to prefix .winedows/desktop-<PID>.log, including GUI apps without stderr. It records
  no input, titles or pixels. Failure LastError is checked; success values are
  informational because SetWindowPos does not specify a successful error value.
- ISO/Joliet/CUE directory-record timestamps and hidden flags, validated timezone/
  calendar handling, malformed-image rollback and live-swap preservation.
- Wine MF responses are reserved for exact callback IDs; later READs retain FIFO
  order and DRAIN flags. Flush cancels old reservations/deferred command references.
  Callback/sample release precedes further decoder pumping; seek completion and
  failure wake deferred reads, and terminal deferred errors release their FIFO head.

Final/component evidence (paths relative to native/windows/test-output):
- Full Wine build/audit: build-runtime08-final-wine.log; runtime08-final-wine-audit.json.
- Final native build: build-runtime08-callback.log. Reviewed desktop source SHA256:
  7107C9287705F15B507693A03F225B13F68F81253C543206ED7B25E76117CB06.
- Core prefix/native graphics: suite-8d067c03702341febbf6f83858a137eb.
- GDI+20: helpers-08/gdiplus-8515c88a921144c89a173486c3290cf4/result.json.
- Imaging defaults: imaging-defaults-30a559040d69403fbe00b879749fd277/result.json.
- XACT40: xact-acb4558979494813b0f7ef7839090080/result.json.
- XACT managed44: xact-managed-4c86d5df8d3d4e5199a2dded96aac66c/result.json.
- Rebuilt XAudio/X3DAudio/XAPO46: xaudio-10b8772132244f6abaf9d6943ede6ac5/result.json.
- Selected binding36: startup-builtins-08/binding-ba68e49f7a75451d8dfc308538e404fd/result.json.
- Native/SxS/import190: startup-imports-b8739115505e4c4cbfbd19b986f69d25/result.json.
- Review controls: startup-builtins-08/native-bound-81fa5c37de2b47c7913ced25c1781f4e,
  dll-context-a04293976ac9450d8633e3d936c84b84 and system-view-dcc5264791e54a119cd1372cfdaad382.
- Final callbacks16: dpi-08/callbacks-d878454ea893476ea9150e8c836cd1f0/result.json.
- Final DPI24: dpi-08/geometry-025bdc440c9b4ab7927b5d47af0fde11/result.json.
- Final GUI trace4: dpi-08/gui-1606d0dd584d481da57018b2dfd5bf71/result.json.
- Final desktop/modes: desktop-4de147a32a834852aabad3d0d15f451e/result.json.
- Final dialogs16: desktop-dialogs-d1eb25bc3b9e45cb9f8c4b4efe962730/result.json.
- Final monitor/SDL14: display-08/monitors-75629b9a929d4b2d9aec7fb6d321571f/result.json.
- Final GDI+/D3D11 helpers16: dpi-08/native-helpers-5a4e52943e4649d0bb40e466652b50f5/result.json.
- Media metadata200: media-08/metadata-4ca2a649c0ac4e529dfb297c30922777/result.json.
  ISO/CUE/CDDA/live-swap regressions also pass (media-f0ed64a939e442bb8f6cf8f2a96a812a,
  cue-0b6866f03db14cc980a62b4eacbe70ce, cdda-media-b67025c2661f44fd90a226009fda011b,
  media-swap-04431ded1bb942d2a563c298799126d2).
- Final MF source SHA256: 88F8545FA1860E9112A7336F466843474AD0072BECDC185FE3DAF095780D2E82.
  Targeted build: build-runtime08-readerfix-final.log; final audit: runtime08-readerfix-audit.json.
- Deterministic queued reader20: reader-queue-08/run-8ae95ae781654cb691df56ee52b21214/result.json.
  Old failure controls: run-0a330f6547994e4c965341fb1fb76c3f and run-bfbb1e86e8eb491da8111f65d158ad28.
  Tail test delivers38 PCM samples then26 EOF callbacks for64 queued reads, botharch.
- Deferred terminal-error cleanup4: reader-queue-08/run-e863abb099524df1b3002c687eb0e156/result.json.
- Unchanged AV/video32: reader-av-cdcef7e68ff543a08d5c0b42d1f411de/result.json.
  Native/Wine x86/x64 lifecycle, drain, queued reads, selection, desktop video and
  repeated queued cases use original package fixtures and original failing MP4.

Game evidence: games-08/selection.json records original hashes/imports;
games-08/results.json records observations, without full-gameplay claims.
- Castle Crashers3.0: Wine D3D9/D3DX9 with native DInput8/XAudio2.7. Title/menu/
  character selection observed; user reports it runs perfectly fine. Held Enter
  worked where short automated taps appeared not to; no proved Wine input defect.
  User session remains on runtime07 (loaderPID51260 at last inspection).
- Castlevania Advance Collection: native and genuine Wine GDI+ title reached.
  All53 executable IAT entries point into Wine on final candidate, proven by
  games-08/castlevania-callback08-iat.json. Geometry trace shows game explicitly
  requests1936x1119 outer size. Maximizing its inner window fits the1280x720 desktop.
  This is distinct from the callback bug below. Firewall handled by user.
  Final verified archive repeats the observed title/fit. All53 imports bind to
  Wine in games-08/castlevania-package08-iat.json; castlevania-package08-geometry.log
  preserves initial geometry before the bounded trace ends.
  User session left open: loaderPID40624, child23524, dist/winedows-0.8-alpha.
- Astalon1.1.0 Unity2019.4.13/IL2CPP: native D3D11 setup and title now fully fit.
  Before/after geometry logs prove requested1280x720 changed from actual1920x1080
  to actual1280x720, with96DPI/awareness2 unchanged. Logs:
  games-08/astalon-native-geometry08.log and astalon-native-geometry-fixed08.log.
  Initial display option was confirmed in private prefix. Wine D3D11/OpenGL still
  stalls on black startup, before and after sizing fix; owned test loaders stopped.
  Native fallback is usable. Unity DNS failures do not justify host network changes.

Remaining limits/anomalies:
- x64 callbacks tail-jumping directly to an API lose caller identity and can retain
  native values. Mixed hidden/contained HWND mappings remain physical and can
  differ from composition through virtual screen coordinates. Cached native
  backend ranges cover already-loaded modules. DPI testing was at96DPI only.
- One x86 static Wine D3D11 helper crashed during teardown after rendering passed:
  dpi-08/native-helpers-de6af1a46b5d4a93a96d8f282c268e47/result.json. Two prior-runtime
  controls, two candidate controls and the final16-case matrix passed afterward.
  Cause unresolved; not claimed fixed. Fixture-only dump support is retained.
- GDI+ private multi-face fonts, general transitive loader semantics, complete
  XACT/effect behavior and broad game compatibility remain incomplete. This is
  game-state separation, not a security sandbox.
- MF post-error source recovery is not established. After a deselected-stream
  terminal error, native refuses reselection; Wine releases pending command state
  but experimental reselect/read and seek/read can stall. This fix covers cleanup,
  not recovery. Preserved failure logs include reader-queue-08/run-7fcf4de2caff4936a6b41b957436b7da.
  An initial single-stream deselection fixture assumed the wrong native error;
  the final two-stream control keeps video selected and isolates audio validation.

## Historical checkpoint: 0.7 DLL overrides, D3DX11 and audio diagnostics (2026-10-29)

The overall goal remains incomplete. This checkpoint follows commit59c81d3. Work uses
test-output/runtime-07; bin, runtime-06 and older stages/releases are unchanged.
There are now36 Wine modules/72 architecture-specific DLLs. No remote publication
or PR. Wine/Linux compatibility parity and extended Blossom gameplay remain unproved.

Listening update after commit853c109 (2026-10-29): the user answered the pending
firewall/title-music question with "Handled; music sounds smooth". Audible title
music in the previously launched game session is now user-confirmed. That session
used the earlier staged runtime; this is not a new listening test of the0.7 archive.
The reply does not identify a cause for the earlier skipping or establish extended
gameplay audio. No additional audio tuning is justified by the current evidence.
The verified0.7 archive and frozen runtimes remain unchanged.

The final native build passes both architectures (build-runtime07-env-fix.log).
The default Wine build also passes (build-runtime07-final-wine.log);68 DLLs match
the component-tested stage byte-for-byte, and four Vulkan DLLs were rebuilt and
copied into runtime-07. Native PE architecture/import audit passes all72 DLLs:
builtin-runtime07-import-audit.json. No MSYS/Cygwin/MinGW runtime or Unix Wine DLL
dependency was found. Core prefix and native graphics regression passes:
suite-9426120b51a34b908f998775e7ebf8fd.

Verified archive: native/windows/dist/winedows-0.7-alpha.zip (55,922,418 bytes).
SHA256: E635F9951AB64C211FAA7015632A8662632C279AED7EBA465EC5644F085DA282.
Extracted validation package-983daeefa8b540619e2334af73f33d8a/result.json passes
83 hashes and32 x86/x64 smokes. These cover D3DX9/11 GPU pixels, compiler47,
Wine D3D11/OpenGL and Vulkan lifecycles, XInput, XAudio, hardware video/AV and
disabled-DLL native controls, dynamic rejection, resource access and rejection
of static dependencies before DLL execution. Final runtime-07 is frozen.

The first restricted-runner package attempt stopped at XInput queries with
0x800401F0. Native Windows XInput controls reproduce it in that runner and pass
outside it; this was not suppressed or treated as a runtime pass. Evidence:
xinput-package-diagnostic-68ce3f789c004598a1652e0c85fd778c/native-{sandbox,unsandboxed}-{32,64}.log.
The final full package test above used normal host device access and passed.
No runtime or host-setting change was made for that environment failure.

- Empty DLL overrides now disable direct, full-path, transitive and startup
  executable loads, with policy carried to children. Resources, already-loaded
  module reuse and fixed native backend scopes remain available. A temporary
  module reference protects reuse against concurrent unload. Requested-name
  metadata survives local handle duplication and successful/failed close cases,
  with generations protecting reused handles. Both opened and final file names
  are checked at image mapping; resource/no-execute mappings remain available.
  Alias metadata is limited to4096 handles per process; inherited/cross-process
  raw file handles do not transfer it. This is not a security sandbox.
- Startup validation follows direct/transitive imports, named/ordinal export
  forwarders, actual preloads and resolved native API-set hosts for the child
  architecture, including AnyCPU. It rechecks observed files, absent candidates
  and inherited DLL directory before resume. Unknown/ambiguous graphs, SxS and
  unsupported parser/loader layouts fail explicitly while disabling is active.
  Core/KnownDLL/API-set disabling, path-qualified override keys and conflicting
  exact/wildcard orders reject explicitly. Ordinary api-/ext- DLL names are
  traversed instead of being trusted by prefix alone.
- Child environment sorting now preserves the first duplicate variable by name,
  matching native lookup; startup policy/PATH use that exact first value.
  An omitted override retains the parent policy; explicit empty/native child
  settings are honored. Final disabled override matrix94/94, zero skips:
  override-disabled-cf380fe6cc8646d486bd6c3d2be5d778/result.json.
  Hard/symbolic link aliases and handle lifecycle16/16, none unavailable:
  disabled-alias-c27e569ae7014751b928f4808d60b32d/result.json. This includes32
  actual handle reuse checks, close-source and protected/failed duplication.
  Focused startup graph138 and prior planner/binder178 cases pass:
  disabled-startup-9b69af6ccaba47749ce8be1791130c81/result.json and
  startup-imports-f27790fedaee4cf894d2eaadd1733198/result.json.
  Native startup80, CLR18 and existing DDraw/D3D9 overrides also pass:
  native-startup-52aa869a8c0c436397997369cfdc4c89/result.json,
  managed-startup-23a562f527a4411582554a107b9bf707/result.json and
  overrides-06bf45de60e54dc8844857f52d71b33e/result.json.
- Real Wine d3dx11_43 is selected from GameRepo imports including Outlast and
  Witcher3 helpers. Its dependency is d3dcompiler_47; native/Wine D3D11 remains
  independently selectable. Microsoft's DXSDK declaration and signed native
  DLLs establish HRESULT CheckVersion semantics; upstream BOOL returns were
  corrected to S_OK/E_FAIL, with the missing public IDL declaration added.
  d3dx11_42 shares that header and was corrected/built/checked too, but is not
  cataloged without a game use case. Evidence: helpers-07/consistency42/result.json.
  Integrated24/24 cases cover both architectures, native/Wine helper and graphics,
  dynamic/static imports, native availability fallback and strict rejection:
  helpers-07/d3dx11-929ba57de60e4bae8a317a36a2e33ecc/result.json.
  Version tuples, SM4/SM5/includes/errors, provider identities, RGBA/BC1 DDS and
  640 GPU pixels pass. Thread-pump creation and texture saving remain stubs.
- Audio diagnostics observe actual WASAPI submissions, existing padding queries
  and normalized device clock through forwarding proxies in copied SDL only.
  Exact Blossom assets are unchanged. No live-game injection, audio capture or
  host audio/security changes. Eight15s host/prefix × idle/render ×44.1/48k cases:
  audio-07/game-audio-timing-4ce08004b1b045f092cdc43a99816a17/result.json.
  Submitted15.00–15.01s in15s; gaps10.40–11.56ms; clock/QPC0.999979–1.000043.
  Every render case completes900 frames with pixel readback.44.1k callback gaps
  near20ms coexist with regular endpoint writes in these fixtures; padding zero
  alone is not an underrun count. A deliberate120ms stall produces130.24ms gap:
  audio-07/game-audio-timing-850b0b089d444c1295b74d829acd7b6a/result.json.
  Observer-disabled host/prefix controls pass:
  audio-07/game-audio-timing-bd0686b66b7c4bdfaa5278f8443f47da/result.json.
  The user's later title-music confirmation is recorded above; extended gameplay
  and the cause of the earlier skipping remain unverified. No buffer adjustment.

Blossom was no longer running at this checkpoint's process check. Prior live
observations below are historical. The title-screen listening check is satisfied;
next audio validation should cover extended gameplay and investigate skipping
only if it recurs. Callback batching alone is not evidence of the cause.
All component agents have released sources/runtime readers.

## Historical checkpoint: 0.6 Vulkan, media notifications and registry transfers (2026-10-29)

Goal remains active and incomplete. This checkpoint follows b206c15. Work uses
test-output/runtime-06; bin and older stages/releases remain unchanged. Full
Wine/Linux compatibility parity and Blossom Tales II audible quality are unproved.
No remote publication or PR.

Verified archive: native/windows/dist/winedows-0.6-alpha.zip (55,296,301 bytes).
SHA256: 625733C10BEBC5490680B264AB5A379108CEED5E85475D58C8BD883B7DC8AADC.
Extracted validation package-66f2a00b89a04ba9ab35e2e74737e1ad/result.json passes
81 file hashes and22 x86/x64 graphics/audio/input/compiler/helper/hardware-video
and WineD3D Vulkan lifecycle smokes. Earlier archives are intact. All component
agents released sources/runtime readers; no active tests remain.

The final Vulkan resource/registration fix passed. Default native/Wine builds passed both architectures:
build-runtime06-final-native.log and build-runtime06-final-wine.log. The catalog
and package now contain 35 Wine modules (70 DLLs); the 66 ordinary Wine DLLs match
the tested stage byte-for-byte. Final build-runtime06-vulkan-resources.log uses
the central wrapper for both Vulkan DLLs/architectures, including Winewrc and
locale prerequisites. Both DLLs retain upstream version resources. Native ICD
registration/unregistration explicitly returns ERROR_NOT_SUPPORTED before writes:
this backend calls Windows' Vulkan loader and must not register itself as a driver
with that loader. Normal Wine's manifest/registration path is unchanged.
Exact final-stage tests pass resource2 and catalog36:
vulkan-06-resources/resources-cd2d3e6a496c4363bc9bad7c2324e78c/result.json and
vulkan-06-resources/catalog-fe8197baf9d842329cd08d447547baf0/result.json. Private
sentinels/registry and host manifest/ICD snapshots remain unchanged. The private
and central builds have identical code/data/import/resource/unwind sections.
builtin-runtime06-import-audit.json verifies70 native PE architectures/dependencies;
no MSYS/Cygwin/GCC/Unix/win32u runtime imports. Runtime Vulkan DLLs retain their
system loader reference for process lifetime; full standalone unload/reload
reference balancing remains a limitation, avoiding driver teardown under DllMain.

- Real Wine Vulkan loader/dispatch/wrappers compile directly from guarded Wine
  sources, with a native Windows backend. No generated source substitutions or
  Unix runtime are needed. Wine's vulkan-1 forwarder is included. Native backend
  calls use fixed system-loader selection and post-load file-identity validation;
  an available shim's failure is authoritative. App/private shadow, malformed
  private DLL, preloaded shadow and repeated backend-load tests pass botharch:
  vulkan-loader-0bb0067c13e94c39a23913cd50fd7af3/result.json (14 required cases).
  Two .local policy tests are unavailable because this host ignores redirection.
  This check is not prevention of code execution before loader validation.
- Wine DXGI creates its private helper window as a nonempty disabled popup only
  on native Windows. WineD3D defers Vulkan swapchain creation/acquisition while
  the client is empty or minimized, retains logical rendering, and recreates on
  restore. Repeated destruction and failed creation clear stale arrays/handles.
  Independent final lifecycle suite passes 20 cases and OpenGL regression 4:
  wined3d-vulkan-b777dc74239c4212a5b4a4b340c87c59/result.json and opengl-result.json.
  Evidence: 60 device cycles, 8,640 exact shader pixels, 348 WineVulkan submissions
  and 132 presentations; ordinary/virtual desktop, initial empty, zero height,
  resize, minimize/restore and recreation; physical display mode unchanged.
- Direct Vulkan selects both Intel UHD770 and RTX4090 on both architectures,
  through native, WineVulkan and upstream forwarder paths (12 cases). Prior
  production catalog suite passes 36 selected-provider/import/fallback cases:
  vulkan-06-production/catalog-c4bccede3625492fa5f8936b19c99644/result.json.
  Final rebuild evidence above supersedes that run. Supported API is Vulkan1.0
  with selected Win32/WSI extensions. New core versions, external-memory sharing
  and debug callbacks are unsupported. WineD3D's first-device-only choice remains
  an upstream limit. Availability fallback does not retry an unsupported API.
- MediaNotifications observes shared session counters and sends WM_DEVICECHANGE
  volume removal/arrival only to live job members' top-level/virtual-root windows.
  Initial state is silent; swap produces removal then arrival. Retained process
  handles and job checks exclude host/other-prefix/loader windows. USER32 handles
  cross-process/x86-x64 payload marshaling. Static payload lifetime survives a
  timed-out recipient. Send timeout is100ms; stop checks run during enumeration.
  Restricted and ordinary desktop suites each pass116 assertions:
  media-notifications-fd09d2b25b924de9ab9edc936bd1daa4/result.json and
  media-notifications-56caadb36d9544b389ac4322fe5f6094/result.json. Four bitness/
  desktop combinations observe30 ordered events per parent/opposite-bitness child
  and zero outside. Delayed dereference, reentrant load, locks/no-ops,5000-event
  overrun/resync, parity/wrap, silent initial state, duplicated job and hung-handler
  bounded shutdown pass. Per-drive order only; excessive backlog is diagnosed and
  dropped/resynchronized; hung windows can miss events. Query-remove vetoes and
  service/device-interface subscriptions are not implemented.
- Registry routes now use native KeyHandleTagsInformation instead of a live
  process-local handle table. Private opens stamp selected WOW64 bits; inherited
  and duplicated handles retain them. x64 KernelBase's app-hive redirect omission
  is handled narrowly using native read-only route queries. Raw native NT open
  semantics and HKCU Software-to-Classes implicit default-view reset are retained.
  Transfer matrix passes all4 native and4 private parent/child architecture pairs:
  registry-transfer-7341ed51ffad462b96b195a64866be02/result.json. HKLM/HKCU, explicit/
  implicit views, local/remote/brokered duplication, reduced access, exact failure
  codes, close-source, protected handles, real handle reuse, writes and host/sibling
  isolation pass. Native tag behavior needs validation on other Windows versions.
  Full registry/COM regression: registry-8deb1809c933475f8b9a4acd6de9d0f6/result.json.
  Imports/COM: registry-import-2110b0613ff44f53a939f4720a2f5aa3/result.json.
  CLR startup18 cases: managed-startup-ffbeab1827084549a0072b09f0998b38/result.json.

Blossom's prior WASAPI progression observations are historical, not a new live
check or audible-quality result. Listening comparison remains pending. Original
game archives and host security/audio settings remain untouched.

Next audio work is a disposable-fixture WASAPI queue/clock diagnostic, not a
change to the live game. Extend game-audio-timing-probe.cpp/test-game-audio-timing.ps1
to observe IAudioRenderClient GetBuffer/ReleaseBuffer frames/timestamps/results,
existing IAudioClient padding queries plus format/buffer/period, and correlated
IAudioClock units/frequency. Compare host/prefix idle and bounded60Hz render work
using the copied exact44.1k title cue; keep the120ms injected stall as calibration.
Existing game-audio-timing-395972cb07db4d71bdb227fe023a4be1/result.json has normal
mixing progress but85 callback gaps over20ms without endpoint-depth evidence.
The separate WinMM two-buffer result remains game-audio-waveout-9d14b1d0b15c4eafb4680a75ce49adbe/result.json.
No new audio fixture implementation/playback was performed in this checkpoint.

## Historical checkpoint: 0.5 objects, disc swaps and audiovisual decoding (2026-10-29)

Goal remains active and incomplete. This checkpoint follows ba68bd6. It uses
test-output/runtime-05; bin/live Blossom, runtime-04 and older release stages
are unchanged. All component agents have released source/runtime readers and
package validation is complete. No remote publication or PR.

Verified archive: native/windows/dist/winedows-0.5-alpha.zip (51,534,120 bytes).
SHA256: 4AA14DE554865B3664963189C0673FD012DF1FAF0E0E879C4CD78C35DE2B1EBA.
Extracted validation package-921c865b6c5241d1a9544163adf12512/result.json passes
77 file hashes and 20 x86/x64 graphics/audio/input/compiler/helper/hardware-video
smokes, including queued AAC/H264 playback. The previous 0.4 archive is intact.

- Final native build passed both architectures: build-runtime05-controller.log.
  Default Wine build passed: build-wine-runtime05-default.log. Its 66 DLLs match
  the tested runtime byte-for-byte. builtin-runtime05-import-audit.json verifies
  both architectures and absence of MSYS/Cygwin/GCC/Unix/win32u runtime imports.
  Catalog/build/package lists now contain 33 Wine modules.
- Named objects passed simultaneous host/two-prefix native and actual CLR tests,
  cross-architecture children, explicit NT roots and query-name roundtrips:
  named-objects-ca825f24b3354491a4c328e5c1876c95/result.json. Native high-resolution
  named timers are unavailable on this host (ERROR_INVALID_PARAMETER baseline).
  Backend regression results: d3d11-e06c79c3b550498896bfc5e7dc016707 (50 cases),
  xaudio-812b38f494bf458abd6caf927c0872ae (46), and
  video-7be9efd15a4f483a9523ad16d0eeb6df (32), each with result.json.
- D3DX9 versions 37/41/42 and compiler42 are real Wine DLLs, both architectures.
  Integrated72 cases pass native/Wine helper and D3D9 providers, dynamic/startup
  imports, exact shader/texture pixels and missing-provider fallback/rejection:
  d3dx9-versions-dbec0431c48c4ebd9fb005e245b6d9a0/result.json. Eight DLL export/import
  audits pass. Read-only inventory scanned4528 PE files across143 game ZIPs.
- Live ISO/CUE swap controller publishes immutable descriptor generations to the
  existing session. Old raw handles retain drive controls; old media data handles
  require reopening. Restricted and ordinary desktop suites each passed 57
  assertions: media-swap-e2c30cdcc9944a9989e46298d2f56489 and
  media-swap-1333ee9f7dc9487c9c309f25d78924a7/result.json. Tests cover CUE/ISO swaps,
  exact files/TOC/PCM, geometry, cross-architecture children, inherited/duplicated
  old handles, stale relative/physical paths, locks, malformed rollback, session
  reset, controller cleanup, and 127-descriptor exhaustion. Pipe names use LOCAL
  for AppContainer and ordinary desktop lookup. Persistent INI/original media/
  retired cache/host drives remain unchanged. No device-change broadcasts.
  Old media suites pass: media-state-0012a00ca81947cf82f1fd3f2b63d561,
  media-809edaa81ade4871bf4dace9cd4b7a6a, cue-261f8918af614805b38444648aecd817,
  cdda-media-3cfce814233d4a679b9fc9c33ee359f1. Optional redirected-root fixture
  skipped when junction creation was denied; equal/nested alias checks passed.
- Direct NT media controls pass x86/x64 synchronous ALERT/NONALERT, 98 assertions
  each plus two native baselines: nt-media-36035751119f450aa5e9891294541ade/result.json.
  Exact statuses/IOSB/bytes/LastError, PCM/TOC, mode/access, invalid/guard/cross-page
  buffers, locks/eject/load and native passthrough checked. Async/event/APC/context
  forms reject before mutation. VirtualQuery preflight and SEH copy leaves keep
  invalid pointers out of stateful handlers; no hostile concurrent-unmap guarantee.
- AAC/H264 AV and color conversion use absent-only native codec defaults. Wine
  MF reader Start coalescing and selection-during-start reconciliation passed
  standalone 24 cases and fresh-prefix 34 cases:
  video-av-86447d899eae40d7b75fbfb5f848e65f/result.json. Actual native codec chain,
  RGB pixels, stereo PCM tones/timestamps, lifecycle/queued/selection/override
  behavior checked: 1,102 video frames, 4,018,176 PCM frames, 1,299 DXVA submits.
  Native D3D11 still required; no generic codec/playback-clock/MediaEngine parity.
  Reviewed reader SHA256:
  1849EC50F81A66967C3AC7A730573FC3CAB24A72BB98BD725A2DCDA391B16FF7.
- Wine Vulkan prototype uses actual Wine dispatch and wrappers with a native
  loader backend. Both architectures pass shader pixels, image transfers/fences
  and Win32 surface presentation: six standalone and four prefix cases. Sources
  and reproducible build overlays are in experimental/vulkan/README.txt. WineD3D
  still fails a zero-height implicit swapchain; no Vulkan catalog/package entry.
  Production trusted-loader/override/fallback/WSI handling remains future work.
- Blossom remains live on WASAPI, 44.1 kHz mixing converted to48 kHz. Bounded
  read-only200 snapshots over10 seconds show queued active audio advancing at
  ~44087 samples/sec; this is not an audible-quality result. Exact title cue
  fixtures pass WASAPI/native DirectSound/Wine DirectSound in runtime05:
  game-audio-timing-395972cb07db4d71bdb227fe023a4be1/result.json.
  Native waveOut control isolates separate WinMM two-buffer slowdown:2x512 queues
  advance53-58% of real time,4 buffers99-100%. No host audio/security settings changed.
  User firewall/audio handoff remains pending; no claim Blossom skipping is fixed.

## Historical checkpoint: 0.4 video, media lifecycle and startup (2026-10-29)

This section supersedes the historical in-progress notes below. Goal remains
active/incomplete. This source checkpoint follows4d8ce90. README/package/default
build lists target0.4 and29 Wine modules. All agents released their sources and
build/runtime readers; no active tests remain. Keep bin/liveBlossom unchanged.

Verified archive: native/windows/dist/winedows-0.4-alpha.zip (42848068bytes).
SHA256:93B8494A9342772DF4DBE5433B89D6ABA7117A1BE0AFF19F95B89574636011D5.
Extracted package test package-bb37933f934a4376a3792d735c52b88e/result.json:
69 hashes and12 x86/x64 graphics/audio/input/compiler/hardwarevideo smokes pass.
Previous0.3 archive remains intact. No remote publication or PR.

- Root integrated startup-imports.cpp into main loader and child process hooks:
  plan before Detours, native paths after shim/Wine, real-export thunk binding
  while suspended, terminate child on any bind/configure exception. Unsupported
  preplanning preserves native launch with diagnostic; malformed selected private
  providers fail. Independent wine_input review addressed unsupported manifests,
  AnyCPU promotion, optional PE directories and paths. SxS/host DLL branches are
  not traversed for private transitive dependencies. No general loader parity.
- Native build-runtime04-loader-fixes.log and default build-wine-runtime04-final.log
  passed botharch. Runtime-04 now has29 modules inclcompiler47/mfreadwrite.
  Import audit builtin-runtime04-import-audit.json:58 DLLs, no MSYS/Cygwin/GCC
  runtime or Unix/win32u imports. Build directories shared; serialize rebuilds.
- Integrated native startup suite passed80 cases:
  native-startup-749074c05c5d4dec9c1dcd92ee449c8c/result.json. New custom-directory
  fallback covers direct/transitive, ANSI env/suspended children, all4archpairs
  and inherited custom directory in both main loaders. Logical C: custom paths
  overlay the prefix before host readthrough; exact System32 root is target-aware
  (SysWOW64 for x86), shared with modules.cpp availability selection. Binder
  validates the unchanged original inherited directory. Originals unchanged.
- Planner fixes are built: successful empty GetDllDirectory fast path, search-slot
  fallback detection, structured overlay/native mapper. Isolated178cases pass:
  startup-imports-c433b4f12a264d90aa04e9ac30da3e0c/result.json. Sources released.
- Wine MF reader final source hash979E16B196F77D0C51B297824FF2C675860BC37A9B9DAD390F475F0EC97AEF3E
  independently reviewed; source-read demand pacing, pending inputs, async refcount,
  callback-before-resume, seek/reset/drain/flush fixed. Catalog mfreadwrite added
  without Wine helper dependency (native MF platform/source/H264 decoder).
- test-video.ps1 integrated result video-e76b86a20e534547b5745bd1e8e3863b:
  24 hardware playback runs,1796 decoded GPU frames/presentation,2073 successful
  native DXVA submissions;2 defaults preservation +6 expected isolation/unsupported
  negatives =32 outcomes. Botharch, native/Wine reader, sync/async, virtualdesktop,
  lifecycle/drain. Separate native fallback2cases:
  video-native-fallback-19eebbacccb4494eb81ee84b3d43aedf/result.json.
  NativeD3D11 ONLY: WineD3D11 has0decoderprofiles and explicitnegative test.
  Generated baselineH264160x96 onIntelUHD770; no generalcodec/MediaEngine/DRM claim.
  Native Flush at decoder-tailEOF then ReadSample without seek timed out in a
  standalone test; native comparison usesFlush+Seek, Wine addsFlush->EOF coverage.
  Multi-transform chain reviewed but not runtime-tested. Video sources/readers released.
- test-package.ps1 includes compiler47 typed-UAV/render/import and WineMF async
  hardware-video lifecycle. Final archive tests passed against the rebuilt loader.

Resolved CLR release hold: stock Detours32 mistook a native64 AnyCPU process
whose PE32 main image landed below4GiB for a32bit process; it cleared IL_ONLY
with the wrong updater, causing BadImageFormatException. Outside-sandbox calls
reproduced this layout; default calls hid it with a high image address. Old0.3
also reproduced. A narrow early IsWow64Process guard in vendored creatwth.cpp
delegates to the existing64bit helper before any header mutation. Independent
isolated15case matrices in both contexts validate cause/fix; see
clr-detours-investigation/external-result.json andsandbox-result.json.
Final integrated managed-startup-8307f01e68f74d878e2eb5f1c0d976a5/result.json:
all18 actualCLR cases pass OUTSIDE sandbox after rebuild, including both
launchers,AnyCPU64,Prefer32,Required32 andcrossarchchildren. Original executables
unchanged. No security settings changed; earlier18case proof had missed layout.

Blossom remains running from OLD bin. User firewall choice/listening answer still
pending; do not interact with security prompt or overwrite live binaries.

## Historical work after 0.3: media lifecycle, video and dialogs (2026-10-29)

Previous goal turn was concrete progress: clean commit4d8ce90 plus verified0.3.
Goal remains active/incomplete. New source below is uncommitted; no0.4 package
yet. Runtime test-output/runtime-04 is isolated from bin/runtime-next/review.
Root built botharchitectures successfully with compiler47, media state, native
video defaults and desktop dialogs (build-runtime04-media-video-desktop.log).

- Wine d3dcompiler_47 built unchanged botharch, catalog entry added. Actual Wine
  compiler -> Wine/native D3D11 SM4/SM5 shader/reflection/macro/include and exact
  pixels plus typed-UAV compute pass20cases in compiler47-92ff7a8262234b33996f849efc6d14ba.
  Structured-buffer writes remain upstream E_NOTIMPL; explicit limitation.
  Root must still update default Wine build/package lists for47.
- New media-state.cpp/.h: loader-owned pagefile mapping,128bit random session
  name, fixed x86/x64 ABI bound to prefix/configuration. Perdrive atomic presence,
  media-change count and global removal-lock count. Child custom env gets actual
  MediaSession.name even if parent clobbers environment key. No persistent config
  changes; fresh loader starts medium present. Eject/reinsert same medium only.
  Ambiguous equal/nested/redirected roots reject eject; no physical device calls.
- Root shim gates absent media on new opens/attributes, old-handle read/write/
  metadata/flush/directory queries, new sections, volume/free-space and IOCTLs.
  Bare drive/device identity/type/drive bit retained. Existing mapped pages and
  in-flight I/O not revoked. CDDA cache refreshes per shared media generation.
  Direct NT device-control calls and WM_DEVICECHANGE not added. Disc swap needs
  full versioned medium descriptor + per-handle generation identity, not INI edits.
- media-state-7921c13d6a2c4425b5f91c922b31c376/result.json:116 assertions, botharch,
  crosschild/customenv/concurrency/locks/exactreload/readonly/cursor, original/cache/
  drives.ini hashes and host drives preserved. Junction fixture explicitly skipped
  after local creation Access denied; equal/nested aliases mandatory and passed.
  ISO media-4aca8962014d4d859060d7a2317503d0, CUE cue-dc3b8db5780249ddab2e33dc64a2d01e,
  CDDAv1 cdda-media-27c31004f48742669bc01832756ba837 regressions pass.
- Desktop dialog bridges now contain native resource/indirect A/W modal/modeless
  dialogs and MessageBox variants. Logical owner/style/coordinates/focus/modality
  retained; SetWindowPos/Placement show paths adopt. Dialog child templates bypass.
  All16 cases in desktop-dialogs-4fec314fcf254aafada4945f8f0efd60 pass, with actual
  BM_CLICK on fixture buttons and no global input. Existing desktop regression
  desktop-fcf19288da0c4cfd8ef022890309a8c5 passes. Popup menus, service notifications,
  full WM_ENTERIDLE/owner destruction and guarded callback details remain gaps.
- Native video-backend defaults integrated in Prefix::init: five exact validated
  native MF classes plus file/MP4 handler and H264 transform/category metadata,
  absent-only, preserve machine/user empty/broken keys, optional when MF missing.
  Video agent has native32/64 prefix proof:24 decoded NV12 frames, changing pixels
  and real native DXVA decoder submissions. New fixture also presents GPU video.
  This proves H264 hardware decoding, unlike earlier profile enumeration.
- Actual Wine mfreadwrite builds without Unix deps and uses native mfplat/codecs.
  Initial reader deadlocked after output pool filled: it drained outputs while
  holding reader lock and queued decoder-owned surfaces consumer could not free.
  Video agent owns an in-progress demand-driven reader.c fix (currently modified):
  one queued output, pending input ownership, incremental EOS drain, flush cleanup.
  Standalone botharch77frame full/seek/partial/EOF loops pass. Async tests and
  compiler-agent independent source review pending. DO NOT package mfreadwrite
  or commit temporary tracing until agent confirms final reviewed source.
- Startup path research: DllPath is one directory; semicolon lists/empty import
  thunks do not solve retained custom-directory fallback. wine_xaudio owns new
  startup-imports.cpp/.h planner/binder (not integrated): bounded real dependency
  walk, real imported symbols/export ordinals, Detours placeholder thunk rewrite
  before resume, no on-disk PE edits. First12 direct/transitive/host-shadow cases
  pass all4 parent/child bitness pairs including32->64. Expanding negative/precedence/
  forwarder/cycle tests. Root must review before launcher/shim integration.

Blossom: root reobserved live desktop HWND14287870 via Sky; Windows Firewall
prompt still overlays title. Async user handoff renewed; no reply/action yet.
Computer Use forbids acting on security requests. No game lifecycle changes.
Audio agent added game-audio-timing diagnostic using exact copied FAudio22.7/
SDL2.24 and real BT2_Title bank, output forced silent. Full14x10sec host/prefix
matrix e2bc547d686043b397013a643345a94c: WASAPI/nativeDS/WineDS ~1000 callbacks/10s,
max gaps11.7ms, no source starvation. WinMM only53.4% audio progression on BOTH
host/prefix, ~40ms gaps. Injected120ms stall validates detector. Classification
233bae0524c54a209797d8740753a346 explicitly flags anomaly; hardwareUnderruns=null.
This bypasses whole game/CLR workload and does NOT resolve reported skipping.
Next audible game A/B: explicit WASAPI, DS native, DS Wine, same graphics/scene.

Active workstream owners: video agent d3d8_validation (reader.c/video helpers),
wine_d3dx (independent reader review), wine_xaudio (startup import planner).
Media/dialog/audio diagnostic sources are stable and released. Root owns shared
shim/common/build/catalog/docs. Base prefix regression prefix04-integrated.log
passed. Any next native build must wait for all runtime-04 readers to release.

## Current checkpoint: 0.3 graphics, input, discs and startup (2026-10-29)

This section supersedes the0.2 checkpoint below. Goal remains active/incomplete.
This source checkpoint follows6ef4c99 and includes the verified0.3 package
implementation and fixtures. Development runtime is test-output/runtime-next.
Keep bin (older Blossom build) and runtime-review (verified0.2) intact. All
six subagent workstreams are released; no build/test sessions remain active.

- Catalog now27 modules perarch: DXGI,D3D11,D3D10/10.1/core,d3dcompiler_39,
  five XInput variants added. DXGI/D3D10/11 choose one provider family because
  of Wine-private interfaces; strict mixed-provider settings fail clearly.
- Wine DXGI callback routes Windows graphics-driver factory calls to system
  DXGI, fixing Intel OpenGL ICD -> WineDXGI -> WineD3D -> ICD recursion. App
  calls remain Wine. Scope bypasses module and NT mapping during backend calls.
- GetSystemWow64DirectoryW(capacity32768) returned length but empty buffer on
  x86; trusted system loader now uses32767 and validates nonempty output.
- Wine XInput frontend uses fixed native Windows XInput hardware transport.
  No controllers attached: physical input/hotplug/vibration/audio untested;
  Winebus generic HID mappings remain unsupported. No input/vibration injected.
- BIN/CUE supports one FILE BINARY, MODE1/2352 track01 followed by AUDIO.
  Private source snapshots/cooked ISO/PCM and v2 extent_start/index01 preserve
  stored INDEX00 and inserted PREGAP. V1 remains supported. Atomic mapping
  publication and malformed-input rollback tested. No EDC/ECC checking,
  raw2352 data reads, subchannels, multi-file/MODE2/audio-only CUE/live swapping.
- COM defaults add only absent IStream/ISequentialStream interface mappings.
  Native Windows intrinsically provides PSFactoryBuffer; no host CLSID imports.
  STA/MTA marshaling/GIT and private registration preservation pass.
- Imaging defaults add three exact absent WIC factory/PNG/JPEG classes using
  validated WindowsCodecs system registrations. Existing private keys, including
  empty/broken classes, preserved. Integrated runtime tests now pass.
- Startup helper standalone tests pass all4 parent/child architecture pairs,
  named imports without ordinal1, transitive imports, optional host DLL shadow,
  long/custom environment preservation. Suspended child's DllPath points into
  validated contiguous child-owned parameter/environment bytes. Production
  integration and46 cases pass. Existing application SetDllDirectory retained; private
  system fallback behind a custom directory remains unsupported.
- Final review added mapped-file identity checks for system graphics/XInput
  backends, permissive fallback after wrong-architecture/malformed builtin
  preflight, and native availability in an application's GetDllDirectory.
  The latter repaired a reproduced custom-directory-only n,b selection error.
  Identity is checked after loading; this does not prevent DLL initialization
  side effects or establish a security boundary. Native load-error retry is
  still not implemented.

Passing evidence under test-output:
- d3d11-137846e3263041918d78e900c59dfba5/result.json:50 cases/1688 assertions,
  actual D3D11 shader/readback/resize/present and D3D10/10.1 clear/readback/present;
  x86/x64, Wine/native/fallback, desktop/windowed, dynamic/static D3D11 imports.
  Intel UHD770 OpenGL4.6 hardware; native d3dcompiler_47. Physical mode unchanged.
  Wine video device has zero decoder profiles; video decoding not established.
- xinput-894d57b2ecda4de39230d73e2b176d2f/result.json:all50 final cases pass.
- cue-2638a00e60dd427f9b237ab4e54a3897/result.json:exact data/PCM/TOC/Q botharch,
  20 malformed CUE/BIN plus4 v2 layout cases perarch, offline originals,
  readonly caches, rollback and unchanged host drives.
- cdda-media-f2c81ca4ef8047a8a21e803a72532508/result.json and
  media-a20d73499a29469e891960a0d8bfe5ca/result.json:CDDAv1/ISO regressions.
- com-marshaling-7f678c3243a34dccb946b2621e62c7ac/result.json:all30 cases.
- imaging-0e9aece3de2b438b859ea9ce52fc42d7/result.json:texture cases pass;
  two public COM gaps recorded before defaults helper integration.
- imaging-defaults-1e5eab92e5e24072b0fb392a6e6e44b0/result.json:five helper/COM
  decoding/private preservation cases pass. Host registrations unchanged.
- builtin-next-import-audit.json:54 DLLs, no MSYS/Cygwin/GCC runtime, win32u or
  Unix imports. build-wine-next.log:default27 module builds pass botharch.
- native-startup-53382cba90604431bc5d1602e8dde4ce/result.json:46 integrated
  startup cases, including CreateProcessA custom ANSI environments and bare
  cmd.exe. Two expected missing-DLL results preserve the custom-directory gap.
- managed-startup-646697a941c942c8b8f2075fbfee1339/result.json:18 real CLR
  parent/child combinations, AnyCPU64/Prefer32/Required32, both launchers.
- imaging-085ddd35a47a460cb45a58e249426e40/result.json:all15 integrated cases,
  including direct WIC COM activation and PNG/JPEG decode, now pass.
- imaging-defaults-d027f82adb124913a22c14801a55398f/result.json:integrated
  default preservation and unchanged host registrations pass.
- com-marshaling-87ae83fa46a944a1aaa60cb1c5d3939e/result.json:all30 pass after
  startup/imaging integration. prefix-next.log -> suite-1697d42a0ab84759848450b1a8add36e.
- desktop-next-final.log -> desktop-7ef2316ad9944f619201e45c0a5a58b0:pass.
- system-loader-7b94b85ecaa2431cbf368e47295ba753/result.json:10 passing identity/
  preflight checks. Two DotLocal rejection exercises explicitly unavailable:
  Windows ignored the benign setup. No DevOverrideEnable/host policy changes;
  rejection/ref-release through DotLocal is not claimed tested.
- package-2dd929a33373440ebee115a81e87e1f2/result.json:actual extracted0.3,
  all65 hashes plus8 x86/x64 D3DX/D3D11 shader, XAudio stream, XInput import
  smoke cases pass. Initial package test lacked staged old fixtures; copied
  those existing probes into runtime-next and reran successfully, archive unchanged.

Deliverable: native/windows/dist/winedows-0.3-alpha.zip (41,200,859 bytes),
SHA256 95A869639BF2208D2BA8221C3E2E293B6BF6702F35B6582CE55BFE9F1B31AEDF.
Contains both architectures of27 Wine DLLs, native loaders/shims, README/CDDA
preparation helper and Wine/Detours/FAudio licenses. No game payload included.
build-runtime-next-final.log records both native builds. GameRepo originals
and host security/registry settings unchanged. No remote fork/PR published.

Blossom remains FNA/.NET, not Unity. Title verified with nativeD3D11; skipping
is NOT resolved. Previous user handoff asked them to handle Windows Firewall
prompt and report music. No listening reply received; no security UI/settings
changed. See prior checkpoint for prefix location and audio A/B plan.

Next: obtain Blossom listening feedback/user firewall handoff, compare per-process
SDL wasapi/directsound/native-or-Wine providers, and advance gameplay. Broader
Wine API coverage, custom-directory search parity, hardware video decoding,
real controller testing, and desktop/window management gaps remain open.
Do not mark overall goal complete.

## Current checkpoint: expanded runtime and Blossom startup (2026-10-29)

This section supersedes older checkpoints below. Goal remains active/incomplete;
Wine-on-Linux compatibility parity is not claimed. Branch: winedows-native.
No remote fork or PR published. The0.2 alpha package is verified; this source
checkpoint includes the implementation and its regression fixtures.

- Builtin catalog now contains 16 modules: wined3d, ddraw, d3d8, d3d9,
  d3dcompiler_43, d3dxof, d3dx9_43, dinput, dinput8, dsound, mcicda,
  xaudio2_7, xaudio2_8, xaudio2_9, x3daudio1_7, xapofx1_5. Both architectures
  build as native PE DLLs. Shared catalog resolves helper dependencies without
  forcing the caller's D3D9 provider. Explicit dependency overrides win.
  Default remains native; b,n/n,b availability fallback remains supported.
- build-wine.ps1 -Modules can rebuild/copy selected modules. Never overlap the
  same architecture build directory or replace runtime DLLs during live tests.
- Native CLR AnyCPU architecture selection now uses CLR flags and host bitness;
  PE32 IL-only AnyCPU Blossom correctly selects the 64-bit shim. EXEs and DLLs
  differ; required/preferred 32-bit, malformed headers, real host CLR tested.
- Registry WOW64 view routing now works from x86 and x64, including relative
  child opens and shared Classes roots. Uses readonly host metadata for routing,
  never copies application values. NtClose removes metadata before handle reuse.
  Lifetime maps and detach disabling avoid x86 CRT teardown failfast.
  Windows .reg imports accept /reg:32 or /reg:64 (default64); Wine text retains
  physical paths. GameRepo architecture controls import view, not process bitness.
  Same-process NtDuplicateObject now propagates views, including close-source,
  protected handles, failures and reuse. Cross-process/inherited handles remain
  unsupported for view propagation.
- Native backend readthrough includes .NETFramework, NDP, OLE, Rpc and audio
  configuration; application Classes remains private. OLE/RPC discovery repairs
  native CLR STA binding. General COM interface marshaling is still incomplete.
- Audio COM adapter selects exact Wine DirectSound/XAudio2.7 factories and
  native MMDeviceEnumerator/ActivateAudioInterfaceWorker factories. Other class
  registrations remain prefix-owned. Native XAudio2.8/2.9 now play in prefixes.
  Legacy XAudio2.7 engine/meter/reverb ordinary COM works for Wine and native;
  native fallback respects existing private registrations, including broken keys.
- XAPO frame queries delegate to FAPO. Actual volume meter processing works.
  Bundled FAPOFX echo/EQ/reverb/mastering-limiter DSP remains upstream stubs;
  helper creation/bypass is not proof those effects process samples.
- D3DX9 supports tested mixed Wine/native graphics. Wine's embedded VKD3D
  compiler emitted ps_2 texld coordinate swizzles rejected by native D3D9.
  d3dbc.c now legalizes unused components and materializes true permutations
  before allocation. Native/Wine rendered uv and uv.yx pixels both pass.
- Virtual CDDA uses private validated 2352-byte PCM tracks, TOC, seek/Q-channel
  and cross-track reads with Wine mcicda. prepare-cdda.ps1 converts original
  OGG/WAV/FLAC/MP3 once using FFmpeg (no runtime dependency). Track02 starts
  require explicit synthetic or ISO-backed data geometry. MCICDA keeps full
  HWND callbacks; x64 MCI notifications now pass. Q-position tracks read/prefetch
  rather than exact playback; upstream natural-end WAIT/NOTIFY limits remain.
- Review fixed aliased folder raw devices with unique private per-letter device
  paths, preserving identity across duplicates and reuse. Hand-edited duplicate
  ISO image paths are rejected before launch; CLI remount can repair them.
  CDDA rejects minute256 wrapping; ISO parsing now releases parent buffers and
  caps aggregate metadata at64MiB. Targeted invalid-input and identity tests pass.

The reviewed build is isolated in test-output/runtime-review; bin still contains
the earlier build used by the live Blossom session. Native/Wine build scripts
both support -OutputDirectory; object directories remain shared/sequential.
Private native DLL selection now checks private app/system overlays before host
readthrough. The x86 loader normalizes SysWOW64 to System32 internally; shim
resolves a readonly native parent directory before prefix translation so current
WOW redirection state and exempt subfolders are preserved. Dynamic loads pass;
private-system native startup imports are not yet validated.

Latest passing evidence under native/windows/test-output:
- build-com-backends.log: native both-architecture build.
- builtin-import-audit.json: all32 Wine DLLs, Windows/Wine dependencies only;
  no MSYS/Cygwin/libgcc/libstdc++/libwinpthread runtime imports.
- d3dx9-7d1c843c7e434ba483148c75bd8fc2b4/result.json: 32 rendered provider/
  architecture/import/desktop cases plus2 fallback and2 strict rejection cases.
- xaudio-c3cea1b94c314dcebe41ae7eb1a771c4/result.json: all46 cases, Wine/native
  2.7/2.8/2.9, refill callbacks, progression, resampling, pause/resume, COM,
  imports, mixed helpers, virtual desktop, volume meter and X3D matrices.
  Silent fixtures, no audio capture; audible game quality remains separate.
- registry-76256efdd4384bb089026ebbe0f37c35/result.json: full registry suite,
  OLE/Rpc private-overrides/host-sibling isolation, same-process duplication,
  63 observed opposite-view handle reuses per architecture.
- cdda-media-b557e1838cdc4d6392c388233da66f4b/result.json: distinct alias drives,
  duplicate/reuse identity, exact MSF bound; media-bfd8e7dfe8b04f6aa2b8c86336c7c21a
  covers ISO metadata cap and raw alias/duplicate image cases.
- cdda-f58140155a01403099df946e6ac57b7c/result.json: all six MCI cases rerun
  against reviewed raw devices, passing.
- review-prefix-suite.log -> suite-57c40bd7672b4a0dab1fa9698ecea7c4.
- review-desktop-suite.log -> desktop-700f78fd823c4d5a8d13b1c81797eb34.
- prefix-native-dll-0d13e02fffc4492f90b48abf09c21514/result.json: 16 dynamic
  DLL loading/order/fallback cases and9 filesystem view assertions matching
  native baseline, including Sysnative, disable/revert and drivers/etc exemption.
- final-review-prefix-suite.log -> suite-0594681ddf2140569d52beb6fdf61315.
- package-03213fa11d4a44d1ac45197f6ddf39e2/result.json: extracted0.2 archive,
  all43 hashes and four x86/x64 D3DX render/XAudio stream/import smoke tests pass.
- cdda-41c79663b0644ba38d354c35cc0ccce0/result.json: six MCI lifecycle cases,
  full HWND notifications and clean exits.
- registry-faceeb9b48f847d8a4ed94273cecc477/result.json: both views/architectures,
  private HKCR/COM, inherited-relative child views, user/host/sibling isolation.
- registry-import-e1cf174f63db4946ac3a3c7d7e3f7c55/result.json: explicit/default
  import views. test-gamerepo.py: two unit tests pass.
- pe-architecture-588d9f3b53df4bc68917d15f50676dad: metadata + actual CLR checks.
- expanded-prefix-suite.log -> suite-0f4291a9cfc24b1a9a059013bc199d6f.
- expanded-overrides-suite.log -> overrides-49d21e7a96a8482e9de287777478f7d2.
- expanded-compat-suite.log -> compat-bc5f71b05ea345f3a6cc6253eada3543.
Earlier ISO/input/DirectSound/D3D8/loader-path evidence remains below.

Blossom Tales II:
- GameRepo UUID8e3e1e3e-110a-4797-9494-749bb8984782, archive2.11.16.0.
  It is FNA/.NET Framework4 with bundled FAudio/SDL2/FNA3D, not Unity.
- Prefix test-output/blossom-tales2-1d9db6f19e314d189cbd5d43fc88c0ee;
  executable C:\app\BlossomTales2.exe. Original archives untouched.
- Fixed native CLR discovery then STA FileLoadException0x800703F0. Isolated
  diagnostic hive proved Rpc NdrOleExtDll alone repairs GIT registration;
  OLE+Rpc configuration permits CLR STA binding. No general host CLSID fallback.
  Created missing private AppData/Roaming/Blossom Tales2 folder for game logger.
- Original game reaches visually verified title screen in1280x720 desktop.
  Logs show native D3D11 RTX4090 and NOW PLAYING: BT2_Title.
  Its audio path is bundled FAudio -> SDL2 -> Windows, not Wine XAudio by default.
- Game left running for user listening check. Windows Firewall prompt overlays
  title screen; user asked to handle it. Agent did not touch security UI/settings.
  Audio skipping and gameplay not yet assessed. Diagnostic details are in
  prefix/diagnostics/startup-diagnosis.json and native-default-after-com-fix.log.
- Follow-up source audit identified FAudio22.7.0 (version export220700),
  SDL2.24.0.0, FNA22.8.0.0. Supported per-process A/B drivers are wasapi,
  directsound (with dsound=n or b), and winmm. Keep the same title scene and
  graphics configuration when comparing. No trials performed yet.
  SDL_AUDIO_SAMPLES is ineffective because FAudio supplies nonzero want.samples;
  FAUDIO_LOG_* is compiled out in this exact game DLL. Its performance glitch
  counter is not implemented, so zero cannot prove smooth playback.
  Sources: https://github.com/FNA-XNA/FAudio/blob/22.07/src/FAudio_platform_sdl2.c
  and https://github.com/libsdl-org/SDL/blob/release-2.24.0/src/audio/SDL_audio.c.
  Use wine64.exe/wine.exe, not winedows.exe, for future launch commands.

Nightmare Creatures:
- Gameplay was visually verified; user confirmed "Music is playing" with the
  GameRepo OGG winmm wrapper. Generic Wine MCICDA game audibility unverified.
- Restored known-working winmm.dll/ini from .ogg-backup copies after generic
  test, hashes matched originals. Game currently closed; all edits in private
  test-output/nightmare-creatures prefix. Original GameRepo untouched.
- Private D: PCM mapping still present, explicitly synthetic150-sector data
  track plus24 prepared music tracks; extracted data/NC_PC label preserved.

Deliverable: native/windows/dist/winedows-0.2-alpha.zip (35,052,809 bytes),
SHA256 ECBCE4FCC64A8BF2B6C5B2C08CC453EDD4945D160048E73F56315D83FA7A044B.
It contains both architectures of all16 Wine modules, native launchers/shims,
README/preparation helper and Wine/Detours/FAudio licenses. It is built from
runtime-review; shared bin remains the older live-game runtime until safe to
replace. No original game archives or host registry/security settings changed.

Next: obtain Blossom listening feedback, dismiss firewall prompt through user
handoff, and progress into gameplay before claiming its skipping issue resolved.
Continue broader Wine porting and the documented loader/COM/desktop limitations.

## Earlier checkpoints (historical; current state above takes precedence)

## Requirements and direction

Native Windows games, no WSL; Wine code and file/registry prefixes, virtual
removable/CD-ROM drives, accelerated graphics/video, and Wine compatibility.
GameRepo: D:\GameRepo\main. Preserve the original archive payloads.
User explicitly clarified that Wine's included implementations and native/builtin
fallback must be preserved. Prefix interception alone was too narrow. Actual
Wine DLL reuse is now implemented for graphics, input, and audio; broader porting remains.

Goal active, not complete. No remote fork or PR published. Do not claim parity.

## Source/builds

Branch winedows-native from Wine 11.0 db11d0fe6a169c457e23d007e20404643d067aa8.
- build.ps1: MSVC static-CRT loader/shims/tests, Detours 4.0.1, both architectures.
- build-wine.ps1: upstream ddraw/d3d8/d3d9/wined3d/dinput/dinput8/dsound/mcicda
  native Windows PE DLLs. mcicda integration is currently under test.
  MSYS2 provides build tools only. Uses MinGW x86/x64, make, flex, bison.
  Flex/bison installed through approved package-manager call.
- tools/winebuild/import.c: native Windows response files fix archive commands
  exceeding Windows' command-line limit. Graphics implementation source unchanged.
- Configure requires relative source paths and matching native build/host
  triplets, --without-mingw --without-freetype; x64 adds --enable-win64.
- DLL imports inspected: Windows libraries and WineD3D only, no MSYS runtime.
- Do not overlap builds in the same output directory.

User added workspace to Defender exception after wine64.exe was quarantined.
Rebuilt normally. Agent did not change security settings.

## Prefix backend

Native NT file/registry redirection, known folders, private RegLoadAppKey hive,
Wine text registry bridge using the original Wine server/unicode.c codec.
One loader process tree per prefix (session lock). Job waits for children before
export. Idle text edits import on next session. Invalid edits preserve inputs;
failed initial import removes partial binary hive. Data-only registry copy is
required: RegCopyTree and RegRenameKey fail on app-hive security restrictions.

CD directory mounts report type/label/serial/CDFS, allow seekable reads, reject
writes/delete/rename destinations. Raw volume paths (\\\\.\\D:) use private
directory handles with virtual device metadata and limited verify/write IOCTLs.
Removable directory mounts inside prefix are writable; external mounts readonly.
ISO9660/Joliet image mounts now retain real raw sectors; see the milestone below.
UDF/CDDA/eject/live swapping remain incomplete (CDDA is the current workstream).

Child A/W hooks cover custom env, null application argument, cross-bitness.
Detours requires DetourFinishHelperProcess at ordinal 1. Small-stack GPU crash
fixed by moving 64KiB buffers off DllMain/hook stacks. Read-only file position
and mode changes must be allowed: these change the open handle, not media.

Not a security sandbox: unsupported NT APIs, raw syscalls, device/UNC namespaces,
services/elevation, IPC, and unhooked launch paths remain gaps.

## Wine builtins and native selection

modules.cpp reads standard private Wine DllOverrides/AppDefaults registry keys
and WINEDLLOVERRIDES. Priority: environment > application > prefix.
Selectable: ddraw/d3d8/d3d9/dinput/dinput8/dsound; b,n; n,b; b; n and long forms.
mcicda was just added for CDDA integration tests. Default still native.
Missing builtin falls back when allowed; strict builtin fails.
Fallback is availability-based, not dependency/initialization-error retry.

WineD3D and frontend DLLs preload before executable import binding. No game DLL
or executable rewriting. Actual IAT bindings verified. Native uses Windows'
normal search; do NOT Detours-preload arbitrary native DLLs: native d3d9 does
not export ordinal 1. Child hooks select proper bitness and effective env.
Full-path loads now preserve selected providers. Disabled/general overrides,
KnownDLLs, and full loader parity remain work. Kernel/window/device backends
remain Windows native; DirectInput/DirectSound use real Wine implementations.
Direct3D8 has passed its own rendering fixture.
use-wine-graphics.ps1 is an optional reversible app-local install utility;
ordinary loader overrides do not need it.

## Verification

- test.ps1 passes unrestricted after latest child integration: x86/x64,
  cross-bitness, tiny stack, custom env, CryptoAPI, file/registry separation,
  registry recovery, CD device/seek/read-only, removable writes, native D3D11.
  9 host assertions plus native probes.
  test-output/suite-4b3f84f517e6410f9f5d8227fe2f0ddd/result.json
- test-wine-graphics.ps1: real Wine DD7/D3D9 render/readback in both bitnesses;
  restored native implementation also passes. Intel UHD770 OpenGL4.6 confirmed.
  Wine emulates Intel HD4000 D3D adapter identifier.
  test-output/wine-graphics-d42983b288914296a95b65df3483e758
- test-dll-overrides.ps1: both orders, mixed providers, static imports, dynamic
  basename loads, both child bitness transitions/custom env, registry priority,
  missing builtin fallback, strict builtin rejection, no app DLL copies.
  test-output/overrides-a1f0631c5a934d4ba9f73437a6a5bc21/result.json
- Xeno Crisis1.0.4: native D3D11 intro/menu/character selection on RTX4090.
  No full gameplay claim. Prefix test-output/xeno-crisis; private LocalLow state.
- Nightmare Creatures1.2: test-output/nightmare-creatures, D: CD label NC_PC.
  Native and Wine DirectDraw hit "requires a 16 bits video card".
  Local dgVoodoo2.64 with DesktopBitDepth16 gets past it, then PowerVR init error.
  Only staged copy contains DDraw.dll/D3DImm.dll/dgVoodoo.conf from GameRepo fixes.
  All test game windows closed.

## Next work

1. Broaden virtual desktop/window-manager coverage; diagnose Nightmare Creatures
   waiting in the native Intel DirectDraw stack (Wine DirectDraw IAT verified).
2. More Wine builtins with meaningful tests: D3D8, audio/input, higher-level DLLs.
3. Full paths/load failures/disabled modules/dependencies/relative child exe
   resolution in DLL selection. Preserve native and builtin choices.
4. Image-backed media, configurable serial/labels, media lifecycle.
5. Broader prefix API coverage and real-game save/relaunch tests.
   gamerepo.py stages Cryo3.0 ZIP layers/registry/drives, not arbitrary profiles.

## Virtual desktop / legacy compatibility milestone (2026-10-29)

User explicitly requested Wine-like virtual desktops and virtual display modes.
Implemented in desktop-host.cpp / desktop.cpp / display.cpp:
- `--desktop [name,]800x600` and `explorer /desktop=name,640x480 PROGRAM`.
- Standard private Wine Explorer Desktop/Desktops settings, per-app selection,
  explicit `--desktop off`. One container per existing prefix session/job.
- 16/32-bit virtual resolutions (including 640x480 absent on this host), host
  window resizing, virtual coordinates/monitor bounds, cursor translation,
  logical styles and focus, cross-bitness child propagation/custom environments.
- Hidden helper windows stay native/hidden. Windows are adopted when shown;
  containing hidden graphics helpers attached driver input queues unnecessarily.
- Parent GUI fixtures must pump messages while waiting for another GUI process.
  A blocking GUI-thread WaitForSingleObject caused a child focus timeout.
- Native-windowed DD/D3D9 and real Wine DD/D3D9 RGB565 hardware pixels verified
  in visible contained windows. Xeno Crisis native D3D11 reaches intro/menu and
  character selection in the container; focus forwarding verified with Return.
- The container is experimental: modal dialog creation, menus, DPI, native DXGI
  exclusive fullscreen, 8-bit palette desktops, raw input, all window APIs and
  fixed-resolution scaling remain incomplete. Uncovered windows can escape it.
- Outside containers, Wine graphics advertise 16-bit copies of real host modes.
  Null mode reset uses startup modes, not unrelated host registry display modes.
  An early fixture switched 1920x1080 to saved 5120x1440; 1920x1080x32@60 was
  restored. Corrected fixtures explicitly compare host state before/after.

compat.cpp and extract-version.ps1 compile Wine ntdll/kernelbase version tables,
query/comparison routines from the upstream sources. AppDefaults Version wins
over prefix Version; `native` opts out. Wine's Win9x missing-DLL error1157 is
preserved (NT uses126). Windows/driver module callers retain native versions;
application/Wine/app-local DLL callers see the configured version. PEB and other
OS-version-dependent behaviors are not fully emulated.

Regression artifacts:
- compat-a36d3d2e11084083be3ab9bc13674c93: version semantics/native opt-out x86/x64.
- suite-281945c1af5d46b99045ab1e1b8300c7: prefix/registry/drives/children/D3D11.
- display-1d154c03182044d6be93730cdee91c58: depth/RGB565/restoration/native view.
- overrides-80d684233ead4bb6a7b06a7d449459ba: DLL order/fallback and unused
  native override fix (native-only d3d8 must not block an unrelated x64 game).
- wine-graphics-0e93e57c04f84476b92414d3ca14f738: native/Wine graphics regressions.
- desktop-613371a47e62417c830aa29ccdba22f0: latest desktop suite, includes hidden
  helper adoption, focus, crossbitness, registry/app/CLI settings, Wine/native
  visible rendering and unchanged host display. Some WineD3D pixel-format
  warnings remain despite successful render/readback/present assertions.
- desktop-visual.log: visibly verified blue 640x480 virtual desktop fixture.
- xeno-desktop.log: real native D3D11 game launch; UI visible in this chat.

Nightmare Creatures still stalls with Wine DD + win98 + virtual desktop; no
successful startup claim. Captured nightmare-hang.dmp and thread offsets show
main thread waiting inside igd9trinity32.dll, with native ddraw additionally
loaded. NC's actual static DirectDraw imports point to Wine DD exports0x20c60
and0x212f0. The Win98 host-driver exclusion and hidden-helper changes did not
resolve the stall. GDB attach itself stalled and was terminated; a local
MiniDumpWriteDump + Wow64GetThreadContext snapshot worked. Original game
archives/executables remain unmodified. Private staged dgVoodoo files remain.

Deliverable: dist/winedows-0.1-desktop-alpha.zip. All17 manifest hashes verified;
the packaged loader/DLLs passed display-probe64 with a640x480 virtual desktop,
Wine DD/D3D9 RGB565 rendering/readback/presentation and reset. Evidence:
test-output/desktop-package-smoke.log. Source builds and all test games/fixture
desktops are closed at this checkpoint. Original goal remains active/incomplete.

## Input/audio/media/loader milestone (2026-10-29, intermediate checkpoint)

User explicitly authorized fan-out; four subagents cover input/registry,
audio/CDDA, D3D8/loader paths, and optical media. Root integrates shared runtime.

- Real upstream Wine dinput/dinput8/dsound now build and select on botharches.
  wrc locale conversion uses Wine locale.nls instead of Windows custom LCIDs;
  Windows collapsed distinct translated resources into 0x1000 and link failed.
  44 unique DirectInput dialog languages now retained; no MSYS runtime imports.
- DirectSound native WASAPI returned stream latency 0, endpoint period100000
  (10ms), buffer3840 frames, 48kHz. Wine mixer previously computed zero fragment
  and never advanced. dlls/dsound/primary.c falls back to GetDevicePeriod only
  for nonpositive latency, rejects empty period/buffer. Actual silent playback
  cursor advancement and position/stop notifications now pass.
- audio-backend.cpp routes MMDeviceEnumerator and playback DirectSound/8
  CoCreateInstance/Ex through native endpoint / selected provider factories;
  preserves COM initialization, partial multi-QI, module lifetime, and unknown
  class forwarding. No broad host COM registry readthrough. Capture/full-duplex,
  CoGetClassObject-only activation, XAudio2/OpenAL remain future work.
- Native COM needed correct registry identities: NtQueryKey returns logical
  names instead of internal app-hive names; User Software Classes canonicalizes
  to SID_Classes (Windows uses this root for HKCR merging). Empty private class
  roots exist even in fresh prefixes. Private machine/user HKCR merge, write
  targets, and real private COM DLL activation all pass botharches.
  Host readthrough includes narrowly scoped MMDevices/Audio configuration.
  Known remaining view issue: x86 explicit 32/64-bit HKLM views alias; x64 passes.
- module-runtime.cpp preserves selected exact builtin modules for explicit
  full paths and module-handle lookups via native loader hooks. Resource/datafile
  loads retain requested file and loader refs are balanced. Tiny-stack-safe
  heap buffers in all new DllMain paths. Native choices stay ordinary Windows.
- drive mount LETTER IMAGE.iso validates ISO9660/Joliet, retains immutable
  snapshot plus readonly extracted filesystem, atomically publishes mapping.
  Actual raw sector reads via bare drive handle, label/serial/CDFS, 2048-byte
  geometry/capacity, original-source preservation pass. Bad images preserve old
  mapping. UDF/BIN-CUE/raw2352/multisession/CDDA not yet supported by this adapter.
  Cache uses image+files storage; old/failed prepared caches remain. ISO timestamps
  and attributes still use extracted host metadata. Win9x serial reversal pending.

Evidence under test-output (all passed unless marked known gap):
- input-871da15aac19448b845f9e2e496ae1ae: dynamic/static, Wine/native/mixed,
  keyboard/mouse, botharches/crossbitness/customenv/desktop/fallback. No synthetic
  or physical input delivery/controller/force-feedback validation claimed.
- d3d8-8a86dfa238d74fe1a8d5dec4cc8d4167: 16 triangle/readback/present cases,
  ordinary/virtual, dynamic/static, Wine32/64, native32, native64 absent -> n,b.
  Actual GPU Intel UHD770 OpenGL4.6; Wine D3D adapter reports emulated HD4000.
- media-b9178f2eac1846379a7f4b5298468cf1: ISO/Joliet x86/x64, rawsectors,
  readonly backing, 16 malformed remounts, offlineoriginal, unchanged hostdrives.
- registry-49c2ac3fd2a046fa93397d95b996049e: native baseline + all private
  name/buffer/HKCR/COM/sibling/host checks. registry-views32.log fails known
  x86-view alias; registry-views64.log passes.
- loader-paths-d41b4139d49f47458afc925c45118abc: 7 configurations/4962 assertions,
  six providers, direct LdrLoadDll, full/missing/case/extensionless paths,
  handles/refcounts/address queries, marker resource/datafile preservation.
- audio-3ce62d9d12254580ad57572285bfa31d: all12 host/Wine/native/desktop cases,
  dynamic/static/COM and real playback progression. Muted silence, no capture.
- final-prefix-suite.log -> suite-eea8475040da4eadb1d57f9195c2ab74.
- final-compat-suite.log -> compat-328721da568740f6824b50141e64ae79.
- final-overrides-suite.log -> overrides-3db13e50b6d546d4aadac3288c878cdc.
- final-desktop-suite.log -> desktop-b7cc7f58e975413e9d2d39dda6d9b9b8.

Nightmare Creatures now reaches actual gameplay, visually verified inside its
640x480 container with all six Wine providers (nightmare-integrated.log).
Prior blank stall is resolved in this integration; individual causality between
fullpath/input/audio/registry fixes has not been isolated. No long-play claim.
User heard missing CDDA. GameRepo already supplies Music/Track02.ogg..Track25.ogg
and compatibility profile specifies ogg_winmm_1.0_x86. Staged the two hash-verified
winmm.dll/winmm.ini files from that existing fix into private NC app directory;
original archives untouched. Relaunched allsixWine+winmm=n, log
nightmare-cdda-native.log. User explicitly confirmed "Music is playing".
Live NC currently uses private app-local WINMM.dll plus Wine dsound; native
system winmm is also loaded for forwarding. UI current game window remains open.

CDDA next work (user steering): preserve working game-native replacement, add
reusable prefix CDDA using actual Wine mcicda and virtual TOC/2352 PCM reads.
Wine mcicda builds botharches and native winmm can load it through private
MCI32 CDAudio registration (route test passed product "Wine's audio CD").
Disc agent adds cdda-media and Drive.audio manifest; audio agent builds MCI
playback lifecycle fixtures; loader agent validates GameRepo OGG wrapper;
registry agent investigates x86 WOW64 alias. Shared native rebuild must wait
until the live NC test closes. No new0.2 package created yet; README/package
script updated for this milestone but final CDDA docs still pending.

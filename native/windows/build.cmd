@echo off
setlocal
set "VCARCH=%1"
if "%1"=="x86" set "VCARCH=x64_x86"
call "%WINEDOWS_VCVARS%" %VCARCH% >nul
if errorlevel 1 exit /b 1
set "ROOT=%~dp0"
if not defined WINEDOWS_BIN set "WINEDOWS_BIN=%ROOT%bin"
set "BITS=64"
set "DETOURS_TARGET_PROCESSOR=X64"
if "%1"=="x86" set "BITS=32"
if "%1"=="x86" set "DETOURS_TARGET_PROCESSOR=X86"
pushd "%ROOT%vendor\Detours-4.0.1\src"
nmake /nologo
if errorlevel 1 exit /b 1
popd
if not exist "%ROOT%build\%1" mkdir "%ROOT%build\%1"
pushd "%ROOT%build\%1"
set "FLAGS=/nologo /std:c++17 /EHsc /MT /O2 /Z7 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /I%ROOT%vendor\Detours-4.0.1\include"
cl /nologo /MT /O2 /c ..\wine_unicode.c
if errorlevel 1 exit /b 1
cl %FLAGS% /c "%ROOT%common.cpp" "%ROOT%registry.cpp" "%ROOT%graphics.cpp" "%ROOT%modules.cpp" "%ROOT%media.cpp" "%ROOT%cdda-media.cpp" "%ROOT%cue-media.cpp" "%ROOT%registry-views.cpp" "%ROOT%com-marshaling.cpp" "%ROOT%imaging-defaults.cpp" "%ROOT%startup-native.cpp" "%ROOT%startup-imports.cpp" "%ROOT%media-state.cpp" "%ROOT%media-descriptor.cpp" "%ROOT%video-backend.cpp" "%ROOT%pe-architecture.cpp"
if errorlevel 1 exit /b 1
cl %FLAGS% "%ROOT%launcher.cpp" "%ROOT%desktop-host.cpp" "%ROOT%media-controller.cpp" "%ROOT%media-notifications.cpp" common.obj registry.obj graphics.obj modules.obj media.obj cdda-media.obj cue-media.obj registry-views.obj com-marshaling.obj imaging-defaults.obj startup-native.obj startup-imports.obj media-state.obj media-descriptor.obj video-backend.obj pe-architecture.obj wine_unicode.obj /Fe:"%WINEDOWS_BIN%\wine%BITS%.exe" /link /DEBUG "%ROOT%vendor\Detours-4.0.1\lib.%DETOURS_TARGET_PROCESSOR%\detours.lib" advapi32.lib shell32.lib d3d11.lib dxgi.lib dxguid.lib dbghelp.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl %FLAGS% /LD "%ROOT%shim.cpp" "%ROOT%compat.cpp" "%ROOT%display.cpp" "%ROOT%desktop.cpp" "%ROOT%audio-backend.cpp" "%ROOT%graphics-backend.cpp" "%ROOT%module-runtime.cpp" "%ROOT%named-objects.cpp" common.obj registry.obj modules.obj media.obj cdda-media.obj cue-media.obj registry-views.obj com-marshaling.obj imaging-defaults.obj startup-native.obj startup-imports.obj media-state.obj media-descriptor.obj video-backend.obj pe-architecture.obj wine_unicode.obj /Fe:"%WINEDOWS_BIN%\winedows%BITS%.dll" /link /DEBUG /DEF:"%ROOT%shim.def" "%ROOT%vendor\Detours-4.0.1\lib.%DETOURS_TARGET_PROCESSOR%\detours.lib" advapi32.lib shell32.lib ole32.lib user32.lib gdi32.lib uuid.lib dxguid.lib
if errorlevel 1 exit /b 1
cl %FLAGS% "%ROOT%probe.cpp" /Fe:"%WINEDOWS_BIN%\probe%BITS%.exe" /link advapi32.lib shell32.lib ole32.lib
if errorlevel 1 exit /b 1
cl %FLAGS% "%ROOT%compat-probe.cpp" /Fe:"%WINEDOWS_BIN%\compat-probe%BITS%.exe"
if errorlevel 1 exit /b 1
cl %FLAGS% "%ROOT%display-probe.cpp" /Fe:"%WINEDOWS_BIN%\display-probe%BITS%.exe" /link user32.lib gdi32.lib dxguid.lib
if errorlevel 1 exit /b 1
cl %FLAGS% "%ROOT%desktop-probe.cpp" /Fe:"%WINEDOWS_BIN%\desktop-probe%BITS%.exe" /link user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl %FLAGS% "%ROOT%audio-probe.cpp" /Fe:"%WINEDOWS_BIN%\audio-probe%BITS%.exe" /link user32.lib ole32.lib uuid.lib dxguid.lib
if errorlevel 1 exit /b 1
cl %FLAGS% /DWINEDOWS_STATIC_AUDIO_IMPORTS "%ROOT%audio-probe.cpp" /Fe:"%WINEDOWS_BIN%\audio-import-probe%BITS%.exe" /link user32.lib ole32.lib uuid.lib dxguid.lib dsound.lib
if errorlevel 1 exit /b 1
cl %FLAGS% "%ROOT%wine-graphics-probe.cpp" /Fe:"%WINEDOWS_BIN%\wine-graphics-probe%BITS%.exe" /link user32.lib gdi32.lib opengl32.lib dxguid.lib
if errorlevel 1 exit /b 1
cl %FLAGS% /DWINEDOWS_STATIC_GRAPHICS_IMPORTS "%ROOT%wine-graphics-probe.cpp" /Fe:"%WINEDOWS_BIN%\wine-import-probe%BITS%.exe" /link user32.lib gdi32.lib opengl32.lib dxguid.lib d3d9.lib ddraw.lib
if errorlevel 1 exit /b 1
popd
endlocal

#!/usr/bin/env bash
# MSYS2 is a build-time shell only. The resulting DLLs are native Windows PE.
set -euo pipefail
export PATH=/usr/bin:$PATH
arch=${1:-x86}
jobs=${2:-8}
IFS=, read -r -a modules <<< "${3:-ddraw,d3d8,d3d9,wined3d,dxgi,d3d11,d3d10core,d3d10_1,d3d10,d3dcompiler_39,dinput,dinput8,xinput1_1,xinput1_2,xinput1_3,xinput1_4,xinput9_1_0,dsound,mcicda,d3dx9_36,d3dx9_37,d3dx9_41,d3dx9_42,d3dcompiler_42,d3dx9_43,d3dx10_43,d3dx11_43,d3dcompiler_43,d3dcompiler_47,d3dxof,xaudio2_7,xaudio2_8,xaudio2_9,x3daudio1_7,xapofx1_5,mfreadwrite,gdiplus,xactengine3_7,glu32}"
mode=${5:-modules}
targets=()
if [[ "$mode" == prepare-vulkan ]]; then
    targets=(tools/winegcc/winegcc.exe tools/winebuild/winebuild.exe tools/wrc/wrc.exe nls/locale.nls)
    for library in advapi32 winecrt0 ucrtbase kernel32 ntdll user32; do
        targets+=("dlls/$library/lib$library.a")
    done
elif [[ "$mode" == modules ]]; then
    for module in "${modules[@]}"; do
        [[ "$module" =~ ^[a-z0-9_]+$ ]] || { echo 'Invalid Wine module name' >&2; exit 2; }
        targets+=("dlls/$module/all")
    done
else
    echo 'Invalid Wine build mode' >&2; exit 2
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../.." && pwd)
wine_source=$(cygpath -u "${6:-${CASKWIN_WINE_SOURCE:-$repo/build/wine-source}}")
wine_source=$(cd -- "$wine_source" && pwd)
source_identity=${7:-}
[[ "$source_identity" =~ ^[a-f0-9]{64}$ ]] || { echo 'Use build-wine.ps1 to resolve and verify the selected Wine source identity.' >&2; exit 2; }
[[ -f "$wine_source/configure" && -f "$wine_source/.caskwin-source.json" ]] || { echo 'Wine source must be prepared with scripts/prepare-wine.ps1.' >&2; exit 2; }
case "$arch" in
    x86) export PATH=/mingw32/bin:/usr/bin:$PATH; triplet=i686-w64-mingw32; options=() ;;
    x64) export PATH=/mingw64/bin:/usr/bin:$PATH; triplet=x86_64-w64-mingw32; options=(--enable-win64) ;;
    *) echo 'Expected x86 or x64' >&2; exit 2 ;;
esac
build="$repo/native/windows/build/wine-$arch"
mkdir -p "$build" "$build/tmp"
export TMPDIR="$build/tmp"
cd "$build"
if [[ -f .caskwin-source-id ]]; then
    [[ "$(cat .caskwin-source-id)" == "$source_identity" ]] || {
        echo "Wine object tree $build belongs to another source/profile/patch set. Remove or archive this build directory before switching profiles." >&2; exit 2;
    }
elif [[ -n "$(find . -mindepth 1 -maxdepth 1 ! -name tmp -print -quit)" ]]; then
    echo "Wine object tree $build has unverified configuration/build files. Remove or archive it before configuring selected sources." >&2; exit 2
else
    # Establish provenance before configure can leave a partial config.cache
    # or generated file behind. A missing Makefile never bypasses this guard.
    printf '%s\n' "$source_identity" >.caskwin-source-id
fi
if [[ ! -f Makefile ]]; then
    echo "Configuring Wine's native $arch DLL build..."
    # Keep source paths relative: native MinGW tools cannot fopen MSYS /c/... paths.
    [[ "$(cygpath -am "$wine_source" | cut -c1-2)" == "$(cygpath -am "$build" | cut -c1-2)" ]] || { echo 'Wine source and native object tree must be on the same Windows drive for relative native-tool paths.' >&2; exit 2; }
    source_relative=$(realpath --relative-to="$build" "$wine_source")
    "$source_relative/configure" --build="$triplet" --host="$triplet" --without-mingw --without-freetype --disable-tests --disable-win16 --cache-file=config.cache \
        "${options[@]}" >configure-output.log 2>&1 || { tail -50 configure-output.log; exit 1; }
fi
if [[ "$mode" == prepare-vulkan ]]; then
    echo "Preparing native Wine Vulkan tools and import libraries ($arch)"
else
    echo "Compiling Wine modules ($arch): ${modules[*]}"
fi
make -j"$jobs" "${targets[@]}" >build-output.log 2>&1 || { tail -70 build-output.log; exit 1; }
if [[ "$mode" == prepare-vulkan ]]; then exit 0; fi
dest="$repo/native/windows/bin/builtin/$arch"
if [[ -n "${4:-}" ]]; then dest="$(cygpath -u "$4")/builtin/$arch"; fi
mkdir -p "$dest"
for module in "${modules[@]}"; do
    cp "dlls/$module/$module.dll" "$dest/"
done
echo "Wine DLLs: $dest"

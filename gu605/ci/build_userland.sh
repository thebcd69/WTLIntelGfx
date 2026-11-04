#!/bin/bash
# build_userland.sh OUTDIR: Mesa ANV + libdrm for x86_64 macOS (userland/BUILD.md, the commits and patches upstream and
# samwu07 built), plus a self-contained AIR bitcode reader (MacPorts LLVM 20 libLLVM.dylib relinked to @loader_path,
# samwu07's add-bitcode-reader.sh), for the CI's macos-15-intel runner. MacPorts binary archives only (port -b).
# OUTDIR gets libvulkan_intel.dylib, libdrm.2.dylib, air-runtime/, USERLAND.txt.
set -euo pipefail
OUT=$1; mkdir -p "$OUT"
# every build step's output, and its end on stdout when a step fails (the CI log is the only record)
BLOG=$OUT/../userland-build.log; : > "$BLOG"
trap 'rc=$?; echo "userland: FAILED (exit $rc) at line $LINENO: $BASH_COMMAND"; tail -80 "$BLOG"' ERR
R=$(cd "$(dirname "$0")/../.." && pwd)
X=$HOME/src/xe-userland; mkdir -p "$X"
DRM_REV=b97cbde15c5c3abfe44d78e8f57139e50f612fec
MESA_REV=bffd91947efc1932da99aca69fc540d6d4201b0c
SPIRVLLVM_TAG=v20.1.19
MP=/opt/local
# The build is x86_64 throughout. On an Apple-silicon runner (macos-14 while the Intel runners are unavailable) the
# caller runs this script under Rosetta (arch -x86_64), MacPorts installs x86_64 archives (build_arch x86_64) and
# every tool, host one (mesa_clc) included, runs as x86_64.
[ "$(uname -m)" = x86_64 ] || { echo "run under x86_64 (on Apple silicon: arch -x86_64 bash $0 ...)"; exit 1; }
case "$(sw_vers -productVersion | cut -d. -f1)" in
  14) MPOS=14-Sonoma ;; 15) MPOS=15-Sequoia ;; 26) MPOS=26-Tahoe ;; *) echo "no MacPorts pkg for macOS $(sw_vers -productVersion)"; exit 1 ;;
esac
JOBS=$(sysctl -n hw.ncpu)
if [ ! -x $MP/bin/port ]; then
  got=""
  for u in https://github.com/macports/macports-base/releases/download/v2.12.7/MacPorts-2.12.7-$MPOS.pkg \
           https://distfiles.macports.org/MacPorts/MacPorts-2.12.7-$MPOS.pkg \
           https://github.com/macports/macports-base/releases/download/v2.11.6/MacPorts-2.11.6-$MPOS.pkg \
           https://distfiles.macports.org/MacPorts/MacPorts-2.11.6-$MPOS.pkg; do
    rm -f /tmp/macports.pkg
    curl -fsSL --retry 3 -o /tmp/macports.pkg "$u" 2>/dev/null || { echo "MacPorts pkg: $u not available"; continue; }
    sig=$(pkgutil --check-signature /tmp/macports.pkg 2>&1 || true)
    if printf '%s\n' "$sig" | grep -qE 'Developer ID Installer: .*\(QTA3A3B7F3\)'; then got=$u; break; fi
    echo "MacPorts pkg: $u signature not accepted:"; printf '%s\n' "$sig" | head -8
  done
  [ -n "$got" ] || { echo "MacPorts pkg signature: no signed MacPorts installer for $MPOS"; exit 1; }
  echo "MacPorts from $got"
  sudo installer -pkg /tmp/macports.pkg -target / > /dev/null
fi
export PATH=$MP/bin:$MP/sbin:$PATH
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ]; then
  grep -q '^build_arch' $MP/etc/macports/macports.conf && sudo sed -i '' 's/^build_arch.*/build_arch x86_64/' $MP/etc/macports/macports.conf \
    || echo 'build_arch x86_64' | sudo tee -a $MP/etc/macports/macports.conf > /dev/null
  echo "Rosetta: MacPorts build_arch x86_64"
fi
sudo port -N selfupdate > /dev/null
sudo port -N -b install llvm-20 clang-20 > /dev/null
sudo port -N install glslang cmake pkgconfig bison flex > /dev/null
# meson and the Python modules Mesa's build needs, in a venv first in PATH (as userland/BUILD.md)
python3 -m venv "$HOME/.venvs/mesa"; "$HOME/.venvs/mesa/bin/pip" -q install meson ninja mako pyyaml packaging
export PATH=$HOME/.venvs/mesa/bin:$PATH
LLVM_CONFIG=$MP/libexec/llvm-20/bin/llvm-config; "$LLVM_CONFIG" --version
SDK=$(xcrun --sdk macosx --show-sdk-path)
export CC=/usr/bin/clang CXX=/usr/bin/clang++ OBJC=/usr/bin/clang SDKROOT=$SDK
# under Rosetta, name the architecture anyway (cmake and meson both read these)
[ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ] && export CFLAGS="-arch x86_64" CXXFLAGS="-arch x86_64" OBJCFLAGS="-arch x86_64" LDFLAGS="-arch x86_64" CMAKE_OSX_ARCHITECTURES=x86_64
at_rev() { [ -d "$1/.git" ] || { git init -q "$1"; git -C "$1" remote add origin "$2"; }; git -C "$1" fetch -q --depth 1 origin "$3"; git -C "$1" checkout -q --force FETCH_HEAD; }
at_rev "$X/drm" https://gitlab.freedesktop.org/mesa/libdrm.git "$DRM_REV"
at_rev "$X/mesa" https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_REV"
git -C "$X/drm" apply "$R/userland/patches/libdrm-macos.patch" "$R/userland/patches/libdrm-xegfx.patch"
git -C "$X/mesa" apply "$R/userland/patches/mesa-macos.patch" "$R/userland/patches/mesa-xegfx.patch"
# SPIRV-LLVM-Translator for mesa_clc (host tool only)
S=$X/SPIRV-LLVM-Translator; at_rev "$S" https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git "refs/tags/$SPIRVLLVM_TAG"
cmake -S "$S" -B "$S/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$X/spirv-llvm" \
  -DLLVM_DIR="$("$LLVM_CONFIG" --cmakedir)" -DLLVM_INCLUDE_TESTS=OFF -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_IGNORE_PREFIX_PATH=/usr/local >> "$BLOG" 2>&1
ninja -C "$S/build" -j "$JOBS" >> "$BLOG" 2>&1; cmake --install "$S/build" >> "$BLOG" 2>&1
# libdrm (core only; libdrm_intel needs libpciaccess, BUILD.md)
export PKG_CONFIG_LIBDIR=$X/prefix/lib/pkgconfig
# meson sees through Rosetta and would build for aarch64 (Mesa's blake3 NEON code broke run 41894886487): name the
# machine. A native file's [build_machine] overrides the detection; every meson setup below gets it.
MF=()
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ]; then
  printf "[build_machine]\nsystem = 'darwin'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n" > "$X/x86_64-machine.ini"
  MF=(--native-file "$X/x86_64-machine.ini")
fi
meson setup ${MF[@]+"${MF[@]}"} "$X/drm/build" "$X/drm" --prefix="$X/prefix" --libdir=lib -Dbuildtype=release \
  -Dc_args=-mmacosx-version-min=11.0 -Dc_link_args=-mmacosx-version-min=11.0 \
  -Dintel=disabled -Damdgpu=disabled -Dradeon=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Domap=disabled \
  -Dexynos=disabled -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled \
  -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dtests=false -Dinstall-test-programs=false >> "$BLOG" 2>&1
ninja -C "$X/drm/build" >> "$BLOG" 2>&1; meson install -C "$X/drm/build" >> "$BLOG" 2>&1
# host mesa_clc + vtn_bindgen2
LLVM_LIB=$("$LLVM_CONFIG" --libdir)
printf "[binaries]\nllvm-config = '%s'\ncmake = 'false'\n" "$LLVM_CONFIG" > "$X/native-llvm.ini"
PKG_CONFIG_LIBDIR= PKG_CONFIG_PATH=$X/spirv-llvm/lib/pkgconfig:$MP/lib/pkgconfig meson setup ${MF[@]+"${MF[@]}"} "$X/mesa/build-host" "$X/mesa" \
  --native-file "$X/native-llvm.ini" --prefix="$X/host" -Dbuildtype=release \
  -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dllvm=enabled -Dshared-llvm=enabled \
  -Dc_link_args=-Wl,-rpath,$LLVM_LIB -Dcpp_link_args=-Wl,-rpath,$LLVM_LIB \
  -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms= -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dglx=disabled -Dglvnd=disabled -Dvulkan-layers= -Dtools= \
  -Dbuild-tests=false -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false >> "$BLOG" 2>&1
ninja -C "$X/mesa/build-host" >> "$BLOG" 2>&1; meson install -C "$X/mesa/build-host" >> "$BLOG" 2>&1
# target ANV, no LLVM; expat from the SDK (BUILD.md)
mkdir -p "$X/sdk-pc"; printf 'Name: expat\nVersion: 2.7.1\nDescription: SDK expat\nLibs: -lexpat\nCflags:\n' > "$X/sdk-pc/expat.pc"
printf "[binaries]\ncmake = 'false'\nllvm-config = 'false'\n" > "$X/native-target.ini"
PATH=$X/host/bin:$PATH PKG_CONFIG_LIBDIR=$X/prefix/lib/pkgconfig:$X/sdk-pc meson setup ${MF[@]+"${MF[@]}"} "$X/mesa/build" "$X/mesa" \
  --native-file "$X/native-target.ini" --prefix="$X/prefix" --libdir=lib -Dbuildtype=release -Db_ndebug=true \
  -Dc_args=-mmacosx-version-min=14.4 -Dcpp_args=-mmacosx-version-min=14.4 -Dobjc_args=-mmacosx-version-min=14.4 \
  -Dc_link_args=-mmacosx-version-min=14.4 -Dcpp_link_args=-mmacosx-version-min=14.4 \
  -Dvulkan-drivers=intel -Dgallium-drivers= -Dplatforms= -Dtools=drm-shim -Dllvm=disabled \
  -Dmesa-clc=system -Dprecomp-compiler=auto \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dglvnd=disabled -Dgbm=disabled \
  -Dvulkan-layers= -Dbuild-tests=false -Dexpat=enabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false -Dvideo-codecs= >> "$BLOG" 2>&1
PATH=$X/host/bin:$PATH ninja -C "$X/mesa/build" >> "$BLOG" 2>&1; meson install -C "$X/mesa/build" >> "$BLOG" 2>&1
cp "$X/prefix/lib/libvulkan_intel.dylib" "$X/prefix/lib/libdrm.2.dylib" "$OUT/"
# AIR bitcode reader: libLLVM + every MacPorts library it needs, relinked to @loader_path (samwu07)
A=$OUT/air-runtime; mkdir -p "$A"; LL=$MP/libexec/llvm-20/lib
resolve() { case "$1" in /opt/local/*) echo "$1";; @rpath/*) b=${1#@rpath/}; for d in "$LL" $MP/lib; do [ -f "$d/$b" ] && { echo "$d/$b"; return; }; done;; esac; }
pull() { local src=$1 base dep real; base=$(basename "$src"); [ -f "$A/$base" ] && return 0
  cp -L "$src" "$A/$base"; chmod 755 "$A/$base"; install_name_tool -id "@loader_path/$base" "$A/$base" 2>/dev/null
  for dep in $(otool -L "$src" | tail -n +2 | awk '{print $1}'); do real=$(resolve "$dep"); [ -n "$real" ] || continue
    [ "$(basename "$real")" = "$base" ] && continue
    install_name_tool -change "$dep" "@loader_path/$(basename "$real")" "$A/$base" 2>/dev/null; pull "$real"; done; }
pull "$LL/libLLVM.dylib"
for f in "$A"/*.dylib; do codesign -s - -f "$f" > /dev/null 2>&1 || true; done
left=$(for f in "$A"/*.dylib; do otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/local|@rpath)' || true; done)
[ -z "$left" ] || { echo "air-runtime still points outside: $left"; exit 1; }
{ echo "libdrm $DRM_REV + libdrm-macos.patch + libdrm-xegfx.patch"; echo "mesa $MESA_REV + mesa-macos.patch + mesa-xegfx.patch"
  echo "LLVM $("$LLVM_CONFIG" --version) (MacPorts) SPIRV-LLVM-Translator $SPIRVLLVM_TAG"
  (cd "$OUT" && shasum -a 256 libvulkan_intel.dylib libdrm.2.dylib air-runtime/*); } > "$OUT/USERLAND.txt"
# everything shipped must be x86_64 (the Mac is Intel; a Rosetta slip would give arm64 libraries)
for f in "$OUT"/*.dylib "$OUT"/air-runtime/*.dylib; do
  a=$(lipo -archs "$f"); case " $a " in *" x86_64 "*) ;; *) echo "$f is $a, not x86_64"; exit 1;; esac; done
echo "built on $(sw_vers -productVersion) $(uname -m) translated=$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" >> "$OUT/USERLAND.txt"
cat "$OUT/USERLAND.txt"

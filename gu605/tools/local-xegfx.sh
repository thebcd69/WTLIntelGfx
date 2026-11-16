#!/bin/bash
# Builds XeGfx.kext on the GU605 itself (about a minute on its 12 threads; no GitHub runner queue).
# Once: ~/src/xe-src/drm-kmod (freebsd/drm-kmod at DRM_KMOD_REV), ~/src/xe-src/freebsd (freebsd-src sys/ at FREEBSD_REV),
# ~/src/xnu/config (apple-oss-distributions/xnu at XNU_TAG) -- the revisions are in .github/workflows/gu605-mac-xegfx.yml.
# On 2026-11-16 they were copied over as one archive (xegfx-build-src.tgz, 101 MB, sha256 0f87dd0840811e18...).
# Usage, from a checkout or an unpacked snapshot of this repository:  bash gu605/tools/local-xegfx.sh
# Result: out/XeGfx.kext, out/XeGfx.kext.zip, out/SHA256SUMS (same layout as the CI prerelease).
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$R"
for d in "$HOME/src/xe-src/drm-kmod" "$HOME/src/xe-src/freebsd/sys" "$HOME/src/xnu/config"; do [ -d "$d" ] || { echo "missing $d"; exit 1; }; done
# shim/fbsd/machine and x86 are links made by harness/score.py: ones copied from another machine point nowhere
for l in machine x86; do if [ -L "shim/fbsd/$l" ] && [ ! -e "shim/fbsd/$l" ]; then mv "shim/fbsd/$l" "${TMPDIR:-/tmp}/stale-$l.$$"; fi; done
[ -f "$HOME/src/xe-src/linux-firmware/mtl_guc_70.bin" ] || python3 -I gu605/tools/fetch_firmware.py --lock gu605/firmware.lock --out "$HOME/src/xe-src/linux-firmware"
mkdir -p out
export XEGFX_FW="$HOME/src/xe-src/linux-firmware" XEGFX_XNU_EXPORTS="$HOME/src/xnu/config" SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
bash build/build_xegfx.sh > out/build-xegfx.log 2>&1 || { tail -20 out/build-xegfx.log; exit 1; }
test -f out/XeGfx.kext/Contents/MacOS/XeGfx
(cd out && rm -f XeGfx.kext.zip && ditto -c -k --keepParent XeGfx.kext XeGfx.kext.zip &&
  shasum -a 256 XeGfx.kext.zip XeGfx.kext/Contents/MacOS/XeGfx XeGfx.kext/Contents/Info.plist > SHA256SUMS && cat SHA256SUMS)

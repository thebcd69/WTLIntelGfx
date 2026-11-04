# GU605: önerilen KIOXIA EFI değişiklikleri (kopya: /home/claude/gu605-efi-work/proposed/)

Taban: `kioxia-snapshot-20261104-1733/EFI/OC/config.plist` sha256 `edfb6abffef138a596a2c91c363748f58a744cb2773552bdffa389650870d22f`.
Ayrıntı ve geri alma: `proposed/CHANGES.md` (aşağıda aynısı) ve docs/GU605-KURULUM.md.

## Anlamsal fark (config.plist)

```
- /DeviceProperties/Add/PciRoot(0x0)/Pci(0x2,0x0) = {"disable-gpu": true}
+ /Kernel/Add[32] = {"Arch": "x86_64", "BundlePath": "XeGfx.kext", "Comment": "GU605: XeGfx (WTLIntelGfx gu605-mtl), idle until gu605-stages.sh loads it", "Enabled": true, "ExecutablePath": "Contents/MacOS/XeGfx", "MaxKernel": "24.99.99", "MinKernel": "24.0.0", "PlistPath": "Contents/Info.plist"}
```

## CHANGES.md

### Proposed KIOXIA EFI changes (GU605, Intel iGPU via WTLIntelGfx gu605-mtl)

Base: `kioxia-snapshot-20261104-1733/EFI/OC/config.plist` (sha256 `edfb6abffef138a596a2c91c363748f58a744cb2773552bdffa389650870d22f`).
Proposed: `EFI/OC/config.plist` here (sha256 in `SHA256SUMS`). Only these files change on the ESP:

| # | File | Change | Why |
|---|---|---|---|
| 1 | `EFI/OC/config.plist` | `DeviceProperties > Add > PciRoot(0x0)/Pci(0x2,0x0)`: key `disable-gpu` removed (the now-empty dictionary for that path removed too) | WhateverGreen hides the iGPU with it; no Intel driver can match |
| 2 | `EFI/OC/config.plist` | `Kernel > Add`: new last entry `XeGfx.kext` (`Contents/MacOS/XeGfx`, `Contents/Info.plist`, Arch x86_64, MinKernel 24.0.0, MaxKernel 24.99.99, Enabled) | XeGfx must be in the boot collection; it stays idle until `gu605-stages.sh` loads it (no IOKit match at boot) |
| 3 | `EFI/OC/Kexts/XeGfx.kext` | new folder, from release `gu605-<sha>` (`EFI/OC/Kexts/XeGfx.kext`) | the kext of change 2 |

Nothing else: boot-args, NullMoth settings, `Kernel > Block` (IONDRVSupport: the OpenCore log shows it never matched,
"Not Found"), ACPI, USB map, WhateverGreen, SMBIOS stay as they are.

Effect per G-Helper mode: **Ultimate** unchanged (Intel framebuffer declines without an iGPU-lit panel; XeGfx idle;
boot runner does nothing). **Eco** Intel framebuffer on the internal panel (software desktop at first), then the boot
runner takes WindowServer to the Intel GPU. **Standard** Intel framebuffer, read-only probe only.

## Rollback

1. Copy `kioxia-snapshot-20261104-1733/EFI/OC/config.plist` back over `EFI/OC/config.plist` (the kext folder can stay:
   without its `Kernel > Add` entry it is never loaded).
2. If macOS does not start at all: G-Helper → **Ultimate** (Intel drivers never start there); or at the OpenCore
   picker add boot-args `-xegfxoff -xegfxnoauto -xeintelfboff`.
3. macOS side: `sudo bash /Library/XeGfx/gu605/gu605-uninstall.sh` (moves everything aside, rebuilds the aux
   collection with NullMoth's kexts only).

## The macOS side (not on the ESP)

`gu605-<sha>.zip` → `sudo bash scripts/gu605-install.sh` (see docs/GU605-KURULUM.md and docs/GU605-TEST.md in the
release).

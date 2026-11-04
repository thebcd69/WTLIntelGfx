/* Copied from samwu07/xegfx-thinkbook14plus-mtl (BSD-2-Clause, Copyright (c) 2026 samwu07), GU605 changes marked. */
/*
 * XgFbMove.kext -- moves the boot framebuffer's GGTT mapping out of i915's GuC area, from inside macOS, on demand.
 *
 * Replaces FbRelocate.efi, whose resident UEFI image left page 0x140005000 referenced after macOS took over
 * ("vm_page_validate_no_references", reproduced in 2 minutes with ramfill on 2026-11-02). Same relocation code
 * (../fbreloc/fbreloc_core.h, tested by test_fbreloc.py), but nothing happens at boot: no IOKit personality, so it
 * runs only when loaded by hand, once, before XeGfx:
 *   sudo kmutil load -b org.local.XgFbMove      (run-xegfx-stages.sh does this before loading XeGfx)
 * The result goes to the kernel log ("XgFbMove: ...") and to the property "xgfbmove-result" on the iGPU's IOPCIDevice.
 * It refuses once XeGfx is loaded (the XeGfxArm resource exists): i915 must never see the surface move under it.
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>
#include <libkern/libkern.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "fbreloc_core.h"
#include <pexpert/pexpert.h>

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (as upstream's single-file kexts do)
extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.local.XgFbMove, "1.2.1", _start, _stop) }

// GU605 branch: the ROG Zephyrus G16 GU605MY iGPU only (samwu07 listed every Meteor/Arrow Lake ID)
static const UInt16 kMtlIds[] = { 0x7d55 };

static IOPCIDevice *find_igpu(void)
{
	OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
	OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
	IOPCIDevice *found = nullptr;

	if (match)
		match->release();
	if (!it)
		return nullptr;
	while (OSObject *o = it->getNextObject()) {
		IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
		if (!d || d->getBusNumber() != 0 || d->getDeviceNumber() != 2 || d->getFunctionNumber() != 0)
			continue;
		if (d->configRead16(kIOPCIConfigVendorID) != 0x8086)
			continue;
		UInt16 id = d->configRead16(kIOPCIConfigDeviceID);
		for (unsigned i = 0; i < sizeof(kMtlIds) / sizeof(kMtlIds[0]); i++)
			if (kMtlIds[i] == id)
				found = d;
		if (found)
			break;
	}
	if (found)
		found->retain();
	it->release();
	return found;
}

static void publish(IOPCIDevice *gpu, const char *s)
{
	IOLog("XgFbMove: %s\n", s);
	if (gpu)
		gpu->setProperty("xgfbmove-result", s);
}

extern "C" kern_return_t _start(kmod_info_t *, void *)
{
	char buf[320];
	struct fbr_result r;
	IOPCIDevice *gpu = find_igpu();
	IOService *res = IOService::getResourceService();
	IOMemoryMap *mm;

	if (PE_parse_boot_argn("-xgfbmoveoff", nullptr, 0)) {
		IOLog("XgFbMove: off by boot-arg -xgfbmoveoff\n");
		if (gpu)
			gpu->release();
		return KERN_SUCCESS;
	}
	if (!gpu) {
		IOLog("XgFbMove: no Meteor/Arrow Lake iGPU at 00:02.0, nothing done\n");
		return KERN_SUCCESS;
	}
	if (res && res->getProperty("XeGfxArm")) {
		publish(gpu, "XgFbMove 1.2.1 refused: XeGfx is already loaded in this boot; restart and move before the stages");
		gpu->release();
		return KERN_SUCCESS;
	}
	if (!(gpu->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace)) {
		publish(gpu, "XgFbMove 1.2.1 refused: the iGPU's memory space is off");
		gpu->release();
		return KERN_SUCCESS;
	}
	mm = gpu->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
	if (!mm || mm->getLength() < (16ull << 20)) {
		publish(gpu, "XgFbMove 1.2.1 refused: BAR0 (registers + GGTT, 16 MiB) could not be mapped");
		if (mm)
			mm->release();
		gpu->release();
		return KERN_SUCCESS;
	}
	// XeGfx is not armed (checked above), so no GPU driver has used the GGTT in this boot: a target holding an earlier
	// boot's entries may be taken (FBR_RECLAIM in fbreloc_core.h)
	{
		/* boot-arg xgfbmove=2: samwu07's rule, move whatever ends above GUC_GGTT_TOP */
		int force = 0;
		PE_parse_boot_argn("xgfbmove", &force, sizeof(force));
		fbr_run((volatile u8 *)mm->getVirtualAddress(), FBR_APPLY | FBR_RECLAIM | (force == 2 ? FBR_FORCE : 0), &r);
	}
	// "valid/same": per target before the move, valid entries / entries equal to the framebuffer's
	snprintf(buf, sizeof(buf), "XgFbMove 1.2.1 status %d (1 moved, 0 not needed, <0 refused) surf 0x%x -> 0x%x size 0x%llx pages %u"
	         "%s; targets valid/same 0x%x %u/%u, 0x%x %u/%u, 0x%x %u/%u",
	         r.status, r.old_surf, r.new_surf, r.size, r.pages, r.reclaimed ? " (reclaimed: target held old entries)" : "",
	         fbr_targets[0], r.tvalid[0], r.tsame[0], fbr_targets[1], r.tvalid[1], r.tsame[1],
	         fbr_targets[2], r.tvalid[2], r.tsame[2]);
	publish(gpu, buf);
	mm->release();
	gpu->release();
	return KERN_SUCCESS;
}

extern "C" kern_return_t _stop(kmod_info_t *, void *)
{
	return KERN_SUCCESS;
}

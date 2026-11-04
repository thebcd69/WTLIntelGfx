"""Run fbreloc_core.h (compiled into out/fbr_test.dll by build.sh, same compiler and flags as the driver) against a fake
16 MiB BAR0: display registers + GGTT at +8 MiB. The fake panel latches PLANE_SURF into PLANE_SURFLIVE at each 'vblank'.
Values for the real machine come from XeGfx's stage 2 probe on 2026-11-01 (WTLIntelGfx issue #13)."""
import ctypes, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
import os
dll = ctypes.CDLL(str(HERE / 'out' / ('fbr_test.dll' if os.name == 'nt' else 'fbr_test.so')))

class Res(ctypes.Structure):
    _fields_ = [('status', ctypes.c_int), ('old_surf', ctypes.c_uint32), ('new_surf', ctypes.c_uint32),
                ('pages', ctypes.c_uint32), ('size', ctypes.c_uint64), ('reclaimed', ctypes.c_uint32),
                ('tvalid', ctypes.c_uint32 * 3), ('tsame', ctypes.c_uint32 * 3)]

APPLY, RECLAIM = 1, 2

TRANSCONF, FRMCOUNT, PCTL, PSTRIDE, PSIZE, PSURF, PSURFLIVE, FLSH = (0x70008, 0x70040, 0x70180, 0x70188, 0x70190,
                                                                     0x7019c, 0x701ac, 0x101008)
GSM = 0x800000
HOOK = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class Bar:
    def __init__(self, surf=0xfe800000, stride=192, size_reg=0x077f0bff, pipe=True, plane=True, latch=True,
                 pte0=0x800003, fill=None):
        self.buf = ctypes.create_string_buffer(16 << 20)
        self.latch = latch
        self.w32(TRANSCONF, 0x80000000 if pipe else 0)
        self.w32(PCTL, 0x84000008 if plane else 0)
        self.w32(PSTRIDE, stride)
        self.w32(PSIZE, size_reg)
        self.w32(PSURF, surf)
        self.w32(PSURFLIVE, surf)
        if fill is not None:                      # every GGTT entry = one scratch value
            struct.pack_into('<%dQ' % (1 << 20), self.buf, GSM, *([fill] * (1 << 20)))
        pages = (stride * 64 * (((size_reg >> 16) & 0xffff) + 1) + 0xfff) >> 12
        for i in range(pages):
            self.wpte((surf >> 12) + i, pte0 + (i << 12))
        self.pages = pages
        self.hook = HOOK(self.vblank)

    def r32(self, off): return struct.unpack_from('<I', self.buf, off)[0]
    def w32(self, off, v): struct.pack_into('<I', self.buf, off, v & 0xffffffff)
    def pte(self, i): return struct.unpack_from('<Q', self.buf, GSM + 8 * i)[0]
    def wpte(self, i, v): struct.pack_into('<Q', self.buf, GSM + 8 * i, v)

    def vblank(self, _bar):
        self.w32(FRMCOUNT, self.r32(FRMCOUNT) + 1)
        if self.latch:
            self.w32(PSURFLIVE, self.r32(PSURF) & ~0xfff)

    def run(self, apply=1):
        dll.fbr_test_set_hook(self.hook)
        r = Res()
        dll.fbr_test_run(ctypes.cast(self.buf, ctypes.c_void_p), apply, ctypes.byref(r))
        return r


fails = 0
def check(name, cond, detail=''):
    global fails
    print(('ok   ' if cond else 'FAIL ') + name + (('  ' + detail) if detail and not cond else ''))
    fails += 0 if cond else 1


# 1. the real 07Y state: 3072x1920 at 0xFE800000 crosses GUC_GGTT_TOP -> moved to 0x80000000
b = Bar(); before = [b.pte((0xfe800000 >> 12) + i) for i in range(b.pages)]
r = b.run()
check('real 155H: moved', r.status == 1 and r.new_surf == 0x80000000, f'status {r.status} new {r.new_surf:#x}')
check('real 155H: size 22.5 MiB, 5760 pages', r.size == 0x1680000 and r.pages == 5760, f'{r.size:#x} {r.pages}')
check('real 155H: all PTEs copied', all(b.pte((0x80000000 >> 12) + i) == before[i] for i in range(b.pages)))
check('real 155H: entry after the copy untouched', b.pte((0x80000000 >> 12) + b.pages) == 0)
check('real 155H: old PTEs left alone', all(b.pte((0xfe800000 >> 12) + i) == before[i] for i in range(b.pages)))
check('real 155H: PLANE_SURF and SURFLIVE point at 0x80000000',
      b.r32(PSURF) == 0x80000000 and b.r32(PSURFLIVE) == 0x80000000, f'{b.r32(PSURF):#x} {b.r32(PSURFLIVE):#x}')
check('real 155H: GGTT flush written', b.r32(FLSH) == 1)

# 2. low bits of PLANE_SURF (flags) survive the move
b = Bar(surf=0xfe800000 | 0x8); r = b.run()
check('flag bits kept', r.status == 1 and b.r32(PSURF) == 0x80000008, f'{b.r32(PSURF):#x}')

# 3. dry run (driver load): verdict only, nothing written
b = Bar(); r = b.run(apply=0)
check('dry run: says it would move', r.status == 1 and r.new_surf == 0x80000000)
check('dry run: nothing written', b.r32(PSURF) == 0xfe800000 and b.pte(0x80000000 >> 12) == 0 and b.r32(FLSH) == 0)

# 4. surface already below GUC_GGTT_TOP: nothing to do
b = Bar(surf=0x10000000); r = b.run()
check('below GuC top: not needed', r.status == 0 and b.r32(PSURF) == 0x10000000 and b.pte(0x80000000 >> 12) == 0)

# 5. surface fully above the GuC slot (2560x1600 at 0xFF000000, the GU605's panel size): left in place on the GU605
#    branch (WTL's Legion runs it unmoved, N-256); FBR_FORCE (boot-arg xgfbmove=2) keeps samwu07's rule and moves it
b = Bar(surf=0xff000000, stride=160, size_reg=(1599 << 16) | 2559); r = b.run()
check('above the GuC slot: not needed', r.status == 0 and b.r32(PSURF) == 0xff000000, f'status {r.status}')
b = Bar(surf=0xff000000, stride=160, size_reg=(1599 << 16) | 2559); r = b.run(apply=1 | 4)
check('above the GuC slot, forced: moved', r.status == 1 and r.new_surf == 0x80000000, f'status {r.status}')
# 5b. a 2560x1600 surface that starts inside the slot (0xFEF00000): moved
b = Bar(surf=0xfef00000, stride=160, size_reg=(1599 << 16) | 2559); r = b.run()
check('inside the GuC slot: moved', r.status == 1 and r.new_surf == 0x80000000, f'status {r.status}')

# 6. pipe or plane off: untouched
check('pipe off', Bar(pipe=False).run().status == -1)
check('plane off', Bar(plane=False).run().status == -2)

# 7. an invalid source entry: refuse
b = Bar(); b.wpte((0xfe800000 >> 12) + 100, 0); r = b.run()
check('invalid source PTE: refused', r.status == -3 and b.r32(PSURF) == 0xfe800000)

# 8. stride 0 / absurd size: refuse
check('stride 0: refused', Bar(stride=0).run().status == -4)

# 9. 0x80000000 busy (distinct valid entries) -> next candidate 0x40000000
b = Bar()
for i in range(0, b.pages, 7):
    b.wpte((0x80000000 >> 12) + i, 0x5000003 + (i << 12))
r = b.run()
check('busy first target: falls back to 0x40000000', r.status == 1 and r.new_surf == 0x40000000, f'{r.new_surf:#x}')

# 10. all three targets busy -> refuse, nothing written
b = Bar()
for t in (0x80000000, 0x40000000, 0xc0000000):
    b.wpte((t >> 12) + 5, 0x7000003); b.wpte((t >> 12) + 6, 0x7001003)
r = b.run()
check('no free target: refused', r.status == -5 and b.r32(PSURF) == 0xfe800000)

# 11. firmware filled the whole GGTT with one scratch entry -> that counts as free
b = Bar(fill=0x9000003); r = b.run()
check('scratch-filled GGTT: moved', r.status == 1 and b.pte(0x80000000 >> 12) == 0x800003, f'status {r.status}')
# ...unless that shared entry is one of the framebuffer's own pages (0x800000 + 22.5 MiB covers 0x1e00000)
b = Bar(fill=0x1e00003); r = b.run()
check('shared entry is a framebuffer page: refused', r.status == -5 and b.r32(PSURF) == 0xfe800000)

# 11b. a warm restart left last boot's copy at 0x80000000 (same entries): reuse it instead of moving on
b = Bar()
for i in range(b.pages):
    b.wpte((0x80000000 >> 12) + i, b.pte((0xfe800000 >> 12) + i))
r = b.run()
check('own copy from an earlier boot: reused', r.status == 1 and r.new_surf == 0x80000000, f'{r.new_surf:#x}')
# ...but a copy that differs in one entry (some other framebuffer) is not ours: next target
b = Bar()
for i in range(b.pages):
    b.wpte((0x80000000 >> 12) + i, b.pte((0xfe800000 >> 12) + i))
b.wpte((0x80000000 >> 12) + 77, 0x9900003)
r = b.run()
check('copy that differs in one entry: not reused', r.status == 1 and r.new_surf == 0x40000000, f'{r.new_surf:#x}')
# four warm boots in a row: every boot lands on the same 0x80000000
b = Bar()
for boot in range(4):
    b.w32(PSURF, 0xfe800000); b.w32(PSURFLIVE, 0xfe800000)    # firmware puts the panel back at its own spot
    r = b.run()
check('four warm boots: always 0x80000000', r.status == 1 and r.new_surf == 0x80000000, f'{r.status} {r.new_surf:#x}')

# 12. the panel never takes the new surface -> old surface restored
b = Bar(latch=False); r = b.run()
check('no latch: restored', r.status == -6 and b.r32(PSURF) == 0xfe800000 and r.new_surf == 0,
      f'status {r.status} surf {b.r32(PSURF):#x}')

# 13. XgFbMove 1.2 (APPLY|RECLAIM). The 2026-11-02 15:09 state: every target full of another session's entries.
def foreign(b, targets=(0x80000000, 0x40000000, 0xc0000000)):
    for t in targets:
        for i in range(b.pages):
            b.wpte((t >> 12) + i, 0x30000003 + (i << 12) + (t >> 20))
b = Bar(); foreign(b); before = [b.pte((0xfe800000 >> 12) + i) for i in range(b.pages)]
r = b.run(APPLY | RECLAIM)
check('reclaim: all targets foreign -> 0x80000000', r.status == 1 and r.new_surf == 0x80000000 and r.reclaimed == 1,
      f'status {r.status} new {r.new_surf:#x} reclaimed {r.reclaimed}')
check('reclaim: framebuffer entries copied over the old ones',
      all(b.pte((0x80000000 >> 12) + i) == before[i] for i in range(b.pages)))
check('reclaim: panel follows', b.r32(PSURF) == 0x80000000 and b.r32(PSURFLIVE) == 0x80000000)
check('reclaim: census before the move', list(r.tvalid) == [5760] * 3 and list(r.tsame) == [0] * 3,
      f'valid {list(r.tvalid)} same {list(r.tsame)}')
check('reclaim: other targets untouched', b.pte(0x40000000 >> 12) == 0x30000003 + (0x40000000 >> 20))
# the same state without RECLAIM (the old UEFI driver's mode) still refuses
b = Bar(); foreign(b); r = b.run(APPLY)
check('no reclaim flag: still refused', r.status == -5 and r.reclaimed == 0 and b.r32(PSURF) == 0xfe800000)
# a free target still wins over reclaiming
b = Bar(); foreign(b, (0x80000000, 0xc0000000)); r = b.run(APPLY | RECLAIM)
check('reclaim: a free target is preferred', r.status == 1 and r.new_surf == 0x40000000 and r.reclaimed == 0,
      f'new {r.new_surf:#x} reclaimed {r.reclaimed}')
# last boot's exact copy still counts as free (not reclaimed), and the census shows it
b = Bar(); foreign(b)
for i in range(b.pages):
    b.wpte((0x80000000 >> 12) + i, b.pte((0xfe800000 >> 12) + i))
r = b.run(APPLY | RECLAIM)
check('reclaim: own copy reused, not reclaimed', r.status == 1 and r.new_surf == 0x80000000 and r.reclaimed == 0
      and r.tsame[0] == 5760, f'reclaimed {r.reclaimed} same {list(r.tsame)}')
# never a target another pipe scans out: pipe B's plane 1 shows 0x80000000 -> 0x40000000
b = Bar(); foreign(b)
b.w32(TRANSCONF + 0x1000, 0x80000000); b.w32(PCTL + 0x1000, 0x84000008)
b.w32(PSTRIDE + 0x1000, 120); b.w32(PSIZE + 0x1000, (1079 << 16) | 1919); b.w32(PSURF + 0x1000, 0x80100000)
r = b.run(APPLY | RECLAIM)
check('reclaim: skips a target pipe B scans', r.status == 1 and r.new_surf == 0x40000000 and r.reclaimed == 1,
      f'new {r.new_surf:#x}')
# ...a disabled pipe B (plane still pointing there) does not count
b = Bar(); foreign(b)
b.w32(PCTL + 0x1000, 0x84000008); b.w32(PSTRIDE + 0x1000, 120); b.w32(PSIZE + 0x1000, (1079 << 16) | 1919)
b.w32(PSURF + 0x1000, 0x80100000); r = b.run(APPLY | RECLAIM)
check('reclaim: pipe B off is ignored', r.status == 1 and r.new_surf == 0x80000000, f'new {r.new_surf:#x}')
# dry run with RECLAIM: verdict only
b = Bar(); foreign(b); r = b.run(RECLAIM)
check('reclaim dry run: nothing written', r.status == 1 and r.reclaimed == 1 and b.r32(PSURF) == 0xfe800000
      and b.pte(0x80000000 >> 12) == 0x30000003 + (0x80000000 >> 20) and b.r32(FLSH) == 0)

print('\n%d failure(s)' % fails)
sys.exit(1 if fails else 0)

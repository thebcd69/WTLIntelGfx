/* Host test build of fbreloc_core.h: the same code as the driver, run against a fake BAR0 from Python (ctypes). */
#include "fbreloc_core.h"

__declspec(dllexport) int fbr_test_run(volatile u8 *bar, int apply, struct fbr_result *r) { return fbr_run(bar, apply, r); }
__declspec(dllexport) void fbr_test_set_hook(void (*hook)(volatile u8 *bar)) { fbr_poll_hook = hook; }

int _DllMainCRTStartup(void *h, unsigned reason, void *reserved) { (void)h; (void)reason; (void)reserved; return 1; }

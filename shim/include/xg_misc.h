/* XeGfx: the misc area's narrow bridge between the FreeBSD side (xg_misc.c) and the XNU side (xg_misc.cpp), plus
 * the hooks other areas use. Plain C types only, so it can be included on either side.
 * <wtl> project: xegfx | author: watchthelight </wtl> */
#ifndef _XG_MISC_H_
#define _XG_MISC_H_
#ifdef __cplusplus
extern "C" {
#endif

/* one-time setup (locks, the static sysctl set, tunables); the kext start calls it after xg_kern_init(), and every
 * misc entry point that needs it calls it lazily too */
void xg_misc_init(void);

/* fetches every static tunable again from the environment as it is now (debug.xegfx_env, boot-args); stage 4 of the
 * GU605 display path calls it after changing the environment. Returns how many tunables it looked at */
int xg_sysctl_reload_tunables(void);

/* MOD_QUIESCE then MOD_UNLOAD to every module registered through module_register_init, newest first; returns the
 * first refusal (the remaining modules stay loaded) */
int xg_misc_module_unload_all(void);

/* XNU side (xg_misc.cpp): read a file from the kext's Contents/Resources through OSKextRequestResource and wait for
 * it. On success *datap is an IOMalloc() copy of *lenp bytes (free it with IOFree). Returns 0 or an errno value.
 * Sleeps: never call it with a spin lock held, or from the kext start routine (kernelmanagerd answers only after
 * the start routine has returned). */
int xg_misc_fw_request(const char *name, void **datap, unsigned long *lenp, unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif

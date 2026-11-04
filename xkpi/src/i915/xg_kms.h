/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * XeGfx, GU605 branch: the KMS bridge's interface (xg_kms.c), shared with the kext's C++ side (xg_kext.cpp keeps a
 * copy of these declarations in its extern "C" block; keep them in step).
 */
#ifndef XG_KMS_H
#define XG_KMS_H

struct xg_kms_mode {
	unsigned int w, h, clock_khz, htotal, hss, hse, vtotal, vss, vse, flags, mhz, preferred;
};

int xg_kms_dump(char *buf, unsigned long len);
int xg_kms_set(const char *name, unsigned int w, unsigned int h, unsigned int mhz, int test, char *msg, unsigned long msglen);
int xg_kms_backlight(int permille, char *msg, unsigned long msglen);
int xg_kms_modes(const char *name, struct xg_kms_mode *out, int max, int *connected, struct xg_kms_mode *cur);
int xg_kms_client_init(void);
extern void (*xg_kms_notify)(void);
extern unsigned int xg_kms_hotplugs;

#endif

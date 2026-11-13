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
int xg_kms_set_scaled(const char *name, unsigned int w, unsigned int h, unsigned int mhz, unsigned int sw, unsigned int sh,
    int test, char *msg, unsigned long msglen);
int xg_kms_big(unsigned long long *phys, unsigned long long *size, unsigned int *maxw, unsigned int *maxh);
int xg_kms_backlight(int permille, char *msg, unsigned long msglen);
int xg_kms_modes(const char *name, struct xg_kms_mode *out, int max, int *connected, struct xg_kms_mode *cur);
int xg_kms_client_init(void);
int xg_kms_edid(const char *name, unsigned char *buf, int len);

/* DDC/CI and EDID over the connector's own DDC bus (gu605-ddc, docs/GU605-DDC.md). One I2C transfer: an optional
 * write of wlen bytes then an optional read of rlen bytes (a repeated start between them). Only 7-bit 0x37 (DDC/CI)
 * may be written and read; 0x50 (EDID) only read, after at most a one-byte offset write. rdone gets the bytes read. */
#define XG_KMS_I2C_WMAX 64
#define XG_KMS_I2C_RMAX 256
struct xg_kms_i2c {
	unsigned int addr, wlen, rlen;
	unsigned char *wbuf, *rbuf;
	int rdone;
};
int xg_kms_i2c(const char *name, struct xg_kms_i2c *x);
extern void (*xg_kms_notify)(void);
extern unsigned int xg_kms_hotplugs;

#endif

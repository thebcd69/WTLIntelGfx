// SPDX-License-Identifier: BSD-2-Clause
// XeGfxIntelFramebuffer, gu605-ddc (docs/GU605-DDC.md), included inside the class body after kms.inc.
// DDC/CI for the external monitor through the IOFramebuffer I2C interface, so the tools that set a monitor's own
// brightness on Intel Macs (ddcctl, MonitorControl, BetterDisplay's DDC mode, ghelper-mac) work here:
//  * start() publishes one I2C bus (IOFBI2CInterfaceInfo; IOFramebuffer::open builds the IOI2CInterface nub from it
//    when WindowServer opens the framebuffer);
//  * doI2CRequest() hands each part of a request to i915 (xgKms op 6 -> xg_kms_i2c: connector->ddc, i.e. DP AUX
//    I2C-over-AUX for DP / USB-C, GMBUS for HDMI) for the connector that has the picture, and only while that is an
//    external monitor (fExternal): the built-in panel has no DDC/CI, its brightness is the backlight handler.
// Safety (checked here and again in xg_kms_i2c): writes only to 7-bit 0x37, reads only from 0x37 or 0x50 (EDID, at
// most a one-byte offset written first); a send is at most 64 bytes, a reply at most 256; the reply delay at most
// 500 ms; any failure answers the request with an error and moves no data. Boot-arg -xeintelfbnoddc: no bus.
// Status without sudo: ioreg -r -w0 -c XeGfxIntelFramebuffer -d1 | grep XeGfxDdc
// Written against IOFramebuffer's and IOI2CInterface's public interfaces (NullMoth's NVRMFB DDC branch, PolyForm-NC,
// was a design reference, none of its code is here).
    bool fDdcBus = false;
    unsigned fDdcCalls = 0, fDdcOk = 0, fDdcErr = 0;
    struct XifbI2c { uint32_t addr, wlen, rlen; uint8_t *wbuf, *rbuf; int rdone; };   // struct xg_kms_i2c

    // before IONDRVFramebuffer::start: only on the GU605 display path
    void ddcPublish() {
        int off = 0;
        if (!fGu605) return;
        if (PE_parse_boot_argn("-xeintelfbnoddc", &off, sizeof off) || PE_parse_boot_argn("-xeintelfbnokms", &off, sizeof off)) {
            setProperty("XeGfxDdc", "off (boot-arg)"); return;
        }
        OSDictionary *bus = OSDictionary::withCapacity(4); OSArray *arr = OSArray::withCapacity(1);
        if (bus && arr) {
            const struct { const char *k; unsigned long long v; } kv[] = {
                { kIOI2CInterfaceIDKey, 0 }, { kIOI2CBusTypeKey, kIOI2CBusTypeI2C },
                { kIOI2CTransactionTypesKey, (1u << kIOI2CNoTransactionType) | (1u << kIOI2CSimpleTransactionType) |
                                             (1u << kIOI2CDDCciReplyTransactionType) | (1u << kIOI2CCombinedTransactionType) },
                { kIOI2CSupportedCommFlagsKey, kIOI2CUseSubAddressCommFlag } };
            for (const auto &e : kv)
                if (OSNumber *n = OSNumber::withNumber(e.v, 32)) { bus->setObject(e.k, n); n->release(); }
            arr->setObject(bus);
            fDdcBus = setProperty(kIOFBI2CInterfaceInfoKey, arr);
        }
        OSSafeReleaseNULL(bus); OSSafeReleaseNULL(arr);
        ddcStatus(fDdcBus ? "bus published; active while an external monitor has the picture" : "bus not published (no memory)");
    }
    void ddcStatus(const char *s) {
        char line[200];
        snprintf(line, sizeof line, "%s | n %u ok %u err %u", s, fDdcCalls, fDdcOk, fDdcErr);
        setProperty("XeGfxDdc", line);
    }
    static IOReturn ddcErr(int e) {
        return e == -ENODEV || e == -ENOENT ? kIOReturnNoDevice : e == -EPERM ? kIOReturnNotPermitted :
               e == -EINVAL ? kIOReturnBadArgument : e == -ETIMEDOUT ? kIOReturnTimeout : kIOReturnIOError;
    }
    // one transfer on the connector that has the picture; NoDevice unless that is an external monitor
    IOReturn ddcXfer(uint8_t addr, uint8_t *w, uint32_t wl, uint8_t *r, uint32_t rl, uint32_t *got, int *rc) {
        char name[sizeof fConnName]; bool ext = false;
        *got = 0; *rc = 0;
        if (fKmsLock) { IOLockLock(fKmsLock); ext = fKmsUp && fExternal; strlcpy(name, fConnName, sizeof name); IOLockUnlock(fKmsLock); }
        if (!ext) return kIOReturnNoDevice;
        XifbI2c x = { addr, wl, rl, w, r, 0 };
        int res = -EIO;
        if (!xifbKmsCall(6, (void *)name, &x, &res) || res) { *rc = res; return ddcErr(res); }
        *got = x.rdone > 0 && (uint32_t)x.rdone <= rl ? (uint32_t)x.rdone : 0;
        return kIOReturnSuccess;
    }
    IOReturn doI2CRequest(UInt32 bus, IOI2CBusTiming *timing, IOI2CRequest *req) override {
        if (!fDdcBus) return IONDRVFramebuffer::doI2CRequest(bus, timing, req);
        if (!req) return kIOReturnBadArgument;
        IOReturn err = kIOReturnSuccess; const char *stage = "start"; int rc = 0;
        const uint8_t sendA = (uint8_t)(req->sendAddress >> 1), replyA = (uint8_t)(req->replyAddress >> 1);
        const bool sub = (req->commFlags & kIOI2CUseSubAddressCommFlag) != 0;
        const bool doSend = req->sendTransactionType != kIOI2CNoTransactionType;
        const bool doReply = req->replyTransactionType != kIOI2CNoTransactionType;
        uint32_t sent = 0, got = 0;
        fDdcCalls++;
        do {
            if (bus != 0) { err = kIOReturnNoDevice; stage = "bus"; break; }
            if ((doSend && (req->sendTransactionType > kIOI2CCombinedTransactionType || (req->sendAddress & 1))) ||
                (doReply && req->replyTransactionType > kIOI2CCombinedTransactionType) || (!doSend && !doReply))
                { err = kIOReturnUnsupportedMode; stage = "type"; break; }
            if ((doSend && sendA != 0x37) || (doReply && replyA != 0x37 && replyA != 0x50)) { err = kIOReturnNotPermitted; stage = "address"; break; }
            if ((doSend && (!req->sendBuffer || !req->sendBytes || req->sendBytes + (sub ? 1 : 0) > 64)) ||
                (doReply && (!req->replyBuffer || !req->replyBytes || req->replyBytes > 256)))
                { err = kIOReturnBadArgument; stage = "buffer"; break; }
            if (doSend) {
                uint8_t tmp[64]; uint32_t n = 0, d = 0;
                if (sub) tmp[n++] = req->sendSubAddress;
                bcopy((const void *)req->sendBuffer, tmp + n, req->sendBytes); n += req->sendBytes;
                if ((err = ddcXfer(sendA, tmp, n, nullptr, 0, &d, &rc))) { stage = "send"; break; }
                sent = req->sendBytes;
            }
            if (!doReply) break;
            uint8_t sa = req->replySubAddress;
            if (doSend) {
                // DDC/CI: >= 40 ms between a request and reading its reply; a longer minReplyDelay up to 500 ms
                uint64_t ns = 0; absolutetime_to_nanoseconds(req->minReplyDelay, &ns);
                uint32_t ms = (uint32_t)(ns / 1000000ull);
                if (ms < 40 && replyA == 0x37) ms = 40;
                if (ms > 500) ms = 500;
                if (ms) IOSleep(ms);
            }
            // a read at a subaddress (EDID offset) is one write+read transfer with a repeated start
            err = ddcXfer(replyA, (sub && !doSend) ? &sa : nullptr, (sub && !doSend) ? 1 : 0, (uint8_t *)req->replyBuffer,
                          req->replyBytes, &got, &rc);
            if (err) { stage = "reply"; got = 0; break; }
            if (req->replyTransactionType == kIOI2CDDCciReplyTransactionType && got >= 2) {
                const uint8_t *r = (const uint8_t *)req->replyBuffer;
                const uint32_t len = (uint32_t)(r[1] & 0x7f) + 3;   // source address, length byte, data, checksum
                if ((r[1] & 0x80) && len < got) got = len;
            }
        } while (false);
        req->sendBytes = sent;
        req->replyBytes = err ? 0 : got;
        req->result = err;
        if (!err) { if (fDdcOk++ == 0) ddcStatus("first transfer ok"); }
        else {
            fDdcErr++;
            if (fDdcErr <= 8 || !(fDdcErr & 0x3f)) {
                char s[120]; snprintf(s, sizeof s, "error 0x%x at %s (i915 %d) on %s", err, stage, rc, fConnName); ddcStatus(s);
                XIFBLOG("ddc: %s", s);
            }
        }
        if (req->completion) (*req->completion)(req);
        return kIOReturnSuccess;
    }

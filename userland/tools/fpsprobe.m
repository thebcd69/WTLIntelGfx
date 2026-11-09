// fpsprobe: a WindowServer-shaped frame loop on one Metal device, measuring where each frame's time goes: commit ->
// the plugin's first vkQueueSubmit (kernelStartTime) -> the GPU starts (GPUStartTime) -> the GPU ends (GPUEndTime) ->
// the completion handler. GU605 branch of XeGfx, for the ~55 frames/s Intel desktop on a 240 Hz panel (2026-11-09:
// WindowServer's commit->GPU 13-26 ms against 4-11 ms of GPU work).
//   fpsprobe [SECONDS]                       the system's Intel Metal device (only if this process is allowed one)
//   NM_TEST_PLUGIN=<XeMTLDriver binary> fpsprobe [SECONDS]   that plugin in this process, on NMXAccel (as blurprobe)
// What one frame holds (what WindowServer's log showed it composites with):
//   - three 2560x1600 BGRA IOSurfaces, one per frame in turn (the swap chain)
//   - a 3840x2160 RGBA16Unorm "wallpaper" (66 MB) over READ-ONLY caller memory: a file mapped PROT_READ, wrapped by
//     newBufferWithBytesNoCopy (the plugin cannot import read-only pages and copies them: "host-pointer import refused
//     ... not all writable memory; that buffer is COPIED - resynced at every submission")
//   - a 16 KB read-only NoCopy buffer bound directly to every window draw (CA's constant data; a host copy that holds the
//     command buffer out of the plugin's submit-ahead)
//   - one Load pass: the wallpaper, then 60 window quads; the backdrop chain: a blit of a 1280x800 region into a 4-level
//     private texture, levels 1-3 each rendered from the one above, then a Load pass that saturates level 3 back over a
//     sidebar; a 256x64 replaceRegion upload every frame (a glyph strip)
// Two loops of SECONDS each (default 5): "paced" commits at 240 Hz with at most 2 frames in flight (Core Animation's
// shape), "burst" as fast as it can with 3 in flight. Prints, per loop:
//   FPS <loop>: <frames/s>; commit->submit / submit->GPU / GPU / end->done / commit->done: avg p95 max (ms)
// and a verdict line against the targets: paced >= 228 frames/s (95% of 240) and commit->GPU start under 2 ms on average.
// With NVMTL_XE_FRAME_DIAG=1 the plugin also writes its WindowServer summaries ("frames (5 s)", "submit (5 s)") to its log.
// Exit 0 when the verdict passes. Build (Command Line Tools only):
//   clang -fobjc-arc -O1 -arch x86_64 -mmacosx-version-min=15.0 -framework Foundation -framework Metal \
//     -framework QuartzCore -framework IOKit -framework IOSurface fpsprobe.m -o fpsprobe
// <wtl> project: xegfx (GU605 branch) | BSD-2-Clause </wtl>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

enum { SW = 2560, SH = 1600, WPW = 3840, WPH = 2160, NWIN = 60, MAXF = 8192 };
static id<MTLDevice> gDev;
static id<MTLCommandQueue> gQ;
static id<MTLLibrary> gLib;

static NSString *const kShaders =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 p [[position]]; float2 uv; };\n"
     "vertex V quad(uint i [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
     "  float2 q = float2((i << 1) & 2, i & 2) * 0.5; float2 c = r.xy + q * r.zw;\n"
     "  V v; v.p = float4(c * 2 - 1, 0, 1); v.uv = float2(q.x, 1 - q.y) * 2; return v; }\n"
     "fragment float4 wall(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge); return float4(t.sample(s, v.uv).rgb, 1); }\n"
     "fragment float4 win(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant float4 *k [[buffer(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::repeat); return t.sample(s, v.uv) * k[0] + k[1]; }\n"
     "fragment float4 lodlin(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant float &l [[buffer(0)]]) {\n"
     "  constexpr sampler s(filter::linear, mip_filter::nearest, address::clamp_to_edge); return t.sample(s, v.uv, level(l)); }\n"
     "fragment float4 satlod(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant float &l [[buffer(0)]]) {\n"
     "  constexpr sampler s(filter::linear, mip_filter::nearest, address::clamp_to_edge); float4 c = t.sample(s, v.uv, level(l));\n"
     "  float y = dot(c.rgb, float3(0.2126, 0.7152, 0.0722)); return float4(clamp(y + (c.rgb - y) * 2.0, 0.0, 1.0), 1.0); }\n";

static id<MTLRenderPipelineState> pso(NSString *frag, BOOL blend)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [gLib newFunctionWithName:@"quad"];
    d.fragmentFunction = [gLib newFunctionWithName:frag];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [gDev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) printf("  pipeline %s: %s\n", frag.UTF8String, e.localizedDescription.UTF8String);
    return p;
}

// a file of n bytes filled by fill(), mapped read-only: the plugin sees caller memory it cannot write
static void *ro_map(size_t n, void (^fill)(uint8_t *p, size_t n))
{
    char path[] = "/tmp/fpsprobe.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    if (ftruncate(fd, (off_t)n)) { close(fd); return NULL; }
    uint8_t *w = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED) { close(fd); return NULL; }
    fill(w, n);
    msync(w, n, MS_SYNC); munmap(w, n);
    void *r = mmap(NULL, n, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return r == MAP_FAILED ? NULL : r;
}

typedef struct { double commit, kstart, gstart, gend, done; } frame_t;
static frame_t gF[MAXF];
static _Atomic int gDone;

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void stat3(double *v, int n, double *avg, double *p95, double *mx)
{
    *avg = *p95 = *mx = 0;
    if (n <= 0) return;
    double s = 0; for (int i = 0; i < n; i++) s += v[i];
    qsort(v, (size_t)n, sizeof *v, cmpd);
    *avg = s / n; *p95 = v[(int)((n - 1) * 0.95)]; *mx = v[n - 1];
}

static double gPacedFps, gPacedQ;

static void run(const char *name, double seconds, int inflight, double hz,
                NSArray<id<MTLTexture>> *targets, id<MTLTexture> wall, id<MTLTexture> winTex, id<MTLBuffer> konst,
                id<MTLTexture> mips, id<MTLTexture> strip)
{
    id<MTLRenderPipelineState> pw = pso(@"wall", NO), pwin = pso(@"win", YES), pl = pso(@"lodlin", NO), ps = pso(@"satlod", NO);
    if (!pw || !pwin || !pl || !ps) { printf("FPS %s: no pipelines\n", name); return; }
    dispatch_semaphore_t sem = dispatch_semaphore_create(inflight);
    uint8_t *glyphs = malloc(256 * 64 * 4);
    atomic_store(&gDone, 0);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t period = hz > 0 ? (uint64_t)(1e9 / hz * tb.denom / tb.numer) : 0;
    const double t0 = CACurrentMediaTime();
    uint64_t next = mach_absolute_time();
    int n = 0;
    while (CACurrentMediaTime() - t0 < seconds && n < MAXF) {
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        if (period) { next += period; const uint64_t now = mach_absolute_time(); if (next > now) mach_wait_until(next); else next = now; }
        @autoreleasepool {
            const int f = n++;
            id<MTLTexture> tgt = targets[(NSUInteger)f % targets.count];
            for (int i = 0; i < 256 * 64; i++) ((uint32_t *)glyphs)[i] = 0xff000000u | (uint32_t)(i * 2654435761u + (unsigned)f);
            [strip replaceRegion:MTLRegionMake2D(0, 0, 256, 64) mipmapLevel:0 withBytes:glyphs bytesPerRow:256 * 4];
            id<MTLCommandBuffer> cb = [gQ commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = tgt; rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            const float full[4] = { 0, 0, 1, 1 };
            [e setRenderPipelineState:pw]; [e setVertexBytes:full length:sizeof full atIndex:0]; [e setFragmentTexture:wall atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e setRenderPipelineState:pwin]; [e setFragmentBuffer:konst offset:0 atIndex:0];
            for (int w = 0; w < NWIN; w++) {
                const float ph = (float)(f * 0.02 + w * 0.37);
                const float r[4] = { 0.1f + 0.35f * (1 + sinf(ph)) * 0.5f + (w % 6) * 0.06f, 0.1f + 0.3f * (1 + cosf(ph * 1.3f)) * 0.5f + (w / 10) * 0.05f, 0.2f, 0.18f };
                [e setVertexBytes:r length:sizeof r atIndex:0];
                [e setFragmentTexture:(w % 5 ? winTex : strip) atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [e endEncoding];
            // the backdrop chain: region -> level 0, levels 1-3 each from the one above, level 3 saturated over a sidebar
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:tgt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(SW / 2, SH / 2, 1)
                     toTexture:mips destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            for (int l = 1; l < 4; l++) {
                MTLRenderPassDescriptor *mp = [MTLRenderPassDescriptor renderPassDescriptor];
                mp.colorAttachments[0].texture = mips; mp.colorAttachments[0].level = (NSUInteger)l;
                mp.colorAttachments[0].loadAction = MTLLoadActionDontCare; mp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> me = [cb renderCommandEncoderWithDescriptor:mp];
                const float lv = (float)(l - 1);
                [me setRenderPipelineState:pl]; [me setVertexBytes:full length:sizeof full atIndex:0];
                [me setFragmentTexture:mips atIndex:0]; [me setFragmentBytes:&lv length:sizeof lv atIndex:0];
                [me drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [me endEncoding];
            }
            MTLRenderPassDescriptor *sp = [MTLRenderPassDescriptor renderPassDescriptor];
            sp.colorAttachments[0].texture = tgt; sp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            sp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> se = [cb renderCommandEncoderWithDescriptor:sp];
            const float side[4] = { 0, 0, 0.12f, 1 }, l3 = 3;
            [se setRenderPipelineState:ps]; [se setVertexBytes:side length:sizeof side atIndex:0];
            [se setFragmentTexture:mips atIndex:0]; [se setFragmentBytes:&l3 length:sizeof l3 atIndex:0];
            [se drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [se endEncoding];
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
                gF[f].done = CACurrentMediaTime(); gF[f].kstart = c.kernelStartTime; gF[f].gstart = c.GPUStartTime; gF[f].gend = c.GPUEndTime;
                atomic_fetch_add(&gDone, 1);
                dispatch_semaphore_signal(sem);
            }];
            gF[f].commit = CACurrentMediaTime();
            [cb commit];
        }
    }
    for (int i = 0; i < inflight; i++) dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    const double t1 = CACurrentMediaTime();
    free(glyphs);
    const int done = atomic_load(&gDone);
    double *q = calloc((size_t)n + 1, sizeof(double) * 5), *v[5];
    int k[5] = { 0 };
    for (int i = 0; i < 5; i++) v[i] = q + (size_t)i * ((size_t)n + 1);
    for (int i = 0; i < n; i++) {
        const frame_t *x = &gF[i];
        if (!x->done) continue;
        if (x->kstart > 0) v[0][k[0]++] = (x->kstart - x->commit) * 1e3;
        if (x->gstart > 0 && x->kstart > 0) v[1][k[1]++] = (x->gstart - x->kstart) * 1e3;
        if (x->gend > 0 && x->gstart > 0) v[2][k[2]++] = (x->gend - x->gstart) * 1e3;
        if (x->gend > 0) v[3][k[3]++] = (x->done - x->gend) * 1e3;
        v[4][k[4]++] = (x->done - x->commit) * 1e3;
    }
    double a[5], p[5], m[5];
    for (int i = 0; i < 5; i++) stat3(v[i], k[i], &a[i], &p[i], &m[i]);
    double cg = 0; int ncg = 0;
    for (int i = 0; i < n; i++) if (gF[i].done && gF[i].gstart > 0) { cg += (gF[i].gstart - gF[i].commit) * 1e3; ncg++; }
    const double fps = done / (t1 - t0);
    printf("FPS %s: %.1f frames/s (%d frames, %d in flight%s)\n", name, fps, done, inflight, hz > 0 ? ", paced at 240 Hz" : ", unpaced");
    printf("  commit->submit  avg %6.2f p95 %6.2f max %7.2f ms\n", a[0], p[0], m[0]);
    printf("  submit->GPU     avg %6.2f p95 %6.2f max %7.2f ms\n", a[1], p[1], m[1]);
    printf("  GPU             avg %6.2f p95 %6.2f max %7.2f ms\n", a[2], p[2], m[2]);
    printf("  GPU end->done   avg %6.2f p95 %6.2f max %7.2f ms\n", a[3], p[3], m[3]);
    printf("  commit->done    avg %6.2f p95 %6.2f max %7.2f ms\n", a[4], p[4], m[4]);
    printf("  commit->GPU start avg %.2f ms over %d timed frames\n", ncg ? cg / ncg : 0, ncg);
    if (hz > 0) { gPacedFps = fps; gPacedQ = ncg ? cg / ncg : 99; }
    free(q);
    memset(gF, 0, sizeof gF);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const double seconds = argc > 1 ? atof(argv[1]) : 5;
        const char *plugin = getenv("NM_TEST_PLUGIN");
        if (plugin) {
            if (!dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) { printf("dlopen: %s\n", dlerror()); return 3; }
            io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
            if (!svc) { printf("no NMXAccel service (stage 7 not run)\n"); return 2; }
            gDev = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc], NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
        } else {
            for (id<MTLDevice> d in MTLCopyAllDevices())
                if ([d.name rangeOfString:@"Intel" options:NSCaseInsensitiveSearch].location != NSNotFound) gDev = d;
        }
        if (!gDev) { printf("no Intel Metal device\n"); return 2; }
        printf("device %s (NVMTL_XE_FPS=%s RO_ONCE=%s HOSTCOPY_AHEAD=%s WORKER_AHEAD=%s EMPTY_TAIL=%s NVMTL_ASYNC_TEX_UPLOAD=%s)\n", gDev.name.UTF8String,
               getenv("NVMTL_XE_FPS") ?: "-", getenv("NVMTL_XE_RO_ONCE") ?: "-", getenv("NVMTL_XE_HOSTCOPY_AHEAD") ?: "-",
               getenv("NVMTL_XE_WORKER_AHEAD") ?: "-", getenv("NVMTL_XE_EMPTY_TAIL") ?: "-", getenv("NVMTL_ASYNC_TEX_UPLOAD") ?: "-");
        gQ = [gDev newCommandQueue];
        NSError *e = nil;
        gLib = [gDev newLibraryWithSource:kShaders options:nil error:&e];
        if (!gLib) { printf("shaders: %s\n", e.localizedDescription.UTF8String); return 3; }
        // the read-only wallpaper and constant data
        const size_t wbytes = (size_t)WPW * 8 * WPH;
        void *wp = ro_map(wbytes, ^(uint8_t *p, size_t n) {
            uint16_t *h = (uint16_t *)p;
            for (size_t i = 0; i < n / 2; i += 4) { const size_t px = i / 4, x = px % WPW, y = px / WPW;
                h[i] = (uint16_t)(x * 17); h[i + 1] = (uint16_t)(y * 29); h[i + 2] = (uint16_t)((x ^ y) * 7); h[i + 3] = 0xffff; }
        });
        void *kp = ro_map(16384, ^(uint8_t *p, size_t n) { float *f = (float *)p; for (size_t i = 0; i < n / 4; i++) f[i] = (i % 8) < 4 ? 0.85f : 0.05f; });
        if (!wp || !kp) { printf("no read-only mappings\n"); return 3; }
        id<MTLBuffer> wb = [gDev newBufferWithBytesNoCopy:wp length:wbytes options:MTLResourceStorageModeShared deallocator:^(void *p, NSUInteger l) { munmap(p, l); }];
        id<MTLBuffer> konst = [gDev newBufferWithBytesNoCopy:kp length:16384 options:MTLResourceStorageModeShared deallocator:^(void *p, NSUInteger l) { munmap(p, l); }];
        MTLTextureDescriptor *wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Unorm width:WPW height:WPH mipmapped:NO];
        wd.usage = MTLTextureUsageShaderRead; wd.storageMode = MTLStorageModeShared;
        id<MTLTexture> wall = [wb newTextureWithDescriptor:wd offset:0 bytesPerRow:(NSUInteger)WPW * 8];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:512 height:512 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> winTex = [gDev newTextureWithDescriptor:td];
        uint32_t *px = malloc(512 * 512 * 4);
        for (int i = 0; i < 512 * 512; i++) px[i] = 0xc0000000u | (uint32_t)((i % 512) * 0x10203u + (i / 512) * 0x30201u);
        [winTex replaceRegion:MTLRegionMake2D(0, 0, 512, 512) mipmapLevel:0 withBytes:px bytesPerRow:512 * 4];
        free(px);
        td.width = 256; td.height = 64;
        id<MTLTexture> strip = [gDev newTextureWithDescriptor:td];
        MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:SW / 2 height:SH / 2 mipmapped:YES];
        md.mipmapLevelCount = 4; md.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; md.storageMode = MTLStorageModePrivate;
        id<MTLTexture> mips = [gDev newTextureWithDescriptor:md];
        NSMutableArray<id<MTLTexture>> *targets = [NSMutableArray new];
        for (int i = 0; i < 3; i++) {
            NSDictionary *props = @{ (id)kIOSurfaceWidth: @(SW), (id)kIOSurfaceHeight: @(SH), (id)kIOSurfaceBytesPerElement: @4,
                                     (id)kIOSurfaceBytesPerRow: @(SW * 4), (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
            IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
            MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:SW height:SH mipmapped:NO];
            sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; sd.storageMode = MTLStorageModeManaged;
            id<MTLTexture> t = s ? [gDev newTextureWithDescriptor:sd iosurface:s plane:0] : nil;
            if (s) CFRelease(s);
            if (t) [targets addObject:t];
        }
        if (!wb || !konst || !wall || !winTex || !strip || !mips || targets.count != 3) {
            printf("setup failed: wallpaper buffer %d texture %d, constants %d, window %d, strip %d, mips %d, targets %lu\n",
                   wb != nil, wall != nil, konst != nil, winTex != nil, strip != nil, mips != nil, (unsigned long)targets.count);
            return 3;
        }
        // warm up (pipelines, first uploads), then the two loops
        run("warmup", 1.0, 2, 240, targets, wall, winTex, konst, mips, strip);
        run("paced", seconds, 2, 240, targets, wall, winTex, konst, mips, strip);
        run("burst", seconds, 3, 0, targets, wall, winTex, konst, mips, strip);
        const int ok = gPacedFps >= 228 && gPacedQ < 2.0;
        printf("FPSPROBE verdict: %s - paced %.1f frames/s (target >= 228), commit->GPU start %.2f ms (target < 2)\n",
               ok ? "PASS" : "FAIL", gPacedFps, gPacedQ);
        return ok ? 0 : 1;
    }
}

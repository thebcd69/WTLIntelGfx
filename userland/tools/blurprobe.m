// blurprobe: the GPU paths behind macOS's backdrop blur ("vibrancy"), one at a time on one Metal device, each checked
// against a CPU reference. GU605 branch of XeGfx, for the see-through/flickering vibrancy on the Intel path.
//   blurprobe [OUTDIR]          the system's Intel Metal device (only if this process is allowed one)
//   NM_TEST_PLUGIN=<XeMTLDriver binary> blurprobe [OUTDIR]   that plugin in this process, on NMXAccel (as mtlprobe)
// Tests (each prints "TEST <name>: PASS|FAIL ..." and writes OUTDIR/<name>.png):
//   metal.rtt_sample   render to a texture, then sample it in the next pass of the same command buffer (x40)
//   metal.genmips      generateMipmaps, then sample level 2 explicitly
//   metal.render_mip   render into level 1 and 2 through texture views (CA's downsample chain)
//   metal.compute_tg   compute blur through threadgroup memory
//   ci.gaussian        Core Image CIGaussianBlur on this device vs Core Image's software renderer
//   mps.gaussian       MPSImageGaussianBlur vs a CPU Gaussian
//   ca.backdrop        CARenderer: a CABackdropLayer with gaussianBlur + colorSaturate (what NSVisualEffectView
//                      builds) over moving content, 30 frames: blurred / unblurred / other per frame (flicker)
//   ca.layerblur       CARenderer: a plain layer with a gaussianBlur filter (content blur, not backdrop)
// Exit 0 when every test passed. Build (Command Line Tools only):
//   clang -fobjc-arc -O1 -arch x86_64 -mmacosx-version-min=15.0 -framework Foundation -framework Metal \
//     -framework QuartzCore -framework CoreImage -framework MetalPerformanceShaders -framework ImageIO \
//     -framework CoreGraphics -framework IOKit blurprobe.m -o blurprobe
// <wtl> project: xegfx (GU605 branch) | BSD-2-Clause </wtl>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreImage/CoreImage.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <ImageIO/ImageIO.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <math.h>
#include <objc/message.h>

enum { W = 512, H = 256 };
static id<MTLDevice> gDev;
static id<MTLCommandQueue> gQ;
static id<MTLLibrary> gLib;
static NSString *gOut;
static int gFails;

static void result(const char *name, int pass, NSString *detail)
{
    printf("TEST %s: %s %s\n", name, pass ? "PASS" : "FAIL", detail.UTF8String);
    fflush(stdout);
    if (!pass) gFails++;
}

// the source picture: 4-pixel vertical stripes of red, blue, white and black over a horizontal brightness ramp, shifted
// by `shift` pixels (moving content, as a dragged window's background)
static void pattern(uint8_t *bgra, int shift)
{
    static const uint8_t col[4][3] = { { 0, 0, 255 }, { 255, 0, 0 }, { 255, 255, 255 }, { 0, 0, 0 } };   // B G R
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *c = col[(((x + shift) & 0x7fffffff) / 4) & 3];
            const int ramp = 128 + (y * 127) / H;
            uint8_t *p = bgra + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(c[0] * ramp / 255); p[1] = (uint8_t)(c[1] * ramp / 255); p[2] = (uint8_t)(c[2] * ramp / 255); p[3] = 255;
        }
}

static double luma(const uint8_t *p) { return 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2]; }
// mean |luma(x) - luma(x+1)| inside a rectangle: high for sharp stripes, near 0 once blurred
static double hf(const uint8_t *img, int w, int x0, int y0, int x1, int y1)
{
    double s = 0; long n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x + 1 < x1; x++, n++) s += fabs(luma(img + ((size_t)y * w + x) * 4) - luma(img + ((size_t)y * w + x + 1) * 4));
    return n ? s / n : 0;
}
// mean (max - min) of the channels: colour saturation
static double sat(const uint8_t *img, int w, int x0, int y0, int x1, int y1)
{
    double s = 0; long n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++, n++) {
            const uint8_t *p = img + ((size_t)y * w + x) * 4;
            int mx = p[0], mn = p[0];
            for (int k = 1; k < 3; k++) { if (p[k] > mx) mx = p[k]; if (p[k] < mn) mn = p[k]; }
            s += mx - mn;
        }
    return n ? s / n : 0;
}
static double meandiff(const uint8_t *a, const uint8_t *b, size_t px, int *maxd)
{
    double s = 0; int m = 0;
    for (size_t i = 0; i < px * 4; i++) { if ((i & 3) == 3) continue; int d = abs((int)a[i] - (int)b[i]); s += d; if (d > m) m = d; }
    if (maxd) *maxd = m;
    return s / (px * 3);
}

static void png(const char *name, const uint8_t *bgra, int w, int h)
{
    if (!gOut) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)bgra, w, h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef im = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    NSString *path = [gOut stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.png", name]];
    CGImageDestinationRef d = im ? CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL) : NULL;
    if (d) { CGImageDestinationAddImage(d, im, NULL); CGImageDestinationFinalize(d); CFRelease(d); }
    if (im) CGImageRelease(im);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

static CGImageRef cgimage(const uint8_t *bgra, int w, int h)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)bgra, w, h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef im = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx); CGColorSpaceRelease(cs);
    return im;
}

static id<MTLTexture> tex(int w, int h, int mips, MTLStorageMode sm, MTLTextureUsage u)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:mips > 1];
    if (mips > 1) d.mipmapLevelCount = mips;
    d.storageMode = sm; d.usage = u;
    return [gDev newTextureWithDescriptor:d];
}
static id<MTLTexture> upload(const uint8_t *bgra, int mips, MTLTextureUsage u)
{
    id<MTLTexture> t = tex(W, H, mips, MTLStorageModeManaged, u | MTLTextureUsageShaderRead);
    [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:bgra bytesPerRow:W * 4];
    return t;
}
// level `lvl` of t into CPU memory (managed: a synchronize blit; private: a copy into a managed twin)
static void readback(id<MTLTexture> t, int lvl, uint8_t *out)
{
    const int w = MAX(1, (int)t.width >> lvl), h = MAX(1, (int)t.height >> lvl);
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    id<MTLTexture> src = t;
    if (t.storageMode == MTLStorageModePrivate) {
        src = tex(w, h, 1, MTLStorageModeManaged, MTLTextureUsageShaderRead);
        [b copyFromTexture:t sourceSlice:0 sourceLevel:lvl sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toTexture:src destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        lvl = 0;
    }
    [b synchronizeResource:src];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    [src getBytes:out bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:lvl];
}

static NSString *const kShaders =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 p [[position]]; float2 uv; };\n"
     "vertex V tri(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V v; v.p = float4(q * 2 - 1, 0, 1); v.uv = float2(q.x, 1 - q.y); return v; }\n"
     "fragment float4 copy(V v [[stage_in]], texture2d<float> t [[texture(0)]]) { constexpr sampler s(filter::nearest); return t.sample(s, v.uv); }\n"
     "fragment float4 box9(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::nearest, address::clamp_to_edge); float dx = 1.0 / t.get_width(); float4 a = 0;\n"
     "  for (int k = -4; k <= 4; k++) a += t.sample(s, v.uv + float2(k * dx, 0)); return a / 9; }\n"
     "fragment float4 lod(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant float &l [[buffer(0)]]) {\n"
     "  constexpr sampler s(filter::nearest, mip_filter::nearest); return t.sample(s, v.uv, level(l)); }\n"
     "fragment float4 down2(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge); return t.sample(s, v.uv); }\n"
     "kernel void hblur(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
     "                  uint2 g [[thread_position_in_grid]], uint2 l [[thread_position_in_threadgroup]], uint2 tg [[threadgroup_position_in_grid]]) {\n"
     "  threadgroup float4 tile[64 + 16]; int x0 = int(tg.x) * 64 - 8; int w = int(src.get_width());\n"
     "  for (int i = int(l.x); i < 80; i += 64) tile[i] = src.read(uint2(clamp(x0 + i, 0, w - 1), g.y));\n"
     "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
     "  float4 a = 0; for (int k = 0; k <= 16; k++) a += tile[int(l.x) + k]; dst.write(a / 17, g); }\n";

static id<MTLRenderPipelineState> pso(NSString *frag)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [gLib newFunctionWithName:@"tri"];
    d.fragmentFunction = [gLib newFunctionWithName:frag];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [gDev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) printf("  pipeline %s: %s\n", frag.UTF8String, e.localizedDescription.UTF8String);
    return p;
}
static void pass(id<MTLCommandBuffer> cb, id<MTLTexture> dst, int level, id<MTLRenderPipelineState> p, id<MTLTexture> src, const float *l)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].level = level;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p]; [e setFragmentTexture:src atIndex:0];
    if (l) [e setFragmentBytes:l length:sizeof(float) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static void cpu_box(const uint8_t *in, uint8_t *out, int r)   // horizontal box of 2r+1, clamped
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 4; c++) {
                int s = 0;
                for (int k = -r; k <= r; k++) s += in[((size_t)y * W + MIN(W - 1, MAX(0, x + k))) * 4 + c];
                out[((size_t)y * W + x) * 4 + c] = (uint8_t)((s + r) / (2 * r + 1));
            }
}
static void cpu_down(const uint8_t *in, int w, int h, uint8_t *out, int f)   // f x f box average
{
    for (int y = 0; y < h / f; y++)
        for (int x = 0; x < w / f; x++)
            for (int c = 0; c < 4; c++) {
                int s = 0;
                for (int j = 0; j < f; j++) for (int i = 0; i < f; i++) s += in[((size_t)(y * f + j) * w + x * f + i) * 4 + c];
                out[((size_t)y * (w / f) + x) * 4 + c] = (uint8_t)((s + f * f / 2) / (f * f));
            }
}

static void t_rtt_sample(void)
{
    id<MTLRenderPipelineState> cp = pso(@"copy"), bp = pso(@"box9");
    if (!cp || !bp) { result("metal.rtt_sample", 0, @"pipeline"); return; }
    uint8_t *src = malloc(W * H * 4), *ref = malloc(W * H * 4), *got = malloc(W * H * 4);
    int bad = 0; double worst = 0;
    id<MTLTexture> mid = tex(W, H, 1, MTLStorageModePrivate, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    id<MTLTexture> out = tex(W, H, 1, MTLStorageModeManaged, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    for (int it = 0; it < 40; it++) {
        pattern(src, it);
        id<MTLTexture> s = upload(src, 1, 0);
        id<MTLCommandBuffer> cb = [gQ commandBuffer];
        pass(cb, mid, 0, cp, s, NULL);   // pass 1 writes mid
        pass(cb, out, 0, bp, mid, NULL); // pass 2 samples it at once
        [cb commit]; [cb waitUntilCompleted];
        readback(out, 0, got);
        cpu_box(src, ref, 4);
        int md; double d = meandiff(got, ref, W * H, &md);
        if (d > 2.0) bad++;
        if (d > worst) worst = d;
        if (it == 0) png("metal.rtt_sample", got, W, H);
    }
    result("metal.rtt_sample", bad == 0, [NSString stringWithFormat:@"%d/40 frames off the CPU box blur (worst mean diff %.2f)", bad, worst]);
    free(src); free(ref); free(got);
}

static void t_genmips(void)
{
    id<MTLRenderPipelineState> lp = pso(@"lod");
    uint8_t *src = malloc(W * H * 4), *got = malloc(W * H * 4), *ref = malloc(W * H * 4);
    pattern(src, 0);
    id<MTLTexture> s = upload(src, 4, 0);
    id<MTLTexture> out = tex(W / 4, H / 4, 1, MTLStorageModeManaged, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder]; [b generateMipmapsForTexture:s]; [b endEncoding];
    float l = 2;
    if (lp) pass(cb, out, 0, lp, s, &l);
    [cb commit]; [cb waitUntilCompleted];
    readback(out, 0, got);
    cpu_down(src, W, H, ref, 4);
    int md; double d = meandiff(got, ref, (W / 4) * (H / 4), &md);
    png("metal.genmips", got, W / 4, H / 4);
    // level 2 read straight back too: a wrong sample path and a wrong mip chain look different
    uint8_t *lv = malloc(W * H * 4); readback(s, 2, lv);
    int md2; double d2 = meandiff(lv, ref, (W / 4) * (H / 4), &md2);
    result("metal.genmips", lp && d < 6 && d2 < 6, [NSString stringWithFormat:@"sampled level 2 vs CPU 4x4 box: mean %.2f max %d; level 2 copied back: mean %.2f max %d", d, md, d2, md2]);
    free(src); free(got); free(ref); free(lv);
}

static void t_render_mip(void)
{
    id<MTLRenderPipelineState> dp = pso(@"down2");
    uint8_t *src = malloc(W * H * 4), *got = malloc(W * H * 4), *ref = malloc(W * H * 4);
    pattern(src, 0);
    id<MTLTexture> t = upload(src, 3, MTLTextureUsageRenderTarget);
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    for (int lv = 1; lv <= 2 && dp; lv++) {
        id<MTLTexture> view = [t newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(lv - 1, 1) slices:NSMakeRange(0, 1)];
        pass(cb, t, lv, dp, view, NULL);   // render into level lv, sampling level lv-1 through a view
    }
    [cb commit]; [cb waitUntilCompleted];
    readback(t, 2, got);
    cpu_down(src, W, H, ref, 4);
    int md; double d = meandiff(got, ref, (W / 4) * (H / 4), &md);
    png("metal.render_mip", got, W / 4, H / 4);
    result("metal.render_mip", dp && d < 6, [NSString stringWithFormat:@"level 2 rendered from level 1 rendered from level 0, vs CPU 4x4 box: mean %.2f max %d (magenta = never rendered)", d, md]);
    free(src); free(got); free(ref);
}

static void t_compute_tg(void)
{
    NSError *e = nil;
    id<MTLComputePipelineState> cp = [gDev newComputePipelineStateWithFunction:[gLib newFunctionWithName:@"hblur"] error:&e];
    if (!cp) { result("metal.compute_tg", 0, e.localizedDescription ?: @"pipeline"); return; }
    uint8_t *src = malloc(W * H * 4), *got = malloc(W * H * 4), *ref = malloc(W * H * 4);
    pattern(src, 3);
    id<MTLTexture> s = upload(src, 1, 0);
    id<MTLTexture> out = tex(W, H, 1, MTLStorageModeManaged, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:cp]; [c setTexture:s atIndex:0]; [c setTexture:out atIndex:1];
    [c dispatchThreadgroups:MTLSizeMake(W / 64, H, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [c endEncoding]; [cb commit]; [cb waitUntilCompleted];
    readback(out, 0, got);
    cpu_box(src, ref, 8);
    int md; double d = meandiff(got, ref, W * H, &md);
    png("metal.compute_tg", got, W, H);
    result("metal.compute_tg", d < 2, [NSString stringWithFormat:@"vs CPU 17-tap box: mean %.2f max %d", d, md]);
    free(src); free(got); free(ref);
}

static void t_ci(void)
{
    uint8_t *src = malloc(W * H * 4), *got = malloc(W * H * 4), *ref = calloc(1, W * H * 4);
    pattern(src, 0);
    CGImageRef im = cgimage(src, W, H);
    CIImage *in = [CIImage imageWithCGImage:im];
    CIFilter *f = [CIFilter filterWithName:@"CIGaussianBlur"]; [f setValue:in forKey:kCIInputImageKey]; [f setValue:@10 forKey:kCIInputRadiusKey];
    CIImage *blur = [f.outputImage imageByCroppingToRect:CGRectMake(0, 0, W, H)];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CIContext *sw = [CIContext contextWithOptions:@{ kCIContextUseSoftwareRenderer: @YES }];
    [sw render:blur toBitmap:ref rowBytes:W * 4 bounds:CGRectMake(0, 0, W, H) format:kCIFormatBGRA8 colorSpace:cs];
    CIContext *gpu = [CIContext contextWithMTLDevice:gDev];
    [gpu render:blur toBitmap:got rowBytes:W * 4 bounds:CGRectMake(0, 0, W, H) format:kCIFormatBGRA8 colorSpace:cs];
    int md; double d = meandiff(got, ref, W * H, &md);
    png("ci.gaussian", got, W, H); png("ci.gaussian-software", ref, W, H);
    result("ci.gaussian", d < 3, [NSString stringWithFormat:@"vs software renderer: mean %.2f max %d; detail %.1f (software %.1f, source %.1f)", d, md,
                                  hf(got, W, 32, 32, W - 32, H - 32), hf(ref, W, 32, 32, W - 32, H - 32), hf(src, W, 32, 32, W - 32, H - 32)]);
    CGColorSpaceRelease(cs); CGImageRelease(im);
    free(src); free(got); free(ref);
}

static void t_mps(void)
{
    if (!MPSSupportsMTLDevice(gDev)) { result("mps.gaussian", 0, @"MPSSupportsMTLDevice says no"); return; }
    uint8_t *src = malloc(W * H * 4), *got = malloc(W * H * 4);
    pattern(src, 0);
    id<MTLTexture> s = upload(src, 1, 0);
    id<MTLTexture> out = tex(W, H, 1, MTLStorageModeManaged, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    MPSImageGaussianBlur *g = [[MPSImageGaussianBlur alloc] initWithDevice:gDev sigma:6];
    id<MTLCommandBuffer> cb = [gQ commandBuffer];
    [g encodeToCommandBuffer:cb sourceTexture:s destinationTexture:out];
    [cb commit]; [cb waitUntilCompleted];
    readback(out, 0, got);
    png("mps.gaussian", got, W, H);
    const double h0 = hf(src, W, 32, 32, W - 32, H - 32), h1 = hf(got, W, 32, 32, W - 32, H - 32);
    const double l0 = sat(src, W, 32, 32, W - 32, H - 32), l1 = sat(got, W, 32, 32, W - 32, H - 32);
    result("mps.gaussian", h1 < h0 * 0.2 && l1 > 5, [NSString stringWithFormat:@"detail %.1f -> %.1f, saturation %.1f -> %.1f (blurred: detail < 20%%, not black)", h0, h1, l0, l1]);
    free(src); free(got);
}

// CARenderer into a managed texture; the layer tree is built by `build`, `frames` frames with content shifted 1 px each
static void t_ca(const char *name, BOOL backdrop)
{
    id<MTLTexture> out = tex(W, H, 1, MTLStorageModeManaged, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    NSMutableDictionary *opt = [NSMutableDictionary dictionary];
    NSString *const *qkey = (NSString *const *)dlsym(RTLD_DEFAULT, "kCARendererMetalCommandQueue");
    if (qkey && *qkey) opt[*qkey] = gQ;
    CARenderer *r = [CARenderer rendererWithMTLTexture:out options:opt];
    Class Filter = NSClassFromString(@"CAFilter"), Backdrop = NSClassFromString(@"CABackdropLayer");
    if (!r || !Filter || (backdrop && !Backdrop)) {
        result(name, 0, [NSString stringWithFormat:@"setup: renderer %d CAFilter %d CABackdropLayer %d", r != nil, Filter != nil, Backdrop != nil]);
        return;
    }
    id (*mk)(id, SEL, id) = (id (*)(id, SEL, id))objc_msgSend;
    id blur = mk(Filter, NSSelectorFromString(@"filterWithType:"), @"gaussianBlur");
    [blur setValue:@20 forKey:@"inputRadius"];
    id satf = mk(Filter, NSSelectorFromString(@"filterWithType:"), @"colorSaturate");
    [satf setValue:@2.0 forKey:@"inputAmount"];
    uint8_t *src = malloc((W + 64) * H * 4), *got = malloc(W * H * 4);
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    CALayer *root = [CALayer layer]; root.frame = CGRectMake(0, 0, W, H); root.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    CALayer *content = [CALayer layer]; content.anchorPoint = CGPointZero;
    root.geometryFlipped = YES;
    [root addSublayer:content];
    CALayer *fx = backdrop ? [Backdrop layer] : [CALayer layer];
    fx.frame = CGRectMake(128, 64, 256, 128);
    if (backdrop) { fx.filters = @[ blur, satf ]; [fx setValue:@YES forKey:@"enabled"]; }
    else { fx.contents = nil; }
    [root addSublayer:fx];
    if (!backdrop) { content.filters = @[ blur ]; }
    [CATransaction commit]; [CATransaction flush];
    r.layer = root; r.bounds = CGRectMake(0, 0, W, H);
    int blurred = 0, sharp = 0, other = 0; double sum0 = 0, sum1 = 0, s0 = 0, s1 = 0;
    const int frames = 30;
    // the region checked: inside the effect layer for a backdrop, the whole picture for a content blur
    const int x0 = backdrop ? 144 : 32, y0 = backdrop ? 80 : 32, x1 = backdrop ? 368 : W - 32, y1 = backdrop ? 176 : H - 32;
    for (int f = 0; f < frames; f++) {
        uint8_t *wide = malloc((size_t)W * H * 4);
        pattern(wide, f);
        CGImageRef im = cgimage(wide, W, H);
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        content.bounds = CGRectMake(0, 0, W, H); content.position = CGPointZero; content.contents = (__bridge id)im;
        [CATransaction commit]; [CATransaction flush];
        [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
        [r addUpdateRect:CGRectMake(0, 0, W, H)];
        [r render];
        [r endFrame];
        readback(out, 0, got);   // same queue as the renderer when the key exists: ordered after its work
        const double h0 = hf(wide, W, x0, y0, x1, y1), h1 = hf(got, W, x0, y0, x1, y1);
        const double c0 = sat(wide, W, x0, y0, x1, y1), c1 = sat(got, W, x0, y0, x1, y1);
        sum0 += h0; sum1 += h1; s0 += c0; s1 += c1;
        if (h1 < h0 * 0.3) blurred++; else if (h1 > h0 * 0.6) sharp++; else other++;
        if (f == 0 || f == frames / 2) png([NSString stringWithFormat:@"%s-%02d", name, f].UTF8String, got, W, H);
        CGImageRelease(im); free(wide);
    }
    result(name, blurred == frames, [NSString stringWithFormat:@"%d blurred, %d unblurred (see-through), %d in between of %d frames; detail %.1f -> %.1f, saturation %.1f -> %.1f%s",
                                     blurred, sharp, other, frames, sum0 / frames, sum1 / frames, s0 / frames, s1 / frames,
                                     qkey ? "" : " (no kCARendererMetalCommandQueue: readbacks may race the renderer)"]);
    free(src); free(got);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc > 1) { gOut = @(argv[1]); [[NSFileManager defaultManager] createDirectoryAtPath:gOut withIntermediateDirectories:YES attributes:nil error:nil]; }
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
        printf("device %s (INTEL_DEBUG=%s NVMTL_NO_FBFETCH=%s NVMTL_NO_DIRTYONLY=%s)\n", gDev.name.UTF8String,
               getenv("INTEL_DEBUG") ?: "-", getenv("NVMTL_NO_FBFETCH") ?: "-", getenv("NVMTL_NO_DIRTYONLY") ?: "-");
        gQ = [gDev newCommandQueue];
        NSError *e = nil;
        gLib = [gDev newLibraryWithSource:kShaders options:nil error:&e];
        if (!gLib) { printf("shaders: %s\n", e.localizedDescription.UTF8String); return 3; }
        const char *only = getenv("BLURPROBE_ONLY");   // e.g. "ca." or "metal.render_mip"
#define RUN(n, call) do { if (!only || strstr(n, only)) { call; } } while (0)
        RUN("metal.rtt_sample", t_rtt_sample());
        RUN("metal.genmips", t_genmips());
        RUN("metal.render_mip", t_render_mip());
        RUN("metal.compute_tg", t_compute_tg());
        RUN("ci.gaussian", t_ci());
        RUN("mps.gaussian", t_mps());
        RUN("ca.layerblur", t_ca("ca.layerblur", NO));
        RUN("ca.backdrop", t_ca("ca.backdrop", YES));
        printf("blurprobe: %d failed\n", gFails);
        return gFails ? 1 : 0;
    }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A frame drawn off screen, put on a window's Metal layer: the swap of a GL that cannot draw to the layer itself (Mesa's Zink,
 * surfaceless here). The frame comes as GL reads it back -- RGBA, bottom row first -- and is uploaded to a texture, then drawn
 * the right way up onto the layer's next drawable, scaled to it if the two differ.
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>

void tl_log_line(const char *fmt, ...);

static NSString *const k_src =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 p [[position]]; float2 uv; };\n"
     "vertex V vs(uint i [[vertex_id]]) {\n"
     "  float2 q = float2((i << 1) & 2, i & 2);\n"
     "  V o; o.p = float4(q * 2.0 - 1.0, 0, 1); o.uv = q; return o;\n"
     "}\n"
     "fragment float4 fs(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
     "  return float4(t.sample(s, v.uv).rgb, 1.0);   /* texel row 0 is GL's bottom row, and uv.y = 0 is the bottom of the screen */\n"
     "}\n";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLRenderPipelineState> g_pipe;
static id<MTLTexture> g_tex;

static bool setup(void)
{
    if (g_pipe) return true;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) return false;
    g_queue = [g_dev newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:k_src options:nil error:&err];
    if (!lib) { tl_log_line("present: shader: %s", err.localizedDescription.UTF8String); return false; }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_pipe = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!g_pipe) { tl_log_line("present: pipeline: %s", err.localizedDescription.UTF8String); return false; }
    tl_log_line("present: frames go to the layer through Metal (%s)", g_dev.name.UTF8String);
    return true;
}

void tl_present_rgba(void *layer_, const void *rgba, int w, int h)
{
    if (!layer_ || !rgba || w <= 0 || h <= 0) return;
    @autoreleasepool {
        pthread_mutex_lock(&g_lock);
        if (!setup()) { pthread_mutex_unlock(&g_lock); return; }
        CAMetalLayer *layer = (__bridge CAMetalLayer *)layer_;
        if (![layer isKindOfClass:[CAMetalLayer class]]) { pthread_mutex_unlock(&g_lock); return; }
        if (layer.device != g_dev) layer.device = g_dev;
        if (layer.pixelFormat != MTLPixelFormatBGRA8Unorm) layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (layer.drawableSize.width != w || layer.drawableSize.height != h) layer.drawableSize = CGSizeMake(w, h);
        if (!g_tex || (int)g_tex.width != w || (int)g_tex.height != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            g_tex = [g_dev newTextureWithDescriptor:td];
        }
        [g_tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:rgba bytesPerRow:(NSUInteger)w * 4];
        id<CAMetalDrawable> dr = [layer nextDrawable];
        if (dr) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = dr.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:g_pipe];
            [enc setFragmentTexture:g_tex atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cb presentDrawable:dr];
            [cb commit];
            /* the texture is rewritten by the next frame's upload: this one must be drawn first */
            [cb waitUntilCompleted];
        }
        pthread_mutex_unlock(&g_lock);
    }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * com.epicgames.ue4.MediaPlayer14, Unreal Engine 4's Java video player (an android.media.MediaPlayer that draws each frame into a GL
 * texture), implemented with AVFoundation. The engine's AndroidMedia plugin plays every movie through it: startup movies, the ones a
 * level puts on a media texture, menu backgrounds. With none, a media texture stays at its clear colour -- the white a game showed
 * wherever a video should be.
 *
 * Unreal opens a movie by a file, an offset and a size (a movie in the OBB is a stretch of the OBB, a stored zip entry). AVFoundation
 * reads whole files, so a stretch is copied once into the app's cache and played from there. AVPlayer plays it, sound and all, on its own
 * clock; an AVPlayerItemVideoOutput hands out the frame of the moment as 32-bit BGRA. The engine asks for a frame once per render frame
 * with the GL name of a texture of the video's size (getVideoLastFrame), and the frame is uploaded into it.
 *
 * That is the engine's path when the GL driver has no GL_OES_EGL_image_external: with it the engine wants an external texture fed by an
 * Android SurfaceTexture, which nothing on Husk produces, so Unreal games are not shown that extension (tl_ue4_hle_install).
 */
#define _DARWIN_C_SOURCE
#import <AVFoundation/AVFoundation.h>
#import <Accelerate/Accelerate.h>
#import <CoreVideo/CoreVideo.h>

#include <math.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "husk-tl-jni.h"
#include "husk-tl-egl.h"
#include "husk-tl-internal.h"

void tl_log_line(const char *fmt, ...);
const char *tl_data_dir(void);
const char *tl_path_resolve(const char *path, char *buf, size_t n);
bool tl_vfile_lookup(const char *path, char *host, size_t n, uint64_t *off, uint64_t *size);
const char *tl_ld_main_apk(void);

#define MP "com/epicgames/ue4/MediaPlayer14"

static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(bool z) { jvalue v; v.j = 0; v.z = z; return v; }
static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vf(float f) { jvalue v; v.j = 0; v.f = f; return v; }

@interface HuskUEPlayer : NSObject
@property (nonatomic, strong) AVPlayer *player;
@property (nonatomic, strong) AVPlayerItem *item;
@property (nonatomic, strong) AVPlayerItemVideoOutput *output;
@property (nonatomic, strong) id endObserver;
@property (nonatomic, copy) NSString *file;
@property (atomic) bool prepared, looping, completed, playing, videoOn, audioOn, hasAudio, hasVideo, uploaded;
@property (atomic) float volume, fps;
@property (atomic) int width, height, bitrate, channels, rate;
@property (atomic) double duration;
@end
@implementation HuskUEPlayer
@end

static HuskUEPlayer *P(tl_jcall *c) { return c->self && c->self->native ? (__bridge HuskUEPlayer *)c->self->native : nil; }

static void drop_item(HuskUEPlayer *p)
{
    if (p.endObserver) { [[NSNotificationCenter defaultCenter] removeObserver:p.endObserver]; p.endObserver = nil; }
    [p.player pause];
    p.player = nil; p.item = nil; p.output = nil;
    p.prepared = false; p.playing = false; p.completed = false; p.uploaded = false;
    p.width = p.height = 0; p.duration = 0; p.hasAudio = p.hasVideo = false;
}

/* MediaPlayer14(boolean swizzlePixels, boolean vulkanRenderer, boolean needTrackInfo) */
static void MP_init(tl_jcall *c)
{
    HuskUEPlayer *p = [HuskUEPlayer new];
    p.videoOn = true; p.audioOn = true; p.volume = 1.0f;
    c->self->native = (void *)CFBridgingRetain(p);
}

/* A stretch of a file as a file of its own, made once in the cache; the whole file is used as it is. */
static NSString *movie_file(const char *host, uint64_t off, uint64_t size, const char *name)
{
    struct stat st;
    if (stat(host, &st) != 0) { tl_log_line("ue4media: %s is not there", host); return nil; }
    const char *dot = strrchr(name, '.');
    bool movie_ext = dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".mov") || !strcasecmp(dot, ".m4v"));
    if (off == 0 && (size == 0 || size == (uint64_t)st.st_size) && movie_ext) return [NSString stringWithUTF8String:host];
    if (size == 0 || off + size > (uint64_t)st.st_size) { tl_log_line("ue4media: %s: %llu bytes at %llu is past its end", host, size, off); return nil; }
    char dir[1024]; snprintf(dir, sizeof(dir), "%s/cache/ue4-movies", tl_data_dir());
    char parent[1024]; snprintf(parent, sizeof(parent), "%s/cache", tl_data_dir());
    mkdir(parent, 0777); mkdir(dir, 0777);
    const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
    char out[1400]; snprintf(out, sizeof(out), "%s/%llu-%llu-%s%s", dir, (unsigned long long)off, (unsigned long long)size, base, movie_ext ? "" : ".mp4");
    for (char *q = out + strlen(dir) + 1; *q; q++) if (*q == ' ' || *q == '(' || *q == ')') *q = '_';
    if (stat(out, &st) == 0 && (uint64_t)st.st_size == size) return [NSString stringWithUTF8String:out];
    FILE *in = fopen(host, "rb"); if (!in) return nil;
    char tmp[1500]; snprintf(tmp, sizeof(tmp), "%s.part", out);
    FILE *o = fopen(tmp, "wb"); if (!o) { fclose(in); return nil; }
    fseeko(in, (off_t)off, SEEK_SET);
    size_t chunk = 1 << 20; uint8_t *buf = malloc(chunk); uint64_t left = size; bool ok = buf != NULL;
    while (ok && left) { size_t n = left < chunk ? (size_t)left : chunk; ok = fread(buf, 1, n, in) == n && fwrite(buf, 1, n, o) == n; left -= n; }
    free(buf); fclose(in); if (fclose(o) != 0) ok = false;
    if (!ok || rename(tmp, out) != 0) { unlink(tmp); tl_log_line("ue4media: copying %s out failed", base); return nil; }
    tl_log_line("ue4media: %s (%llu bytes) copied out to play", base, (unsigned long long)size);
    return [NSString stringWithUTF8String:out];
}

static bool set_file(tl_jcall *c, const char *path, uint64_t off, uint64_t size)
{
    HuskUEPlayer *p = P(c);
    if (!p || !path || !*path) return false;
    drop_item(p);
    char host[1024]; uint64_t voff = 0, vsize = 0;
    if (tl_vfile_lookup(path, host, sizeof(host), &voff, &vsize)) { if (!size) size = vsize - off; off += voff; }
    else { char buf[1024]; snprintf(host, sizeof(host), "%s", tl_path_resolve(path, buf, sizeof(buf))); }
    NSString *f = movie_file(host, off, size, path);
    p.file = f;
    tl_log_line("ue4media: setDataSource(%s, %llu, %llu) -> %s", path, (unsigned long long)off, (unsigned long long)size, f ? "ok" : "failed");
    return f != nil;
}

/* setDataSource(String path, long offset, long size) */
static void MP_setDataSourceFile(tl_jcall *c)
{
    c->ret = vz(set_file(c, tl_jni_string(c->args[0].l), (uint64_t)c->args[1].j, (uint64_t)c->args[2].j));
}

/* setDataSource(AssetManager, String assetPath, long offset, long size): a stretch of a stored entry of the APK's assets */
static void MP_setDataSourceAsset(tl_jcall *c)
{
    const char *name = tl_jni_string(c->args[1].l), *apk = tl_ld_main_apk();
    bool ok = false;
    if (name && apk) {
        char entry[600]; snprintf(entry, sizeof(entry), "%s%s", strncmp(name, "assets/", 7) ? "assets/" : "", name);
        tl_zip z; char err[160];
        if (tl_zip_open(&z, apk, err, sizeof(err))) {
            const tl_zip_entry *e = tl_zip_find(&z, entry);
            if (e && e->method == 0 && e->local_offset + 30 < z.size) {
                const uint8_t *h = z.map + e->local_offset;
                uint16_t nlen, elen; memcpy(&nlen, h + 26, 2); memcpy(&elen, h + 28, 2);
                uint64_t data = e->local_offset + 30 + nlen + elen, off = (uint64_t)c->args[2].j, size = (uint64_t)c->args[3].j;
                if (!size) size = e->usize - off;
                tl_zip_close(&z);
                ok = set_file(c, apk, data + off, size);
            } else {
                tl_log_line("ue4media: asset %s is %s", entry, e ? "compressed in the APK" : "not in the APK");
                tl_zip_close(&z);
            }
        }
    }
    c->ret = vz(ok);
}

static void MP_setDataSourceURL(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    const char *u = tl_jni_string(c->args[0].l);
    if (!p || !u) { c->ret = vz(false); return; }
    drop_item(p);
    if (!strncmp(u, "file://", 7)) { c->ret = vz(set_file(c, u + 7, 0, 0)); return; }
    p.file = [NSString stringWithUTF8String:u];
    tl_log_line("ue4media: setDataSourceURL(%s)", u);
    c->ret = vz(true);
}

static void MP_false(tl_jcall *c) { c->ret = vz(false); }
static void MP_nop(tl_jcall *c) { (void)c; }

static void MP_prepare(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    if (!p || !p.file) return;
    NSURL *url = [p.file hasPrefix:@"/"] ? [NSURL fileURLWithPath:p.file] : [NSURL URLWithString:p.file];
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [asset loadValuesAsynchronouslyForKeys:@[ @"tracks", @"duration", @"playable" ] completionHandler:^{ dispatch_semaphore_signal(sem); }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    NSError *err = nil;
    if ([asset statusOfValueForKey:@"tracks" error:&err] != AVKeyValueStatusLoaded) {
        tl_log_line("ue4media: %s cannot be read: %s", p.file.lastPathComponent.UTF8String, err ? err.localizedDescription.UTF8String : "timed out");
        return;
    }
    AVAssetTrack *v = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject, *a = [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
    p.hasVideo = v != nil; p.hasAudio = a != nil;
    if (v) {
        CGSize sz = CGSizeApplyAffineTransform(v.naturalSize, v.preferredTransform);
        p.width = (int)fabs(sz.width); p.height = (int)fabs(sz.height);
        p.fps = v.nominalFrameRate > 0 ? v.nominalFrameRate : 30.0f; p.bitrate = (int)v.estimatedDataRate;
    }
    if (a) {
        CMAudioFormatDescriptionRef fd = (__bridge CMAudioFormatDescriptionRef)a.formatDescriptions.firstObject;
        const AudioStreamBasicDescription *d = fd ? CMAudioFormatDescriptionGetStreamBasicDescription(fd) : NULL;
        p.channels = d ? (int)d->mChannelsPerFrame : 2; p.rate = d ? (int)d->mSampleRate : 48000;
    }
    p.duration = CMTIME_IS_NUMERIC(asset.duration) ? CMTimeGetSeconds(asset.duration) : 0;
    p.item = [AVPlayerItem playerItemWithAsset:asset];
    p.output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) }];
    [p.item addOutput:p.output];
    p.player = [AVPlayer playerWithPlayerItem:p.item];
    p.player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    p.player.volume = p.audioOn ? p.volume : 0.0f;
    __weak HuskUEPlayer *weak = p;
    p.endObserver = [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:p.item queue:nil usingBlock:^(NSNotification *n) {
        (void)n; HuskUEPlayer *s = weak; if (!s) return;
        if (s.looping) { [s.player seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero]; [s.player play]; }
        else { s.playing = false; }
        s.completed = true;
    }];
    for (int i = 0; i < 300 && p.item.status == AVPlayerItemStatusUnknown; i++) usleep(10000);
    if (p.item.status == AVPlayerItemStatusFailed) { tl_log_line("ue4media: %s fails to play: %s", p.file.lastPathComponent.UTF8String, p.item.error.localizedDescription.UTF8String); return; }
    p.prepared = true;
    tl_log_line("ue4media: %s prepared: %dx%d at %.1f fps, %.1f s, %s", p.file.lastPathComponent.UTF8String, p.width, p.height, p.fps, p.duration, p.hasAudio ? "with sound" : "silent");
}

static void MP_start(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p && p.player) { p.completed = false; [p.player play]; p.playing = true; } }
static void MP_pause(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p && p.player) { [p.player pause]; p.playing = false; } }
static void MP_stop(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p && p.player) { [p.player pause]; [p.player seekToTime:kCMTimeZero]; p.playing = false; } }
static void MP_reset(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p) { drop_item(p); p.file = nil; } }
static void MP_release(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    if (!p) return;
    drop_item(p);
    CFBridgingRelease(c->self->native); c->self->native = NULL;
}
static void MP_seekTo(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    if (p && p.player) { [p.player seekToTime:CMTimeMakeWithSeconds(c->args[0].i / 1000.0, 1000) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero]; p.completed = false; }
}
static void MP_setLooping(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p) p.looping = c->args[0].z; }
static void MP_isLooping(tl_jcall *c) { HuskUEPlayer *p = P(c); c->ret = vz(p && p.looping); }
static void MP_isPlaying(tl_jcall *c) { HuskUEPlayer *p = P(c); c->ret = vz(p && p.playing); }
static void MP_isPrepared(tl_jcall *c) { HuskUEPlayer *p = P(c); c->ret = vz(p && p.prepared); }
static void MP_didComplete(tl_jcall *c) { HuskUEPlayer *p = P(c); bool d = p && p.completed; if (d) p.completed = false; c->ret = vz(d); }
static void MP_duration(tl_jcall *c) { HuskUEPlayer *p = P(c); c->ret = vi(p ? (int)(p.duration * 1000.0) : 0); }
static void MP_position(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    double t = p && p.player ? CMTimeGetSeconds(p.player.currentTime) : 0;
    c->ret = vi(t == t && t > 0 ? (int)(t * 1000.0) : 0);
}
static void MP_width(tl_jcall *c) { HuskUEPlayer *p = P(c); c->ret = vi(p ? p.width : 0); }
static void MP_height(tl_jcall *c) { HuskUEPlayer *p = P(c); c->ret = vi(p ? p.height : 0); }
static void MP_setVideoEnabled(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p) p.videoOn = c->args[0].z; }
static void MP_setAudioEnabled(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p) { p.audioOn = c->args[0].z; p.player.volume = p.audioOn ? p.volume : 0.0f; } }
static void MP_setAudioVolume(tl_jcall *c) { HuskUEPlayer *p = P(c); if (p) { p.volume = c->args[0].f; p.player.volume = p.audioOn ? p.volume : 0.0f; } }
static void MP_externalTexture(tl_jcall *c) { c->ret = vi(-1); }
static void MP_null(tl_jcall *c) { c->ret = vl(NULL); }

/* the track lists: one video track (index 0) and one sound track (index 1) at most, as MediaExtractor would number them */
static void MP_videoTracks(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    int n = p && p.hasVideo ? 1 : 0;
    jobj *arr = tl_jni_new_obj_array(tl_jni_class(MP "$VideoTrackInfo"), (uint32_t)n);
    if (n) {
        jobj *t = tl_jni_new_object(tl_jni_class(MP "$VideoTrackInfo"));
        tl_jni_set_field(t, "Index", "I", vi(0));
        tl_jni_set_field(t, "MimeType", "Ljava/lang/String;", vl(tl_jni_new_string("video/avc")));
        tl_jni_set_field(t, "DisplayName", "Ljava/lang/String;", vl(tl_jni_new_string("Video Track 0")));
        tl_jni_set_field(t, "Language", "Ljava/lang/String;", vl(tl_jni_new_string("und")));
        tl_jni_set_field(t, "BitRate", "I", vi(p.bitrate));
        tl_jni_set_field(t, "Width", "I", vi(p.width));
        tl_jni_set_field(t, "Height", "I", vi(p.height));
        tl_jni_set_field(t, "FrameRate", "F", vf(p.fps));
        arr->oarr.v[0] = t;
    }
    c->ret = vl(arr);
}
static void MP_audioTracks(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    int n = p && p.hasAudio ? 1 : 0;
    jobj *arr = tl_jni_new_obj_array(tl_jni_class(MP "$AudioTrackInfo"), (uint32_t)n);
    if (n) {
        jobj *t = tl_jni_new_object(tl_jni_class(MP "$AudioTrackInfo"));
        tl_jni_set_field(t, "Index", "I", vi(1));
        tl_jni_set_field(t, "MimeType", "Ljava/lang/String;", vl(tl_jni_new_string("audio/mp4a-latm")));
        tl_jni_set_field(t, "DisplayName", "Ljava/lang/String;", vl(tl_jni_new_string("Audio Track 0")));
        tl_jni_set_field(t, "Language", "Ljava/lang/String;", vl(tl_jni_new_string("und")));
        tl_jni_set_field(t, "Channels", "I", vi(p.channels));
        tl_jni_set_field(t, "SampleRate", "I", vi(p.rate));
        arr->oarr.v[0] = t;
    }
    c->ret = vl(arr);
}
static void MP_captionTracks(tl_jcall *c) { c->ret = vl(tl_jni_new_obj_array(tl_jni_class(MP "$CaptionTrackInfo"), 0)); }

/* ------------------------------------------------------------------ frames into the engine's texture */

static struct {
    void (*bind)(unsigned, unsigned);
    void (*geti)(unsigned, int *);
    void (*store)(unsigned, int);
    void (*bind_buf)(unsigned, unsigned);
    void (*level_iv)(unsigned, int, unsigned, int *);
    void (*sub)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    unsigned (*err)(void);
    bool ok;
} G;

static bool gl_load(void)
{
    if (G.ok) return true;
#define R(f, n) G.f = (__typeof__(G.f))tl_egl_resolve(n)
    R(bind, "glBindTexture"); R(geti, "glGetIntegerv"); R(store, "glPixelStorei"); R(bind_buf, "glBindBuffer");
    R(level_iv, "glGetTexLevelParameteriv"); R(sub, "glTexSubImage2D"); R(err, "glGetError");
#undef R
    G.ok = G.bind && G.geti && G.store && G.bind_buf && G.sub && G.err;
    return G.ok;
}

static bool flip_rows(void) { static int f = -1; if (f < 0) { const char *k = getenv("TL_UE4_VIDEO_FLIP"); f = k && *k == '1'; } return f; }

static bool upload(HuskUEPlayer *p, CVPixelBufferRef pb, unsigned tex)
{
    if (!gl_load()) return false;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = CVPixelBufferGetBaseAddress(pb);
    int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    int old_tex = 0, old_pbo = 0, align = 4, rowlen = 0, skipr = 0, skipp = 0;
    G.geti(0x8069 /* TEXTURE_BINDING_2D */, &old_tex); G.geti(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &old_pbo);
    G.geti(0x0CF5, &align); G.geti(0x0CF2, &rowlen); G.geti(0x0CF3, &skipr); G.geti(0x0CF4, &skipp);
    while (G.err()) {}
    G.bind(0x0DE1, tex);
    int tw = w, th = h, ifmt = 0;
    if (G.level_iv) { G.level_iv(0x0DE1, 0, 0x1000, &tw); G.level_iv(0x0DE1, 0, 0x1001, &th); G.level_iv(0x0DE1, 0, 0x1003, &ifmt); }
    if (tw <= 0 || th <= 0) { tw = w; th = h; }
    int cw = w < tw ? w : tw, ch = h < th ? h : th;
    bool bgra = ifmt == 0x80E1 /* BGRA_EXT */ || ifmt == 0x93A1 /* BGRA8_EXT */;
    if (old_pbo) G.bind_buf(0x88EC, 0);
    G.store(0x0CF5, 4); G.store(0x0CF3, 0); G.store(0x0CF4, 0);
    const uint8_t *src = base; size_t sstride = stride;
    static uint8_t *conv; static size_t conv_cap;
    if (!bgra || flip_rows()) {
        size_t need = (size_t)cw * ch * 4;
        if (need > conv_cap) { free(conv); conv = malloc(need); conv_cap = conv ? need : 0; }
        if (conv) {
            for (int y = 0; y < ch; y++) {
                const uint8_t *s = base + (size_t)(flip_rows() ? h - 1 - y : y) * stride;
                uint8_t *d = conv + (size_t)y * cw * 4;
                if (bgra) memcpy(d, s, (size_t)cw * 4);
                else for (int x = 0; x < cw; x++) { d[4 * x] = s[4 * x + 2]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x]; d[4 * x + 3] = 255; }
            }
            src = conv; sstride = (size_t)cw * 4;
        }
    }
    G.store(0x0CF2, (int)(sstride / 4));
    G.sub(0x0DE1, 0, 0, 0, cw, ch, bgra ? 0x80E1 : 0x1908, 0x1401, src);
    unsigned e = G.err();
    G.store(0x0CF5, align); G.store(0x0CF2, rowlen); G.store(0x0CF3, skipr); G.store(0x0CF4, skipp);
    if (old_pbo) G.bind_buf(0x88EC, (unsigned)old_pbo);
    G.bind(0x0DE1, (unsigned)old_tex);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (!p.uploaded || e) {
        tl_log_line("ue4media: %s: frames %dx%d into texture %u (%dx%d, format %#x)%s", p.file.lastPathComponent.UTF8String, w, h, tex, tw, th, ifmt,
                    e ? " -- GL error" : "");
        p.uploaded = true;
    }
    return e == 0;
}

/* getVideoLastFrame(int destTexture): the frame of the moment into the texture; false when there is no new one */
static void MP_getVideoLastFrame(tl_jcall *c)
{
    c->ret = vz(false);
    /* called every frame on the game's render thread, which has no autorelease pool of its own to drain */
    @autoreleasepool {
        HuskUEPlayer *p = P(c);
        if (!p || !p.output || !p.videoOn) return;
        CMTime t = [p.output itemTimeForMachAbsoluteTime:(int64_t)mach_absolute_time()];
        if (![p.output hasNewPixelBufferForItemTime:t]) return;
        CVPixelBufferRef pb = [p.output copyPixelBufferForItemTime:t itemTimeForDisplay:NULL];
        if (!pb) return;
        c->ret = vz(upload(p, pb, (unsigned)c->args[0].i));
        CVPixelBufferRelease(pb);
    }
}

/* updateVideoFrame(int externalTexture): the SurfaceTexture path, never chosen here (no GL_OES_EGL_image_external): no frame */
static void MP_updateVideoFrame(tl_jcall *c)
{
    HuskUEPlayer *p = P(c);
    jobj *info = tl_jni_new_object(tl_jni_class(MP "$FrameUpdateInfo"));
    double t = p && p.player ? CMTimeGetSeconds(p.player.currentTime) : 0;
    tl_jni_set_field(info, "CurrentPosition", "I", vi(t == t && t > 0 ? (int)(t * 1000.0) : 0));
    tl_jni_set_field(info, "FrameReady", "Z", vz(false));
    tl_jni_set_field(info, "RegionChanged", "Z", vz(false));
    tl_jni_set_field(info, "UScale", "F", vf(1)); tl_jni_set_field(info, "VScale", "F", vf(1));
    tl_jni_set_field(info, "UOffset", "F", vf(0)); tl_jni_set_field(info, "VOffset", "F", vf(0));
    c->ret = vl(info);
}

static void MP_remoteFileExists(tl_jcall *c) { c->ret = vl(c->args[0].l); }

static const tl_jhle k_ue4media[] = {
    { MP, "<init>", "(ZZZ)V", MP_init },
    { MP, "setDataSource", "(Ljava/lang/String;JJ)Z", MP_setDataSourceFile },
    { MP, "setDataSource", "(Landroid/content/res/AssetManager;Ljava/lang/String;JJ)Z", MP_setDataSourceAsset },
    { MP, "setDataSourceURL", "(Ljava/lang/String;)Z", MP_setDataSourceURL },
    { MP, "setDataSourceArchive", "(JJ)Z", MP_false },
    { MP, "RemoteFileExists", "(Ljava/lang/String;)Ljava/lang/String;", MP_remoteFileExists },
    { MP, "prepare", "()V", MP_prepare },
    { MP, "prepareAsync", "()V", MP_prepare },
    { MP, "start", "()V", MP_start },
    { MP, "pause", "()V", MP_pause },
    { MP, "stop", "()V", MP_stop },
    { MP, "reset", "()V", MP_reset },
    { MP, "release", "()V", MP_release },
    { MP, "seekTo", "(I)V", MP_seekTo },
    { MP, "setLooping", "(Z)V", MP_setLooping },
    { MP, "isLooping", "()Z", MP_isLooping },
    { MP, "isPlaying", "()Z", MP_isPlaying },
    { MP, "isPrepared", "()Z", MP_isPrepared },
    { MP, "didComplete", "()Z", MP_didComplete },
    { MP, "didResolutionChange", "()Z", MP_false },
    { MP, "getDuration", "()I", MP_duration },
    { MP, "getCurrentPosition", "()I", MP_position },
    { MP, "getVideoWidth", "()I", MP_width },
    { MP, "getVideoHeight", "()I", MP_height },
    { MP, "setVideoEnabled", "(Z)V", MP_setVideoEnabled },
    { MP, "setAudioEnabled", "(Z)V", MP_setAudioEnabled },
    { MP, "setAudioVolume", "(F)V", MP_setAudioVolume },
    { MP, "getExternalTextureId", "()I", MP_externalTexture },
    { MP, "getVideoLastFrame", "(I)Z", MP_getVideoLastFrame },
    { MP, "getVideoLastFrameData", "()Ljava/nio/Buffer;", MP_null },
    { MP, "updateVideoFrame", "(I)L" MP "$FrameUpdateInfo;", MP_updateVideoFrame },
    { MP, "initBitmapRenderer", "()V", MP_nop },
    { MP, "selectTrack", "(I)V", MP_nop },
    { MP, "GetVideoTracks", "()[L" MP "$VideoTrackInfo;", MP_videoTracks },
    { MP, "GetAudioTracks", "()[L" MP "$AudioTrackInfo;", MP_audioTracks },
    { MP, "GetCaptionTracks", "()[L" MP "$CaptionTrackInfo;", MP_captionTracks },
    { NULL, NULL, NULL, NULL },
};

void tl_ue4_media_install(void)
{
    tl_jni_declare("android/media/MediaPlayer", "java/lang/Object");
    tl_jni_declare(MP, "android/media/MediaPlayer");
    tl_jni_register_hle(k_ue4media);
}

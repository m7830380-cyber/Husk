package android.view;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;

/**
 * Copies what a window (or the view holding a surface) shows into a bitmap. Husk draws the view tree in software into the
 * destination, then reports the result on the given handler. The listener is always called: apps commonly block on a
 * latch until it is (a theme switch takes a window screenshot this way), so a copy that never answers hangs them.
 */
@SuppressWarnings({"unchecked", "rawtypes"})
public final class PixelCopy {
    public static final int SUCCESS = 0;
    public static final int ERROR_UNKNOWN = 1;
    public static final int ERROR_TIMEOUT = 2;
    public static final int ERROR_SOURCE_NO_DATA = 3;
    public static final int ERROR_SOURCE_INVALID = 4;
    public static final int ERROR_DESTINATION_INVALID = 5;

    public interface OnPixelCopyFinishedListener { void onPixelCopyFinished(int copyResult); }

    protected PixelCopy() {}

    static int copy(View v, Rect src, Bitmap dst) {
        if (dst == null || dst.isRecycled() || !dst.isMutable()) return ERROR_DESTINATION_INVALID;
        if (v == null) return ERROR_SOURCE_INVALID;
        int vw = v.getWidth(), vh = v.getHeight();
        if (vw <= 0 || vh <= 0) return ERROR_SOURCE_NO_DATA;
        Rect r = src != null && !src.isEmpty() ? src : new Rect(0, 0, vw, vh);
        Canvas c = null;
        try {
            dst.eraseColor(0);
            c = new Canvas(dst);
            c.scale(dst.getWidth() / (float) r.width(), dst.getHeight() / (float) r.height());
            c.translate(-r.left, -r.top);
            v.draw(c);
            return SUCCESS;
        } catch (Throwable t) {
            return ERROR_UNKNOWN;
        } finally {
            if (c != null) c.huskRelease();
        }
    }

    static void finish(final OnPixelCopyFinishedListener l, Handler h, final int result) {
        if (l == null) return;
        if (h == null) h = new Handler(Looper.getMainLooper());
        if (h.getLooper() == Looper.myLooper()) { l.onPixelCopyFinished(result); return; }
        h.post(new Runnable() { public void run() { l.onPixelCopyFinished(result); } });
    }

    static View viewOf(Window w) { return w == null ? null : w.peekDecorView(); }

    public static void request(Window source, Bitmap dest, OnPixelCopyFinishedListener l, Handler h) { request(source, null, dest, l, h); }
    public static void request(Window source, Rect srcRect, Bitmap dest, OnPixelCopyFinishedListener l, Handler h) { finish(l, h, copy(viewOf(source), srcRect, dest)); }
    public static void request(SurfaceView source, Bitmap dest, OnPixelCopyFinishedListener l, Handler h) { request(source, null, dest, l, h); }
    public static void request(SurfaceView source, Rect srcRect, Bitmap dest, OnPixelCopyFinishedListener l, Handler h) { finish(l, h, copy(source, srcRect, dest)); }
    public static void request(Surface source, Bitmap dest, OnPixelCopyFinishedListener l, Handler h) { request(source, null, dest, l, h); }
    public static void request(Surface source, Rect srcRect, Bitmap dest, OnPixelCopyFinishedListener l, Handler h) {
        finish(l, h, source == null ? ERROR_SOURCE_INVALID : dest == null || dest.isRecycled() ? ERROR_DESTINATION_INVALID : ERROR_SOURCE_NO_DATA);
    }

    public static void request(Request r, java.util.concurrent.Executor ex, java.util.function.Consumer<Result> cb) { r.request(ex, cb); }

    public static final class Request {
        View view; Rect srcRect; Bitmap bitmap; boolean surface;
        Request() {}
        public Bitmap getDestinationBitmap() { return bitmap; }
        public Rect getSourceRect() { return srcRect; }
        public void request(java.util.concurrent.Executor ex, final java.util.function.Consumer<Result> cb) {
            Bitmap b = bitmap;
            if (b == null && view != null && view.getWidth() > 0 && view.getHeight() > 0) {
                Rect r = srcRect != null && !srcRect.isEmpty() ? srcRect : new Rect(0, 0, view.getWidth(), view.getHeight());
                b = Bitmap.createBitmap(r.width(), r.height(), Bitmap.Config.ARGB_8888);
            }
            final Result res = new Result();
            res.bitmap = b;
            res.status = surface ? ERROR_SOURCE_NO_DATA : b == null ? ERROR_SOURCE_NO_DATA : copy(view, srcRect, b);
            if (cb == null) return;
            Runnable run = new Runnable() { public void run() { cb.accept(res); } };
            if (ex != null) ex.execute(run); else run.run();
        }
        public static final class Builder {
            private final Request r = new Request();
            Builder() {}
            public static Builder ofSurface(Surface s) { Builder b = new Builder(); b.r.surface = true; return b; }
            public static Builder ofSurface(SurfaceView s) { Builder b = new Builder(); b.r.view = s; return b; }
            public static Builder ofWindow(View v) { Builder b = new Builder(); b.r.view = v == null ? null : v.getRootView(); return b; }
            public static Builder ofWindow(Window w) { Builder b = new Builder(); b.r.view = viewOf(w); return b; }
            public Builder setDestinationBitmap(Bitmap bm) { r.bitmap = bm; return this; }
            public Builder setSourceRect(Rect rc) { r.srcRect = rc; return this; }
            public Request build() { Request x = new Request(); x.view = r.view; x.srcRect = r.srcRect; x.bitmap = r.bitmap; x.surface = r.surface; return x; }
        }
    }

    public static final class Result {
        Bitmap bitmap; int status;
        Result() {}
        public Bitmap getBitmap() { return bitmap; }
        public int getStatus() { return status; }
    }
}

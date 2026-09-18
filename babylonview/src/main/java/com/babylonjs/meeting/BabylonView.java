package com.babylonjs.meeting;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;

import com.babylonjs.embedding.BabylonNative;

/**
 * A transparent Babylon Native view.
 *
 * <p>Renders a Babylon.js scene into a {@link SurfaceView} using the
 * {@link BabylonNative} embedding layer (backed by
 * {@code libBabylonNativeEmbedding.so}).
 *
 * <h3>Transparency</h3>
 * The surface is configured so that OpenGL back-buffer pixels whose alpha
 * component is {@code 0} composite as fully transparent, letting whatever is
 * behind this view show through:
 * <ul>
 *   <li>{@link SurfaceView#setZOrderOnTop(boolean) setZOrderOnTop(true)} —
 *       places the surface on top of the window so it is blended by the
 *       compositor rather than punched through as an opaque hole.</li>
 *   <li>{@link SurfaceHolder#setFormat(int) setFormat(PixelFormat.TRANSLUCENT)}
 *       — requests an RGBA_8888 buffer with a real alpha channel.</li>
 * </ul>
 * bgfx's GL backend already requests an EGL config with
 * {@code EGL_ALPHA_SIZE = 8} for its default RGBA8 back buffer, so the alpha
 * written by the scene is preserved through composition.
 *
 * <p>For pixels to actually be transparent, the Babylon.js scene must clear
 * with a zero-alpha colour, e.g.
 * {@code scene.clearColor = new BABYLON.Color4(0, 0, 0, 0);} on the JS side.
 *
 * <p>The {@code runtimeHandle} is borrowed from the host, which owns the
 * Runtime's lifetime. This view owns only the native View handle and mirrors
 * the Surface lifecycle: attach in {@code surfaceCreated}, resize in
 * {@code surfaceChanged}, detach in {@code surfaceDestroyed}.
 *
 * <p>All sizes and coordinates passed to native are physical pixels — the
 * native View queries DPR internally.
 */
public class BabylonView extends FrameLayout
        implements SurfaceHolder.Callback2, View.OnTouchListener {

    private static final FrameLayout.LayoutParams CHILD_LAYOUT_PARAMS =
            new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT);

    private final SurfaceView surfaceView;

    /** Runtime handle borrowed from the host. Not owned by this view. */
    private final long runtimeHandle;

    /** Native View handle (0 if not attached). Owned by this view. */
    private long viewHandle = 0;

    public BabylonView(Context context, long runtimeHandle) {
        super(context);
        this.runtimeHandle = runtimeHandle;

        this.surfaceView = new SurfaceView(context);
        this.surfaceView.setLayoutParams(CHILD_LAYOUT_PARAMS);

        // --- Transparency setup -------------------------------------------
        // Composite the surface on top of the window so it is alpha-blended
        // instead of replacing the window with an opaque region...
        this.surfaceView.setZOrderOnTop(true);
        // ...and request a buffer with an alpha channel (RGBA_8888).
        this.surfaceView.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        // ------------------------------------------------------------------

        this.surfaceView.getHolder().addCallback(this);
        this.addView(this.surfaceView);

        setOnTouchListener(this);

        // Drive the render loop from onDraw/invalidate.
        setWillNotDraw(false);
    }

    /** Part of {@link SurfaceHolder.Callback}; not called directly by clients. */
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        viewHandle = BabylonNative.viewAttach(runtimeHandle, holder.getSurface());
    }

    /** Part of {@link SurfaceHolder.Callback}; not called directly by clients. */
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (viewHandle != 0) {
            BabylonNative.viewResize(viewHandle, width, height);
        }
    }

    /** Part of {@link SurfaceHolder.Callback}; not called directly by clients. */
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (viewHandle != 0) {
            BabylonNative.viewDetach(viewHandle);
            viewHandle = 0;
        }
    }

    /** Part of {@link SurfaceHolder.Callback2}; not called directly by clients. */
    @Deprecated
    @Override
    public void surfaceRedrawNeeded(SurfaceHolder holder) {
        // Redraw happens on the bgfx thread; nothing to do here.
    }

    @Override
    public boolean onTouch(View v, MotionEvent event) {
        if (viewHandle == 0) {
            return false;
        }

        int pointerId = event.getPointerId(event.getActionIndex());
        float x = event.getX(event.getActionIndex());
        float y = event.getY(event.getActionIndex());

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                BabylonNative.viewPointerDown(viewHandle, pointerId, x, y);
                break;
            case MotionEvent.ACTION_MOVE:
                BabylonNative.viewPointerMove(viewHandle, pointerId, x, y);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                BabylonNative.viewPointerUp(viewHandle, pointerId, x, y);
                break;
        }
        return true;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (viewHandle != 0) {
            BabylonNative.runtimeUpdateMeetingVideoTextures(runtimeHandle);
            BabylonNative.viewRenderFrame(viewHandle);
        }
        invalidate();
    }
}

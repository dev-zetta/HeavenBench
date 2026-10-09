package app.zetta.heavenbench;

import android.content.Context;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import org.json.JSONException;
import org.json.JSONObject;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

public final class HeavenActivity extends SDLActivity {
    private float renderScale() {
        String directory = getIntent().getStringExtra("storage");
        if (directory == null) return 1.0f;
        File config = new File(directory, "config.json");
        // This launcher-only option does not rewrite the renderer's config.
        if (!config.isFile() || config.length() > 1024 * 1024) return 1.0f;
        try {
            Object value = new JSONObject(new String(Files.readAllBytes(config.toPath()),
                StandardCharsets.UTF_8)).opt("android_render_scale");
            if (!(value instanceof Number)) return 1.0f;
            double scale = ((Number) value).doubleValue();
            if (!Double.isFinite(scale) || scale < 0.125 || scale > 1.0) return 1.0f;
            return (float) scale;
        } catch (IOException | JSONException ignored) {
            return 1.0f;
        }
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        float scale = renderScale();
        if (scale == 1.0f) return new SDLSurface(context);
        View.OnGenericMotionListener motionListener = getMotionListener();
        return new RenderSurface(context, scale, motionListener);
    }

    private static final class RenderSurface extends SDLSurface {
        private final float scale;
        private int requestedWidth, requestedHeight;

        RenderSurface(Context context, float scale, View.OnGenericMotionListener motionListener) {
            super(context);
            this.scale = scale;
            setOnGenericMotionListener((view, event) -> {
                // SDL uses wheel axes and relative mouse deltas directly.
                // Only absolute mouse hover uses Surface pixel coordinates.
                int source = event.getSource();
                boolean absoluteMouse = source == InputDevice.SOURCE_MOUSE ||
                    source == (InputDevice.SOURCE_MOUSE | InputDevice.SOURCE_TOUCHSCREEN);
                if (!absoluteMouse || event.getActionMasked() != MotionEvent.ACTION_HOVER_MOVE) {
                    return motionListener.onGenericMotion(view, event);
                }
                MotionEvent mapped = mapToBuffer(event);
                try {
                    return motionListener.onGenericMotion(view, mapped);
                } finally {
                    if (mapped != event) mapped.recycle();
                }
            });
        }

        private void requestBuffer(int width, int height) {
            if (width <= 0 || height <= 0) return;
            int bufferWidth = Math.max(1, Math.round(width * scale));
            int bufferHeight = Math.max(1, Math.round(height * scale));
            if (bufferWidth == requestedWidth && bufferHeight == requestedHeight) return;
            requestedWidth = bufferWidth;
            requestedHeight = bufferHeight;
            getHolder().setFixedSize(bufferWidth, bufferHeight);
        }

        @Override
        protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
            super.onSizeChanged(width, height, oldWidth, oldHeight);
            requestBuffer(width, height);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            requestBuffer(getWidth(), getHeight());
            // Do not start SDL at an intermediate native-sized buffer. The
            // next callback publishes the actual requested pixel extent.
            if (requestedWidth <= 0 || requestedHeight <= 0 ||
                width != requestedWidth || height != requestedHeight) {
                mIsSurfaceReady = false;
                return;
            }
            super.surfaceChanged(holder, format, width, height);
        }

        private MotionEvent mapToBuffer(MotionEvent event) {
            if (getWidth() <= 0 || getHeight() <= 0 || !mIsSurfaceReady) return event;
            MotionEvent mapped = MotionEvent.obtain(event);
            Matrix transform = new Matrix();
            transform.setScale(mWidth / getWidth(), mHeight / getHeight());
            mapped.transform(transform);
            return mapped;
        }

        @Override
        public boolean onTouch(View view, MotionEvent event) {
            if (!mIsSurfaceReady) return false;
            int source = event.getSource();
            boolean mouse = source == InputDevice.SOURCE_MOUSE ||
                source == (InputDevice.SOURCE_MOUSE | InputDevice.SOURCE_TOUCHSCREEN);
            // Captured-pointer dispatch already reports relative motion and
            // remains inherited unchanged. Preserve relative button events too.
            if (source == InputDevice.SOURCE_MOUSE_RELATIVE || (mouse && hasPointerCapture())) {
                return super.onTouch(view, event);
            }
            MotionEvent mapped = mapToBuffer(event);
            try {
                return super.onTouch(view, mapped);
            } finally {
                if (mapped != event) mapped.recycle();
            }
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "heaven_benchmark" };
    }

    @Override
    protected String[] getArguments() {
        String directory = getIntent().getStringExtra("storage");
        if (directory == null) return new String[0];
        File root = new File(directory);
        ArrayList<String> arguments = new ArrayList<>();
        // SDL_main requires the private storage bridge argument first.
        Collections.addAll(arguments,
            "--android-user-dir", root.getAbsolutePath(),
            "--config", new File(root, "config.json").getAbsolutePath(),
            "--data", new File(root, "assets.zip").getAbsolutePath());
        // Override a saved desktop window mode without rewriting it.
        arguments.add("--fullscreen");
        // SDL starts this thread only after the Android Surface is ready. Its
        // pixel extent is the explicit render request for both launcher modes;
        // the saved desktop-sized configuration remains untouched.
        Rect surfaceFrame = mSurface != null
            ? new Rect(mSurface.getHolder().getSurfaceFrame()) : null;
        if (surfaceFrame != null && surfaceFrame.width() > 0 && surfaceFrame.height() > 0) {
            Collections.addAll(arguments,
                "--width", Integer.toString(surfaceFrame.width()),
                "--height", Integer.toString(surfaceFrame.height()));
        }
        if (getIntent().getBooleanExtra("benchmark", false)) arguments.add("--benchmark");
        MicroprofileLaunch.append(arguments, getIntent(), root);
        return arguments.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
}

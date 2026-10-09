#!/usr/bin/env python3
"""Check SDL Java staging against the downloaded source pin, without a device."""

import hashlib
import os
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

import prepare_sdl_java


SDL_SOURCE = Path(sys.argv.pop(1)).resolve()


def digest_tree(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


class SdlJavaStagingTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven-sdl-java-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "SDL"
        self.relative = Path("android-project/app/src/main/java")
        shutil.copytree(SDL_SOURCE / self.relative, self.source / self.relative)
        self.output = self.root / "generated"

    def test_staging_preserves_original_and_unmodified_java(self):
        before = digest_tree(self.source)
        prepare_sdl_java.prepare(self.source, self.output)
        self.assertEqual(digest_tree(self.source), before)
        for path in (self.source / self.relative).rglob("*.java"):
            destination = self.output / path.relative_to(self.source / self.relative)
            if path.name not in prepare_sdl_java.SOURCE_HASHES:
                self.assertEqual(path.read_bytes(), destination.read_bytes())
            else:
                self.assertTrue(destination.read_text().startswith("// HeavenBench Android integration:"))

    def test_changed_upstream_fails_before_replacing_valid_output(self):
        prepare_sdl_java.prepare(self.source, self.output)
        before = digest_tree(self.output)
        path = self.source / self.relative / "org/libsdl/app/SDLAudioManager.java"
        path.write_bytes(path.read_bytes() + b"\n// unreviewed update\n")
        with self.assertRaisesRegex(ValueError, "Unreviewed SDL Java input"):
            prepare_sdl_java.prepare(self.source, self.output)
        self.assertEqual(digest_tree(self.output), before)

    def test_rebuild_is_identical(self):
        prepare_sdl_java.prepare(self.source, self.output)
        before = digest_tree(self.output)
        prepare_sdl_java.prepare(self.source, self.output)
        self.assertEqual(digest_tree(self.output), before)

    def test_unmarked_files_are_preserved(self):
        self.output.mkdir()
        sentinel = self.output / "local-file"
        sentinel.write_text("preserve me")
        with self.assertRaisesRegex(ValueError, "unmarked generated directory"):
            prepare_sdl_java.prepare(self.source, self.output)
        self.assertEqual(sentinel.read_text(), "preserve me")

    def test_gradle_created_empty_output_is_supported(self):
        self.output.mkdir()
        prepare_sdl_java.prepare(self.source, self.output)
        self.assertTrue((self.output / prepare_sdl_java.MARKER).is_file())

    def test_legacy_receiver_guard_has_matching_signature_permission(self):
        # The only lint annotation relies on a cross-file runtime contract:
        # the receiver's sender permission must actually be declared/granted.
        prepare_sdl_java.prepare(self.source, self.output)
        manager = (self.output / "org/libsdl/app/HIDDeviceManager.java").read_text()
        suffix = '.permission.USB_EVENTS'
        self.assertIn(f'mContext.getPackageName() + "{suffix}", null)', manager)
        manifest = ET.parse(Path(__file__).parent / "app/src/main/AndroidManifest.xml").getroot()
        attribute = "{http://schemas.android.com/apk/res/android}"
        name = "${applicationId}" + suffix
        declarations = [entry for entry in manifest.findall("permission")
                        if entry.get(attribute + "name") == name]
        self.assertEqual(len(declarations), 1)
        self.assertEqual(declarations[0].get(attribute + "protectionLevel"), "signature")
        self.assertTrue(any(entry.get(attribute + "name") == name
                            for entry in manifest.findall("uses-permission")))


def java_method(source, declaration):
    start = source.index(declaration)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


class HeavenActivityArgumentsTest(unittest.TestCase):
    def test_actual_launcher_surface_and_input_contracts(self):
        # Execute the app-owned activity and the pinned SDL touch/mouse methods
        # against framework stubs. Native callbacks are recorded, not rendered.
        java_home = os.environ.get("JAVA_HOME")
        executable_suffix = ".exe" if os.name == "nt" else ""

        def java_tool(name):
            explicit = (Path(java_home) / "bin" / (name + executable_suffix)
                        if java_home else None)
            resolved = explicit if explicit and explicit.is_file() else shutil.which(name)
            if not resolved:
                self.fail(f"The Android launcher contract test requires JDK tool {name}")
            return str(resolved)

        javac, java = java_tool("javac"), java_tool("java")
        upstream = SDL_SOURCE / "android-project/app/src/main/java/org/libsdl/app"
        touch_method = java_method((upstream / "SDLSurface.java").read_text(),
                                   "public boolean onTouch(")
        captured_method = java_method((upstream / "SDLSurface.java").read_text(),
                                      "public boolean onCapturedPointerEvent(")
        listeners = (upstream / "SDLControllerManager.java").read_text()
        listeners = listeners[listeners.index("class SDLGenericMotionListener_API12"):]
        with tempfile.TemporaryDirectory(prefix="heaven-launcher-surface-") as temporary:
            root = Path(temporary)
            sources = {
                "android/graphics/Rect.java": """package android.graphics;
public final class Rect {
    private final int width, height;
    public Rect(int width, int height) { this.width = width; this.height = height; }
    public Rect(Rect other) { this(other.width, other.height); }
    public int width() { return width; }
    public int height() { return height; }
}
""",
                "android/graphics/Matrix.java": """package android.graphics;
public final class Matrix {
    public float x = 1, y = 1;
    public void setScale(float sx, float sy) { x = sx; y = sy; }
}
""",
                "android/os/Bundle.java": "package android.os; public final class Bundle {}\n",
                "android/os/Build.java": """package android.os;
public final class Build {
    public static final class VERSION { public static final int SDK_INT = 35; }
}
""",
                "android/content/Context.java": "package android.content; public class Context {}\n",
                "android/content/Intent.java": """package android.content;
public final class Intent {
    private final java.util.Map<String, Object> extras = new java.util.HashMap<>();
    public Intent(String storage, boolean benchmark) {
        if (storage != null) putExtra("storage", storage);
        putExtra("benchmark", benchmark);
    }
    public Intent putExtra(String name, String value) { extras.put(name, value); return this; }
    public Intent putExtra(String name, boolean value) { extras.put(name, value); return this; }
    public String getStringExtra(String name) {
        Object value = extras.get(name); return value instanceof String ? (String)value : null;
    }
    public boolean getBooleanExtra(String name, boolean fallback) {
        Object value = extras.get(name); return value instanceof Boolean ? (Boolean)value : fallback;
    }
}
""",
                "android/view/WindowManager.java": """package android.view;
public final class WindowManager {
    public static final class LayoutParams { public static final int FLAG_KEEP_SCREEN_ON = 128; }
}
""",
                "android/view/Window.java": """package android.view;
public final class Window { public void addFlags(int flags) {} }
""",
                "android/view/SurfaceHolder.java": """package android.view;
import android.graphics.Rect;
public interface SurfaceHolder {
    Rect getSurfaceFrame();
    void setFixedSize(int width, int height);
}
""",
                "android/view/InputDevice.java": """package android.view;
public final class InputDevice {
    public static final int SOURCE_JOYSTICK = 0x1000010;
    public static final int SOURCE_MOUSE = 0x2002;
    public static final int SOURCE_TOUCHSCREEN = 0x1002;
    public static final int SOURCE_MOUSE_RELATIVE = 0x20004;
}
""",
                "android/view/View.java": """package android.view;
public class View {
    public interface OnGenericMotionListener { boolean onGenericMotion(View view, MotionEvent event); }
    private OnGenericMotionListener listener;
    private int width, height;
    private boolean captured;
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    protected void onSizeChanged(int w, int h, int oldW, int oldH) {}
    public void layout(int w, int h) {
        int oldW = width, oldH = height; width = w; height = h;
        onSizeChanged(w, h, oldW, oldH);
    }
    public void setOnGenericMotionListener(OnGenericMotionListener value) { listener = value; }
    public boolean dispatchGeneric(MotionEvent event) { return listener.onGenericMotion(this, event); }
    public boolean hasPointerCapture() { return captured; }
    public void requestPointerCapture() { captured = true; }
    public void releasePointerCapture() { captured = false; }
}
""",
                "android/view/MotionEvent.java": """package android.view;
import android.graphics.Matrix;
public final class MotionEvent {
    public static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3,
        ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6, ACTION_HOVER_MOVE = 7, ACTION_SCROLL = 8,
        ACTION_BUTTON_PRESS = 11, ACTION_BUTTON_RELEASE = 12;
    public static final int AXIS_HSCROLL = 10, AXIS_VSCROLL = 9, AXIS_RELATIVE_X = 27, AXIS_RELATIVE_Y = 28;
    public static MotionEvent lastCopy;
    private final int source, action;
    private final int[] ids;
    private final float[] x, y, pressure;
    public float historyX, historyY;
    public float scrollX = 3, scrollY = -2, relativeX = 7, relativeY = -9;
    public boolean recycled;
    public MotionEvent(int source, int action, int[] ids, float[] x, float[] y) {
        this.source = source; this.action = action; this.ids = ids.clone();
        this.x = x.clone(); this.y = y.clone(); this.pressure = new float[ids.length];
        java.util.Arrays.fill(pressure, 0.75f);
        historyX = x[0] - 16; historyY = y[0] - 24;
    }
    public static MotionEvent obtain(MotionEvent event) {
        MotionEvent copy = new MotionEvent(event.source, event.action, event.ids, event.x, event.y);
        copy.historyX = event.historyX; copy.historyY = event.historyY;
        copy.scrollX = event.scrollX; copy.scrollY = event.scrollY;
        copy.relativeX = event.relativeX; copy.relativeY = event.relativeY;
        lastCopy = copy; return copy;
    }
    public void transform(Matrix transform) {
        for (int i = 0; i < x.length; i++) { x[i] *= transform.x; y[i] *= transform.y; }
        historyX *= transform.x; historyY *= transform.y;
    }
    public void recycle() { recycled = true; }
    public int getSource() { return source; }
    public int getActionMasked() { return action & 255; }
    public int getActionIndex() { return action >> 8; }
    public int getPointerCount() { return ids.length; }
    public int getDeviceId() { return 1; }
    public int getPointerId(int index) { return ids[index]; }
    public int getButtonState() { return 1; }
    public float getX(int index) { return x[index]; }
    public float getY(int index) { return y[index]; }
    public float getPressure(int index) { return pressure[index]; }
    public float getAxisValue(int axis) { return getAxisValue(axis, 0); }
    public float getAxisValue(int axis, int index) {
        if (axis == AXIS_HSCROLL) return scrollX;
        if (axis == AXIS_VSCROLL) return scrollY;
        return axis == AXIS_RELATIVE_X ? relativeX : relativeY;
    }
}
""",
                "org/json/JSONException.java": """package org.json;
public final class JSONException extends Exception {
    private static final long serialVersionUID = 1L;
    public JSONException() { super("Invalid mock JSON"); }
}
""",
                "org/json/JSONObject.java": """package org.json;
public final class JSONObject {
    private final Object value;
    public JSONObject(String source) throws JSONException {
        String json = source.trim();
        if (!json.startsWith("{") || !json.endsWith("}")) throw new JSONException();
        int key = json.indexOf("\\\"android_render_scale\\\"");
        if (key < 0) { value = null; return; }
        int colon = json.indexOf(':', key);
        String token = json.substring(colon + 1, json.length() - 1).trim();
        if (token.startsWith("\\\"")) value = token;
        else {
            try { value = Double.valueOf(token); }
            catch (NumberFormatException failure) { throw new JSONException(); }
        }
    }
    public Object opt(String name) { return value; }
}
""",
                "org/libsdl/app/SDLActivity.java": """package org.libsdl.app;
import android.content.Context;
import android.content.Intent;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import java.util.ArrayList;
public class SDLActivity {
    protected static SDLSurface mSurface;
    private static final SDLGenericMotionListener_API12 motion = new SDLGenericMotionListener_API26();
    public static final ArrayList<float[]> touches = new ArrayList<>();
    public static float[] mouse;
    private Intent intent;
    protected static SDLGenericMotionListener_API12 getMotionListener() { return motion; }
    public static boolean isDeXMode() { return false; }
    public static View getContentView() { return mSurface; }
    public static void onNativeMouse(int button, int action, float x, float y, boolean relative) {
        mouse = new float[] {button, action, x, y, relative ? 1 : 0};
    }
    public static void onNativeTouch(int device, int id, int action, float x, float y, float pressure) {
        touches.add(new float[] {id, action, x, y, pressure});
    }
    public static void setSurface(SDLSurface surface) { mSurface = surface; }
    public static void setSurface(Rect frame) {
        mSurface = frame == null ? null : new SDLSurface(new Context());
        if (mSurface != null) mSurface.holder.frame = frame;
    }
    public final void setIntent(Intent value) { intent = value; }
    public Intent getIntent() { return intent; }
    protected SDLSurface createSDLSurface(Context context) { return new SDLSurface(context); }
    protected String[] getLibraries() { return new String[0]; }
    protected String[] getArguments() { return new String[0]; }
    protected void onCreate(Bundle state) {}
    public Window getWindow() { return new Window(); }
}
""",
                "org/libsdl/app/SDLSurface.java": """package org.libsdl.app;
import android.content.Context;
import android.graphics.Rect;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
public class SDLSurface extends View {
    protected float mWidth = 1, mHeight = 1;
    public boolean mIsSurfaceReady;
    public final Holder holder = new Holder();
    public int publishedResizes;
    public SDLSurface(Context context) { setOnGenericMotionListener(SDLActivity.getMotionListener()); }
    public Holder getHolder() { return holder; }
    public void surfaceChanged(SurfaceHolder surface, int format, int width, int height) {
        mWidth = width; mHeight = height; mIsSurfaceReady = true; publishedResizes++;
    }
    public static final class Holder implements SurfaceHolder {
        public Rect frame = new Rect(0, 0);
        public int fixedWidth, fixedHeight, requests;
        public Rect getSurfaceFrame() { return frame; }
        public void setFixedSize(int width, int height) { fixedWidth = width; fixedHeight = height; requests++; }
    }
""" + touch_method + "\n" + captured_method + "\n}\n",
                "org/libsdl/app/SDLGenericMotionListener_API12.java": """package org.libsdl.app;
import android.os.Build;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
""" + listeners,
                "org/libsdl/app/SDLControllerManager.java": """package org.libsdl.app;
import android.view.MotionEvent;
public final class SDLControllerManager {
    public static boolean handleJoystickMotionEvent(MotionEvent event) { return false; }
}
""",
                "app/zetta/heavenbench/ArgumentContract.java": """package app.zetta.heavenbench;
import android.content.Context;
import android.content.Intent;
import android.graphics.Rect;
import android.view.InputDevice;
import android.view.MotionEvent;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;
public final class ArgumentContract {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void close(float actual, float expected, String message) {
        require(Math.abs(actual - expected) < 0.00001f, message + ": " + actual + " != " + expected);
    }
    private static MotionEvent event(int source, int action, float x, float y) {
        return new MotionEvent(source, action, new int[] {7}, new float[] {x}, new float[] {y});
    }
    private static SDLSurface surface(HeavenActivity activity, File storage, String json) throws Exception {
        Files.createDirectories(storage.toPath());
        Files.write(new File(storage, "config.json").toPath(), json.getBytes(StandardCharsets.UTF_8));
        activity.setIntent(new Intent(storage.getPath(), false));
        SDLSurface surface = activity.createSDLSurface(new Context());
        SDLActivity.setSurface(surface); return surface;
    }
    private static void publish(SDLSurface surface, int w, int h) {
        surface.holder.frame = new Rect(w, h);
        surface.surfaceChanged(surface.getHolder(), 1, w, h);
    }
    public static void main(String[] args) throws Exception {
        HeavenActivity activity = new HeavenActivity();
        SDLActivity.setSurface(new Rect(3392, 2400));
        activity.setIntent(new Intent(null, true));
        require(activity.getArguments().length == 0, "missing storage must not invent renderer arguments");
        File storage = new File(args[0]);
        String[] expected = {"--android-user-dir", storage.getAbsolutePath(),
            "--config", new File(storage, "config.json").getAbsolutePath(),
            "--data", new File(storage, "assets.zip").getAbsolutePath(),
            "--fullscreen", "--width", "3392", "--height", "2400"};
        activity.setIntent(new Intent(args[0], false));
        String[] actual = activity.getArguments();
        require(Arrays.equals(actual, expected), "Explore bridge/renderer arguments: " + Arrays.toString(actual));
        require(Arrays.equals(Arrays.copyOfRange(actual, 2, actual.length),
                              Arrays.copyOfRange(expected, 2, expected.length)), "bridge removal");
        activity.setIntent(new Intent(args[0], true));
        String[] benchmark = activity.getArguments();
        require(benchmark.length == expected.length + 1 &&
                Arrays.equals(Arrays.copyOf(benchmark, expected.length), expected) &&
                benchmark[expected.length].equals("--benchmark"), "Benchmark preserves the same prefix");
        Intent requestedTrace = new Intent(null, false)
            .putExtra("microprofile", "tablet-capture.1")
            .putExtra("microprofile_seconds", "0.5")
            .putExtra("microprofile_visible", true);
        Intent forwardedTrace = new Intent(args[0], true);
        MicroprofileLaunch.forward(requestedTrace, forwardedTrace);
        activity.setIntent(forwardedTrace);
        String[] trace = activity.getArguments();
        require(Arrays.equals(Arrays.copyOf(trace, benchmark.length), benchmark),
            "trace extras preserve benchmark and exact storage/display prefix");
        require(Arrays.equals(Arrays.copyOfRange(trace, benchmark.length, trace.length), new String[] {
            "--microprofile", new File(new File(storage, "profiles"), "tablet-capture.1").getAbsolutePath(),
            "--microprofile-seconds", "0.5", "--microprofile-visible"}),
            "forwarded trace arguments stay in writable app profiles directory");
        Intent traceExplore = new Intent(args[0], false);
        MicroprofileLaunch.forward(requestedTrace, traceExplore);
        activity.setIntent(traceExplore);
        require(Arrays.equals(Arrays.copyOf(activity.getArguments(), expected.length), expected),
            "trace Explore preserves exact existing arguments");
        for (String numeric : new String[] {"NaN", "Infinity", "0.01", "3601", "--unknown"}) {
            Intent invalidSeconds = new Intent(args[0], false).putExtra("microprofile_seconds", numeric);
            activity.setIntent(invalidSeconds);
            String[] forwarded = activity.getArguments();
            require(forwarded[forwarded.length - 2].equals("--microprofile-seconds") &&
                forwarded[forwarded.length - 1].equals(numeric),
                "native CLI owns finite/range/dependency validation without Java coercion");
        }
        for (String unsafe : new String[] {"", ".", "..", "../escape", "dir/file", "/absolute", "dir\\file", "x" + (char)0}) {
            boolean rejected = false;
            try { MicroprofileLaunch.forward(new Intent(null, false).putExtra("microprofile", unsafe),
                    new Intent(args[0], false)); }
            catch (IllegalArgumentException error) { rejected = true; }
            require(rejected, "trace basename rejects path traversal and NUL");
        }
        activity.setIntent(new Intent(args[0], true));
        SDLActivity.setSurface(new Rect(0, 2400));
        require(Arrays.equals(activity.getArguments(), new String[] {
            "--android-user-dir", storage.getAbsolutePath(),
            "--config", new File(storage, "config.json").getAbsolutePath(),
            "--data", new File(storage, "assets.zip").getAbsolutePath(),
            "--fullscreen", "--benchmark"}), "nonpositive Surface must not override dimensions");
        SDLActivity.setSurface((SDLSurface) null);
        activity.setIntent(new Intent(args[0], false));
        require(activity.getArguments().length == 7, "unavailable Surface preserves the storage bridge");

        for (String json : new String[] {"{}", "malformed", "{\\\"android_render_scale\\\":\\\"0.375\\\"}",
                "{\\\"android_render_scale\\\":0.124}", "{\\\"android_render_scale\\\":1.001}",
                "{\\\"android_render_scale\\\":NaN}", "{\\\"android_render_scale\\\":Infinity}",
                "{\\\"android_render_scale\\\":1}"}) {
            SDLSurface nativeSurface = surface(activity, storage, json);
            require(nativeSurface.getClass() == SDLSurface.class, "invalid/default scale uses original SDL Surface: " + json);
            nativeSurface.layout(3392, 2400);
            require(nativeSurface.holder.requests == 0, "native default does not fix buffer size");
        }
        new File(storage, "config.json").delete();
        require(activity.createSDLSurface(new Context()).getClass() == SDLSurface.class, "missing config preserves native");
        SDLSurface low = surface(activity, storage, "{\\\"android_render_scale\\\":0.375}");
        low.layout(3392, 2400);
        require(low.holder.fixedWidth == 1272 && low.holder.fixedHeight == 900, "same-aspect lower buffer");
        publish(low, 3392, 2400);
        require(!low.mIsSurfaceReady && low.publishedResizes == 0, "intermediate native buffer cannot start SDL");
        publish(low, 1272, 900);
        require(low.mIsSurfaceReady && low.publishedResizes == 1, "actual scaled buffer starts SDL");
        String[] scaledArguments = activity.getArguments();
        require(scaledArguments[0].equals("--android-user-dir") && scaledArguments[8].equals("1272") &&
                scaledArguments[10].equals("900"), "startup/scoring uses actual buffer, not physical display");
        activity.setIntent(new Intent(args[0], true));
        require(activity.getArguments()[11].equals("--benchmark"), "scaled Benchmark uses actual dimensions");
        String configBytes = Files.readString(new File(storage, "config.json").toPath());
        require(configBytes.equals("{\\\"android_render_scale\\\":0.375}"), "launcher never rewrites config");
        MotionEvent fingers = new MotionEvent(InputDevice.SOURCE_TOUCHSCREEN, MotionEvent.ACTION_MOVE,
            new int[] {7, 11}, new float[] {1696, 848}, new float[] {1200, 600});
        SDLActivity.touches.clear(); low.onTouch(low, fingers);
        require(SDLActivity.touches.size() == 2, "all touch pointers forwarded");
        close(SDLActivity.touches.get(0)[2], .5f, "center touch x");
        close(SDLActivity.touches.get(0)[3], .5f, "center touch y");
        close(SDLActivity.touches.get(1)[2], .25f, "secondary touch x");
        close(SDLActivity.touches.get(1)[3], .25f, "secondary touch y");
        require(SDLActivity.touches.get(1)[0] == 11 && SDLActivity.touches.get(1)[4] == .75f, "pointer IDs/pressure preserved");
        close(fingers.getX(0), 1696, "original event untouched");
        require(!fingers.recycled && MotionEvent.lastCopy.recycled, "only mapped copy recycled");
        close(MotionEvent.lastCopy.historyX, fingers.historyX * .375f, "touch history mapped");
        SDLActivity.touches.clear();
        low.onTouch(low, new MotionEvent(InputDevice.SOURCE_TOUCHSCREEN,
            MotionEvent.ACTION_POINTER_DOWN | (1 << 8), new int[] {7, 11}, new float[] {1696, 848}, new float[] {1200, 600}));
        require(SDLActivity.touches.size() == 1 && SDLActivity.touches.get(0)[0] == 11, "pointer action index preserved");
        SDLActivity.touches.clear(); low.onTouch(low, new MotionEvent(InputDevice.SOURCE_TOUCHSCREEN,
            MotionEvent.ACTION_CANCEL, new int[] {7, 11}, new float[] {1696, 848}, new float[] {1200, 600}));
        require(SDLActivity.touches.size() == 2 && SDLActivity.touches.get(0)[1] == MotionEvent.ACTION_UP,
            "cancel releases all pointers");
        low.onTouch(low, event(InputDevice.SOURCE_MOUSE, MotionEvent.ACTION_DOWN, 1696, 1200));
        close(SDLActivity.mouse[2], 636, "absolute mouse touch x");
        close(SDLActivity.mouse[3], 450, "absolute mouse touch y");
        low.dispatchGeneric(event(InputDevice.SOURCE_MOUSE, MotionEvent.ACTION_HOVER_MOVE, 1696, 1200));
        close(SDLActivity.mouse[2], 636, "absolute mouse hover x");
        low.dispatchGeneric(event(InputDevice.SOURCE_MOUSE | InputDevice.SOURCE_TOUCHSCREEN,
            MotionEvent.ACTION_HOVER_MOVE, 1696, 1200));
        close(SDLActivity.mouse[3], 450, "DeX absolute hover y");
        low.dispatchGeneric(event(InputDevice.SOURCE_MOUSE, MotionEvent.ACTION_SCROLL, 1696, 1200));
        close(SDLActivity.mouse[2], 3, "wheel x unchanged"); close(SDLActivity.mouse[3], -2, "wheel y unchanged");
        low.dispatchGeneric(event(InputDevice.SOURCE_MOUSE_RELATIVE, MotionEvent.ACTION_HOVER_MOVE, 7, -9));
        close(SDLActivity.mouse[2], 7, "relative hover x unchanged"); require(SDLActivity.mouse[4] == 1, "relative flag");
        low.onCapturedPointerEvent(event(InputDevice.SOURCE_MOUSE_RELATIVE, MotionEvent.ACTION_MOVE, 7, -9));
        close(SDLActivity.mouse[3], -9, "captured relative y unchanged");
        low.requestPointerCapture();
        low.onTouch(low, event(InputDevice.SOURCE_MOUSE, MotionEvent.ACTION_DOWN, 7, -9));
        close(SDLActivity.mouse[2], 7, "captured button x unchanged");
        low.releasePointerCapture();
        low.layout(2400, 3392);
        require(low.holder.fixedWidth == 900 && low.holder.fixedHeight == 1272, "orientation requests swapped extent");
        publish(low, 900, 1272); SDLActivity.touches.clear();
        low.onTouch(low, event(InputDevice.SOURCE_TOUCHSCREEN, MotionEvent.ACTION_DOWN, 1200, 1696));
        close(SDLActivity.touches.get(0)[2], .5f, "rotated touch x"); close(SDLActivity.touches.get(0)[3], .5f, "rotated touch y");
        low.layout(3200, 1800);
        require(low.holder.fixedWidth == 1200 && low.holder.fixedHeight == 675, "resized lower buffer");
        SDLSurface minimum = surface(activity, storage, "{\\\"android_render_scale\\\":0.125}");
        minimum.layout(3392, 2400);
        require(minimum.holder.fixedWidth == 424 && minimum.holder.fixedHeight == 300, "minimum supported scale");
        System.out.println("PASS actual HeavenActivity bridge, scale, Surface lifecycle, SDL multi-touch and mouse contracts");
    }
}
""",
            }
            for relative, source in sources.items():
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(source)
            launcher = Path(__file__).parent / "app/src/main/java/app/zetta/heavenbench/HeavenActivity.java"
            destination = root / "app/zetta/heavenbench/HeavenActivity.java"
            shutil.copyfile(launcher, destination)
            trace_helper = launcher.with_name("MicroprofileLaunch.java")
            shutil.copyfile(trace_helper, destination.with_name(trace_helper.name))
            classes = root / "classes"
            command = [javac, "-Xlint:all,-auxiliaryclass,-fallthrough", "-Werror", "-d", str(classes),
                       *map(str, sorted(root.rglob("*.java")))]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            invocation = [java, "-cp", str(classes), "app.zetta.heavenbench.ArgumentContract",
                          str(root / "storage with spaces")]
            executed = subprocess.run(invocation, capture_output=True, text=True, timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            self.assertIn("PASS actual HeavenActivity", executed.stdout)

            # Missing buffer mapping must fail real pinned-SDL normalization:
            # the same physical center would otherwise become (1.333,1.333).
            source = destination.read_text()
            self.assertEqual(source.count("mapped.transform(transform);"), 1)
            destination.write_text(source.replace("mapped.transform(transform);", ""))
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            negative = subprocess.run(invocation, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(negative.returncode, 0, "unmapped coordinates must be rejected")
            self.assertIn("center touch x", negative.stderr)


if __name__ == "__main__":
    unittest.main()

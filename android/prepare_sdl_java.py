#!/usr/bin/env python3
"""Stage SDL 2.32.10 Java glue with explicit Android permission/receiver guards.

The downloaded SDL sources and native library remain unchanged. Check exact
upstream inputs before applying these small integration changes; a new SDL pin
requires reviewing its permission paths instead of silently reusing this patch.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile


SOURCE_HASHES = {
    "HIDDeviceBLESteamController.java": "78a07450afa0d7078496a219a32b4bce5c00671317bc228333e9bff5e3da500d",
    "HIDDeviceManager.java": "7f16fa997af31b70dabf136f5d6430f8f00437bfb0a6bc2457e9046e2fe34325",
    "SDLAudioManager.java": "27db11b8436dc4b8393241120ce9107b081203a27fa39c36efd42a4a7ec9af9d",
}
MARKER = ".heaven-sdl-java.json"


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"Expected exactly one SDL integration anchor: {before!r}")
    return text.replace(before, after, 1)


def bluetooth_guard(text, signature, failure, indent="        "):
    # Keep the check at each asynchronous entry point so a grant at native
    # initialization is never assumed to cover a later callback/queued command.
    guard = (
        f"\n{indent}if (SDL.getContext() == null || (Build.VERSION.SDK_INT >= 31 &&\n"
        f"{indent}    SDL.getContext().checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT)\n"
        f"{indent}        != android.content.pm.PackageManager.PERMISSION_GRANTED)) {{\n"
        f"{indent}    {failure}\n{indent}}}\n"
    )
    anchor = "\n" + signature
    return replace_once(text, anchor, anchor + guard)


def adapt(name, text):
    if name == "HIDDeviceBLESteamController.java":
        for signature, failure, indent in (
            ("        public void run() {", "mResult = false; return;", "            "),
            ("    private BluetoothGatt connectGatt(boolean managed) {", "return null;", "        "),
            ("    protected int getConnectionState() {", "return BluetoothProfile.STATE_DISCONNECTED;", "        "),
            ("    public void reconnect() {", "return;", "        "),
            ("    protected void checkConnectionForChromebookIssue() {", "return;", "        "),
            ("    private boolean probeService(HIDDeviceBLESteamController controller) {", "return false;", "        "),
            ("    public void onServicesDiscovered(BluetoothGatt gatt, int status) {", "return;", "        "),
            ("    public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status) {", "return;", "        "),
            ("                    public void run() {\n                        mGatt.discoverServices();", "return;", "                        "),
        ):
            if "mGatt.discoverServices" in signature:
                # The operation is queued: put the guard before the API call,
                # inside the Runnable rather than only in its caller.
                before = signature
                head = "                    public void run() {"
                guarded = bluetooth_guard("\n" + head, head, failure, indent).lstrip("\n")
                text = replace_once(text, before, guarded + "                        mGatt.discoverServices();")
            else:
                text = bluetooth_guard(text, signature, failure, indent)
        text = replace_once(text, "            g.disconnect();\n            g.close();", """            try {
                g.disconnect();
                g.close();
            } catch (SecurityException exception) {
                // Cleanup must still clear Java state if a permission was revoked.
                Log.w(TAG, "Bluetooth permission revoked during shutdown", exception);
            }""")
    elif name == "HIDDeviceManager.java":
        for signature, failure in (
            ("    public void chromebookConnectionHandler() {", "return;"),
            ("    public boolean isSteamController(BluetoothDevice bluetoothDevice) {", "return false;"),
        ):
            text = bluetooth_guard(text, signature, failure)
        text = replace_once(text, "        mContext.registerReceiver(mUsbBroadcast, filter);", """        // USB system events and our own PendingIntent require no external sender.
        if (Build.VERSION.SDK_INT >= 33) {
            mContext.registerReceiver(mUsbBroadcast, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            // Before API 33, a signature permission provides the same sender
            // restriction (the compatibility strategy used by AndroidX Core).
            // Lint does not recognize that manifest-backed permission guard.
            // Scope the flag-only warning to this one legacy API expression.
            @android.annotation.SuppressLint("UnspecifiedRegisterReceiverFlag")
            Intent unusedStickyIntent = mContext.registerReceiver(mUsbBroadcast, filter,
                mContext.getPackageName() + ".permission.USB_EVENTS", null);
        }""")
    elif name == "SDLAudioManager.java":
        signature = "    protected static int[] open(boolean isCapture, int sampleRate, int audioFormat, int desiredChannels, int desiredFrames, int deviceId) {"
        text = replace_once(text, signature, signature + """
        // SDL requests capture permission on demand in its native audio driver.
        // Recheck here before constructing AudioRecord; playback is unaffected.
        if (isCapture && (mContext == null || mContext.checkSelfPermission(
                android.Manifest.permission.RECORD_AUDIO)
                != android.content.pm.PackageManager.PERMISSION_GRANTED)) {
            return null;
        }
""")
    return ("// HeavenBench Android integration: generated permission and receiver guards.\n"
            "// Original SDL 2.32.10 input is retained; see android/prepare_sdl_java.py.\n" + text)


def prepare(source, output):
    directory = source / "android-project/app/src/main/java"
    package = directory / "org/libsdl/app"
    adapted = {}
    for name, expected in SOURCE_HASHES.items():
        content = (package / name).read_bytes()
        if hashlib.sha256(content).hexdigest() != expected:
            raise ValueError(f"Unreviewed SDL Java input: {name}; expected SDL 2.32.10")
        adapted[name] = adapt(name, content.decode("utf-8")).encode("utf-8")
    if output.exists() and any(output.iterdir()) and not (output / MARKER).is_file():
        raise ValueError(f"Refusing to replace an unmarked generated directory: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".sdl-java-", dir=output.parent))
    try:
        shutil.copytree(directory, temporary, dirs_exist_ok=True)
        for name, content in adapted.items():
            (temporary / "org/libsdl/app" / name).write_bytes(content)
        (temporary / MARKER).write_text(json.dumps({"upstream_sha256": SOURCE_HASHES}, indent=2) + "\n")
        if output.exists():
            shutil.rmtree(output)
        temporary.rename(output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        prepare(arguments.source.resolve(), arguments.output.resolve())
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Execute the actual Android SDL entry against host JNI/Filament stubs."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class NativeBridgeTest(unittest.TestCase):
    def test_sdl_owned_arguments_survive_renderer_and_cleanup(self):
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++")
        self.assertTrue(compiler, "The Android native bridge regression needs a host C++ compiler")
        source = Path(__file__).resolve().parent / "native/android_main.cpp"
        with tempfile.TemporaryDirectory(prefix="heaven-android-entry-") as temporary:
            root = Path(temporary)
            (root / "SDL2").mkdir()
            (root / "SDL2/SDL.h").write_text("#pragma once\n#define DECLSPEC\n")
            (root / "jni.h").write_text("#pragma once\n#define JNIEXPORT\n#define JNICALL\n#define JNI_ERR -1\nusing jint = int;\nstruct JavaVM {};\n")
            (root / "private/backend").mkdir(parents=True)
            (root / "private/backend/VirtualMachineEnv.h").write_text(
                '#pragma once\n#include <jni.h>\nnamespace filament { struct VirtualMachineEnv { '
                'static jint JNI_OnLoad(JavaVM*) { return 0x00010006; } }; }\n')
            test = root / "test.cpp"
            test.write_text(r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" int SDL_main(int, char**);
static std::vector<std::string> expected;
static bool mutate_renderer_array = false;
static int calls = 0;
static void require(bool value, const char* label) {
    if (!value) { std::fprintf(stderr, "FAILED: %s\n", label); std::exit(1); }
}
int heaven_main(int argc, char* argv[]) {
    ++calls;
    require(argc == static_cast<int>(expected.size()), "renderer argc");
    require(argv[argc] == nullptr, "renderer null terminator");
    for (int i = 0; i < argc; ++i)
        require(argv[i] && expected[i] == argv[i], "renderer argument order/value");
    // A C entry is allowed to reorder its own argument pointers. This must not
    // mutate the caller-owned JNI array that SDL later releases.
    if (mutate_renderer_array && argc > 1) argv[1] = argv[0];
    return 37;
}
static void trial(const std::string& directory, const std::vector<std::string>& flags, bool mutate) {
    std::vector<std::string> strings{"app_process", "--android-user-dir", directory};
    strings.insert(strings.end(), flags.begin(), flags.end());
    std::vector<char*> owned;
    for (const auto& value : strings) {
        owned.push_back(::strdup(value.c_str()));
        require(owned.back(), "argument allocation");
    }
    owned.push_back(nullptr);
    const auto original = owned;
    expected = {"app_process"};
    expected.insert(expected.end(), flags.begin(), flags.end());
    mutate_renderer_array = mutate;
    const int before = calls;
    require(SDL_main(static_cast<int>(strings.size()), owned.data()) == 37, "renderer exit propagated");
    require(calls == before + 1, "renderer called once");
    require(owned == original, "SDL-owned pointer array preserved");
    for (std::size_t i = 0; i < strings.size(); ++i)
        require(strings[i] == owned[i], "SDL-owned string preserved");
    require(directory == std::getenv("HEAVEN_USER_DIR"), "user path forwarded");
    require(directory + "/resources" == std::getenv("HEAVEN_RESOURCE_DIR"), "resource path forwarded");
    // Model nativeRunMain's original-argc cleanup after SDL_main returns. Every
    // allocation must still occur exactly once in the original pointer array.
    for (std::size_t i = 0; i < strings.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j)
            require(owned[i] != original[j], "no duplicate SDL cleanup pointer");
        std::free(owned[i]);
    }
}
int main(int argc, char* argv[]) {
    require(argc == 2, "test directory argument");
    const std::string directory = argv[1];
    trial(directory, {}, false);
    trial(directory, {"--config", "config.json", "--data", "assets.zip", "--benchmark"}, false);
    trial(directory, {"--config", "config.json", "--microprofile", "profiles/tablet", "--microprofile-seconds", "5.0", "--microprofile-visible"}, true);
    char program[] = "app_process";
    char invalid[] = "--wrong-bridge";
    char* malformed[]{program, invalid, nullptr};
    const int before = calls;
    require(SDL_main(2, malformed) == 1 && calls == before, "missing bridge fails before renderer");
    require(malformed[0] == program && malformed[1] == invalid && !malformed[2], "invalid caller ownership retained");
    std::puts("PASS actual SDL bridge, renderer arguments, caller ownership and original-argc cleanup");
}
''')
            binary = root / "native-bridge-test"
            subprocess.run([str(compiler), "-std=c++23", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root), str(source), str(test), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(root)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS actual SDL bridge", result.stdout)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()

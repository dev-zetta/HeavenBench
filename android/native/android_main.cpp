// SDL's Java activity looks up this unmangled entry in libheaven_benchmark.so.
// CMake renames the desktop main in main.cpp to heaven_main for Android only.
#include <SDL2/SDL.h>
#include <private/backend/VirtualMachineEnv.h>

#include <jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>
#include <unistd.h>

int heaven_main(int argc, char* argv[]);

// This app links native Filament directly, so filament_jni.so cannot register
// the JavaVM for us. Filament explicitly permits this private entry for native
// integrations; the OpenGL fallback's external-stream manager requires it even
// when the scene contains no external streams.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(
        JavaVM* vm, [[maybe_unused]] void* reserved) {
    try {
        return filament::VirtualMachineEnv::JNI_OnLoad(vm);
    } catch (...) {
        return JNI_ERR;
    }
}

extern "C" DECLSPEC int SDL_main(int argc, char* argv[]) try {
    // Java supplies this private bridge argument before normal renderer flags.
    if (argc < 3 || std::strcmp(argv[1], "--android-user-dir") != 0) {
        std::fprintf(stderr, "HeavenBench: missing Android storage directory\n");
        return 1;
    }
    if (setenv("HEAVEN_USER_DIR", argv[2], 1) != 0 || chdir(argv[2]) != 0) {
        std::perror("HeavenBench: Android storage directory");
        return 1;
    }
    const std::string resources = std::string(argv[2]) + "/resources";
    if (setenv("HEAVEN_RESOURCE_DIR", resources.c_str(), 1) != 0) {
        std::perror("HeavenBench: Android resource directory");
        return 1;
    }
    // SDL owns every original argument and frees them after this call returns.
    // Give the renderer a separate pointer array so removing our bridge flags
    // cannot duplicate pointers in SDL's cleanup loop.
    std::vector<char*> renderer_argv;
    renderer_argv.reserve(static_cast<std::size_t>(argc - 1));
    renderer_argv.push_back(argv[0]);
    renderer_argv.insert(renderer_argv.end(), argv + 3, argv + argc + 1);
    return heaven_main(argc - 2, renderer_argv.data());
} catch (const std::exception& error) {
    std::fprintf(stderr, "HeavenBench: Android startup failed: %s\n", error.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "HeavenBench: Android startup failed\n");
    return 1;
}

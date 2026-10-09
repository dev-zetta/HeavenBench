#ifndef HEAVENBENCH_TESTS_STDIO_COMPAT_H_
#define HEAVENBENCH_TESTS_STDIO_COMPAT_H_

#include <cstdio>

#ifdef _WIN32
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

// Test-only descriptor adapters: capture the same C FILE streams on both CRTs.
namespace heaven::test {

inline std::FILE* temporary_file() {
#ifdef _WIN32
    // MSVC tmpfile uses the drive root, which is not writable by ordinary
    // users. Exclusively create in the user temp directory and let the CRT
    // delete the file when its last descriptor closes, including redirects.
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error) return nullptr;
    static std::atomic<unsigned int> serial{0};
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto path = directory / ("heaven-test-" + std::to_string(::_getpid()) +
            "-" + std::to_string(serial.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
        int descriptor = -1;
        const errno_t result = ::_wsopen_s(&descriptor, path.c_str(),
            _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_TEMPORARY,
            _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (result == EEXIST) continue;
        if (result != 0) return nullptr;
        std::FILE* stream = ::_fdopen(descriptor, "w+b");
        if (!stream) ::_close(descriptor);
        return stream;
    }
    return nullptr;
#else
    return std::tmpfile();
#endif
}

inline int file_descriptor(std::FILE* stream) {
#ifdef _WIN32
    return ::_fileno(stream);
#else
    return ::fileno(stream);
#endif
}

inline int duplicate_fd(int descriptor) {
#ifdef _WIN32
    return ::_dup(descriptor);
#else
    return ::dup(descriptor);
#endif
}

inline int duplicate_to_fd(int source, int destination) {
#ifdef _WIN32
    return ::_dup2(source, destination);
#else
    return ::dup2(source, destination);
#endif
}

inline int close_fd(int descriptor) {
#ifdef _WIN32
    return ::_close(descriptor);
#else
    return ::close(descriptor);
#endif
}

} // namespace heaven::test

#endif // HEAVENBENCH_TESTS_STDIO_COMPAT_H_

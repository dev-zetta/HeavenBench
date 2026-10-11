#ifndef HEAVENBENCH_SRC_RHI_MACOS_SURFACE_H_
#define HEAVENBENCH_SRC_RHI_MACOS_SURFACE_H_

struct SDL_Window;

namespace heaven {

// Filament's Apple Vulkan platform borrows a CAMetalLayer. Keep its SDL view
// alive until the swap chain and engine have finished using that layer.
class MacOsSurface final {
public:
    explicit MacOsSurface(SDL_Window* window);
    ~MacOsSurface();
    MacOsSurface(const MacOsSurface&) = delete;
    MacOsSurface& operator=(const MacOsSurface&) = delete;

    [[nodiscard]] void* native_layer() const;

private:
    void* view_ = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_MACOS_SURFACE_H_

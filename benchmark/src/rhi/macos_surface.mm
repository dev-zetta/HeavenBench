#include "rhi/macos_surface.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace heaven {

MacOsSurface::MacOsSurface(SDL_Window* window) {
    // SDL owns the Cocoa view, its resize event watch, and the layer reference.
    // This converts the window's Vulkan flag to Metal after Filament has loaded
    // Vulkan independently. Both drivers still render through MoltenVK.
    view_ = SDL_Metal_CreateView(window);
}

MacOsSurface::~MacOsSurface() {
    if (view_) SDL_Metal_DestroyView(view_);
}

void* MacOsSurface::native_layer() const {
    if (!view_) return nullptr;
    @autoreleasepool {
        auto* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view_);
        if (!layer || ![layer isKindOfClass:[CAMetalLayer class]]) {
            SDL_SetError("SDL did not provide a CAMetalLayer for the Vulkan surface");
            return nullptr;
        }

        // Refresh from Cocoa's actual backing extent before swap-chain creation
        // or recreation. SDL logical dimensions are points on Retina displays.
        auto* view = (__bridge NSView*)view_;
        const NSSize points = view.bounds.size;
        const NSSize pixels = [view convertSizeToBacking:points];
        if (points.width > 0.0 && points.height > 0.0) {
            layer.contentsScale = pixels.height / points.height;
            layer.drawableSize = NSSizeToCGSize(pixels);
        }
        layer.opaque = YES;
        return (__bridge void*)layer;
    }
}

} // namespace heaven

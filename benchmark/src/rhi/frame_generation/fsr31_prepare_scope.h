#pragma once

struct FfxInterface;

// Private seam in the generated backend copy, not the fetched SDK interface.
// Return the previous policy so scoped cleanup is also correct under nesting.
extern "C" bool heavenFsr31VkDeferViewRotation(FfxInterface* backend, bool defer) noexcept;

namespace heaven {

// FI Prepare and Dispatch belong to one provider record. Prepare must restore
// borrowed image layouts and resource indices without retiring its live views.
// Final Dispatch performs the record's single ring advance. This scope never
// marks GPU work complete, and does not permit retrying a partial SDK record.
class Fsr31PrepareViewScope final {
public:
    explicit Fsr31PrepareViewScope(FfxInterface& backend) noexcept
        : backend_(&backend), was_deferred_(heavenFsr31VkDeferViewRotation(backend_, true)) {}
    ~Fsr31PrepareViewScope() noexcept {
        heavenFsr31VkDeferViewRotation(backend_, was_deferred_);
    }
    Fsr31PrepareViewScope(const Fsr31PrepareViewScope&) = delete;
    Fsr31PrepareViewScope& operator=(const Fsr31PrepareViewScope&) = delete;
    Fsr31PrepareViewScope(Fsr31PrepareViewScope&&) = delete;
    Fsr31PrepareViewScope& operator=(Fsr31PrepareViewScope&&) = delete;

private:
    FfxInterface* backend_;
    bool was_deferred_;
};

} // namespace heaven

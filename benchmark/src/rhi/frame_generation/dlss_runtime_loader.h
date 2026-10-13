#pragma once

#include <filesystem>
#include <string>

namespace heaven {

// Internal platform boundary, so SDK integration code can be compiled against
// the real headers without a Windows SDK on the host used for contract checks.
class DlssRuntimeLoader final {
public:
    using Function = void (*)();
    ~DlssRuntimeLoader();
    bool load(const std::filesystem::path& absolute_directory, std::string& reason);
    Function find_export(const char* name) const noexcept;
    bool loaded() const noexcept { return module_ != nullptr; }
private:
    void* module_ = nullptr;
};

} // namespace heaven

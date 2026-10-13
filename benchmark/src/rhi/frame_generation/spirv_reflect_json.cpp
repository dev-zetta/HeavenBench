#include <spirv_reflect.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

// Use the same upstream reflection library and descriptor classification as
// FidelityFX_SC, without that tool's Windows-only process/compiler dependencies.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    SpvReflectShaderModule module{};
    if (bytes.empty() || spvReflectCreateShaderModule(bytes.size(), bytes.data(), &module) != SPV_REFLECT_RESULT_SUCCESS) return 3;
    uint32_t count = 0;
    if (spvReflectEnumerateDescriptorBindings(&module, &count, nullptr) != SPV_REFLECT_RESULT_SUCCESS) return 4;
    std::vector<SpvReflectDescriptorBinding*> bindings(count);
    if (spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data()) != SPV_REFLECT_RESULT_SUCCESS) return 5;
    std::cout << '[';
    bool first = true;
    for (const auto* b : bindings) {
        const char* category = nullptr;
        switch (b->descriptor_type) {
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER: category = "cbv"; break;
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE: category = "srvTexture"; break;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE: category = "uavTexture"; break;
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER: category = "sampler"; break;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                category = b->resource_type == SPV_REFLECT_RESOURCE_FLAG_SRV ? "srvBuffer" : "uavBuffer";
                break;
            default: spvReflectDestroyShaderModule(&module); return 6;
        }
        const std::string name = b->name ? b->name : "";
        // Shader identifier names are not arbitrary JSON strings.
        if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) return 7;
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"category\":\"" << category << "\",\"name\":\"" << name
                  << "\",\"binding\":" << b->binding << ",\"count\":" << b->count
                  << ",\"set\":" << b->set << '}';
    }
    std::cout << "]\n";
    spvReflectDestroyShaderModule(&module);
}

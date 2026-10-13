#!/usr/bin/env python3
"""Apply pinned SDK setup cleanup and provider-owned image-view retirement.

Only a build-directory copy may be supplied. Exact matches intentionally reject
SDK drift; the fetched source and its integrity manifest remain untouched.
"""

import argparse
from pathlib import Path


def replace_once(source: str, before: str, after: str) -> str:
    if source.count(before) != 1:
        raise ValueError(f"Pinned SDK pipeline contract changed: {before[:100]!r}")
    return source.replace(before, after, 1)


def transactional_pipeline(source: str) -> str:
    begin = source.index("FfxErrorCode CreatePipelineVK(FfxInterface* backendInterface,\n")
    end = source.index("\nFfxErrorCode DestroyPipelineVK(", begin)
    function = source[begin:end]
    declaration = "    BackendContext_VK::PipelineLayout* pPipelineLayout = &backendContext->pPipelineLayouts[effectContext.nextPipelineLayout++];"
    cleanup = declaration + """

    // HeavenBench: keep every partial pipeline allocation owned until commit.
    // No GPU work references these objects while pipeline creation is running.
    *pPipelineLayout = {};
    *outPipeline = {};
    struct PipelineCreationCleanup {
        BackendContext_VK* backend;
        BackendContext_VK::PipelineLayout* layout;
        FfxPipelineState* output;
        uint32_t* nextLayout;
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        VkPipeline computePipeline = VK_NULL_HANDLE;
        bool committed = false;
        ~PipelineCreationCleanup() {
            auto& vk = backend->vkFunctionTable;
            if (shaderModule) vk.vkDestroyShaderModule(backend->device, shaderModule, nullptr);
            if (committed) return;
            if (computePipeline) vk.vkDestroyPipeline(backend->device, computePipeline, nullptr);
            if (layout->pipelineLayout) vk.vkDestroyPipelineLayout(backend->device, layout->pipelineLayout, nullptr);
            for (auto& set : layout->descriptorSets)
                if (set) vk.vkFreeDescriptorSets(backend->device, backend->descriptorPool, 1, &set);
            if (layout->descriptorSetLayout) vk.vkDestroyDescriptorSetLayout(backend->device, layout->descriptorSetLayout, nullptr);
            for (auto sampler : layout->samplers)
                if (sampler) vk.vkDestroySampler(backend->device, sampler, nullptr);
            *layout = {};
            *output = {};
            --*nextLayout;
        }
    } cleanup{backendContext, pPipelineLayout, outPipeline, &effectContext.nextPipelineLayout};
"""
    function = replace_once(function, declaration, cleanup)
    function = replace_once(function, "\n    VkShaderModule shaderModule = VK_NULL_HANDLE;", "\n    VkShaderModule& shaderModule = cleanup.shaderModule;")
    function = replace_once(function, "\n    VkPipeline computePipeline = VK_NULL_HANDLE;", "\n    VkPipeline& computePipeline = cleanup.computePipeline;")
    function = replace_once(function,
        "    // done with shader module, so clean up\n    backendContext->vkFunctionTable.vkDestroyShaderModule(backendContext->device, shaderModule, nullptr);",
        "    // The transaction guard releases the shader module on every exit.")
    function = replace_once(function, "    return FFX_OK;\n}", "    cleanup.committed = true;\n    return FFX_OK;\n}")
    return source[:begin] + function + source[end:]


def transactional_backend(source: str) -> str:
    begin = source.index("FfxErrorCode CreateBackendContextVK(FfxInterface* backendInterface, FfxEffect effect, FfxEffectBindlessConfig* bindlessConfig, FfxUInt32* effectContextId)\n{")
    end = source.index("\nFfxErrorCode GetDeviceCapabilitiesVK(", begin)
    function = source[begin:end]
    constructor = "        new (&backendContext->uniformBufferMutex) std::mutex();"
    function = replace_once(function, constructor, constructor + """

        // HeavenBench: no effect context owns globals until first setup commits.
        struct BackendCreationCleanup {
            BackendContext_VK* backend;
            bool committed = false;
            ~BackendCreationCleanup() {
                if (committed) return;
                auto& vk = backend->vkFunctionTable;
                if (backend->uniformBufferMem) vk.vkUnmapMemory(backend->device, backend->uniformBufferMemory);
                if (backend->uniformBuffer) vk.vkDestroyBuffer(backend->device, backend->uniformBuffer, nullptr);
                if (backend->uniformBufferMemory) vk.vkFreeMemory(backend->device, backend->uniformBufferMemory, nullptr);
                if (backend->descriptorPool) vk.vkDestroyDescriptorPool(backend->device, backend->descriptorPool, nullptr);
                backend->uniformBufferMutex.~mutex();
                resetBackendContext(backend);
            }
        } globalCleanup{backendContext};
""")
    function = replace_once(function,
        "    }\n\n    // Increment the ref count",
        "        globalCleanup.committed = true;\n    }\n\n    // Increment the ref count")
    return source[:begin] + function + source[end:]


def validate_resource_upload(source: str) -> str:
    call = "        backendInterface->fpCreateResource(backendInterface, &uploadDesc, effectContextId, &copySrc);"
    return replace_once(source, call,
        "        FFX_VALIDATE(backendInterface->fpCreateResource(backendInterface, &uploadDesc, effectContextId, &copySrc));")



def provider_record_views(source: str) -> str:
    # FI Prepare and Dispatch unregister the same effect context independently.
    # Keep resource/layout cleanup, but advance views only at final Dispatch.
    source = replace_once(source,
        "    uint32_t                numDeviceExtensions = 0;",
        "    // HeavenBench: FI Prepare and Dispatch share one provider-owned view slot.\n"
        "    bool                    heavenDeferViewRotation = false;\n\n"
        "    uint32_t                numDeviceExtensions = 0;")
    source = replace_once(source,
        "// dispose dynamic resources: This should be called at the end of the frame",
        "// Private provider seam; does not change the fetched SDK or public SDK ABI.\n"
        'extern "C" bool heavenFsr31VkDeferViewRotation(FfxInterface* backendInterface, bool defer) noexcept\n'
        "{\n"
        "    auto* backend = static_cast<BackendContext_VK*>(backendInterface->scratchBuffer);\n"
        "    const bool previous = backend->heavenDeferViewRotation;\n"
        "    backend->heavenDeferViewRotation = defer;\n"
        "    return previous;\n"
        "}\n\n"
        "// dispose dynamic resources: This should be called at the end of the frame")
    return replace_once(source,
        "    // destroy the views of the next frame\n"
        "    effectContext.frameIndex = (effectContext.frameIndex + 1) % FFX_MAX_QUEUED_FRAMES;\n"
        "    destroyDynamicViews(backendContext, effectContextId, effectContext.frameIndex);",
        "    // Rotate exactly once per provider record, after its final FI Dispatch.\n"
        "    // Prepare still unregisters resources and restores borrowed image layouts.\n"
        "    if (!backendContext->heavenDeferViewRotation) {\n"
        "        effectContext.frameIndex = (effectContext.frameIndex + 1) % FFX_MAX_QUEUED_FRAMES;\n"
        "        destroyDynamicViews(backendContext, effectContextId, effectContext.frameIndex);\n"
        "    }")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = provider_record_views(validate_resource_upload(transactional_backend(transactional_pipeline(args.input.read_text()))))
    args.output.write_text(result)


if __name__ == "__main__":
    main()

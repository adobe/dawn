#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/log.h>

#include "dawn/webgpu.h"
#include "dawn/webgpu_cpp.h"
#include "dawn/native/DawnNative.h"


// Access internal JNI context helpers
#include "JNIContext.h" 

namespace {
    const char* kLogTag = "DawnManualJNI";
}

extern "C" {

JNIEXPORT jboolean JNICALL Java_androidx_webgpu_helper_TexturesUtils_blitHardwareBufferToTexture(
    JNIEnv* env, jclass clazz, jobject deviceObj, jobject hardwareBufferObj, jobject destTextureObj) {
    
    if (hardwareBufferObj == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "HardwareBuffer object is null");
        return JNI_FALSE;
    }

    if (deviceObj == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device object is null");
        return JNI_FALSE;
    }

    if (destTextureObj == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Destination texture object is null");
        return JNI_FALSE;
    }

    // Get the native WGPUDevice from the Kotlin object
    jclass deviceClass = env->GetObjectClass(deviceObj);
    jmethodID getHandleMethod = env->GetMethodID(deviceClass, "getHandle", "()J");
    if (getHandleMethod == nullptr) {
         __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not find getHandle method on GPUDevice");
         return JNI_FALSE;
    }

    // Get the native WGPUTexture from the destination texture object
    jclass textureClass = env->GetObjectClass(destTextureObj);
    jmethodID getTextureHandleMethod = env->GetMethodID(textureClass, "getHandle", "()J");
    if (getTextureHandleMethod == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not find getHandle method on GPUTexture");
        return JNI_FALSE;
    }

    jlong deviceHandle = env->CallLongMethod(deviceObj, getHandleMethod);
    jlong destTextureHandle = env->CallLongMethod(destTextureObj, getTextureHandleMethod);
    WGPUDevice rawDevice = reinterpret_cast<WGPUDevice>(deviceHandle);
    wgpu::Device device(rawDevice);
    if (!device) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create device from handle");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created device from handle: %p", device.Get());

    // Wrap the destination texture handle (don't take ownership - Kotlin owns it)
    WGPUTexture rawDestTexture = reinterpret_cast<WGPUTexture>(destTextureHandle);
    wgpu::Texture destTexture(rawDestTexture);
    if (!destTexture) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to get destination texture from handle");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Using destination texture: %p", destTexture.Get());
    
    // Check if the device has the required feature enabled.
    // Also check backend type
    wgpu::Adapter adapter = device.GetAdapter();
    if (!adapter) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not get adapter from device!");
        return JNI_FALSE;        
    }    

    wgpu::AdapterInfo info;
    adapter.GetInfo(&info);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Device Backend: %d (Vulkan=%d, GLES=%d)", 
        info.backendType, WGPUBackendType_Vulkan, WGPUBackendType_OpenGLES);

    bool hasRequiredFeatures = true;
    if (!device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer)) {
        hasRequiredFeatures = false;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature SharedTextureMemoryAHardwareBuffer enabled!");
    }
    if (!device.HasFeature(wgpu::FeatureName::YCbCrVulkanSamplers)) {
        hasRequiredFeatures = false;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature YCbCrVulkanSamplers enabled!");
    }
    if (!device.HasFeature(wgpu::FeatureName::StaticSamplers)) {
        hasRequiredFeatures = false;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature StaticSamplers enabled!");
    }
    if (!device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD)) {
        hasRequiredFeatures = false;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature SharedFenceSyncFD enabled!");
    }

    if(!hasRequiredFeatures) {
        // Log available features to help debug
        wgpu::SupportedFeatures supportedFeatures;
        device.GetFeatures(&supportedFeatures);
        
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "Device has %zu enabled features:", supportedFeatures.featureCount);
        for (size_t i = 0; i < supportedFeatures.featureCount; ++i) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag, " - Feature: 0x%X", supportedFeatures.features[i]);
        }
        return JNI_FALSE;
   }

    AHardwareBuffer* buffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBufferObj);
    if (buffer == nullptr) {
         __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to acquire AHardwareBuffer from HardwareBuffer object");
         return JNI_FALSE;
    }

    // Acquire the buffer to ensure it stays valid while we are using it
    AHardwareBuffer_acquire(buffer);

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);

    __android_log_print(ANDROID_LOG_INFO, kLogTag, 
        "Acquired AHardwareBuffer: %p. Width: %u, Height: %u, Layers: %u, Format: %u, Usage: %llu, Stride: %u", 
        buffer, desc.width, desc.height, desc.layers, desc.format, (unsigned long long)desc.usage, desc.stride);

    // Create SharedTextureMemory
    wgpu::SharedTextureMemoryAHardwareBufferDescriptor stmAHardwareBufferDesc = {};
    stmAHardwareBufferDesc.handle = buffer;    
    // This PoC just needs to ensure it works for External YUV formats
    stmAHardwareBufferDesc.useExternalFormat = true;

    // Use C API for the Device operations to avoid ownership issues with the passed Device.
    wgpu::SharedTextureMemoryDescriptor cStmDesc = {};
    cStmDesc.nextInChain = &stmAHardwareBufferDesc;
    cStmDesc.label = { "ManualJNI_SharedTextureMemory", WGPU_STRLEN };

    wgpu::SharedTextureMemory memory = device.ImportSharedTextureMemory(&cStmDesc);
        
    if (!memory) {
         __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create SharedTextureMemory (C API returned null)");
         return JNI_FALSE;
    }

    wgpu::SharedTextureMemoryProperties properties = {};
    wgpu::SharedTextureMemoryAHardwareBufferProperties ahbProperties = {};
    properties.nextInChain = &ahbProperties;
    memory.GetProperties(&properties);

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "SharedTextureMemory Properties: Format=%d, Usage=%lu, Size=%dx%d", 
            properties.format, (unsigned long)properties.usage, properties.size.width, properties.size.height);

    // If properties are empty (Format=0, Usage=0), it means ImportSharedTextureMemory failed to reflect properties from AHB.
    // This implies that AHB functions failed or properties were invalid.
    if (properties.format == wgpu::TextureFormat::Undefined) {
         __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SharedTextureMemory has Undefined format. Import likely failed internally. Aborting.");
         return JNI_FALSE;
    }
    wgpu::SamplerDescriptor samplerDesc = {};
    samplerDesc.nextInChain = &ahbProperties.yCbCrInfo;
    
    wgpu::Sampler yCbCrSampler = device.CreateSampler(&samplerDesc);    
    if(!yCbCrSampler) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create YCbCr sampler");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created YCbCr sampler: %p", yCbCrSampler.Get());
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "  YCbCr Info: vkFormat=%u, externalFormat=%llu",
        ahbProperties.yCbCrInfo.vkFormat,
        (unsigned long long)ahbProperties.yCbCrInfo.externalFormat);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "  YCbCr Model=%u, Range=%u, ChromaFilter=%d",
        ahbProperties.yCbCrInfo.vkYCbCrModel,
        ahbProperties.yCbCrInfo.vkYCbCrRange,
        static_cast<int>(ahbProperties.yCbCrInfo.vkChromaFilter));
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "  Chroma Offset: X=%u, Y=%u",
        ahbProperties.yCbCrInfo.vkXChromaOffset,
        ahbProperties.yCbCrInfo.vkYChromaOffset);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "  Component Swizzle: R=%u, G=%u, B=%u, A=%u",
        ahbProperties.yCbCrInfo.vkComponentSwizzleRed,
        ahbProperties.yCbCrInfo.vkComponentSwizzleGreen,
        ahbProperties.yCbCrInfo.vkComponentSwizzleBlue,
        ahbProperties.yCbCrInfo.vkComponentSwizzleAlpha);
    
    // Create BindGroupLayout
    // For YCbCr, we use a static sampler with YCbCr descriptor.
    // The shader uses texture_2d<f32> - the YCbCr conversion happens in the sampler.
    // Note: For static samplers, the sampler is NOT passed in the bind group entries.
    // Instead, it's baked into the bind group layout.
    std::vector<wgpu::BindGroupLayoutEntry> bindGroupLayoutEntries(2);
    
    wgpu::StaticSamplerBindingLayout staticSamplerBinding = {};
    staticSamplerBinding.sampler = yCbCrSampler;
    staticSamplerBinding.sampledTextureBinding = 1;

    // Binding 0: Static sampler (visible to fragment shader)
    bindGroupLayoutEntries[0].binding = 0;
    bindGroupLayoutEntries[0].visibility = wgpu::ShaderStage::Fragment;
    bindGroupLayoutEntries[0].nextInChain = &staticSamplerBinding;

    // Binding 1: Texture (visible to fragment shader)
    bindGroupLayoutEntries[1].binding = 1;
    bindGroupLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
    bindGroupLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    bindGroupLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    bindGroupLayoutEntries[1].texture.multisampled = false;

    wgpu::BindGroupLayoutDescriptor layoutDesc = {};
    layoutDesc.entryCount = bindGroupLayoutEntries.size();
    layoutDesc.entries = bindGroupLayoutEntries.data();

    wgpu::BindGroupLayout bindGroupLayout = device.CreateBindGroupLayout(&layoutDesc);
    if(!bindGroupLayout) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create BindGroupLayout");
        return JNI_FALSE;
    }
    
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created BindGroupLayout: %p", bindGroupLayout.Get());
    

    // Create Texture to hold the YCbCr data
    wgpu::TextureDescriptor descriptor = {};
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.size.width = desc.width;
    descriptor.size.height = desc.height;
    descriptor.size.depthOrArrayLayers = 1u;
    descriptor.sampleCount = 1u;
    descriptor.format = wgpu::TextureFormat::External;
    descriptor.mipLevelCount = 1u;
    descriptor.usage = wgpu::TextureUsage::TextureBinding;

    wgpu::Texture texture = memory.CreateTexture(&descriptor);
    AHardwareBuffer_release(buffer);

    if(!texture) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create Texture");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created Texture: %p", texture.Get());
    

    // --- Begin Access to the shared texture memory BEFORE creating texture view ---
    wgpu::SharedTextureMemoryVkImageLayoutBeginState vkBeginState = {};
    vkBeginState.oldLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED
    vkBeginState.newLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED

    wgpu::SharedTextureMemoryBeginAccessDescriptor beginAccessDesc = {};
    beginAccessDesc.nextInChain = &vkBeginState;
    beginAccessDesc.initialized = true;
    beginAccessDesc.fenceCount = 0;

    wgpu::Status status = memory.BeginAccess(texture, &beginAccessDesc);
    if (status != wgpu::Status::Success) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to BeginAccess status=%d", (int)status);
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "BeginAccess successful!");
   
    // Copy the entire YCbCrVkDescriptor - external formats use externalFormat, not vkFormat
    wgpu::YCbCrVkDescriptor yCbCrDescTex = ahbProperties.yCbCrInfo;
    
    wgpu::TextureViewDescriptor textureViewDesc = {};
    textureViewDesc.format = wgpu::TextureFormat::External;
    textureViewDesc.dimension = wgpu::TextureViewDimension::e2D;
    textureViewDesc.baseMipLevel = 0;
    textureViewDesc.mipLevelCount = 1u;
    textureViewDesc.baseArrayLayer = 0;
    textureViewDesc.arrayLayerCount = 1u;
    textureViewDesc.nextInChain = &yCbCrDescTex;

    wgpu::TextureView textureView = texture.CreateView(&textureViewDesc);
    if(!textureView) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create TextureView");
        return JNI_FALSE;
    }
    else {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created TextureView: %p", textureView.Get());
    }
    
    // Only pass the texture view - the sampler is static (baked into the layout)
    std::vector<wgpu::BindGroupEntry> bindGroupEntries(1);
    bindGroupEntries[0].binding = 1;
    bindGroupEntries[0].textureView = textureView;

    wgpu::BindGroupDescriptor bindGroupDescriptor = {};
    bindGroupDescriptor.layout = bindGroupLayout;
    bindGroupDescriptor.entryCount = bindGroupEntries.size();
    bindGroupDescriptor.entries = bindGroupEntries.data();
    wgpu::BindGroup bindGroup = device.CreateBindGroup(&bindGroupDescriptor);
    if(!bindGroup) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create BindGroup");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created BindGroup: %p", bindGroup.Get());
    

    // --- Create Shader Module for YCbCr blit ---
    // With static samplers, the sampler is declared in WGSL but bound via the layout.
    // The YCbCr conversion happens automatically in the sampler.
    const char* blitShaderSource = R"(
        @group(0) @binding(0) var mySampler : sampler;
        @group(0) @binding(1) var myTexture : texture_2d<f32>;

        struct VertexOutput {
            @builtin(position) position : vec4f,
            @location(0) texCoord : vec2f,
        }

        @vertex
        fn vs_main(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
            // Full-screen triangle (3 vertices, no vertex buffer needed)
            var pos = array<vec2f, 3>(
                vec2f(-1.0, -1.0),
                vec2f( 3.0, -1.0),
                vec2f(-1.0,  3.0)
            );
            var uv = array<vec2f, 3>(
                vec2f(0.0, 1.0),
                vec2f(2.0, 1.0),
                vec2f(0.0, -1.0)
            );
            var output : VertexOutput;
            output.position = vec4f(pos[vertexIndex], 0.0, 1.0);
            output.texCoord = uv[vertexIndex];
            return output;
        }

        @fragment
        fn fs_main(@location(0) texCoord : vec2f) -> @location(0) vec4f {
            return textureSample(myTexture, mySampler, texCoord);
        }
    )";

    wgpu::ShaderSourceWGSL wgslDesc;
    wgslDesc.code = blitShaderSource;
    wgpu::ShaderModuleDescriptor shaderModuleDesc = {};
    shaderModuleDesc.nextInChain = &wgslDesc;
    wgpu::ShaderModule shaderModule = device.CreateShaderModule(&shaderModuleDesc);
    if (!shaderModule) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create shader module");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created shader module: %p", shaderModule.Get());

    // --- Create Pipeline Layout using our bind group layout ---
    wgpu::PipelineLayoutDescriptor pipelineLayoutDesc = {};
    pipelineLayoutDesc.bindGroupLayoutCount = 1;
    pipelineLayoutDesc.bindGroupLayouts = &bindGroupLayout;
    wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDesc);
    if (!pipelineLayout) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create pipeline layout");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created pipeline layout: %p", pipelineLayout.Get());

    // --- Create Render Pipeline ---
    wgpu::ColorTargetState colorTarget = {};
    colorTarget.format = wgpu::TextureFormat::RGBA8Unorm;
    colorTarget.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState fragmentState = {};
    fragmentState.module = shaderModule;
    fragmentState.entryPoint = "fs_main";
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;

    wgpu::RenderPipelineDescriptor pipelineDesc = {};
    pipelineDesc.layout = pipelineLayout;
    pipelineDesc.vertex.module = shaderModule;
    pipelineDesc.vertex.entryPoint = "vs_main";
    pipelineDesc.fragment = &fragmentState;
    pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipelineDesc.multisample.count = 1;
    pipelineDesc.multisample.mask = 0xFFFFFFFF;

    wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipelineDesc);
    if (!pipeline) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create render pipeline");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created render pipeline: %p", pipeline.Get());

    // --- Execute the blit render pass ---
    // (BeginAccess was already called earlier, right after texture creation)
    wgpu::TextureView destView = destTexture.CreateView();
    if (!destView) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create destination texture view");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created destination texture view: %p", destView.Get());
    
    wgpu::RenderPassColorAttachment colorAttachment = {};
    colorAttachment.view = destView;
    colorAttachment.loadOp = wgpu::LoadOp::Clear;
    colorAttachment.storeOp = wgpu::StoreOp::Store;
    colorAttachment.clearValue = {0.0, 1.0, 0.0, 1.0}; // Green = blit didn't run

    wgpu::RenderPassDescriptor renderPassDesc = {};
    renderPassDesc.colorAttachmentCount = 1;
    renderPassDesc.colorAttachments = &colorAttachment;

    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    if (!encoder) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create command encoder");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created command encoder: %p", encoder.Get());

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&renderPassDesc);
    if (!pass) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to begin render pass");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Began render pass: %p", pass.Get());

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Setting pipeline: %p", pipeline.Get());
    pass.SetPipeline(pipeline);
    
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Setting bind group: %p (layout: %p)", bindGroup.Get(), bindGroupLayout.Get());
    pass.SetBindGroup(0, bindGroup);
    
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Drawing 3 vertices...");
    pass.Draw(3); // Full-screen triangle
    
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Ending render pass...");
    pass.End();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Render pass ended");

    // Tick device to process any pending errors before Finish
    // device.Tick();
    // __android_log_print(ANDROID_LOG_INFO, kLogTag, "Device ticked, calling encoder.Finish()...");
    
    wgpu::CommandBuffer commands = encoder.Finish();
    if (!commands) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to finish command encoder");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Command buffer created: %p", commands.Get());

    device.GetQueue().Submit(1, &commands);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Blit render pass submitted!");
    
    // Note: No explicit wait needed here. Dawn handles internal synchronization on the same device.
    // The fences from EndAccess are only needed when passing to a different device.

    // --- End Access to the shared texture memory ---
    wgpu::SharedTextureMemoryVkImageLayoutEndState vkEndState = {};
    wgpu::SharedTextureMemoryEndAccessState endAccessState = {};
    endAccessState.nextInChain = &vkEndState;

    status = memory.EndAccess(texture, &endAccessState);
    if (status != wgpu::Status::Success) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to EndAccess status=%d", (int)status);
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "EndAccess complete, vkNewLayout=%d", vkEndState.newLayout);

    // // Tick device to process any pending work/cleanup before returning
    // // This helps when this function is called repeatedly (e.g., for each video frame)
    // device.Tick();

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "YCbCr blit complete!");
    
    // Note: We don't call MoveToCHandle() on destTexture because Kotlin owns it.
    // The C++ wrapper will release its reference when going out of scope,
    // but since we didn't take ownership (just wrapped the handle), this is fine.
    
    return JNI_TRUE;
}

}

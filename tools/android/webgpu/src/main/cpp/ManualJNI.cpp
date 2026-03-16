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

    class WGPUCache final
    {
    public:
        WGPUCache(const wgpu::Device& device)
        : _device(device)
        {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Initializing WGPUCache");
        }

        bool init()
        {
            // Check if the device has the required feature enabled.
            // Also check backend type
            wgpu::Adapter adapter = _device.GetAdapter();
            if (!adapter) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not get adapter from device!");
                return false;
            }

            wgpu::AdapterInfo info;
            adapter.GetInfo(&info);
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "Device Backend: %d (Vulkan=%d, GLES=%d)",
                                info.backendType, WGPUBackendType_Vulkan, WGPUBackendType_OpenGLES);

            bool hasRequiredFeatures = true;
            if (!_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer)) {
                hasRequiredFeatures = false;
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature SharedTextureMemoryAHardwareBuffer enabled!");
            }
            if (!_device.HasFeature(wgpu::FeatureName::YCbCrVulkanSamplers)) {
                hasRequiredFeatures = false;
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature YCbCrVulkanSamplers enabled!");
            }
            if (!_device.HasFeature(wgpu::FeatureName::StaticSamplers)) {
                hasRequiredFeatures = false;
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature StaticSamplers enabled!");
            }
            if (!_device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD)) {
                hasRequiredFeatures = false;
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Device does NOT have feature SharedFenceSyncFD enabled!");
            }

            if(!hasRequiredFeatures) {
                // Log available features to help debug
                wgpu::SupportedFeatures supportedFeatures;
                _device.GetFeatures(&supportedFeatures);

                __android_log_print(ANDROID_LOG_INFO, kLogTag, "Device has %zu enabled features:", supportedFeatures.featureCount);
                for (size_t i = 0; i < supportedFeatures.featureCount; ++i) {
                    __android_log_print(ANDROID_LOG_INFO, kLogTag, " - Feature: 0x%X", supportedFeatures.features[i]);
                }

                return false;
            }

            return true;
        }

        ~WGPUCache()
        {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Destroying WGPUCache");
        }

        const wgpu::Device& device() const
        {
            return _device;
        }

        wgpu::Sampler getOrCreateSampler(const wgpu::YCbCrVkDescriptor& yCbCrInfo)
        {
            if (_sampler) {
                return _sampler;
            }

            _yCbCrInfo = yCbCrInfo;
            _yCbCrInfo.nextInChain = nullptr;

            wgpu::SamplerDescriptor samplerDesc = {};
            samplerDesc.nextInChain = &_yCbCrInfo;

            _sampler = _device.CreateSampler(&samplerDesc);
            if (_sampler) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created YCbCr sampler: %p", _sampler.Get());
            } else {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create YCbCr sampler");
            }

            return _sampler;
        }

        wgpu::BindGroupLayout getOrCreateBindGroupLayout()
        {
            if (_bindGroupLayout) {
                return _bindGroupLayout;
            }

            if (!_sampler) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Cannot create BindGroupLayout: sampler not created yet");
                return nullptr;
            }

            std::vector<wgpu::BindGroupLayoutEntry> entries(2);

            wgpu::StaticSamplerBindingLayout staticSamplerBinding = {};
            staticSamplerBinding.sampler = _sampler;
            staticSamplerBinding.sampledTextureBinding = 1;

            // Binding 0: Static sampler (visible to fragment shader)
            entries[0].binding = 0;
            entries[0].visibility = wgpu::ShaderStage::Fragment;
            entries[0].nextInChain = &staticSamplerBinding;

            // Binding 1: Texture (visible to fragment shader)
            entries[1].binding = 1;
            entries[1].visibility = wgpu::ShaderStage::Fragment;
            entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
            entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            entries[1].texture.multisampled = false;

            wgpu::BindGroupLayoutDescriptor layoutDesc = {};
            layoutDesc.entryCount = entries.size();
            layoutDesc.entries = entries.data();

            _bindGroupLayout = _device.CreateBindGroupLayout(&layoutDesc);
            if (_bindGroupLayout) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created BindGroupLayout: %p", _bindGroupLayout.Get());
            } else {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create BindGroupLayout");
            }

            return _bindGroupLayout;
        }

        wgpu::RenderPipeline getOrCreatePipeline()
        {
            if (_pipeline) {
                return _pipeline;
            }

            wgpu::BindGroupLayout bindGroupLayout = getOrCreateBindGroupLayout();
            if (!bindGroupLayout) {
                return nullptr;
            }

            const char* blitShaderSource = R"(
                @group(0) @binding(0) var mySampler : sampler;
                @group(0) @binding(1) var myTexture : texture_2d<f32>;

                struct VertexOutput {
                    @builtin(position) position : vec4f,
                    @location(0) texCoord : vec2f,
                }

                @vertex
                fn vs_main(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
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
            wgpu::ShaderModule shaderModule = _device.CreateShaderModule(&shaderModuleDesc);
            if (!shaderModule) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create shader module");
                return nullptr;
            }
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created shader module: %p", shaderModule.Get());

            wgpu::PipelineLayoutDescriptor pipelineLayoutDesc = {};
            pipelineLayoutDesc.bindGroupLayoutCount = 1;
            pipelineLayoutDesc.bindGroupLayouts = &bindGroupLayout;
            wgpu::PipelineLayout pipelineLayout = _device.CreatePipelineLayout(&pipelineLayoutDesc);
            if (!pipelineLayout) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create pipeline layout");
                return nullptr;
            }
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created pipeline layout: %p", pipelineLayout.Get());

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

            _pipeline = _device.CreateRenderPipeline(&pipelineDesc);
            if (_pipeline) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag, "Created render pipeline: %p", _pipeline.Get());
            } else {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create render pipeline");
            }

            return _pipeline;
        }

    private:
        const wgpu::Device _device;
        wgpu::YCbCrVkDescriptor _yCbCrInfo = {};
        wgpu::Sampler _sampler;
        wgpu::BindGroupLayout _bindGroupLayout;
        wgpu::RenderPipeline _pipeline;
    };

    std::unique_ptr<WGPUCache> s_WGPUCache;
}

extern "C" {

    //public external fun initializeCache(device: GPUDevice)
JNIEXPORT jboolean JNICALL Java_androidx_webgpu_helper_TexturesUtils_initializeCache(
JNIEnv* env, jclass clazz, jobject deviceObj) {

    // Get the native WGPUDevice from the Kotlin object
    jclass deviceClass = env->GetObjectClass(deviceObj);
    if (deviceClass == nullptr) {
        return false;
    }

    jmethodID getHandleMethod = env->GetMethodID(deviceClass, "getHandle", "()J");
    if (getHandleMethod == nullptr) {
        return false;
    }

    jlong deviceHandle = env->CallLongMethod(deviceObj, getHandleMethod);
    WGPUDevice rawDevice = reinterpret_cast<WGPUDevice>(deviceHandle);
    wgpu::Device device(rawDevice);

    s_WGPUCache = std::make_unique<WGPUCache>(device);
    if (!s_WGPUCache->init())
    {
        s_WGPUCache.reset();
    }

    return true;
}

JNIEXPORT void JNICALL Java_androidx_webgpu_helper_TexturesUtils_destroyCache(
JNIEnv* env, jclass clazz, jobject deviceObj) {
    s_WGPUCache.release();
}

JNIEXPORT jboolean JNICALL Java_androidx_webgpu_helper_TexturesUtils_blitHardwareBufferToTexture(
    JNIEnv* env, jclass clazz, jobject hardwareBufferObj, jobject destTextureObj) {

    if (s_WGPUCache == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "WGPU cache not initialized");
        return JNI_FALSE;
    }

    if (hardwareBufferObj == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "HardwareBuffer object is null");
        return JNI_FALSE;
    }

    if (destTextureObj == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Destination texture object is null");
        return JNI_FALSE;
    }

    // Get the native WGPUTexture from the destination texture object
    jclass textureClass = env->GetObjectClass(destTextureObj);
    jmethodID getTextureHandleMethod = env->GetMethodID(textureClass, "getHandle", "()J");
    if (getTextureHandleMethod == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not find getHandle method on GPUTexture");
        return JNI_FALSE;
    }

    jlong destTextureHandle = env->CallLongMethod(destTextureObj, getTextureHandleMethod);

    // Wrap the destination texture handle (don't take ownership - Kotlin owns it)
    WGPUTexture rawDestTexture = reinterpret_cast<WGPUTexture>(destTextureHandle);
    wgpu::Texture destTexture(rawDestTexture);
    if (!destTexture) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to get destination texture from handle");
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Using destination texture: %p", destTexture.Get());

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

    const auto& device = s_WGPUCache->device();

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
    wgpu::Sampler yCbCrSampler = s_WGPUCache->getOrCreateSampler(ahbProperties.yCbCrInfo);
    if (!yCbCrSampler) {
        return JNI_FALSE;
    }

    wgpu::RenderPipeline pipeline = s_WGPUCache->getOrCreatePipeline();
    if (!pipeline) {
        return JNI_FALSE;
    }
    wgpu::BindGroupLayout bindGroupLayout = s_WGPUCache->getOrCreateBindGroupLayout();

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

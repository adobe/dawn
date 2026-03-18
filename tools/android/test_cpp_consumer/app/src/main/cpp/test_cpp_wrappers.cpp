#include <webgpu/webgpu_cpp.h>
#include <android/log.h>

#define TAG "WebGPU_CPP_Test"

// Verify that the C++ wrapper headers are consumable from the prefab and that
// the C API symbols resolve when linking against webgpu_c_bundled.
extern "C" void test_cpp_wrappers() {
    wgpu::InstanceDescriptor desc = {};
    wgpu::Instance instance = wgpu::CreateInstance(&desc);
    if (instance != nullptr) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Successfully created wgpu::Instance via C++ wrappers");
    } else {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to create wgpu::Instance");
    }
}

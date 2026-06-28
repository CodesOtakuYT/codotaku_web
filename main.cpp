#define SDL_MAIN_USE_CALLBACKS 1
#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#include <memory>
#include <source_location>
#include <span>
#include <SDL3/SDL_main.h>

#include "SDL3/SDL_log.h"
#include "SDL3/SDL_vulkan.h"
#include <include/gpu/vk/VulkanBackendContext.h>
#include <include/gpu/vk/VulkanMemoryAllocator.h>
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"
#include <include/gpu/vk/VulkanExtensions.h>
#include <include/gpu/vk/VulkanPreferredFeatures.h>

#include <VkBootstrap.h>

#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/vk/VulkanGraphiteContext.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/BackendSemaphore.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/MutableTextureState.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkCanvas.h"

auto chkSDL(bool result, std::source_location loc = {}) {
    if (!result) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "%s:%d:%d [%s]: %s", loc.file_name(), loc.line(), loc.column(),
                     loc.function_name(), SDL_GetError());
        std::exit(1);
    }
}

template<typename T>
auto chkVkb(vkb::Result<T> result, std::source_location loc = {}) -> T {
    if (!result) {
        auto message = result.error().message();
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "%s:%d:%d [%s]: %s (%d)", loc.file_name(), loc.line(), loc.column(),
                     loc.function_name(), message.c_str(), result.vk_result());
        std::exit(1);
    }
    return result.value();
}

static auto chk(VkResult res, std::source_location loc = {}) -> void {
    if (res != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "%s:%d:%d [%s]: %d", loc.file_name(), loc.line(), loc.column(),
                     loc.function_name(), res);
        std::exit(1);
    }
}

static constexpr int MAX_FRAMES = 2;

struct App {
    VkAllocationCallbacks *allocator = nullptr;

    vkb::Instance instance_;
    vkb::InstanceDispatchTable vkInstance_;
    SDL_Window *window_;
    VkSurfaceKHR surface_;
    vkb::PhysicalDevice physicalDevice_;
    vkb::Device device_;
    vkb::DispatchTable vk_;
    VkQueue graphicsQueue_;
    VkQueue presentQueue_;
    uint32_t graphicsQueueIndex_;
    uint32_t presentQueueIndex_;
    vkb::Swapchain swapchain_;
    std::vector<VkImage> swapchainImages_;
    std::unique_ptr<skgpu::graphite::Context> context_;
    std::unique_ptr<skgpu::graphite::Recorder> recorder_;

    struct Frame {
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkSemaphore signal = VK_NULL_HANDLE;
        VkFence submitFence = VK_NULL_HANDLE;
    };

    std::vector<Frame> frames;
    std::vector<sk_sp<SkSurface> > surfaces;
    int frameIdx = 0;
    uint32_t currentImage = 0;
    uint64_t presentID = 0;

    App() = default;

    auto Init(std::span<char *> args) -> SDL_AppResult {
        chkSDL(SDL_SetAppMetadata("Codotaku Web", "0.0.1", "com.codotaku.web"));
        chkSDL(SDL_Init(SDL_INIT_VIDEO));
        chkSDL(SDL_Vulkan_LoadLibrary(nullptr));
        auto vkProc = SDL_Vulkan_GetVkGetInstanceProcAddr();
        chkSDL(vkProc);
        auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkProc);
        vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);

        uint32_t instanceExtensionCount;
        std::span instanceExtensions{SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount), instanceExtensionCount};

        instanceBuilder
                .request_validation_layers()
                .require_api_version(VK_API_VERSION_1_4)
                .use_default_debug_messenger()
                .set_allocation_callbacks(allocator);

        for (auto instanceExtension: instanceExtensions)
            instanceBuilder.enable_extension(instanceExtension);

        instance_ = chkVkb(instanceBuilder.build());
        vkInstance_ = instance_.make_table();

        window_ = SDL_CreateWindow("Codotaku Web", 800, 600,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        chkSDL(window_);

        chkSDL(SDL_Vulkan_CreateSurface(window_, instance_.instance, allocator, &surface_));

        vkb::PhysicalDeviceSelector physicalDeviceSelector(instance_, surface_);
        auto physicalDeviceResult = physicalDeviceSelector.select();
        physicalDevice_ = chkVkb(physicalDeviceResult);

        vkb::DeviceBuilder deviceBuilder(physicalDevice_);
        auto deviceResult = deviceBuilder.build();
        device_ = chkVkb(deviceResult);

        auto [graphicsQueue, graphicsQueueIndex] = chkVkb(device_.get_queue_and_index(vkb::QueueType::graphics));
        auto [presentQueue, presentQueueIndex] = chkVkb(device_.get_queue_and_index(vkb::QueueType::present));
        graphicsQueue_ = graphicsQueue;
        presentQueue_ = presentQueue;
        graphicsQueueIndex_ = graphicsQueueIndex;
        presentQueueIndex_ = presentQueueIndex;

        vkb::SwapchainBuilder swapchainBuilder(device_);
        swapchain_ = chkVkb(swapchainBuilder.build());
        swapchainImages_ = chkVkb(swapchain_.get_images());

        auto getProc = [gipa = instance_.fp_vkGetInstanceProcAddr,
                    gdpa = device_.fp_vkGetDeviceProcAddr]
        (const char *name, VkInstance instance, VkDevice device) -> PFN_vkVoidFunction {
            if (device != VK_NULL_HANDLE) return gdpa(device, name);
            return gipa(instance, name);
        };

        skgpu::VulkanBackendContext vulkanBackendContext{
            .fInstance = instance_.instance,
            .fPhysicalDevice = physicalDevice_.physical_device,
            .fDevice = device_.device,
            .fQueue = graphicsQueue_,
            .fGraphicsQueueIndex = graphicsQueueIndex_,
            .fMaxAPIVersion = VK_API_VERSION_1_4,
            .fVkExtensions = nullptr,
            .fDeviceFeatures = nullptr,
            .fDeviceFeatures2 = nullptr,
            .fGetProc = getProc,
            .fProtectedContext = skgpu::Protected::kNo,
            .fDeviceLostContext = {},
            .fDeviceLostProc = {}
        };
        vulkanBackendContext.fMemoryAllocator = skgpu::VulkanMemoryAllocators::Make(
            vulkanBackendContext, skgpu::ThreadSafe::kNo);

        vk_ = device_.make_table();

        frames.resize(MAX_FRAMES);
        for (auto &f: frames) {
            VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            chk(vk_.createSemaphore(&sci, allocator, &f.acquire));
            chk(vk_.createSemaphore(&sci, allocator, &f.signal));
            VkFenceCreateInfo fci{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            chk(vk_.createFence(&fci, allocator, &f.submitFence));
        }

        skgpu::graphite::ContextOptions contextOptions;
        context_ = skgpu::graphite::ContextFactory::MakeVulkan(vulkanBackendContext, contextOptions);
        if (!context_) {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to create graphite context");
            std::exit(1);
        }

        recorder_ = context_->makeRecorder();

        rebuildSurfaces();

        chkSDL(SDL_ShowWindow(window_));
        return SDL_APP_CONTINUE;
    }

    auto Iterate() -> SDL_AppResult {
        if (!context_) return SDL_APP_CONTINUE;

        auto &frame = frames[frameIdx];

        // CPU pacing: wait for this frame slot's previous submission to complete
        chk(vk_.waitForFences(1, &frame.submitFence, VK_TRUE, UINT64_MAX));
        chk(vk_.resetFences(1, &frame.submitFence));

        // Acquire next swapchain image
        uint32_t nextImage;
        auto acqRes = vk_.acquireNextImageKHR(
            swapchain_.swapchain, UINT64_MAX, frame.acquire, VK_NULL_HANDLE, &nextImage);
        if (acqRes == VK_ERROR_OUT_OF_DATE_KHR) {
            rebuildSwapchain();
            return SDL_APP_CONTINUE;
        }
        chk(acqRes);
        currentImage = nextImage;

        // Draw: simple animated clear
        static float hue = 0.0f;
        hue += 0.01f;
        if (hue > 1.0f) hue -= 1.0f;
        auto canvas = surfaces[currentImage]->getCanvas();
        SkScalar hsv[3] = {hue, 1.0f, 1.0f};
        canvas->clear(SkColor4f::FromColor(SkHSVToColor(hsv)));

        // Snap recorder and submit recording to GPU
        auto recording = recorder_->snap();
        if (!recording) {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "snap() failed");
            return SDL_APP_CONTINUE;
        }

        skgpu::graphite::InsertRecordingInfo info{};
        info.fRecording = recording.get();
        info.fTargetSurface = surfaces[currentImage].get();

        auto presentState = skgpu::MutableTextureStates::MakeVulkan(
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, presentQueueIndex_);
        info.fTargetTextureState = &presentState;

        info.fNumWaitSemaphores = 1;
        auto backendAcquire = skgpu::graphite::BackendSemaphores::MakeVulkan(frame.acquire);
        info.fWaitSemaphores = &backendAcquire;

        // Signal semaphore for present synchronization
        info.fNumSignalSemaphores = 1;
        auto backendSignal = skgpu::graphite::BackendSemaphores::MakeVulkan(frame.signal);
        info.fSignalSemaphores = &backendSignal;

        context_->insertRecording(info);
        context_->submit(skgpu::graphite::SyncToCpu::kNo);

        // Present
        VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &frame.signal,
            .swapchainCount = 1,
            .pSwapchains = &swapchain_.swapchain,
            .pImageIndices = &currentImage,
        };
        auto presRes = vk_.queuePresentKHR(presentQueue_, &presentInfo);
        if (presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR) {
            rebuildSwapchain();
        } else {
            chk(presRes);
        }

        frameIdx = (frameIdx + 1) % MAX_FRAMES;
        return SDL_APP_CONTINUE;
    }

    auto Event(SDL_Event *event) -> SDL_AppResult {
        switch (event->type) {
            case SDL_EVENT_QUIT:
                return SDL_APP_SUCCESS;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_EXPOSED:
                rebuildSwapchain();
                return SDL_APP_CONTINUE;
            default:
                return SDL_APP_CONTINUE;
        }
    }

    auto Quit(SDL_AppResult result) -> void {
        if (!context_) return;
        surfaces.clear();
        context_->submit(skgpu::graphite::SyncToCpu::kYes);
        for (auto &f: frames) {
            if (f.acquire != VK_NULL_HANDLE) vk_.destroySemaphore(f.acquire, allocator);
            if (f.signal != VK_NULL_HANDLE) vk_.destroySemaphore(f.signal, allocator);
            if (f.submitFence != VK_NULL_HANDLE) vk_.destroyFence(f.submitFence, allocator);
        }
        frames.clear();
        vkb::destroy_swapchain(swapchain_);
        recorder_.reset();
        context_.reset();
        vkb::destroy_device(device_);
        vkInstance_.destroySurfaceKHR(surface_, allocator);
        SDL_DestroyWindow(window_);
        vkb::destroy_instance(instance_);
    }

private:
    void rebuildSwapchain() {
        surfaces.clear();
        context_->submit(skgpu::graphite::SyncToCpu::kYes);

        VkSwapchainKHR old = swapchain_.swapchain;
        vkb::SwapchainBuilder swapchainBuilder(device_);
        swapchainBuilder.set_old_swapchain(old);
        auto newSwapchain = chkVkb(swapchainBuilder.build());
        vk_.destroySwapchainKHR(old, allocator);
        swapchain_ = newSwapchain;

        swapchainImages_ = chkVkb(swapchain_.get_images());
        rebuildSurfaces();
    }

    void rebuildSurfaces() {
        surfaces.clear();
        for (auto img: swapchainImages_) {
            skgpu::graphite::VulkanTextureInfo texInfo(
                VK_SAMPLE_COUNT_1_BIT,
                skgpu::Mipmapped::kNo,
                0,
                swapchain_.image_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                VK_SHARING_MODE_EXCLUSIVE,
                VK_IMAGE_ASPECT_COLOR_BIT,
                {});
            skgpu::VulkanAlloc zeroAlloc{};
            auto backendTex = skgpu::graphite::BackendTextures::MakeVulkan(
                {static_cast<int>(swapchain_.extent.width), static_cast<int>(swapchain_.extent.height)},
                texInfo, VK_IMAGE_LAYOUT_UNDEFINED, VK_QUEUE_FAMILY_IGNORED,
                img, zeroAlloc);
            auto surf = SkSurfaces::WrapBackendTexture(
                recorder_.get(), backendTex, SkColorSpace::MakeSRGB(), nullptr);
            if (!surf) {
                surfaces.clear();
                throw std::runtime_error{"WrapBackendTexture failed for swapchain image"};
            }
            surfaces.push_back(std::move(surf));
        }
    }
};

auto SDL_AppInit(void **appstate, int argc, char **argv) -> SDL_AppResult {
    auto app = std::make_unique<App>();
    auto result = app->Init({argv, static_cast<size_t>(argc)});
    *appstate = app.release();
    return result;
}

auto SDL_AppIterate(void *appstate) -> SDL_AppResult {
    auto app = static_cast<App *>(appstate);
    return app->Iterate();
}

auto SDL_AppEvent(void *appstate, SDL_Event *event) -> SDL_AppResult {
    auto app = static_cast<App *>(appstate);
    return app->Event(event);
}

auto SDL_AppQuit(void *appstate, SDL_AppResult result) -> void {
    auto app = static_cast<App *>(appstate);
    app->Quit(result);
}

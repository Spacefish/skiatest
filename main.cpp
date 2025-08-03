#include <chrono>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include "perfbuffer.hpp"
#include <cmath>
#include <algorithm>
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanPreferredFeatures.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include "include/gpu/vk/VulkanTypes.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/TextureInfo.h"
#include "include/gpu/graphite/vk/VulkanGraphiteContext.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"


std::unique_ptr<skgpu::graphite::Context> sGraphiteContext = nullptr;

VkDevice device;

int graphicsQueueFamilyIndex = -1;
VkQueue graphicsQueue;

VkCommandPool commandPool;

VkSwapchainKHR swapChain;
uint32_t swapchainImageCount = 0;
std::vector<VkImage> swapChainImages;

std::vector<sk_sp<SkSurface>> skiaSwapChainSurfaces;

double velocity[3] = {0, 0.02, 0.08}; // Different velocities for each circle
double posY[3] = {1.0, 1.5, 3.7};
auto last_drawcall = std::chrono::high_resolution_clock::now();

#define PERF_BUFFER_SIZE 256

perf::PerfBuffer frameTimesDraw(PERF_BUFFER_SIZE);
perf::PerfBuffer frameTimesPhysics(PERF_BUFFER_SIZE);

int meterToPixel(double meter) {
    return static_cast<int>(meter * 100.0); // Assuming 1 meter = 100 pixels
}

double pixelToMeter(int pixel) {
    return static_cast<double>(pixel) / 100.0; // Assuming 1 meter = 100 pixels
}

std::optional<double> inline solve_quadratic(double a, double b, double c, double max_t) {
    double disc = b * b - 4 * a * c;
    if (disc < 0) {
        return std::nullopt; // No real roots
    }
    double sqrt_d = std::sqrt(disc);
    double denom = 2 * a;
    double t1 = (-b - sqrt_d) / denom;
    double t2 = (-b + sqrt_d) / denom;
    double hit_t = std::numeric_limits<double>::infinity();
    if (t1 > 0 && t1 <= max_t) hit_t = t1;
    if (t2 > 0 && t2 <= max_t && t2 < hit_t) hit_t = t2;
    if (hit_t == std::numeric_limits<double>::infinity()) {
        return std::nullopt; // No valid positive roots in [0, max_t]
    }
    return hit_t;
}

auto last_physicsframe = std::chrono::high_resolution_clock::now();

void physics() {
    // Calculate time since last physics frame (delta time)
    auto now = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration_cast<std::chrono::microseconds>(now - last_physicsframe).count() / 1000000.0;
    last_physicsframe = now;

    double floor = 5.0; // Floor at y = 5.0
    // physics simulation
    for (int c = 0; c < 3; ++c) {
        double y = posY[c];
        double v = velocity[c];
        double a = 9.81;
        double aa = 0.5 * a;
        double ymax = floor - 0.5; // adjust bounce point by radius of the circle

        // Check for collision within dt
        double hit_t = std::numeric_limits<double>::infinity();

        // Collision with bottom (y = ymax, so y(t) = y + v*t + aa*t^2 = ymax)
        auto t_bottom = solve_quadratic(aa, v, y - ymax, dt);
        if (t_bottom && *t_bottom < hit_t) hit_t = *t_bottom;

        // Collision with top (y = 0.0, so y(t) = y + v*t + aa*t^2 = 0.0)
        auto t_top = solve_quadratic(aa, v, y, dt);
        if (t_top && *t_top < hit_t) hit_t = *t_top;

        if (hit_t <= dt) {
            // Collision detected: advance to hit time, bounce, then advance remaining time
            double y_hit = y + v * hit_t + aa * hit_t * hit_t;
            double v_hit = v + a * hit_t;
            double v_new = -v_hit; // Elastic bounce
            double t_rem = dt - hit_t;
            posY[c] = y_hit + v_new * t_rem + aa * t_rem * t_rem;
            velocity[c] = v_new + a * t_rem;
        } else {
            // No collision: standard parabolic update
            posY[c] = y + v * dt + aa * dt * dt; // y(t) = y + v*t + 0.5*a*t^2
            velocity[c] = v + a * dt; // v(t) = v + a*t
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - last_physicsframe).count();
    frameTimesPhysics.addSample(elapsed);
}

uint32_t inline map(uint32_t x, uint32_t in_min, uint32_t in_max, uint32_t out_min, uint32_t out_max) {
    // Avoid division by zero
    if (in_max == in_min) {
        return out_min; // Return out_min as a safe default
    }
    
    // Compute using 64-bit arithmetic to prevent overflow
    uint64_t numerator = (uint64_t)(x - in_min) * (out_max - out_min);
    uint64_t denominator = in_max - in_min;
    return out_min + (uint32_t)(numerator / denominator);
}

void draw() {
    uint32_t imageIndex;
    VkResult acquire_result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &imageIndex);

    auto start = std::chrono::high_resolution_clock::now();
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
        printf("Failed to acquire swapchain image: %d\n", acquire_result);
        return;
    }

    sk_sp<SkSurface> activeSurface = skiaSwapChainSurfaces[imageIndex];
    // auto recorder = skiaRecorders[imageIndex].get(); // Get the associated Recorder

    auto rec = activeSurface->recorder();
    
    SkCanvas* canvas = activeSurface->getCanvas();
    canvas->clear(SK_ColorBLACK);

    // draw circles with different colors based on velocity
    for(int c = 0; c < 3; ++c) {
        SkPaint paint;
        float scaledVelocity = std::abs(velocity[c] / 15.0f);
        paint.setColor({std::clamp(scaledVelocity, 0.0f, 1.0f), 0.0f, 0.35f, 1.0f});
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kFill_Style);

        canvas->drawCircle(meterToPixel(1 + c * 1.5), meterToPixel(posY[c]), meterToPixel(0.5), paint); // Draw a circle at (100 + c * 150, posY[c]) with radius 50
    }

    // PERF GRAPH
    if(true) {
        int perfGraphHeight = 75;
        int perfGraphWidth = PERF_BUFFER_SIZE;
        // draw box
        SkPaint perfGraphPaint;
        perfGraphPaint.setColor(SK_ColorWHITE);
        perfGraphPaint.setStyle(SkPaint::kStroke_Style);
        perfGraphPaint.setStrokeWidth(2);
        canvas->drawRect(SkRect::MakeXYWH(10, 10, perfGraphWidth, perfGraphHeight), perfGraphPaint);

        auto frameTimesDrawSamples = frameTimesDraw.getSamples();
        auto frameTimesPhysicsSamples = frameTimesPhysics.getSamples();

        auto minFrameTime = *std::min_element(frameTimesDrawSamples.begin(), frameTimesDrawSamples.end());
        auto maxFrameTime = *std::max_element(frameTimesDrawSamples.begin(), frameTimesDrawSamples.end());

        auto minPhysicsTime = *std::min_element(frameTimesPhysicsSamples.begin(), frameTimesPhysicsSamples.end());
        auto maxPhysicsTime = *std::max_element(frameTimesPhysicsSamples.begin(), frameTimesPhysicsSamples.end());

        SkPaint linePaint;
        linePaint.setStyle(SkPaint::kStroke_Style);
        linePaint.setStrokeWidth(1);

        linePaint.setColor(SK_ColorGREEN);
        for(int c = 0; c < PERF_BUFFER_SIZE; ++c) {
            auto yFt = map(frameTimesDrawSamples[c], minFrameTime, maxFrameTime, 0, perfGraphHeight);
            int x = c;
            SkPoint point = SkPoint::Make(x + 10, 75 - yFt + 10);
            //canvas->drawPoint(point, linePaint);
        }

        linePaint.setColor(SK_ColorMAGENTA);
        for(int c = 0; c < PERF_BUFFER_SIZE; ++c) {
            auto yPhy = map(frameTimesPhysicsSamples[c], minPhysicsTime, maxPhysicsTime, 0, perfGraphHeight);
            int x = c;
            SkPoint point = SkPoint::Make(x + 10, 75 - yPhy + 10);
            //canvas->drawPoint(point, linePaint);
        }
    }

    std::unique_ptr<skgpu::graphite::Recording> recording = rec->snap();
    sGraphiteContext->insertRecording({recording.get()});
    // Submit the drawing commands
    // sContext->submit();
    sGraphiteContext->submit();

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count();
    frameTimesDraw.addSample(duration);

    // Present the swapchain image
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 0; // No semaphores to wait on
    presentInfo.pWaitSemaphores = nullptr;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapChain;
    presentInfo.pImageIndices = &imageIndex; // Use the first image
    presentInfo.pResults = nullptr; // No results to return
    VkResult result = vkQueuePresentKHR(graphicsQueue, &presentInfo);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swapchain image: %d\n", result);
    }
}

int main() {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize glfw\n");
        return -1;
    }
    if (glfwVulkanSupported() == GLFW_FALSE)
    {
        fprintf(stderr, "glfw does not support Vulkan\n");
        return -1;
    }


    ///
    /// Get supported Vulkan extensions
    ///
    uint32_t instanceExtensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);
    if (instanceExtensionCount == 0) {
        fprintf(stderr, "No Vulkan instance extensions found\n");
        return -1;
    }

    std::vector<VkExtensionProperties> instanceExtensions(instanceExtensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, instanceExtensions.data());

    printf("Found %d supported Vulkan instance extensions\n", instanceExtensionCount);
    for (uint32_t i = 0; i < instanceExtensionCount; ++i) {
        printf("  %s\n", instanceExtensions[i].extensionName);
    }
    
    std::vector<const char*> requiredInstanceExtensions = {};

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    // print required Vulkan extensions
    printf("glfw Required Vulkan extensions:\n");
    for (uint32_t i = 0; i < glfwExtensionCount; ++i) {
        printf("  %s\n", glfwExtensions[i]);
        requiredInstanceExtensions.push_back(glfwExtensions[i]);
    }
    
    // init skiaFeatures
    skgpu::VulkanPreferredFeatures skiaFeatures;
    skiaFeatures.init(VK_API_VERSION_1_3);

    skiaFeatures.addToInstanceExtensions(instanceExtensions.data(), instanceExtensionCount, requiredInstanceExtensions);

    // print requiredInstanceExtensions to see what Skia adds
    printf("Skia required Instance extensions:\n");
    for (int c = glfwExtensionCount; c < requiredInstanceExtensions.size(); ++c) {
        printf("  %s\n", requiredInstanceExtensions[c]);
    }
    
    // vkinstance
    VkInstance instance;
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Skia Vulkan Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Skia";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    // enable required extensions
    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance\n");
        return -1;
    }


    ///
    // PHYSICAL DEVICE
    //
    VkPhysicalDevice physicalDevice;
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        fprintf(stderr, "No Vulkan physical devices found\n");
        return -1;
    }
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
    physicalDevice = physicalDevices[0]; // Use the first physical device for simplicity

    uint32_t deviceExtensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &deviceExtensionCount, nullptr);
    if (deviceExtensionCount == 0) {
        fprintf(stderr, "No Vulkan device extensions found\n");
        return -1;
    }

    std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &deviceExtensionCount, deviceExtensions.data());

    printf("Found %d supported Vulkan device extensions\n", deviceExtensionCount);
    for (uint32_t i = 0; i < deviceExtensionCount; ++i) {
        printf("  %s\n", deviceExtensions[i].extensionName);
    }

    VkPhysicalDeviceFeatures2 features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = nullptr;
    skiaFeatures.addFeaturesToQuery(deviceExtensions.data(), deviceExtensionCount, features);

    vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

    std::vector<const char*> requiredDeviceExtensions = {};

    skiaFeatures.addFeaturesToEnable(requiredDeviceExtensions, features);

    printf("Skia required Device extensions:\n");
    for (int c = 0; c < requiredDeviceExtensions.size(); ++c) {
        printf("  %s\n", requiredDeviceExtensions[c]);
    }

    // we need a swapchain for Skia, so we need to ensure VK_KHR_swapchain is enabled
    // TODO: check if the device supports swapchain
    requiredDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    printf("Device Extensions to load:\n");
    for(int c = 0; c < requiredDeviceExtensions.size(); ++c) {
        printf("  %s\n", requiredDeviceExtensions[c]);
    }

    ///
    // GRAPHICS QUEUE
    //
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0) {
        fprintf(stderr, "No queue families found\n");
        return -1;
    }
    else {
        printf("Found %d queue families\n", queueFamilyCount);
    }

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        printf("Queue family %d: count = %d, flags = %u\n", i, queueFamilies[i].queueCount, queueFamilies[i].queueFlags);
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            printf("Found graphics queue family at index %d\n", i);
            graphicsQueueFamilyIndex = i;
        }
    }

    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    float queuePriority = 1.0f;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    ///
    // Logical Device
    //
    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = requiredDeviceExtensions.size();
    deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();
    deviceCreateInfo.pEnabledFeatures = nullptr; // No specific features needed for this example
    deviceCreateInfo.pNext = &features; // Add the features we queried earlier
    if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan device\n");
        return -1;
    }        

    // create window
    GLFWwindow* window;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(640, 480, "Skia Window", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }
    printf("GLFW window created successfully\n");

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(instance, window, NULL, &surface);
    if (err)
    {
        fprintf(stderr, "Failed to create window surface\n");
        return -1;
    }
    printf("Vulkan surface created successfully\n");

    

    ///
    /// Get graphics Queue and create skia surface
    ///
    vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &graphicsQueue);
    if (graphicsQueue == VK_NULL_HANDLE) {
        fprintf(stderr, "Failed to get graphics queue\n");
        return -1;
    }
    printf("Graphics queue created successfully\n");

    ///
    /// Skia Vulkan context
    ///
    skgpu::VulkanBackendContext backendContext;
    backendContext.fInstance = instance;
    backendContext.fPhysicalDevice = physicalDevice;
    backendContext.fDevice = device;
    backendContext.fQueue = graphicsQueue;
    backendContext.fGraphicsQueueIndex = graphicsQueueFamilyIndex; // Default queue index
    backendContext.fMaxAPIVersion = VK_API_VERSION_1_2;
    backendContext.fDeviceFeatures2 = &features;
    backendContext.fGetProc = [](const char* proc_name, VkInstance instance, VkDevice device) {
		if (device != VK_NULL_HANDLE) {
            printf("GetProcAddr: %s (device)\n", proc_name);
			return vkGetDeviceProcAddr(device, proc_name);
		}
        printf("GetProcAddr: %s (instance)\n", proc_name);
		return vkGetInstanceProcAddr(instance, proc_name);
		};
    skgpu::VulkanExtensions vkExtensions;
    vkExtensions.init(backendContext.fGetProc, instance, physicalDevice,
                      requiredInstanceExtensions.size(), requiredInstanceExtensions.data(),
                      requiredDeviceExtensions.size(), requiredDeviceExtensions.data());
    backendContext.fVkExtensions = &vkExtensions;
    backendContext.fProtectedContext = skgpu::Protected(false);

    skgpu::graphite::ContextOptions options;
    sGraphiteContext = skgpu::graphite::ContextFactory::MakeVulkan(backendContext, options);
    if (!sGraphiteContext) {
        fprintf(stderr, "Failed to create Skia Graphite Vulkan context\n");
        return -1;
    }

    auto recorder = sGraphiteContext->makeRecorder().release();
    if (!recorder) {
        printf("Could not make recorder\n");
        return 1;
    }

    // get surface formats
    uint32_t formatCount;
    std::vector<VkSurfaceFormatKHR> availableFormats;
    VkSurfaceFormatKHR surfaceFormat = {};
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (formatCount != 0) {
        availableFormats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, availableFormats.data());
    }
    for(const auto& format : availableFormats) {
        if(format.format == VK_FORMAT_R8G8B8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            printf("Found supported surface format: VK_FORMAT_R8G8B8A8_UNORM ColorSpace: VK_COLOR_SPACE_SRGB_NONLINEAR_KHR\n");
            surfaceFormat = format;
            break;
        }
    }
    if(!surfaceFormat.format) {
        fprintf(stderr, "No suitable surface format found\n");
        return -1;
    }

    // create swapchain
    VkSwapchainCreateInfoKHR swapchainCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = surface,
        .minImageCount = 2, // Double buffering
        .imageFormat = surfaceFormat.format,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageExtent = VkExtent2D{ .width = 640, .height = 480}, // Set the size of the swapchain images
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, // Single queue family
        .queueFamilyIndexCount = 0, // because of exclude sharing mode
        .pQueueFamilyIndices = nullptr,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, // No transformation
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, // Opaque composite alpha
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, // FIFO present
        .clipped = VK_FALSE, // Clipped rendering
        .oldSwapchain = VK_NULL_HANDLE // No old swapchain
    };

    if (vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapChain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create swapchain\n");
        return -1;
    }

    vkGetSwapchainImagesKHR(device, swapChain, &swapchainImageCount, nullptr);
    swapChainImages.resize(swapchainImageCount);
    vkGetSwapchainImagesKHR(device, swapChain, &swapchainImageCount, swapChainImages.data());

    for(const auto& image : swapChainImages) {
        printf("Swapchain image: %p\n", (void*)image);
        
        SkSurfaceProps props(0, kUnknown_SkPixelGeometry);

        skgpu::graphite::VulkanTextureInfo vulkanTextureInfo{};
        //vulkanTextureInfo.fFlags = 0
        vulkanTextureInfo.fFormat = surfaceFormat.format;
        vulkanTextureInfo.fSampleCount = 1;
        vulkanTextureInfo.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
        vulkanTextureInfo.fImageUsageFlags = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vulkanTextureInfo.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vulkanTextureInfo.fFlags = VK_SAMPLE_COUNT_1_BIT;

        auto backendTexture = skgpu::graphite::BackendTextures::MakeVulkan(
            SkISize::Make(640, 480),
            vulkanTextureInfo,
            VkImageLayout::VK_IMAGE_LAYOUT_UNDEFINED,
            (uint32_t)graphicsQueueFamilyIndex,
            image,
            skgpu::VulkanAlloc{} // No VulkanAlloc needed for this example
        );
        if(!backendTexture.isValid()) {
            printf("Failed to create BackendTexture from VkImage\n");
            return -1;
        }

        sk_sp<SkSurface> skiaSurface = SkSurfaces::WrapBackendTexture(
            recorder,
            backendTexture,
            SkColorType::kRGBA_8888_SkColorType,
            SkColorSpace::MakeSRGB(),
            &props,
            nullptr,
            nullptr,
            ""
        );
        if (!skiaSurface.get()) {
            printf("Failed to create Skia surface for Graphite backend texture\n");
            return -1;
        }
        else {
            skiaSwapChainSurfaces.push_back(skiaSurface);
            printf("Created Skia surface for Graphite backend texture successfully\n");
        }
    }

    last_drawcall = std::chrono::high_resolution_clock::now();
    while (!glfwWindowShouldClose(window)) {
        physics();
        draw();
        
        // not relevant for Vulkan as we need to use our own swapchain via vkQueuePresentKHR
        // glfwSwapBuffers(window);
        glfwPollEvents();
    }

    //sSurface.reset();
    //sContext.reset();
    glfwTerminate();
    return 0;
}
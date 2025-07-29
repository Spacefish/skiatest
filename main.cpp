#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#define SK_VULKAN
#define SK_GANESH
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanPreferredFeatures.h"
#include "include/gpu/vk/VulkanMemoryAllocator.h"

GrDirectContext* sContext = nullptr;
SkSurface* sSurface = nullptr;


void draw() {
    SkCanvas* canvas = sSurface->getCanvas();
    canvas->clear(SK_ColorBLUE);  // Fill blue
    sContext->flush();
}

int main() {
    std::unique_ptr<skgpu::VulkanExtensions> skiaExtensions(new skgpu::VulkanExtensions());

    
    skiaExtensions->dump(); // Print available Vulkan extensions

    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }
    if (glfwVulkanSupported() == GLFW_FALSE)
    {
        fprintf(stderr, "Vulkan not supported\n");
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

    printf("Found %d Vulkan instance extensions\n", instanceExtensionCount);
    for (uint32_t i = 0; i < instanceExtensionCount; ++i) {
        printf("  %s\n", instanceExtensions[i].extensionName);
    }
    

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    // print required Vulkan extensions
    printf("glfw Required Vulkan extensions:\n");
    for (uint32_t i = 0; i < glfwExtensionCount; ++i) {
        printf("  %s\n", glfwExtensions[i]);
    }
    
    std::unique_ptr<skgpu::VulkanPreferredFeatures> preferredFeatures(new skgpu::VulkanPreferredFeatures());
    preferredFeatures->init(VK_API_VERSION_1_2);

    std::vector<const char*> skiaExtensionsList(20);
    preferredFeatures->addToInstanceExtensions(instanceExtensions.data(), instanceExtensionCount, skiaExtensionsList);

    // print skiExtensionList to see what Skia adds
    printf("Skia Vulkan preferred extensions:\n");
    for (const char* ext : skiaExtensionsList) {
        printf("  %s\n", ext);
    }
    
    // vkinstance
    VkInstance instance;
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Skia Vulkan Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Skia";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

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

    int graphicsQueueFamilyIndex = -1;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        printf("Queue family %d: count = %d, flags = %u\n", i, queueFamilies[i].queueCount, queueFamilies[i].queueFlags);
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            printf("Found graphics queue family at index %d\n", i);
            graphicsQueueFamilyIndex = i;
        }
    }

    VkQueue graphicsQueue;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    float queuePriority = 1.0f;
    queueCreateInfo.pQueuePriorities = &queuePriority;



    ///
    // Logical Device
    //
    VkDevice device;
    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 0; // No extensions needed for this example
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

    // create skia surface from VkSurfaceKHR
    skgpu::VulkanBackendContext backendContext;
    backendContext.fInstance = instance;
    backendContext.fPhysicalDevice = physicalDevice;
    backendContext.fDevice = device;
    backendContext.fQueue = graphicsQueue;
    backendContext.fGraphicsQueueIndex = 0; // Default queue index
    backendContext.fMaxAPIVersion = VK_API_VERSION_1_2;
    

    sContext = GrDirectContexts::MakeVulkan(backendContext).release();
    if (!sContext) {
        fprintf(stderr, "Failed to create Skia Vulkan context\n");
        return -1;
    }

    while (!glfwWindowShouldClose(window)) {
        draw();
        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    delete sSurface;
    delete sContext;
    glfwTerminate();
    return 0;
}
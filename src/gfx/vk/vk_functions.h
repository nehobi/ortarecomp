/* The Vulkan functions lib/lsfg and the frame generation presenter
 * (lsfg_present.cpp, test/lsfg_test.cpp) call, for volk.h's stand-in. One X() per function, by
 * the level it is loaded at. */

/* Loaded without an instance. */
#define PDO_VK_GLOBAL_FUNCTIONS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceVersion)

/* Loaded for the instance: its own functions and, through its dispatch,
 * the device's. */
#define PDO_VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

#define PDO_VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkBindBufferMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkBindImageMemory) \
    X(vkGetImageMemoryRequirements) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkFreeDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdDispatch) \
    X(vkCmdBlitImage) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdClearColorImage)

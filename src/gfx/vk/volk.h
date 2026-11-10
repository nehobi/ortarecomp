/* A stand-in for volk (https://github.com/zeux/volk) for lib/lsfg, which
 * calls Vulkan through volk's globals: the same globals and the same four
 * loading functions, for only the functions in vk_functions.h. The loader
 * library comes from SDL (SDL_Vulkan_LoadLibrary), which finds it as it
 * does for its own Vulkan use. Not volk's code: written for this project. */
#ifndef PDO_VOLK_H
#define PDO_VOLK_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include "vk_functions.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the Vulkan loader and its global functions. */
VkResult volkInitialize(void);
/* As volkInitialize, from a vkGetInstanceProcAddr the host already has. */
void volkInitializeCustom(PFN_vkGetInstanceProcAddr handler);
/* The instance's functions, and the device's through its dispatch. */
void volkLoadInstance(VkInstance instance);
/* The device's functions straight from the driver (one device in use). */
void volkLoadDevice(VkDevice device);

#define PDO_VK_DECLARE(name) extern PFN_##name name;
PDO_VK_GLOBAL_FUNCTIONS(PDO_VK_DECLARE)
PDO_VK_INSTANCE_FUNCTIONS(PDO_VK_DECLARE)
PDO_VK_DEVICE_FUNCTIONS(PDO_VK_DECLARE)
#undef PDO_VK_DECLARE
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

#ifdef __cplusplus
}
#endif

#endif

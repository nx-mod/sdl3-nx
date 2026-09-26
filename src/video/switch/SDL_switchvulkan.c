/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

/*
 * Vulkan on the Switch: Mesa's NVK, linked statically - there is no loader to
 * open, so vkGetInstanceProcAddr is the driver's own export. Surfaces are
 * VK_NN_vi_surface on the default NWindow.
 */

#if defined(SDL_VIDEO_VULKAN) && defined(SDL_VIDEO_DRIVER_SWITCH)

#define VK_USE_PLATFORM_VI_NN
#include "SDL_switchvideo.h"
#include "SDL_switchvulkan.h"

#include <switch.h>

// SDL builds with VK_NO_PROTOTYPES; this is the one entry point it needs.
extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);

bool SWITCH_Vulkan_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    (void)path; // nothing to open
    _this->vulkan_config.vkGetInstanceProcAddr = (SDL_FunctionPointer)vkGetInstanceProcAddr;
    _this->vulkan_config.vkEnumerateInstanceExtensionProperties =
        (SDL_FunctionPointer)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    SDL_strlcpy(_this->vulkan_config.loader_path, "static NVK", SDL_arraysize(_this->vulkan_config.loader_path));
    return true;
}

void SWITCH_Vulkan_UnloadLibrary(SDL_VideoDevice *_this)
{
    _this->vulkan_config.vkGetInstanceProcAddr = NULL;
    _this->vulkan_config.vkEnumerateInstanceExtensionProperties = NULL;
}

char const * const *SWITCH_Vulkan_GetInstanceExtensions(SDL_VideoDevice *_this, Uint32 *count)
{
    static const char *const extensionsForSwitch[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME
    };
    if (count) {
        *count = SDL_arraysize(extensionsForSwitch);
    }
    return extensionsForSwitch;
}

bool SWITCH_Vulkan_CreateSurface(SDL_VideoDevice *_this, SDL_Window *window, VkInstance instance,
                                 const struct VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    SDL_WindowData *data = window->internal;
    PFN_vkGetInstanceProcAddr getProcAddr = (PFN_vkGetInstanceProcAddr)_this->vulkan_config.vkGetInstanceProcAddr;
    if (!getProcAddr) {
        return SDL_SetError("Vulkan is not loaded");
    }
    PFN_vkCreateViSurfaceNN createViSurface =
        (PFN_vkCreateViSurfaceNN)getProcAddr(instance, "vkCreateViSurfaceNN");
    if (!createViSurface) {
        return SDL_SetError(VK_NN_VI_SURFACE_EXTENSION_NAME " extension is not enabled in the Vulkan instance.");
    }

    // The swapchain needs the window's buffer queue to itself: a GL surface
    // made for this window (SDL_GPU claims plain windows) has to go first.
    if (data && data->egl_surface != EGL_NO_SURFACE) {
        SDL_EGL_MakeCurrent(_this, NULL, NULL);
        SDL_EGL_DestroySurface(_this, data->egl_surface);
        data->egl_surface = EGL_NO_SURFACE;
    }

    VkViSurfaceCreateInfoNN createInfo;
    SDL_zero(createInfo);
    createInfo.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    createInfo.window = nwindowGetDefault();
    VkResult result = createViSurface(instance, &createInfo, allocator, surface);
    if (result != VK_SUCCESS) {
        return SDL_SetError("vkCreateViSurfaceNN failed: %s", SDL_Vulkan_GetResultString(result));
    }
    return true;
}

void SWITCH_Vulkan_DestroySurface(SDL_VideoDevice *_this, VkInstance instance, VkSurfaceKHR surface,
                                  const struct VkAllocationCallbacks *allocator)
{
    if (_this->vulkan_config.vkGetInstanceProcAddr) {
        SDL_Vulkan_DestroySurface_Internal(_this->vulkan_config.vkGetInstanceProcAddr, instance, surface, allocator);
    }
}

#endif

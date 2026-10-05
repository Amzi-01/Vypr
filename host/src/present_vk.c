/*
 * Vulkan present path: the GPU reads the frame straight out of shared memory.
 *
 * Both other backends copy every frame on the CPU first - out of the ring and
 * into memory the GPU can DMA from - and only then ask the GPU to fetch it.
 * Measured at 3840x2160 on the RTX 5050 with a game running, that was 6.5 ms
 * of memcpy followed by 8.5 ms of GPU copy on the graphics queue, all of it on
 * the thread that also handles input.
 *
 * VK_EXT_external_memory_host removes the first step. The ring is already
 * ordinary host RAM - the guest wrote it there over PCIe - so it is imported as
 * a VkBuffer once, and every later frame is a single DMA out of it. Running
 * that DMA on the dedicated copy engine rather than the graphics queue matters
 * as much again: the same 4K copy measured 8.1 ms on the graphics queue and
 * 3.5 ms on the copy engine, which also keeps it from queueing behind whatever
 * game is using the host GPU.
 *
 * Nothing here blocks the caller. An upload is recorded and submitted; the
 * present waits for it on the GPU through a timeline semaphore. The CPU is
 * back in the event loop within microseconds either way.
 *
 * Should the import be refused, frames go through a host-visible staging
 * buffer instead - the SDL_GPU path's one memcpy, still on the copy engine.
 */
#include "present_internal.h"

#ifdef VYPR_HAVE_VULKAN

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <string.h>

#define VK_MAX_IMPORTS     8
#define VK_MAX_SC_IMAGES   8

/* A ring handed to the GPU. Kept until something overlapping replaces it. */
struct vk_import {
    const uint8_t  *base;
    uint64_t        bytes;
    VkBuffer        buf;
    VkDeviceMemory  mem;
};

/*
 * Everything one GPU needs, shared by a window and its popups.
 *
 * Imports live here rather than per window because a popup reads its own
 * slot's ring, and a ring imported twice is refused by some drivers.
 */
struct vk_shared {
    int                 refs;
    VkInstance          inst;
    VkPhysicalDevice    pd;
    VkDevice            dev;
    uint32_t            gfx_family, xfer_family;
    VkQueue             gfx_q, xfer_q;
    VkPhysicalDeviceMemoryProperties memprops;
    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_ptr_props;
    uint64_t            import_align;
    bool                can_import;

    /* Timeline semaphores, one per queue. Their values only ever rise, which
     * is what makes cross-queue ordering a comparison instead of bookkeeping
     * over binary semaphores that must be waited exactly once. */
    VkSemaphore         xfer_tl, gfx_tl;
    uint64_t            xfer_value, gfx_value;

    struct vk_import    imports[VK_MAX_IMPORTS];
    int                 import_count;
    char                name[64];
};

struct vk_tex {
    VkImage         img;
    VkDeviceMemory  mem;
    uint32_t        w, h;
    uint64_t        ready_at;   /* xfer timeline value when its upload lands */
    uint64_t        read_at;    /* gfx timeline value when its last blit is done */
};

/* One command buffer per in-flight submit; reused once its value is reached. */
struct vk_cmd {
    VkCommandBuffer cb;
    uint64_t        done_at;
};

struct vk_state {
    struct vk_shared *sh;
    SDL_Window       *win;
    VkSurfaceKHR      surface;

    VkSwapchainKHR    sc;
    VkFormat          sc_format;
    VkExtent2D        sc_extent;
    uint32_t          sc_count;
    VkImage           sc_images[VK_MAX_SC_IMAGES];
    VkSemaphore       sc_done[VK_MAX_SC_IMAGES];  /* render finished, per image */
    VkSemaphore       acquired[2];
    uint64_t          acquired_done[2];   /* gfx value of the submit that waited on it */
    int               acquire_at;
    VkPresentModeKHR  mode;
    bool              sc_stale;

    VkCommandPool     gfx_pool, xfer_pool;
    struct vk_cmd     gfx_cmd[2], xfer_cmd[2];
    int               gfx_at, xfer_at;

    struct vk_tex     tex[2];
    int               cur;          /* texture holding the newest frame, or -1 */
    uint32_t          src_w, src_h;

    /*
     * Damage accumulation.
     *
     * Off, the two textures ping-pong: each whole frame goes to the one the
     * present is not reading, so upload and present overlap. A damage frame
     * carries only its changed rectangles and has to land on the frame already
     * held, so it cannot ping-pong - it would paint onto the wrong, older
     * texture. On the first damage frame the window latches onto the single
     * texture that holds the newest full frame and keeps using it for every
     * frame after, full or partial. Upload and present then take turns on that
     * one texture, serialised by the timelines they already use; the frames
     * are tiny, so the lost overlap costs nothing. Untouched whenever damage is
     * off, which is the default.
     */
    bool              accumulate;
    int               acc_index;

    /* Fallback when the ring cannot be imported. */
    VkBuffer          stage;
    VkDeviceMemory    stage_mem;
    void             *stage_ptr;
    uint64_t          stage_bytes;
    uint64_t          stage_busy_until;   /* xfer value of the last copy out of it */

    bool              warned_staged;
    uint64_t          ns_upload, ns_present;
};

#define VK_CHECK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "vypr: vulkan: %s failed (%d)\n", (what), (int)r_); goto fail; } } while (0)

static int find_memory_type(const struct vk_shared *sh, uint32_t bits,
                            VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < sh->memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (sh->memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static bool has_extension(const VkExtensionProperties *ext, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ext[i].extensionName, name)) return true;
    return false;
}

/* ---- shared device ---------------------------------------------------- */

static void shared_release(struct vk_shared *sh)
{
    if (!sh || --sh->refs > 0) return;
    if (sh->dev) {
        vkDeviceWaitIdle(sh->dev);
        for (int i = 0; i < sh->import_count; i++) {
            vkDestroyBuffer(sh->dev, sh->imports[i].buf, NULL);
            vkFreeMemory(sh->dev, sh->imports[i].mem, NULL);
        }
        if (sh->xfer_tl) vkDestroySemaphore(sh->dev, sh->xfer_tl, NULL);
        if (sh->gfx_tl)  vkDestroySemaphore(sh->dev, sh->gfx_tl, NULL);
        vkDestroyDevice(sh->dev, NULL);
    }
    if (sh->inst) vkDestroyInstance(sh->inst, NULL);
    SDL_free(sh);
}

static VkSemaphore timeline_create(VkDevice dev)
{
    VkSemaphoreTypeCreateInfo tci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci };
    VkSemaphore s = VK_NULL_HANDLE;
    if (vkCreateSemaphore(dev, &sci, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

/* The instance has to exist before the window's surface can, and the surface
 * before a device can be chosen that presents to it - so this is in two parts,
 * with the caller creating the surface in between. */
static struct vk_shared *shared_create_instance(void)
{
    struct vk_shared *sh = SDL_calloc(1, sizeof(*sh));
    if (!sh) return NULL;
    sh->refs = 1;

    Uint32 n_ext = 0;
    const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&n_ext);
    if (!ext) {
        fprintf(stderr, "vypr: vulkan: no instance extensions: %s\n", SDL_GetError());
        goto fail;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vypr",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = n_ext,
        .ppEnabledExtensionNames = ext,
    };
    VK_CHECK(vkCreateInstance(&ici, NULL, &sh->inst), "vkCreateInstance");
    return sh;

fail:
    shared_release(sh);
    return NULL;
}

static bool shared_create_device(struct vk_shared *sh, VkSurfaceKHR surface)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(sh->inst, &n, NULL);
    if (n == 0) { fprintf(stderr, "vypr: vulkan: no devices\n"); return false; }
    VkPhysicalDevice pds[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(sh->inst, &n, pds);

    /* The GPU that can present to this window, preferring a real one. */
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.apiVersion < VK_API_VERSION_1_2) continue;

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, NULL);
        VkQueueFamilyProperties qf[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qf);

        bool presents = false;
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surface, &ok);
            if (ok && (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) presents = true;
        }
        if (!presents) continue;

        int score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 3 :
                    pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) { best = (int)i; best_score = score; }
    }
    if (best < 0) { fprintf(stderr, "vypr: vulkan: no device presents to this window\n"); return false; }
    sh->pd = pds[best];

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(sh->pd, &pp);
    vkGetPhysicalDeviceMemoryProperties(sh->pd, &sh->memprops);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(sh->pd, &qn, NULL);
    VkQueueFamilyProperties qf[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(sh->pd, &qn, qf);

    sh->gfx_family = UINT32_MAX;
    for (uint32_t q = 0; q < qn && sh->gfx_family == UINT32_MAX; q++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(sh->pd, q, surface, &ok);
        if (ok && (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) sh->gfx_family = q;
    }

    /* The copy engine: a family that does transfers and nothing else. Its
     * granularity has to allow arbitrary sizes, or windows of odd dimensions
     * would need padding the ring does not have. */
    sh->xfer_family = sh->gfx_family;
    for (uint32_t q = 0; q < qn; q++) {
        const VkQueueFlags f = qf[q].queueFlags;
        const VkExtent3D g = qf[q].minImageTransferGranularity;
        if ((f & VK_QUEUE_TRANSFER_BIT) &&
            !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
            g.width == 1 && g.height == 1 && g.depth == 1) {
            sh->xfer_family = q;
            break;
        }
    }

    uint32_t n_dext = 0;
    vkEnumerateDeviceExtensionProperties(sh->pd, NULL, &n_dext, NULL);
    VkExtensionProperties *dext = SDL_calloc(n_dext ? n_dext : 1, sizeof(*dext));
    if (!dext) return false;
    vkEnumerateDeviceExtensionProperties(sh->pd, NULL, &n_dext, dext);
    const bool have_swapchain = has_extension(dext, n_dext, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    sh->can_import = has_extension(dext, n_dext, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    SDL_free(dext);
    if (!have_swapchain) { fprintf(stderr, "vypr: vulkan: no swapchain support\n"); return false; }

    const char *want[3];
    uint32_t n_want = 0;
    want[n_want++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (sh->can_import) {
        want[n_want++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
        want[n_want++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    }

    if (sh->can_import) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
        };
        VkPhysicalDeviceProperties2 pp2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp,
        };
        vkGetPhysicalDeviceProperties2(sh->pd, &pp2);
        sh->import_align = hp.minImportedHostPointerAlignment;
        if (sh->import_align == 0) sh->import_align = 4096;
    }

    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    vkGetPhysicalDeviceFeatures2(sh->pd, &f2);
    if (!f12.timelineSemaphore) { fprintf(stderr, "vypr: vulkan: no timeline semaphores\n"); return false; }
    VkPhysicalDeviceVulkan12Features enable12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE,
    };

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci[2] = {
        { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = sh->gfx_family, .queueCount = 1, .pQueuePriorities = &prio },
        { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = sh->xfer_family, .queueCount = 1, .pQueuePriorities = &prio },
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enable12,
        .queueCreateInfoCount = sh->xfer_family == sh->gfx_family ? 1 : 2,
        .pQueueCreateInfos = qci,
        .enabledExtensionCount = n_want,
        .ppEnabledExtensionNames = want,
    };
    if (vkCreateDevice(sh->pd, &dci, NULL, &sh->dev) != VK_SUCCESS) {
        fprintf(stderr, "vypr: vulkan: vkCreateDevice failed\n");
        return false;
    }
    vkGetDeviceQueue(sh->dev, sh->gfx_family, 0, &sh->gfx_q);
    vkGetDeviceQueue(sh->dev, sh->xfer_family, 0, &sh->xfer_q);

    if (sh->can_import)
        sh->get_host_ptr_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)
            vkGetDeviceProcAddr(sh->dev, "vkGetMemoryHostPointerPropertiesEXT");
    if (!sh->get_host_ptr_props) sh->can_import = false;

    sh->xfer_tl = timeline_create(sh->dev);
    sh->gfx_tl  = timeline_create(sh->dev);
    if (!sh->xfer_tl || !sh->gfx_tl) return false;

    snprintf(sh->name, sizeof(sh->name), "vulkan%s%s",
             sh->can_import ? " zero-copy" : " staged",
             sh->xfer_family != sh->gfx_family ? ", copy engine" : "");
    fprintf(stderr, "vypr: %s on %s\n", sh->name, pp.deviceName);
    return true;
}

static void timeline_wait(VkDevice dev, VkSemaphore s, uint64_t value)
{
    if (value == 0) return;
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &s, .pValues = &value,
    };
    vkWaitSemaphores(dev, &wi, UINT64_MAX);
}

/* ---- ring import ------------------------------------------------------- */

static void import_drop(struct vk_shared *sh, int i)
{
    /* Something may still be copying out of it. Window churn is the only way
     * here, so a full idle is affordable. */
    vkDeviceWaitIdle(sh->dev);
    vkDestroyBuffer(sh->dev, sh->imports[i].buf, NULL);
    vkFreeMemory(sh->dev, sh->imports[i].mem, NULL);
    sh->imports[i] = sh->imports[--sh->import_count];
}

/* The imported buffer holding `f`'s ring, importing it on first sight. NULL
 * when it cannot be imported, which sends the frame down the staged path. */
static const struct vk_import *import_ring(struct vk_shared *sh, const struct vypr_frame_view *f)
{
    if (!sh->can_import || !f->ring || !f->ring_bytes) return NULL;

    const uint8_t *lo = f->ring, *hi = f->ring + f->ring_bytes;
    for (int i = 0; i < sh->import_count; i++) {
        const struct vk_import *im = &sh->imports[i];
        if (im->base == lo && im->bytes == f->ring_bytes) return im;
    }

    if (((uintptr_t)lo % sh->import_align) || (f->ring_bytes % sh->import_align)) return NULL;

    /* A ring the host re-carved: whatever overlaps it is stale. */
    for (int i = 0; i < sh->import_count; ) {
        const struct vk_import *im = &sh->imports[i];
        if (im->base < hi && lo < im->base + im->bytes) import_drop(sh, i);
        else i++;
    }
    if (sh->import_count == VK_MAX_IMPORTS) import_drop(sh, 0);

    VkMemoryHostPointerPropertiesEXT hpp = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    if (sh->get_host_ptr_props(sh->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                               lo, &hpp) != VK_SUCCESS || !hpp.memoryTypeBits)
        return NULL;

    struct vk_import im = { .base = lo, .bytes = f->ring_bytes };
    VkExternalMemoryBufferCreateInfo ebi = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebi,
        .size = f->ring_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(sh->dev, &bci, NULL, &im.buf) != VK_SUCCESS) return NULL;

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(sh->dev, im.buf, &mr);
    const int type = find_memory_type(sh, mr.memoryTypeBits & hpp.memoryTypeBits, 0);
    VkImportMemoryHostPointerInfoEXT imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = (void *)lo,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
        .allocationSize = f->ring_bytes, .memoryTypeIndex = (uint32_t)(type < 0 ? 0 : type),
    };
    if (type < 0 || vkAllocateMemory(sh->dev, &mai, NULL, &im.mem) != VK_SUCCESS ||
        vkBindBufferMemory(sh->dev, im.buf, im.mem, 0) != VK_SUCCESS) {
        if (im.mem) vkFreeMemory(sh->dev, im.mem, NULL);
        vkDestroyBuffer(sh->dev, im.buf, NULL);
        return NULL;
    }

    sh->imports[sh->import_count] = im;
    return &sh->imports[sh->import_count++];
}

/* ---- per-window state --------------------------------------------------- */

static void swapchain_destroy(struct vk_state *p, bool keep_handle)
{
    VkDevice dev = p->sh->dev;
    for (uint32_t i = 0; i < p->sc_count; i++)
        if (p->sc_done[i]) { vkDestroySemaphore(dev, p->sc_done[i], NULL); p->sc_done[i] = VK_NULL_HANDLE; }
    p->sc_count = 0;
    if (!keep_handle && p->sc) { vkDestroySwapchainKHR(dev, p->sc, NULL); p->sc = VK_NULL_HANDLE; }
}

static bool swapchain_create(struct vk_state *p)
{
    struct vk_shared *sh = p->sh;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(p->win, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;   /* minimised */

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(sh->pd, p->surface, &caps) != VK_SUCCESS)
        return false;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "vypr: vulkan: swapchain cannot be a copy target\n");
        return false;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        ext.width  = (uint32_t)pw;
        ext.height = (uint32_t)ph;
    }
    if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
    if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (ext.width == 0 || ext.height == 0) return false;

    /* One more than the minimum: mailbox needs a spare to replace into. */
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > VK_MAX_SC_IMAGES) count = VK_MAX_SC_IMAGES;

    VkSwapchainKHR old = p->sc;
    swapchain_destroy(p, true);

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = p->surface,
        .minImageCount = count,
        .imageFormat = p->sc_format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                                              -caps.supportedCompositeAlpha),
        .presentMode = p->mode,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(sh->dev, &sci, NULL, &p->sc);
    if (old) vkDestroySwapchainKHR(sh->dev, old, NULL);
    if (r != VK_SUCCESS) {
        p->sc = VK_NULL_HANDLE;
        fprintf(stderr, "vypr: vulkan: vkCreateSwapchainKHR failed (%d)\n", (int)r);
        return false;
    }

    uint32_t n = VK_MAX_SC_IMAGES;
    vkGetSwapchainImagesKHR(sh->dev, p->sc, &n, p->sc_images);
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n; i++)
        if (vkCreateSemaphore(sh->dev, &semi, NULL, &p->sc_done[i]) != VK_SUCCESS) {
            p->sc_count = i;
            return false;
        }
    p->sc_count = n;
    p->sc_extent = ext;
    p->sc_stale = false;
    return true;
}

static void tex_destroy(struct vk_state *p, struct vk_tex *t)
{
    if (t->img) vkDestroyImage(p->sh->dev, t->img, NULL);
    if (t->mem) vkFreeMemory(p->sh->dev, t->mem, NULL);
    memset(t, 0, sizeof(*t));
}

static bool tex_ensure(struct vk_state *p, struct vk_tex *t, uint32_t w, uint32_t h)
{
    struct vk_shared *sh = p->sh;
    if (t->img && t->w == w && t->h == h) return true;

    /* Both queues have to be finished with the old one. */
    timeline_wait(sh->dev, sh->xfer_tl, t->ready_at);
    timeline_wait(sh->dev, sh->gfx_tl, t->read_at);
    tex_destroy(p, t);

    const uint32_t families[2] = { sh->gfx_family, sh->xfer_family };
    const bool concurrent = sh->gfx_family != sh->xfer_family;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        /* Concurrent rather than an ownership hand-off on every frame: the
         * texture is written by the copy engine and read by the graphics
         * queue, always in that order, with a semaphore between. */
        .sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = concurrent ? 2 : 0,
        .pQueueFamilyIndices = concurrent ? families : NULL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(sh->dev, &ii, NULL, &t->img) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(sh->dev, t->img, &mr);
    const int type = find_memory_type(sh, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)(type < 0 ? 0 : type),
    };
    if (type < 0 || vkAllocateMemory(sh->dev, &mai, NULL, &t->mem) != VK_SUCCESS ||
        vkBindImageMemory(sh->dev, t->img, t->mem, 0) != VK_SUCCESS) {
        tex_destroy(p, t);
        return false;
    }
    t->w = w;
    t->h = h;
    return true;
}

static bool stage_ensure(struct vk_state *p, uint64_t bytes)
{
    struct vk_shared *sh = p->sh;
    if (p->stage && p->stage_bytes >= bytes) return true;

    timeline_wait(sh->dev, sh->xfer_tl, p->stage_busy_until);
    if (p->stage_mem) { vkUnmapMemory(sh->dev, p->stage_mem); vkFreeMemory(sh->dev, p->stage_mem, NULL); }
    if (p->stage) vkDestroyBuffer(sh->dev, p->stage, NULL);
    p->stage = VK_NULL_HANDLE; p->stage_mem = VK_NULL_HANDLE; p->stage_ptr = NULL; p->stage_bytes = 0;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    if (vkCreateBuffer(sh->dev, &bci, NULL, &p->stage) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(sh->dev, p->stage, &mr);
    const int type = find_memory_type(sh, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)(type < 0 ? 0 : type),
    };
    if (type < 0 || vkAllocateMemory(sh->dev, &mai, NULL, &p->stage_mem) != VK_SUCCESS ||
        vkBindBufferMemory(sh->dev, p->stage, p->stage_mem, 0) != VK_SUCCESS ||
        vkMapMemory(sh->dev, p->stage_mem, 0, VK_WHOLE_SIZE, 0, &p->stage_ptr) != VK_SUCCESS)
        return false;
    p->stage_bytes = bytes;
    return true;
}

/* A command buffer from a two-deep ring, waiting for its last use if needed. */
static VkCommandBuffer cmd_next(struct vk_state *p, struct vk_cmd *ring, int *at,
                                VkSemaphore tl)
{
    struct vk_cmd *c = &ring[*at];
    *at = (*at + 1) % 2;
    timeline_wait(p->sh->dev, tl, c->done_at);
    vkResetCommandBuffer(c->cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(c->cb, &bi);
    return c->cb;
}

static void vk_destroy(void *impl);

static void *vk_create(SDL_Window *win, void *share_impl)
{
    if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_VULKAN)) return NULL;

    struct vk_state *p = SDL_calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->win = win;
    p->cur = -1;

    struct vk_state *share = share_impl;
    if (share) {
        p->sh = share->sh;
        p->sh->refs++;
    } else {
        p->sh = shared_create_instance();
        if (!p->sh) { SDL_free(p); return NULL; }
    }
    struct vk_shared *sh = p->sh;

    if (!SDL_Vulkan_CreateSurface(win, sh->inst, NULL, &p->surface)) {
        fprintf(stderr, "vypr: vulkan: surface: %s\n", SDL_GetError());
        goto fail;
    }
    if (!sh->dev && !shared_create_device(sh, p->surface)) goto fail;

    VkBool32 presents = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(sh->pd, sh->gfx_family, p->surface, &presents);
    if (!presents) { fprintf(stderr, "vypr: vulkan: shared device cannot present here\n"); goto fail; }

    /* Plain UNORM, the format the frames arrive in. An sRGB swapchain would
     * have the blit re-encode every pixel and wash the picture out. */
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(sh->pd, p->surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(sh->pd, p->surface, &nf, fmts);
    p->sc_format = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            p->sc_format = fmts[i].format;
            if (p->sc_format == VK_FORMAT_B8G8R8A8_UNORM) break;
        }
    if (p->sc_format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "vypr: vulkan: no 8-bit UNORM swapchain format\n");
        goto fail;
    }

    /* MAILBOX for the same reason as the SDL_GPU path: the newest frame
     * replaces one still waiting rather than queueing behind it, without
     * tearing. FIFO only where it is all there is. */
    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(sh->pd, p->surface, &nm, NULL);
    VkPresentModeKHR modes[16];
    if (nm > 16) nm = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(sh->pd, p->surface, &nm, modes);
    p->mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nm; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) p->mode = VK_PRESENT_MODE_MAILBOX_KHR;

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = sh->gfx_family,
    };
    VK_CHECK(vkCreateCommandPool(sh->dev, &pci, NULL, &p->gfx_pool), "command pool");
    pci.queueFamilyIndex = sh->xfer_family;
    VK_CHECK(vkCreateCommandPool(sh->dev, &pci, NULL, &p->xfer_pool), "command pool");

    VkCommandBuffer cbs[2];
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->gfx_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2,
    };
    VK_CHECK(vkAllocateCommandBuffers(sh->dev, &cai, cbs), "command buffers");
    p->gfx_cmd[0].cb = cbs[0]; p->gfx_cmd[1].cb = cbs[1];
    cai.commandPool = p->xfer_pool;
    VK_CHECK(vkAllocateCommandBuffers(sh->dev, &cai, cbs), "command buffers");
    p->xfer_cmd[0].cb = cbs[0]; p->xfer_cmd[1].cb = cbs[1];

    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_CHECK(vkCreateSemaphore(sh->dev, &semi, NULL, &p->acquired[0]), "semaphore");
    VK_CHECK(vkCreateSemaphore(sh->dev, &semi, NULL, &p->acquired[1]), "semaphore");

    p->sc_stale = true;   /* built on first present, once the window has a size */

    fprintf(stderr, "vypr: presenting with %s, %s\n", sh->name,
            p->mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return p;

fail:
    vk_destroy(p);
    return NULL;
}

static void vk_destroy(void *impl)
{
    struct vk_state *p = impl;
    if (!p) return;
    struct vk_shared *sh = p->sh;
    if (sh && sh->dev) {
        vkDeviceWaitIdle(sh->dev);
        tex_destroy(p, &p->tex[0]);
        tex_destroy(p, &p->tex[1]);
        if (p->stage_mem) { vkUnmapMemory(sh->dev, p->stage_mem); vkFreeMemory(sh->dev, p->stage_mem, NULL); }
        if (p->stage) vkDestroyBuffer(sh->dev, p->stage, NULL);
        for (int i = 0; i < 2; i++)
            if (p->acquired[i]) vkDestroySemaphore(sh->dev, p->acquired[i], NULL);
        swapchain_destroy(p, false);
        if (p->gfx_pool)  vkDestroyCommandPool(sh->dev, p->gfx_pool, NULL);
        if (p->xfer_pool) vkDestroyCommandPool(sh->dev, p->xfer_pool, NULL);
    }
    if (sh && p->surface) SDL_Vulkan_DestroySurface(sh->inst, p->surface, NULL);
    shared_release(sh);
    SDL_free(p);
}

static const char *vk_driver(void *impl)
{
    const struct vk_state *p = impl;
    return p->sh->name;
}

static bool vk_upload(void *impl, const struct vypr_frame_view *f)
{
    struct vk_state *p = impl;
    struct vk_shared *sh = p->sh;
    const uint64_t t0 = SDL_GetTicksNS();

    if (f->stride % 4 != 0) return false;   /* bufferRowLength is in texels */

    /*
     * A damage frame paints only its rectangles, so it needs the frame already
     * held to paint onto. The first frame of a session is always whole, so this
     * guards only against a misbehaving guest.
     */
    const bool partial = (f->flags & VYPR_PUB_DAMAGE_RECTS) && f->damage_count > 0;
    if (partial && p->cur < 0) return true;

    /* Where this frame lands - see `accumulate`. */
    int k;
    if (p->accumulate) {
        k = p->acc_index;
    } else if (partial) {
        p->accumulate = true;
        p->acc_index = p->cur;      /* the texture that holds the newest full frame */
        k = p->acc_index;
    } else {
        k = p->cur < 0 ? 0 : 1 - p->cur;
    }
    struct vk_tex *t = &p->tex[k];
    if (!tex_ensure(p, t, f->width, f->height)) return false;

    const uint64_t bytes = (uint64_t)f->stride * f->height;
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize base_off = 0;

    const struct vk_import *im = import_ring(sh, f);
    if (im) {
        src = im->buf;
        base_off = (VkDeviceSize)(f->pixels - im->base);
    } else {
        if (!p->warned_staged) {
            p->warned_staged = true;
            fprintf(stderr, "vypr: vulkan: ring not importable, copying through a staging buffer\n");
        }
        if (!stage_ensure(p, bytes)) return false;
        timeline_wait(sh->dev, sh->xfer_tl, p->stage_busy_until);
        if (partial) {
            /* Only the changed rows, each kept at its own offset so the copies
             * below address the stage buffer exactly as they would the ring. */
            for (uint32_t r = 0; r < f->damage_count; r++) {
                const struct vypr_rect d = f->damage[r];
                for (uint32_t y = d.y; y < d.y + d.h; y++) {
                    const size_t o = (size_t)y * f->stride + (size_t)d.x * 4;
                    memcpy((uint8_t *)p->stage_ptr + o, f->pixels + o, (size_t)d.w * 4);
                }
            }
        } else {
            memcpy(p->stage_ptr, f->pixels, bytes);
        }
        src = p->stage;
    }

    VkCommandBuffer cb = cmd_next(p, p->xfer_cmd, &p->xfer_at, sh->xfer_tl);

    /* A whole frame discards what the texture held (UNDEFINED); a damage frame
     * keeps it (TRANSFER_SRC, where the last present left it) and paints over
     * it. The source stage orders this after the previous copy either way. */
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = partial ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = t->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_dst);

    if (partial) {
        VkBufferImageCopy regions[VYPR_MAX_DAMAGE_RECTS];
        for (uint32_t r = 0; r < f->damage_count; r++) {
            const struct vypr_rect d = f->damage[r];
            regions[r] = (VkBufferImageCopy){
                .bufferOffset = base_off + (VkDeviceSize)d.y * f->stride + (VkDeviceSize)d.x * 4,
                .bufferRowLength = f->stride / 4,
                .bufferImageHeight = d.h,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageOffset = { (int32_t)d.x, (int32_t)d.y, 0 },
                .imageExtent = { d.w, d.h, 1 },
            };
        }
        vkCmdCopyBufferToImage(cb, src, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               f->damage_count, regions);
    } else {
        VkBufferImageCopy region = {
            .bufferOffset = base_off,
            .bufferRowLength = f->stride / 4,
            .bufferImageHeight = f->height,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { f->width, f->height, 1 },
        };
        vkCmdCopyBufferToImage(cb, src, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    VkImageMemoryBarrier to_src = to_dst;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = 0;   /* the semaphore makes it visible to the reader */
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_src);
    vkEndCommandBuffer(cb);

    /* Waits on the GPU, not here, for the last blit out of this texture. */
    const uint64_t wait_value = t->read_at;
    const uint64_t signal_value = ++sh->xfer_value;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo tsi = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wait_value,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signal_value,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &sh->gfx_tl, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &sh->xfer_tl,
    };
    if (vkQueueSubmit(sh->xfer_q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        fprintf(stderr, "vypr: vulkan: upload submit failed\n");
        return false;
    }
    p->xfer_cmd[(p->xfer_at + 1) % 2].done_at = signal_value;
    if (!im) p->stage_busy_until = signal_value;

    t->ready_at = signal_value;
    p->cur = k;
    p->src_w = f->width;
    p->src_h = f->height;

    p->ns_upload += SDL_GetTicksNS() - t0;
    return true;
}

static void vk_present(void *impl)
{
    struct vk_state *p = impl;
    struct vk_shared *sh = p->sh;
    const uint64_t t0 = SDL_GetTicksNS();

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(p->win, &pw, &ph);
    if (pw <= 0 || ph <= 0) goto out;   /* minimised: nothing to draw into */
    if (p->sc && ((uint32_t)pw != p->sc_extent.width || (uint32_t)ph != p->sc_extent.height))
        p->sc_stale = true;

    uint32_t img = 0;
    VkSemaphore acquired = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (p->sc_stale || !p->sc) {
            /* The old images may still be in a present the queue holds, and
             * their semaphores with them. Resizes only, so a full wait. */
            vkQueueWaitIdle(sh->gfx_q);
            if (!swapchain_create(p)) goto out;
        }
        /* An acquire may only signal a semaphore nothing is still waiting on. */
        timeline_wait(sh->dev, sh->gfx_tl, p->acquired_done[p->acquire_at]);
        acquired = p->acquired[p->acquire_at];
        VkResult r = vkAcquireNextImageKHR(sh->dev, p->sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &img);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) p->sc_stale = true;   /* use it, rebuild next time */
            break;
        }
        acquired = VK_NULL_HANDLE;
        if (r != VK_ERROR_OUT_OF_DATE_KHR) {
            fprintf(stderr, "vypr: vulkan: acquire failed (%d)\n", (int)r);
            goto out;
        }
        p->sc_stale = true;
    }
    if (!acquired) goto out;
    const int acquire_slot = p->acquire_at;
    p->acquire_at ^= 1;

    VkCommandBuffer cb = cmd_next(p, p->gfx_cmd, &p->gfx_at, sh->gfx_tl);
    VkImage dst = p->sc_images[img];

    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = dst,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_dst);

    const struct vk_tex *t = p->cur >= 0 ? &p->tex[p->cur] : NULL;
    int dx = 0, dy = 0, dw = 0, dh = 0;
    if (t) vypr_fit_rect((int)p->sc_extent.width, (int)p->sc_extent.height,
                         p->src_w, p->src_h, &dx, &dy, &dw, &dh);

    /* Black margins, only when the picture does not cover the window - for the
     * common case of matching sizes the blit writes every pixel itself. */
    if (!t || dx > 0 || dy > 0 || (uint32_t)dw < p->sc_extent.width ||
        (uint32_t)dh < p->sc_extent.height) {
        const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        if (t) {
            VkMemoryBarrier mb = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &mb, 0, NULL, 0, NULL);
        }
    }

    if (t) {
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)p->src_w, (int32_t)p->src_h, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { dx, dy, 0 }, { dx + dw, dy + dh, 1 } },
        };
        /* Nearest when it is pixel for pixel, so text stays exactly as the
         * guest drew it; linear only when it really is being scaled. */
        const VkFilter filter = ((uint32_t)dw == p->src_w && (uint32_t)dh == p->src_h)
                                    ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        vkCmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
    }

    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_present);
    vkEndCommandBuffer(cb);

    /* Wait for the swapchain image and, on the GPU, for the upload of the frame
     * being drawn. Signal the timeline for whoever reuses this texture, and the
     * per-image semaphore the present waits on. */
    const VkSemaphore waits[2] = { acquired, sh->xfer_tl };
    const uint64_t wait_values[2] = { 0, t ? t->ready_at : 0 };
    const VkPipelineStageFlags wait_stages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT };
    const uint64_t gfx_value = ++sh->gfx_value;
    const VkSemaphore signals[2] = { sh->gfx_tl, p->sc_done[img] };
    const uint64_t signal_values[2] = { gfx_value, 0 };
    VkTimelineSemaphoreSubmitInfo tsi = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 2, .pWaitSemaphoreValues = wait_values,
        .signalSemaphoreValueCount = 2, .pSignalSemaphoreValues = signal_values,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
        .waitSemaphoreCount = 2, .pWaitSemaphores = waits, .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 2, .pSignalSemaphores = signals,
    };
    if (vkQueueSubmit(sh->gfx_q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        fprintf(stderr, "vypr: vulkan: present submit failed\n");
        goto out;
    }
    p->gfx_cmd[(p->gfx_at + 1) % 2].done_at = gfx_value;
    p->acquired_done[acquire_slot] = gfx_value;
    if (t) p->tex[p->cur].read_at = gfx_value;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &p->sc_done[img],
        .swapchainCount = 1, .pSwapchains = &p->sc, .pImageIndices = &img,
    };
    const VkResult r = vkQueuePresentKHR(sh->gfx_q, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) p->sc_stale = true;

out:
    p->ns_present += SDL_GetTicksNS() - t0;
}

static void vk_take_timings(void *impl, uint64_t *upload_ns, uint64_t *present_ns)
{
    struct vk_state *p = impl;
    if (upload_ns)  *upload_ns  = p->ns_upload;
    if (present_ns) *present_ns = p->ns_present;
    p->ns_upload = p->ns_present = 0;
}

const struct present_ops present_vk_ops = {
    .name         = "vulkan",
    .create       = vk_create,
    .destroy      = vk_destroy,
    .driver       = vk_driver,
    .upload       = vk_upload,
    .present      = vk_present,
    .take_timings = vk_take_timings,
};

#endif /* VYPR_HAVE_VULKAN */

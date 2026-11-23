#include "renderer.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(__APPLE__) || defined(__ANDROID__)
#include <dlfcn.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "bloom_down_frag_spv.h"
#include "bloom_up_frag_spv.h"
#include "car_frag_spv.h"
#include "car_model.h"
#include "car_vert_spv.h"
#include "fxaa_frag_spv.h"
#include "hdr_output_frag_spv.h"
#include "inspector_preview_frag_spv.h"
#include "mblur_tile_frag_spv.h"
#include "motion_vectors_frag_spv.h"
#include "platform_support.h"
#include "post_frag_spv.h"
#include "post_vert_spv.h"
#include "ssao_frag_spv.h"
#include "sscs_frag_spv.h"
#include "temporal_upscale_frag_spv.h"
#include "ui_frag_spv.h"
#include "ui_vert_spv.h"
#include "upscale_frag_spv.h"

namespace carview {

#define VK_TRY(expr, what)                                                       \
  do {                                                                           \
    const VkResult vk_try_result = (expr);                                       \
    if (vk_try_result != VK_SUCCESS) {                                           \
      error = std::string(what) + " failed (VkResult " +                         \
              std::to_string(int(vk_try_result)) + ")";                          \
      return false;                                                              \
    }                                                                            \
  } while (0)

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);
    ++static_cast<Renderer*>(user)->validation_messages_;
  }
  return VK_FALSE;
}

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                   VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access, uint32_t levels = 1) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {aspect, 0, levels, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

void mat4_mul(const float a[16], const float b[16], float out[16]) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                       a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
}

bool mat4_inv(const float m[16], float out[16]) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-12f) return false;
  const float inv_det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * inv_det;
  return true;
}

}  // namespace

Renderer::~Renderer() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    destroy_swapchain(false);
    destroy_targets();
    destroy_buffer(vertex_buffer_);
    destroy_buffer(index_buffer_);
    destroy_buffer(capture_buffer_);
    destroy_buffer(inspector_offscreen_buf_);
    destroy_buffer(ui_font_);
    for (FrameSlot& s : slots_) {
      if (s.overlay_mapped) vmaUnmapMemory(allocator_, s.overlay.allocation);
      if (s.ui_instances_mapped) vmaUnmapMemory(allocator_, s.ui_instances.allocation);
      if (s.ui_staging_mapped) vmaUnmapMemory(allocator_, s.ui_staging.allocation);
      if (s.lut_staging_mapped) vmaUnmapMemory(allocator_, s.lut_staging.allocation);
      if (s.inspector_readback_mapped) vmaUnmapMemory(allocator_, s.inspector_readback.allocation);
      s.overlay_mapped = s.ui_instances_mapped = s.ui_staging_mapped = s.lut_staging_mapped =
          s.inspector_readback_mapped = nullptr;
      destroy_buffer(s.overlay);
      destroy_buffer(s.ui_instances);
      destroy_buffer(s.ui_staging);
      destroy_buffer(s.lut_staging);
      destroy_buffer(s.inspector_readback);
      if (s.timestamps) vkDestroyQueryPool(device_, s.timestamps, nullptr);
      if (s.image_ready) vkDestroySemaphore(device_, s.image_ready, nullptr);
      if (s.fence) vkDestroyFence(device_, s.fence, nullptr);
    }
    destroy_image(ui_atlas_);
    if (ui_pipeline_sdr_ && ui_pipeline_sdr_ != ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_sdr_, nullptr);
    if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
    if (ui_layout_) vkDestroyPipelineLayout(device_, ui_layout_, nullptr);
    if (ui_set_layout_) vkDestroyDescriptorSetLayout(device_, ui_set_layout_, nullptr);
    if (ui_pool_) vkDestroyDescriptorPool(device_, ui_pool_, nullptr);
    if (ui_sampler_) vkDestroySampler(device_, ui_sampler_, nullptr);
    destroy_image(texture_);
    destroy_image(bench_output_);
    destroy_modern_passes();
    if (post_quarter_) vkDestroyPipeline(device_, post_quarter_, nullptr);
    if (post_bloom_down_) vkDestroyPipeline(device_, post_bloom_down_, nullptr);
    if (post_bloom_up_) vkDestroyPipeline(device_, post_bloom_up_, nullptr);
    if (post_final_) vkDestroyPipeline(device_, post_final_, nullptr);
    if (post_final_sdr_) vkDestroyPipeline(device_, post_final_sdr_, nullptr);
    if (post_layout_) vkDestroyPipelineLayout(device_, post_layout_, nullptr);
    if (post_set_layout_) vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
    if (post_pool_) vkDestroyDescriptorPool(device_, post_pool_, nullptr);
    if (post_sampler_) vkDestroySampler(device_, post_sampler_, nullptr);
    if (post_depth_sampler_) vkDestroySampler(device_, post_depth_sampler_, nullptr);
    destroy_buffer(post_optical_buf_);
    destroy_image(lut3d_);
    destroy_image(lum_16_);
    destroy_image(lum_adapt_[0]);
    destroy_image(lum_adapt_[1]);
    destroy_image(shadow_map_);
    if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
    if (timestamps_) vkDestroyQueryPool(device_, timestamps_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    for (VkCommandPool p : all_pools_) vkDestroyCommandPool(device_, p, nullptr);
    save_pipeline_cache();
    if (pipeline_cache_) {
      vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
      pipeline_cache_ = VK_NULL_HANDLE;
    }
    if (allocator_) {
      vmaDestroyAllocator(allocator_);
      allocator_ = VK_NULL_HANDLE;
    }
    vkDestroyDevice(device_, nullptr);
  }
  if (instance_) {
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    vkDestroyInstance(instance_, nullptr);
  }
}

bool Renderer::init(const Options& options, std::string& error) {
  options_ = options;
  graphics_ = modern::clamp_graphics(options.graphics);
  lut_strength_ = graphics_.lut_strength / 100.0f;
  frames_in_flight_ = std::clamp<uint32_t>(options.frames_in_flight, 1, kFramesInFlight);
  vsync_ = graphics_.vsync;
  if (options.vsync != modern::VSync::kFifo && graphics_.vsync == modern::VSync::kFifo) vsync_ = options.vsync;
  if (const char* e = std::getenv("SHIFT_VSYNC"); e && *e)  // fifo | mailbox | immediate (tuning)
    vsync_ = std::string(e) == "immediate" ? modern::VSync::kImmediate
             : std::string(e) == "mailbox" ? modern::VSync::kMailbox : modern::VSync::kFifo;
  if (const char* e = std::getenv("SHIFT_FRAMES_IN_FLIGHT"); e && *e)
    frames_in_flight_ = std::clamp<uint32_t>(uint32_t(std::atoi(e)), 1, kFramesInFlight);
  render_thread_ = std::this_thread::get_id();
  bool volk_ok = false;
#if defined(__ANDROID__)
  // Optional Turnip / custom Vulkan driver hook (files/state/drivers or SHIFT_ANDROID_GPU_DRIVER).
  if (const char* drv = std::getenv("SHIFT_ANDROID_GPU_DRIVER"); drv && *drv && std::strcmp(drv, "system") != 0) {
    if (void* lib = dlopen(drv, RTLD_NOW | RTLD_LOCAL)) {
      if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))) {
        volkInitializeCustom(gipa);
        volk_ok = true;
        std::fprintf(stderr, "[vulkan] loaded custom Android driver: %s\n", drv);
      }
    }
  }
#elif defined(__APPLE__)
  // Probe bundled MoltenVK / Vulkan loader inside Shift Engine.app/Contents/Frameworks
  // before falling back to system/Homebrew Vulkan loader.
  if (const char* base = SDL_GetBasePath()) {
    std::error_code ec;
    const std::filesystem::path fw = std::filesystem::path(base).parent_path() / "Frameworks";
    const std::filesystem::path candidates[] = {
        fw / "libvulkan.1.dylib",
        fw / "libMoltenVK.dylib",
        "/opt/homebrew/lib/libvulkan.1.dylib",
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libvulkan.1.dylib",
    };
    for (const auto& c : candidates) {
      if (!std::filesystem::is_regular_file(c, ec)) continue;
      if (void* lib = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))) {
          volkInitializeCustom(gipa);
          volk_ok = true;
          break;
        }
      }
    }
  }
#endif
  if (!volk_ok && volkInitialize() != VK_SUCCESS) {
    error = "no Vulkan loader found (libvulkan / vulkan-1.dll / libMoltenVK.dylib)";
    return false;
  }
  const uint32_t loader_ver = volkGetInstanceVersion();
#if defined(__ANDROID__) || defined(__APPLE__)
  if (loader_ver < VK_API_VERSION_1_1) {
    error = "the Vulkan loader is older than 1.1";
    return false;
  }
#else
  if (loader_ver < VK_API_VERSION_1_3) {
    error = "the Vulkan loader is older than 1.3";
    return false;
  }
#endif
  if (!create_instance(error) || !pick_device(error) || !create_device(error)) return false;
  {
    std::filesystem::path exe_dir;
    if (const char* base = SDL_GetBasePath(); base && *base) exe_dir = base;
    upscaler_runtime_ = probe_upscaler_runtime(device_vendor_id_, exe_dir);
    if (const char* tf = std::getenv("SHIFT_TEMPORAL_FORCE"); tf && *tf == '1') {
      upscaler_runtime_.support.temporal = true;
    }
  }
  if (options_.window) {
    if (!create_swapchain(error)) return false;
    for (FrameSlot& s : slots_) {
      VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VK_TRY(vkCreateSemaphore(device_, &semaphore, nullptr, &s.image_ready), "vkCreateSemaphore");
      VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      VK_TRY(vkCreateFence(device_, &fence, nullptr, &s.fence), "vkCreateFence");
      VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc.commandPool = pool_;
      alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      alloc.commandBufferCount = 1;
      VK_TRY(vkAllocateCommandBuffers(device_, &alloc, &s.cmd), "vkAllocateCommandBuffers");
      if (timestamp_ns_ > 0) {
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = kTimestamps;
        if (vkCreateQueryPool(device_, &qp, nullptr, &s.timestamps) != VK_SUCCESS) s.timestamps = VK_NULL_HANDLE;
      }
    }
  } else {
    color_format_ = VK_FORMAT_R8G8B8A8_UNORM;
  }
  const auto pipe_t0 = std::chrono::steady_clock::now();
  const bool pipe_ok = create_pipeline(scene_format(), error);
  pipeline_compile_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipe_t0).count();
  return pipe_ok;
}

bool Renderer::create_instance(std::string& error) {
  std::vector<const char*> extensions;
  if (options_.window) {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) {
      error = std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError();
      return false;
    }
    extensions.assign(names, names + count);
    uint32_t inst_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &inst_ext_count, nullptr);
    std::vector<VkExtensionProperties> inst_exts(inst_ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &inst_ext_count, inst_exts.data());
    for (const auto& ext : inst_exts) {
      if (std::strcmp(ext.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) {
        extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        hdr_colorspace_ext_ = true;
        break;
      }
    }
  }
  std::vector<const char*> layers;
  if (options_.validation) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    const bool found = std::any_of(available.begin(), available.end(), [](const auto& l) {
      return std::strcmp(l.layerName, kValidationLayer) == 0;
    });
    if (found) {
      layers.push_back(kValidationLayer);
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
      std::fprintf(stderr, "validation layer not found (set VK_LAYER_PATH); continuing without\n");
      options_.validation = false;
    }
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "carview";
  app.pEngineName = "shift-engine";
  app.apiVersion = volkGetInstanceVersion() >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  // Enable VK_KHR_portability_enumeration when offered (required for MoltenVK on macOS).
  {
    uint32_t ext_count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr) == VK_SUCCESS && ext_count > 0) {
      std::vector<VkExtensionProperties> avail(ext_count);
      vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, avail.data());
      const bool has_portability = std::any_of(avail.begin(), avail.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
      });
      if (has_portability) {
        if (std::none_of(extensions.begin(), extensions.end(), [](const char* n) {
              return std::strcmp(n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
            })) {
          extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
      } else {
        // Direct libMoltenVK.dylib (without libvulkan loader) does not advertise
        // VK_KHR_portability_enumeration even though SDL3 requests it on macOS.
        extensions.erase(
            std::remove_if(extensions.begin(), extensions.end(), [](const char* n) {
              return std::strcmp(n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
            }),
            extensions.end());
      }
    }
  }
  info.enabledExtensionCount = uint32_t(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  info.enabledLayerCount = uint32_t(layers.size());
  info.ppEnabledLayerNames = layers.data();
  VK_TRY(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
  volkLoadInstance(instance_);
  if (options_.validation) {
    VkDebugUtilsMessengerCreateInfoEXT messenger{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = debug_callback;
    messenger.pUserData = this;
    VK_TRY(vkCreateDebugUtilsMessengerEXT(instance_, &messenger, nullptr, &messenger_),
           "vkCreateDebugUtilsMessengerEXT");
  }
  if (options_.window && !SDL_Vulkan_CreateSurface(options_.window, instance_, nullptr, &surface_)) {
    error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
    return false;
  }
  return true;
}

bool Renderer::pick_device(std::string& error) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  int best_score = -1;
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(candidate, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) continue;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(candidate, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) continue;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, family_props.data());
    for (uint32_t i = 0; i < families; ++i) {
      if (!(family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      if (surface_) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present);
        if (!present) continue;
      }
      int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                  props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 :
                  props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
      if (score > best_score) {
        best_score = score;
        physical_ = candidate;
        queue_family_ = i;
        device_name_ = props.deviceName;
        device_vendor_id_ = props.vendorID;
      }
      break;
    }
  }
  if (!physical_) {
    error = "no Vulkan 1.3 device with dynamic rendering and synchronization2";
    return false;
  }
  vkGetPhysicalDeviceMemoryProperties(physical_, &memory_properties_);
  lazy_memory_supported_ = false;
  for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
    if (memory_properties_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
      lazy_memory_supported_ = true;
      break;
    }
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical_, &props);
  pipeline_cache_key_.vendor_id = props.vendorID;
  pipeline_cache_key_.device_id = props.deviceID;
  std::memcpy(pipeline_cache_key_.uuid.data(), props.pipelineCacheUUID, sizeof(pipeline_cache_key_.uuid));
  const VkSampleCountFlags counts =
      props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
  samples_ = VK_SAMPLE_COUNT_1_BIT;
  for (uint32_t s : {8u, 4u, 2u}) {
    if (s <= options_.msaa && (counts & s)) {
      samples_ = VkSampleCountFlagBits(s);
      break;
    }
  }
  {
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, family_props.data());
    if (queue_family_ < families && family_props[queue_family_].timestampValidBits > 0)
      timestamp_ns_ = props.limits.timestampPeriod;
  }
  VkPhysicalDeviceDepthStencilResolveProperties ds_resolve{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
  VkPhysicalDeviceDriverProperties drv_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  ds_resolve.pNext = &drv_props;
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props2.pNext = &ds_resolve;
  vkGetPhysicalDeviceProperties2(physical_, &props2);
  depth_resolve_supported_ = (ds_resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0;
  device_info_.device_name = props.deviceName;
  device_info_.vendor_id = props.vendorID;
  device_info_.device_id = props.deviceID;
  switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: device_info_.device_type = "discrete"; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: device_info_.device_type = "integrated"; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: device_info_.device_type = "virtual"; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: device_info_.device_type = "cpu"; break;
    default: device_info_.device_type = "other"; break;
  }
  device_info_.api_version = props.apiVersion;
  device_info_.driver_version = props.driverVersion;
  device_info_.driver_name = drv_props.driverName;
  device_info_.driver_info = drv_props.driverInfo;
  device_info_.has_timestamps = timestamp_ns_ > 0.0;
  {
    auto contains_ci = [](const std::string& hay, const char* needle) {
      std::string h = hay, n = needle;
      for (char& c : h) c = char(std::tolower(static_cast<unsigned char>(c)));
      for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
      return h.find(n) != std::string::npos;
    };
    device_info_.is_moltenvk = (drv_props.driverID == VK_DRIVER_ID_MOLTENVK) ||
                               contains_ci(device_info_.driver_name, "moltenvk") ||
                               contains_ci(device_info_.driver_info, "moltenvk");
    device_info_.is_turnip = (drv_props.driverID == VK_DRIVER_ID_MESA_TURNIP) ||
                             contains_ci(device_info_.driver_name, "turnip") ||
                             contains_ci(device_info_.driver_info, "turnip");
  }
  VkFormatProperties format_props;
  vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_D32_SFLOAT, &format_props);
  depth_format_ = (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                      ? VK_FORMAT_D32_SFLOAT
                      : VK_FORMAT_D16_UNORM;
  return true;
}

bool Renderer::create_device(std::string& error) {
  const float priorities[2] = {1.0f, 0.5f};
  uint32_t family_queues = 1;
  {
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, family_props.data());
    if (queue_family_ < families && family_props[queue_family_].queueCount >= 2 &&
        !std::getenv("SHIFT_NO_TRANSFER_QUEUE"))
      family_queues = 2;
  }
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = queue_family_;
  queue.queueCount = family_queues;
  queue.pQueuePriorities = priorities;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  // The car shaders' alpha-tested discards compile to demote (a Vulkan 1.3
  // required feature, but it must still be enabled).
  f13.shaderDemoteToHelperInvocation = VK_TRUE;
  // Compute shaders compiled with --target-env=vulkan1.3 may emit LocalSizeId.
  f13.maintenance4 = VK_TRUE;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;
  // The GPU-driven world (Slice 11): indirect draws with a GPU count, a
  // bindless texture array and compute culling. Needs Vulkan 1.2's
  // descriptor indexing and draw-indirect-count, and first-instance
  // indirect draws; without them the world is drawn the old way.
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f13.pNext = &f12;
  {
    VkPhysicalDeviceVulkan13Features o13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features o12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    o13.pNext = &o12;
    VkPhysicalDeviceFeatures2 offered2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    offered2.pNext = &o13;
    vkGetPhysicalDeviceFeatures2(physical_, &offered2);
    f13.maintenance4 = o13.maintenance4;
    VkPhysicalDeviceDescriptorIndexingProperties di{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &di;
    vkGetPhysicalDeviceProperties2(physical_, &p2);
    const bool env_off = [] {
      const char* e = std::getenv("SHIFT_GPU_WORLD");
      return e && *e == '0';
    }();
    const modern::SplitBindlessLimits bl_limits{
        di.maxPerStageDescriptorUpdateAfterBindSamplers,
        di.maxPerStageDescriptorUpdateAfterBindSampledImages,
        di.maxDescriptorSetUpdateAfterBindSampledImages,
    };
    gpu_driven_ = !env_off && o12.drawIndirectCount && o12.descriptorIndexing && o12.runtimeDescriptorArray &&
                  o12.shaderSampledImageArrayNonUniformIndexing && o12.descriptorBindingPartiallyBound &&
                  o12.descriptorBindingSampledImageUpdateAfterBind && o12.descriptorBindingUpdateUnusedWhilePending &&
                  offered2.features.multiDrawIndirect && offered2.features.drawIndirectFirstInstance &&
                  modern::split_bindless_supported(bl_limits, 4096);
    if (gpu_driven_) {
      f12.drawIndirectCount = VK_TRUE;
      f12.descriptorIndexing = VK_TRUE;
      f12.runtimeDescriptorArray = VK_TRUE;
      f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
      f12.descriptorBindingPartiallyBound = VK_TRUE;
      f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
      f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
      f2.features.multiDrawIndirect = VK_TRUE;
      f2.features.drawIndirectFirstInstance = VK_TRUE;
    }
  }
  {
    VkPhysicalDeviceFeatures offered{};
    vkGetPhysicalDeviceFeatures(physical_, &offered);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    if (offered.samplerAnisotropy && options_.anisotropy > 1.0f) {
      f2.features.samplerAnisotropy = VK_TRUE;
      anisotropy_ = std::min(options_.anisotropy, props.limits.maxSamplerAnisotropy);
    }
  }
  std::vector<const char*> extensions;
  if (surface_) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  bool has_portability_subset = false;
  {
    uint32_t dev_ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, nullptr);
    std::vector<VkExtensionProperties> dev_exts(dev_ext_count);
    if (dev_ext_count > 0) vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, dev_exts.data());
    bool has_vrs_ext = false;
    for (const auto& e : dev_exts) {
      if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
        extensions.push_back("VK_KHR_portability_subset");
        has_portability_subset = true;
        device_info_.portability_subset = true;
      } else if (std::strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0) {
        has_vrs_ext = true;
      }
    }
    if (has_vrs_ext) {
      VkPhysicalDeviceFragmentShadingRateFeaturesKHR offered_vrs{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
      VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      q2.pNext = &offered_vrs;
      vkGetPhysicalDeviceFeatures2(physical_, &q2);
      if (offered_vrs.pipelineFragmentShadingRate) {
        vrs_feat.pipelineFragmentShadingRate = VK_TRUE;
        vrs_feat.pNext = f2.pNext;
        f2.pNext = &vrs_feat;
        extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        vrs_supported_ = true;
      }
    }
  }
  if (std::getenv("SHIFT_VK_REPORT") != nullptr) {
    VkPhysicalDeviceProperties dp{};
    vkGetPhysicalDeviceProperties(physical_, &dp);
    const auto cap = evaluate_vulkan_capabilities(dp.deviceName, dp.apiVersion, f13.dynamicRendering == VK_TRUE,
                                                  f13.synchronization2 == VK_TRUE, has_portability_subset);
    std::fprintf(stderr, "%s\n", cap.report_line.c_str());
  }
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  info.pNext = &f2;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queue;
  info.enabledExtensionCount = uint32_t(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  VK_TRY(vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
  volkLoadDevice(device_);
  {
    std::vector<uint8_t> initial_cache;
    pipeline_cache_loaded_ = false;
    pipeline_cache_loaded_bytes_ = 0;
    const char* no_pc = std::getenv("SHIFT_NO_PIPELINE_CACHE");
    const bool cache_disabled = no_pc && *no_pc == '1';
    if (!cache_disabled && !options_.pipeline_cache_path.empty()) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(options_.pipeline_cache_path, ec)) {
        std::ifstream in(options_.pipeline_cache_path, std::ios::binary | std::ios::ate);
        if (in) {
          const auto sz = in.tellg();
          if (sz > 0 && sz < (128LL << 20)) {
            std::vector<uint8_t> raw(static_cast<size_t>(sz));
            in.seekg(0);
            if (in.read(reinterpret_cast<char*>(raw.data()), sz)) {
              if (modern::decode_pipeline_cache_file(raw.data(), raw.size(), pipeline_cache_key_, initial_cache)) {
                pipeline_cache_loaded_ = true;
                pipeline_cache_loaded_bytes_ = initial_cache.size();
              }
            }
          }
        }
      }
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = initial_cache.size();
    pci.pInitialData = initial_cache.empty() ? nullptr : initial_cache.data();
    if (vkCreatePipelineCache(device_, &pci, nullptr, &pipeline_cache_) != VK_SUCCESS && !initial_cache.empty()) {
      pipeline_cache_loaded_ = false;
      pipeline_cache_loaded_bytes_ = 0;
      pci.initialDataSize = 0;
      pci.pInitialData = nullptr;
      vkCreatePipelineCache(device_, &pci, nullptr, &pipeline_cache_);
    }
  }
  {
    VmaVulkanFunctions funcs{};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo alloc_info{};
    // Default locking (flags = 0): stage_world() and reaper_ run off the main thread.
    alloc_info.flags = 0;
    alloc_info.physicalDevice = physical_;
    alloc_info.device = device_;
    alloc_info.pVulkanFunctions = &funcs;
    alloc_info.instance = instance_;
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_TRY(vmaCreateAllocator(&alloc_info, &allocator_), "vmaCreateAllocator");
  }
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  // Uploads off the render thread (traffic and car models) go to the second
  // queue of the family, so they never wait behind a frame's submission.
  if (family_queues > 1) vkGetDeviceQueue(device_, queue_family_, 1, &transfer_queue_);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = queue_family_;
  VK_TRY(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
  return true;
}

void Renderer::save_pipeline_cache() const {
  if (!device_ || !pipeline_cache_ || options_.pipeline_cache_path.empty()) return;
  const char* no_pc = std::getenv("SHIFT_NO_PIPELINE_CACHE");
  if (no_pc && *no_pc == '1') return;
  size_t sz = 0;
  if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, nullptr) != VK_SUCCESS || sz == 0) return;
  if (pipeline_cache_loaded_ && sz == pipeline_cache_loaded_bytes_) return;
  std::vector<uint8_t> data(sz);
  if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, data.data()) != VK_SUCCESS || sz == 0) return;
  data.resize(sz);
  const auto encoded = modern::encode_pipeline_cache_file(pipeline_cache_key_, data.data(), data.size());
  std::error_code ec;
  if (options_.pipeline_cache_path.has_parent_path()) {
    std::filesystem::create_directories(options_.pipeline_cache_path.parent_path(), ec);
  }
  const auto tmp_path = options_.pipeline_cache_path.string() + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(encoded.data()), std::streamsize(encoded.size()));
    if (!out.good()) return;
  }
  std::filesystem::rename(tmp_path, options_.pipeline_cache_path, ec);
  if (ec) {
    // Windows rename fails if target exists; replace via remove + rename.
    std::error_code ec2;
    std::filesystem::remove(options_.pipeline_cache_path, ec2);
    std::filesystem::rename(tmp_path, options_.pipeline_cache_path, ec2);
    if (ec2) std::filesystem::remove(tmp_path, ec2);
  }
}

VkDeviceSize Renderer::device_local_bytes() const {
  VkDeviceSize total = 0;
  for (uint32_t i = 0; i < memory_properties_.memoryHeapCount; ++i) {
    if (memory_properties_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      total += memory_properties_.memoryHeaps[i].size;
  }
  return total;
}

Renderer::VramStats Renderer::vram_stats() const {
  VramStats out;
  if (!allocator_) return out;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
  vmaGetHeapBudgets(allocator_, budgets);
  for (uint32_t i = 0; i < memory_properties_.memoryHeapCount; ++i) {
    out.block_count += budgets[i].statistics.blockCount;
    out.allocation_count += budgets[i].statistics.allocationCount;
    out.block_bytes += budgets[i].statistics.blockBytes;
    out.allocation_bytes += budgets[i].statistics.allocationBytes;
    if (memory_properties_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      out.device_local_usage += budgets[i].usage;
      out.device_local_budget += budgets[i].budget;
    }
  }
  return out;
}

Renderer::VramStats Renderer::vram_detailed_stats() const {
  VramStats out = vram_stats();
  if (!allocator_) return out;
  VmaTotalStatistics stats{};
  vmaCalculateStatistics(allocator_, &stats);
  out.block_count = stats.total.statistics.blockCount;
  out.allocation_count = stats.total.statistics.allocationCount;
  out.block_bytes = stats.total.statistics.blockBytes;
  out.allocation_bytes = stats.total.statistics.allocationBytes;
  return out;
}

bool Renderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags flags, Buffer& out) {
  if (!allocator_ || size == 0) return false;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    alloc.flags = (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
                      ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                      : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }
  if (vmaCreateBuffer(allocator_, &info, &alloc, &out.buffer, &out.allocation, nullptr) != VK_SUCCESS) {
    out = Buffer{};
    return false;
  }
  out.size = size;
  return true;
}

void Renderer::destroy_buffer(Buffer& b) {
  if ((b.buffer || b.allocation) && allocator_) vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
  b = Buffer{};
}

bool Renderer::create_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                            VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                            VkImageAspectFlags aspect, Image& out) {
  if (!allocator_) return false;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  if ((usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) && lazy_memory_supported_) {
    alloc.preferredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
  }
  VmaAllocationInfo alloc_info{};
  if (vmaCreateImage(allocator_, &info, &alloc, &out.image, &out.allocation, &alloc_info) != VK_SUCCESS) {
    out = Image{};
    return false;
  }
  out.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {aspect, 0, levels, 0, 1};
  if (vkCreateImageView(device_, &view, nullptr, &out.view) != VK_SUCCESS) {
    destroy_image(out);
    return false;
  }
  return true;
}

void Renderer::destroy_image(Image& i) {
  if (i.view && device_) vkDestroyImageView(device_, i.view, nullptr);
  if ((i.image || i.allocation) && allocator_) vmaDestroyImage(allocator_, i.image, i.allocation);
  i = Image{};
}

// A one-time submit that waits for its own work only (a fence), never the
// whole queue: frames in flight keep drawing, and any thread may call it. On
// a thread other than the render thread it uses the second queue when the
// family has one (asynchronous uploads).
bool Renderer::submit_once(const std::function<void(VkCommandBuffer)>& record) {
  const bool render_thread = std::this_thread::get_id() == render_thread_;
  // A command pool is externally synchronised: the render thread has its
  // own, other threads borrow one from the free list for the call.
  VkCommandPool use_pool = pool_;
  if (!render_thread) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!free_pools_.empty()) {
      use_pool = free_pools_.back();
      free_pools_.pop_back();
    } else {
      VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool.queueFamilyIndex = queue_family_;
      if (vkCreateCommandPool(device_, &pool, nullptr, &use_pool) != VK_SUCCESS) return false;
      all_pools_.push_back(use_pool);
    }
  }
  auto give_back = [&] {
    if (render_thread) return;
    std::lock_guard<std::mutex> lock(pool_mutex_);
    free_pools_.push_back(use_pool);
  };
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = use_pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd;
  if (vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) {
    give_back();
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  record(cmd);
  vkEndCommandBuffer(cmd);
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  bool ok = vkCreateFence(device_, &fence_info, nullptr, &fence) == VK_SUCCESS;
  if (ok) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const bool async = !render_thread && transfer_queue_ != VK_NULL_HANDLE;
    {
      // One mutex for both queues keeps this simple: a submit is brief.
      std::lock_guard<std::mutex> lock(queue_mutex_);
      ok = vkQueueSubmit(async ? transfer_queue_ : queue_, 1, &submit, fence) == VK_SUCCESS;
    }
    ok = ok && vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
  }
  if (fence) vkDestroyFence(device_, fence, nullptr);
  vkFreeCommandBuffers(device_, use_pool, 1, &cmd);
  give_back();
  return ok;
}

void Renderer::wait_idle() {
  if (!device_) return;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    vkDeviceWaitIdle(device_);
  }
  frames_done_ = frames_submitted_;
}

bool Renderer::upload_buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                             Buffer& out) {
  staging_upload_bytes_ += uint64_t(size);
  Buffer staging;
  if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging))
    return false;
  void* mapped = nullptr;
  if (vmaMapMemory(allocator_, staging.allocation, &mapped) != VK_SUCCESS) {
    destroy_buffer(staging);
    return false;
  }
  std::memcpy(mapped, data, size_t(size));
  vmaUnmapMemory(allocator_, staging.allocation);
  bool ok = create_buffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
  if (ok) {
    ok = submit_once([&](VkCommandBuffer cmd) {
      VkBufferCopy copy{0, 0, size};
      vkCmdCopyBuffer(cmd, staging.buffer, out.buffer, 1, &copy);
    });
  }
  destroy_buffer(staging);
  return ok;
}

bool Renderer::replace_geometry(const CarModel& model, std::string& error) {
  vkDeviceWaitIdle(device_);
  destroy_buffer(vertex_buffer_);
  destroy_buffer(index_buffer_);
  if (!upload_buffer(model.vertices.data(), model.vertices.size() * sizeof(Vertex),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer_) ||
      !upload_buffer(model.indices.data(), model.indices.size() * sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer_)) {
    error = "mesh upload failed";
    return false;
  }
  index_count_ = uint32_t(model.indices.size());
  return true;
}

bool Renderer::upload(const CarModel& model, std::string& error) {
  if (!upload_buffer(model.vertices.data(), model.vertices.size() * sizeof(Vertex),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer_) ||
      !upload_buffer(model.indices.data(), model.indices.size() * sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer_)) {
    error = "mesh upload failed";
    return false;
  }
  index_count_ = uint32_t(model.indices.size());

  // The atlas, every decoded level, as RGBA8.
  const auto& levels = model.texture_levels;
  VkDeviceSize total = 0;
  for (const auto& level : levels) total += level.rgba.size();
  Buffer staging;
  if (!create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging)) {
    error = "texture staging buffer";
    return false;
  }
  uint8_t* mapped = nullptr;
  if (vmaMapMemory(allocator_, staging.allocation, reinterpret_cast<void**>(&mapped)) != VK_SUCCESS) {
    destroy_buffer(staging);
    error = "texture staging map";
    return false;
  }
  std::vector<VkBufferImageCopy> copies;
  VkDeviceSize offset = 0;
  for (uint32_t i = 0; i < levels.size(); ++i) {
    std::memcpy(mapped + offset, levels[i].rgba.data(), levels[i].rgba.size());
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    copy.imageExtent = {levels[i].width, levels[i].height, 1};
    copies.push_back(copy);
    offset += levels[i].rgba.size();
  }
  vmaUnmapMemory(allocator_, staging.allocation);
  const uint32_t level_count = uint32_t(levels.size());
  if (!create_image(levels[0].width, levels[0].height, level_count, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, texture_)) {
    destroy_buffer(staging);
    error = "texture image";
    return false;
  }
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    image_barrier(cmd, texture_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, level_count);
    vkCmdCopyBufferToImage(cmd, staging.buffer, texture_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                           copies.data());
    image_barrier(cmd, texture_.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                  level_count);
  });
  destroy_buffer(staging);
  if (!ok) {
    error = "texture upload";
    return false;
  }

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler.maxLod = float(level_count);
  VK_TRY(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler");
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_),
         "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = descriptor_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &set_layout_;
  VK_TRY(vkAllocateDescriptorSets(device_, &alloc, &descriptor_set_), "vkAllocateDescriptorSets");
  VkDescriptorImageInfo image_info{sampler_, texture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = descriptor_set_;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  return true;
}

bool Renderer::create_pipeline(VkFormat color_format, std::string& error) {
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 1;
  set.pBindings = &binding;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_),
         "vkCreateDescriptorSetLayout");
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(FrameConstants)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &set_layout_;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_),
         "vkCreatePipelineLayout");

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
  VK_TRY(make_module(kCar_vert_spv, sizeof(kCar_vert_spv), vert), "vertex shader");
  VK_TRY(make_module(kCar_frag_spv, sizeof(kCar_frag_spv), frag), "fragment shader");
  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};

  VkVertexInputBindingDescription vertex_binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[4] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, occlusion)}};
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &vertex_binding;
  vertex_input.vertexAttributeDescriptionCount = 4;
  vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;  // strip winding is not established yet
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = samples_;
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = options_.reverse_z ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[3] = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = vrs_supported_ ? 3u : 2u;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format;
  rendering.depthAttachmentFormat = depth_format_;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout_;
  const VkResult result =
      vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  VK_TRY(result, "vkCreateGraphicsPipelines");
  return true;
}

bool Renderer::create_targets(uint32_t width, uint32_t height, std::string& error) {
  destroy_targets();
  const modern::Extent re = options_.post.enabled
                                ? modern::scene_extent({width, height}, graphics_.scale, graphics_.base_540p)
                                : modern::Extent{width, height};
  const uint32_t rw = re.w ? re.w : width;
  const uint32_t rh = re.h ? re.h : height;
  if (samples_ != VK_SAMPLE_COUNT_1_BIT &&
      !create_image(rw, rh, 1, scene_format(), samples_,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, msaa_color_)) {
    error = "MSAA colour target";
    return false;
  }
  const VkImageUsageFlags depth_usage =
      samples_ != VK_SAMPLE_COUNT_1_BIT
          ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
          : (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  if (!create_image(rw, rh, 1, depth_format_, samples_, depth_usage,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_)) {
    error = "depth target";
    return false;
  }
  if (!create_image(rw, rh, 1, depth_format_, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_resolved_)) {
    error = "resolved depth target";
    return false;
  }
  if (options_.post.enabled) {
    // Resolved HDR scene (always needed when post is on) + lazily allocated optional passes.
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!create_image(rw, rh, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                      hdr_)) {
      error = "post hdr target";
      return false;
    }
    const auto post_t0 = std::chrono::steady_clock::now();
    if (!create_post(error)) return false;
    if (!create_modern_passes(error)) return false;
    pipeline_compile_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - post_t0).count();
    const auto up_choice = modern::resolve_upscaler(graphics_.upscaler, upscaler_runtime_.support, {rw, rh},
                                                    {width, height});
    const modern::ModernTargetPlan plan = modern::plan_modern_targets(
        graphics_, {rw, rh}, {width, height}, options_.post.bloom, hdr_mode_, hdr_pattern_, inspector_active_,
        inspector_view_ == InspectorView::kMotion, up_choice, options_.post.motion_blur);
    allocated_targets_ = plan;

    if (plan.bloom) {
      // 6-level Jimenez / Dual-Kawase pyramid starting at half-resolution (rw/2 x rh/2).
      uint32_t bw = std::max(1u, rw / 2), bh = std::max(1u, rh / 2);
      for (uint32_t k = 0; k < kBloomLevels; ++k) {
        bloom_w_[k] = bw;
        bloom_h_[k] = bh;
        if (!create_image(bw, bh, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                          bloom_a_[k]) ||
            !create_image(bw, bh, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                          bloom_b_[k])) {
          error = "post bloom pyramid";
          return false;
        }
        bw = std::max(1u, bw / 2);
        bh = std::max(1u, bh / 2);
      }
    }
    if (plan.ssao) {
      const bool full_ao = modern::gtao_full_res(graphics_.ssao);
      const uint32_t aow = full_ao ? rw : std::max(1u, rw / 2);
      const uint32_t aoh = full_ao ? rh : std::max(1u, rh / 2);
      ao_width_ = aow;
      ao_height_ = aoh;
      if (!create_image(aow, aoh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ao_raw_) ||
          !create_image(aow, aoh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ao_tmp_) ||
          !create_image(aow, aoh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ao_blur_)) {
        error = "post gtao targets";
        return false;
      }
    }
    if (plan.sscs) {
      const bool full_cs = modern::contact_shadow_full_res(graphics_.contact_shadows);
      sscs_width_ = full_cs ? rw : std::max(1u, rw / 2);
      sscs_height_ = full_cs ? rh : std::max(1u, rh / 2);
      if (!create_image(sscs_width_, sscs_height_, 1, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, sscs_)) {
        error = "post sscs target";
        return false;
      }
    }
    const VkImageUsageFlags sdr_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (plan.modern_chain) {
      if (!create_image(rw, rh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, scene_sdr_) ||
          !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, stand_in_)) {
        error = "post modern targets";
        return false;
      }
    }
    if (plan.fxaa_scene &&
        !create_image(rw, rh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, scene_fxaa_)) {
      error = "post fxaa target";
      return false;
    }
    if (plan.fxaa_standin &&
        !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, stand_in_fxaa_)) {
      error = "post standin fxaa target";
      return false;
    }
    if (plan.upscale_tmp &&
        !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, upscale_tmp_)) {
      error = "post upscale target";
      return false;
    }
    if (plan.motion_vec &&
        !create_image(rw, rh, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      motion_vec_)) {
      error = "post motion target";
      return false;
    }
    if (plan.mblur_tiles) {
      mblur_tile_w_ = std::max(1u, (rw + 15u) / 16u);
      mblur_tile_h_ = std::max(1u, (rh + 15u) / 16u);
      if (!create_image(mblur_tile_w_, mblur_tile_h_, 1, VK_FORMAT_R16G16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, mblur_tile_) ||
          !create_image(mblur_tile_w_, mblur_tile_h_, 1, VK_FORMAT_R16G16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, mblur_neighbor_)) {
        error = "post mblur tile targets";
        return false;
      }
    }
    if (plan.temporal_hist &&
        (!create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, temporal_hist_[0]) ||
         !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, temporal_hist_[1]))) {
      error = "post temporal targets";
      return false;
    }
    if (plan.inspector &&
        !create_image(kInspectorPreviewW, kInspectorPreviewH, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, inspector_thumb_)) {
      error = "inspector thumbnail image";
      return false;
    }
    temporal_reset_ = true;
    temporal_idx_ = 0;
    // Wire the 14 post descriptor sets:
    //   0: lum16 (hdr, hdr)
    //   1: lum_adapt (lum_16, lum_adapt[1])
    //   2: bloom_down[0] (hdr, hdr) -> bloom_a[0] (Karis prefilter)
    //   3..7: bloom_down[1..5] (bloom_a[k-1], bloom_a[k-1]) -> bloom_a[k]
    //   8: bloom_up[4] (bloom_a[4], bloom_a[5]) -> bloom_b[4]
    //   9: bloom_up[3] (bloom_a[3], bloom_b[4]) -> bloom_b[3]
    //   10: bloom_up[2] (bloom_a[2], bloom_b[3]) -> bloom_b[2]
    //   11: bloom_up[1] (bloom_a[1], bloom_b[2]) -> bloom_b[1]
    //   12: bloom_up[0] (bloom_a[0], bloom_b[1]) -> bloom_b[0]
    //   13: composite (hdr, bloom_b[0])
    const VkImageView fallback_v = ao_white_.view;
    const VkImageView vel_fb = vel_zero_.view ? vel_zero_.view : fallback_v;
    auto bv = [&](const Image& img) { return img.view ? img.view : fallback_v; };
    VkImageView sources[kPostSetCount][2] = {};
    sources[0][0] = hdr_.view; sources[0][1] = hdr_.view;
    sources[1][0] = lum_16_.view; sources[1][1] = lum_adapt_[1].view;
    sources[2][0] = hdr_.view; sources[2][1] = hdr_.view;
    for (uint32_t k = 1; k < kBloomLevels; ++k) {
      sources[2 + k][0] = bv(bloom_a_[k - 1]); sources[2 + k][1] = bv(bloom_a_[k - 1]);
    }
    sources[8][0] = bv(bloom_a_[4]);  sources[8][1] = bv(bloom_a_[5]);
    sources[9][0] = bv(bloom_a_[3]);  sources[9][1] = bv(bloom_b_[4]);
    sources[10][0] = bv(bloom_a_[2]); sources[10][1] = bv(bloom_b_[3]);
    sources[11][0] = bv(bloom_a_[1]); sources[11][1] = bv(bloom_b_[2]);
    sources[12][0] = bv(bloom_a_[0]); sources[12][1] = bv(bloom_b_[1]);
    sources[13][0] = hdr_.view;       sources[13][1] = bv(bloom_b_[0]);

    std::vector<VkDescriptorImageInfo> images(kPostSetCount * 9);
    std::vector<VkDescriptorBufferInfo> buf_infos(kPostSetCount);
    std::vector<VkWriteDescriptorSet> writes(kPostSetCount * 10);
    const VkImageView ao_view = (graphics_.ssao != modern::SsaoQuality::kOff && ao_blur_.view) ? ao_blur_.view
                                                                                               : ao_white_.view;
    const VkImageView sscs_view =
        (graphics_.contact_shadows != modern::ContactShadows::kOff && sscs_.view) ? sscs_.view : ao_white_.view;
    const VkImageView motion_view = motion_vec_.view ? motion_vec_.view : vel_fb;
    const VkImageView neighbor_view = mblur_neighbor_.view ? mblur_neighbor_.view : vel_fb;
    for (uint32_t s = 0; s < kPostSetCount; ++s) {
      // Only the final composite (s == 13) samples lum_adapt_[0]; pass 1 renders into lum_adapt_[0],
      // so earlier sets bind lum_adapt_[1] (already SHADER_READ_ONLY_OPTIMAL) to avoid a layout conflict.
      const VkImageView adapt_view = (s == 13) ? lum_adapt_[0].view : lum_adapt_[1].view;
      const VkImageView views[4] = {sources[s][0], sources[s][1], lut3d_.view, adapt_view};
      for (uint32_t b = 0; b < 4; ++b) {
        const size_t iidx = size_t(s) * 9 + b;
        const size_t widx = size_t(s) * 10 + b;
        images[iidx] = {post_sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[iidx];
      }
      {
        const size_t iidx = size_t(s) * 9 + 4;
        const size_t widx = size_t(s) * 10 + 4;
        images[iidx] = {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = 4;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[iidx];
      }
      {
        const size_t widx = size_t(s) * 10 + 5;
        buf_infos[s] = {post_optical_buf_.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = 5;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &buf_infos[s];
      }
      const VkImageView extra_views[4] = {ao_view, sscs_view, motion_view, neighbor_view};
      for (uint32_t e = 0; e < 4; ++e) {
        const size_t iidx = size_t(s) * 9 + 5 + e;
        const size_t widx = size_t(s) * 10 + 6 + e;
        images[iidx] = {post_sampler_, extra_views[e], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = 6 + e;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[iidx];
      }
    }
    vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);
    update_modern_descriptors();
  }
  render_width_ = rw;
  render_height_ = rh;
  target_width_ = width;
  target_height_ = height;
  target_color_format_ = color_format_;
  targets_dirty_ = false;
  ++target_recreate_count_;
  return true;
}

bool Renderer::ensure_shadow_map(std::string& error) {
  if (shadow_map_.view) return true;
  const uint32_t size = options_.shadow.size ? options_.shadow.size : 1;
  const uint32_t width = size > 1 ? size * 2 : 1;
  if (!create_image(width, size, 1, kShadowFormat, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT,
                    shadow_map_)) {
    error = "shadow map";
    return false;
  }
  shadow_size_ = size;
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  sampler.compareEnable = VK_TRUE;
  sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  sampler.maxLod = 0.0f;
  if (vkCreateSampler(device_, &sampler, nullptr, &shadow_sampler_) != VK_SUCCESS) {
    error = "shadow sampler";
    return false;
  }
  // Cleared to the far plane (lit) and left ready to sample.
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = shadow_map_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearDepthStencilValue far{1.0f, 0};
    vkCmdClearDepthStencilImage(cmd, shadow_map_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1,
                                &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
  });
  if (!ok) error = "shadow map clear";
  return ok;
}

void Renderer::apply_vrs(VkCommandBuffer cmd) const {
  if (!vrs_supported_ || !vkCmdSetFragmentShadingRateKHR) return;
  const VkExtent2D sz = graphics_.vrs ? VkExtent2D{2, 2} : VkExtent2D{1, 1};
  const VkFragmentShadingRateCombinerOpKHR ops[2] = {
      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
  vkCmdSetFragmentShadingRateKHR(cmd, &sz, ops);
}

void Renderer::set_graphics_settings(const modern::GraphicsSettings& g) {
  const modern::GraphicsSettings clamped = modern::clamp_graphics(g);
  const bool need_targets =
      clamped.scale != graphics_.scale || clamped.base_540p != graphics_.base_540p ||
      clamped.ssao != graphics_.ssao || clamped.contact_shadows != graphics_.contact_shadows ||
      clamped.velocity_blur != graphics_.velocity_blur || clamped.hdr != graphics_.hdr ||
      clamped.hdr_compare != graphics_.hdr_compare ||
      clamped.aa != graphics_.aa || clamped.upscaler != graphics_.upscaler;
  const bool hdr_changed = clamped.hdr != graphics_.hdr;
  graphics_ = clamped;
  lut_strength_ = graphics_.lut_strength / 100.0f;
  set_vsync(graphics_.vsync);
  if (need_targets) targets_dirty_ = true;
  if (hdr_changed && options_.window) swapchain_dirty_ = true;
}

void Renderer::set_camera_projection(float near_plane, float far_plane, float proj00, float proj11) {
  cam_near_ = std::max(near_plane, 0.01f);
  cam_far_ = std::max(far_plane, cam_near_ + 1.0f);
  cam_proj00_ = proj00;
  cam_proj11_ = proj11;
}

void Renderer::set_camera_vp(const float vp[16], float jitter_uv_x, float jitter_uv_y) {
  if (!vp) return;
  std::memcpy(cam_vp_, vp, sizeof(cam_vp_));
  cam_jitter_uv_x_ = jitter_uv_x;
  cam_jitter_uv_y_ = jitter_uv_y;
  cam_vp_set_ = true;
}

void Renderer::set_inspector_preview(bool active, InspectorView view) {
  if (active != inspector_active_ || (active && view != inspector_view_)) targets_dirty_ = true;
  inspector_active_ = active;
  inspector_view_ = view;
  if (!active) inspector_has_rgba_ = false;
}

void Renderer::set_hiz_preview_view(VkImageView view) {
  if (view == hiz_preview_view_) return;
  hiz_preview_view_ = view;
  if (!modern_ready_ || !inspector_sets_[5] || !post_depth_sampler_ || !post_sampler_ || !depth_resolved_.view) return;
  if (options_.window && device_) wait_idle();
  const VkDescriptorImageInfo info =
      hiz_preview_view_ ? VkDescriptorImageInfo{post_sampler_, hiz_preview_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                        : VkDescriptorImageInfo{post_depth_sampler_, depth_resolved_.view,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = inspector_sets_[5];
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &info;
  vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

bool Renderer::take_inspector_preview(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  if (!inspector_has_rgba_ || inspector_rgba_.empty()) return false;
  rgba = inspector_rgba_;
  width = kInspectorPreviewW;
  height = kInspectorPreviewH;
  inspector_has_rgba_ = false;
  return true;
}

void Renderer::set_post_lut(std::span<const uint8_t> rgba, uint32_t size) {
  if (size < 2 || size > 65) return;
  const size_t want = size_t(size) * size * size * 4u;
  if (rgba.size() < want) return;
  lut_pending_.assign(rgba.begin(), rgba.begin() + ptrdiff_t(want));
  lut_pending_size_ = size;
  lut_dirty_ = true;
}

bool Renderer::ensure_lut_image(uint32_t size, std::string& error) {
  if (size < 2 || size > 65) return false;
  if (lut3d_.image && lut_image_size_ == size) return true;
  if (device_) wait_idle();
  destroy_image(lut3d_);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_3D;
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent = {size, size, size};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo alloc_info{};
  VK_TRY(vmaCreateImage(allocator_, &info, &alloc, &lut3d_.image, &lut3d_.allocation, &alloc_info),
         "vmaCreateImage (3D LUT)");
  lut3d_.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = lut3d_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_3D;
  view.format = VK_FORMAT_R8G8B8A8_UNORM;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_TRY(vkCreateImageView(device_, &view, nullptr, &lut3d_.view), "vkCreateImageView (3D LUT)");
  lut_image_size_ = size;
  if (!submit_once([&](VkCommandBuffer cmd) {
        image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      })) {
    error = "3D LUT layout transition";
    return false;
  }
  if (post_pool_ && post_sampler_) {
    std::vector<VkDescriptorImageInfo> imgs(kPostSetCount);
    std::vector<VkWriteDescriptorSet> writes(kPostSetCount);
    for (uint32_t s = 0; s < kPostSetCount; ++s) {
      imgs[s] = {post_sampler_, lut3d_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[s] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[s].dstSet = post_sets_[s];
      writes[s].dstBinding = 2;
      writes[s].descriptorCount = 1;
      writes[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[s].pImageInfo = &imgs[s];
    }
    vkUpdateDescriptorSets(device_, kPostSetCount, writes.data(), 0, nullptr);
  }
  return true;
}

bool Renderer::create_post(std::string& error) {
  if (post_final_) return true;
  // Create auto-exposure targets (16x16 + 1x1 ping-pong) and 16x16x16 3D LUT once.
  const VkImageUsageFlags lum_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!create_image(16, 16, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, lum_usage, VK_IMAGE_ASPECT_COLOR_BIT, lum_16_) ||
      !create_image(1, 1, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, lum_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                    lum_adapt_[0]) ||
      !create_image(1, 1, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, lum_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                    lum_adapt_[1])) {
    error = "post luminance targets";
    return false;
  }
  // Create 16x16x16 3D RGBA8 LUT image + host-visible staging buffer.
  {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_3D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {16, 16, 16};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VmaAllocationInfo alloc_info{};
    VK_TRY(vmaCreateImage(allocator_, &info, &alloc, &lut3d_.image, &lut3d_.allocation, &alloc_info),
           "vmaCreateImage (3D LUT)");
    lut3d_.bytes = alloc_info.size;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = lut3d_.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_3D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_TRY(vkCreateImageView(device_, &view, nullptr, &lut3d_.view), "vkCreateImageView (3D LUT)");

    constexpr VkDeviceSize kLutBytes = 16 * 16 * 16 * 4;
    Buffer lut_staging;
    if (!create_buffer(kLutBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, lut_staging)) {
      error = "3D LUT staging buffer";
      return false;
    }
    void* lut_staging_mapped = nullptr;
    VK_TRY(vmaMapMemory(allocator_, lut_staging.allocation, &lut_staging_mapped), "map 3D LUT staging");
    // Fill identity ramp by default.
    auto* dst = static_cast<uint8_t*>(lut_staging_mapped);
    for (uint32_t b = 0; b < 16; ++b)
      for (uint32_t g = 0; g < 16; ++g)
        for (uint32_t r = 0; r < 16; ++r) {
          const size_t o = ((size_t(b) * 16 + g) * 16 + r) * 4;
          dst[o + 0] = uint8_t(std::lround(double(r) * 255.0 / 15.0));
          dst[o + 1] = uint8_t(std::lround(double(g) * 255.0 / 15.0));
          dst[o + 2] = uint8_t(std::lround(double(b) * 255.0 / 15.0));
          dst[o + 3] = 255;
        }
    const bool ok = submit_once([&](VkCommandBuffer cmd) {
      image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkBufferImageCopy reg{};
      reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      reg.imageExtent = {16, 16, 16};
      vkCmdCopyBufferToImage(cmd, lut_staging.buffer, lut3d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
      image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      // Clear lum_adapt_[0..1] to neutral initial luminance and transition to SHADER_READ_ONLY_OPTIMAL.
      const VkClearColorValue init_lum{{0.35f, 0.35f, 0.0f, 1.0f}};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      for (int i = 0; i < 2; ++i) {
        image_barrier(cmd, lum_adapt_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd, lum_adapt_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &init_lum, 1, &range);
        image_barrier(cmd, lum_adapt_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      }
    });
    vmaUnmapMemory(allocator_, lut_staging.allocation);
    destroy_buffer(lut_staging);
    if (!ok) {
      error = "3D LUT initial upload";
      return false;
    }
    lum_reset_ = true;
  }

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(device_, &sampler, nullptr, &post_sampler_), "post sampler");
  VkSamplerCreateInfo dsampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  dsampler.magFilter = dsampler.minFilter = VK_FILTER_NEAREST;
  dsampler.addressModeU = dsampler.addressModeV = dsampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(device_, &dsampler, nullptr, &post_depth_sampler_), "post depth sampler");
  constexpr VkDeviceSize kOpticalUboBytes = 14 * 16;  // 14 vec4s = 224 bytes
  if (!create_buffer(kOpticalUboBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, post_optical_buf_)) {
    error = "post optical UBO";
    return false;
  }
  const VkDescriptorSetLayoutBinding bindings[10] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 10;
  set.pBindings = bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &post_set_layout_), "post set layout");
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &post_set_layout_;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &post_layout_), "post pipeline layout");
  const VkDescriptorPoolSize pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPostSetCount * 9},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kPostSetCount}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = kPostSetCount;
  pool.poolSizeCount = 2;
  pool.pPoolSizes = pool_sizes;
  VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &post_pool_), "post descriptor pool");
  VkDescriptorSetLayout layouts[kPostSetCount];
  for (uint32_t i = 0; i < kPostSetCount; ++i) layouts[i] = post_set_layout_;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = post_pool_;
  alloc.descriptorSetCount = kPostSetCount;
  alloc.pSetLayouts = layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &alloc, post_sets_), "post descriptor sets");

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, frag_bdown = VK_NULL_HANDLE, frag_bup = VK_NULL_HANDLE;
  VK_TRY(make_module(kPost_vert_spv, sizeof(kPost_vert_spv), vert), "post vertex shader");
  VK_TRY(make_module(kPost_frag_spv, sizeof(kPost_frag_spv), frag), "post fragment shader");
  VK_TRY(make_module(kBloom_down_frag_spv, sizeof(kBloom_down_frag_spv), frag_bdown), "bloom down shader");
  VK_TRY(make_module(kBloom_up_frag_spv, sizeof(kBloom_up_frag_spv), frag_bup), "bloom up shader");
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  auto build_post_pipe = [&](VkShaderModule fmod, VkFormat format, VkPipeline& dst) -> VkResult {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fmod, "main",
         nullptr}};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = post_layout_;
    return vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &dst);
  };
  VkResult result = build_post_pipe(frag, scene_format(), post_quarter_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag_bdown, scene_format(), post_bloom_down_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag_bup, scene_format(), post_bloom_up_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag, color_format_, post_final_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag, VK_FORMAT_R8G8B8A8_UNORM, post_final_sdr_);
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  vkDestroyShaderModule(device_, frag_bdown, nullptr);
  vkDestroyShaderModule(device_, frag_bup, nullptr);
  VK_TRY(result, "post pipelines");
  return true;
}

bool Renderer::create_modern_passes(std::string& error) {
  if (modern_ready_) return true;
  // 1x1 neutral fallback textures:
  //   ao_white_: RGBA8 {1.0, 0.5, 0.5, 1.0} (full visibility + neutral +Z view bent normal)
  //   vel_zero_: RGBA16F {0, 0, 0, 1000} (zero motion vector, 0 reactive, far depth)
  if (!ao_white_.image) {
    if (!create_image(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      ao_white_) ||
        !create_image(1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      vel_zero_)) {
      error = "modern fallback targets";
      return false;
    }
    const bool ok = submit_once([&](VkCommandBuffer cmd) {
      const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      const VkClearColorValue neutral_ao{{1.0f, 0.5f, 0.5f, 1.0f}};
      const VkClearColorValue zero_vel{{0.0f, 0.0f, 0.0f, 1000.0f}};
      image_barrier(cmd, ao_white_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      image_barrier(cmd, vel_zero_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      vkCmdClearColorImage(cmd, ao_white_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &neutral_ao, 1, &rng);
      vkCmdClearColorImage(cmd, vel_zero_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_vel, 1, &rng);
      image_barrier(cmd, ao_white_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      image_barrier(cmd, vel_zero_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!ok) {
      error = "modern fallback clear";
      return false;
    }
  }
  const VkDescriptorSetLayoutBinding ssao_bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo ssao_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ssao_set_info.bindingCount = 2;
  ssao_set_info.pBindings = ssao_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &ssao_set_info, nullptr, &ssao_set_layout_), "ssao set layout");

  const VkDescriptorSetLayoutBinding tex_binding{
      0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo tex_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  tex_set_info.bindingCount = 1;
  tex_set_info.pBindings = &tex_binding;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &tex_set_info, nullptr, &single_tex_set_layout_),
         "single tex set layout");

  const VkDescriptorSetLayoutBinding temporal_bindings[3] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo temp_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  temp_set_info.bindingCount = 3;
  temp_set_info.pBindings = temporal_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &temp_set_info, nullptr, &temporal_set_layout_), "temporal set layout");

  auto make_layout = [&](VkDescriptorSetLayout set_layout, uint32_t push_bytes, VkPipelineLayout& out,
                         const char* what) -> bool {
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &set_layout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    VK_TRY(vkCreatePipelineLayout(device_, &info, nullptr, &out), what);
    return true;
  };
  if (!make_layout(ssao_set_layout_, 48, ssao_layout_, "ssao layout") ||
      !make_layout(single_tex_set_layout_, 48, sscs_layout_, "sscs layout") ||
      !make_layout(single_tex_set_layout_, 16, mblur_tile_layout_, "mblur tile layout") ||
      !make_layout(single_tex_set_layout_, 16, fxaa_layout_, "fxaa layout") ||
      !make_layout(single_tex_set_layout_, 48, upscale_layout_, "upscale layout") ||
      !make_layout(single_tex_set_layout_, 48, hdr_layout_, "hdr layout") ||
      !make_layout(single_tex_set_layout_, 80, motion_layout_, "motion layout") ||
      !make_layout(temporal_set_layout_, 48, temporal_layout_, "temporal layout") ||
      !make_layout(single_tex_set_layout_, 16, inspector_layout_, "inspector layout"))
    return false;

  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 32;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VK_TRY(vkCreateDescriptorPool(device_, &pool_info, nullptr, &modern_pool_), "modern descriptor pool");

  VkDescriptorSetLayout ssao_layouts[3] = {ssao_set_layout_, ssao_set_layout_, ssao_set_layout_};
  VkDescriptorSetAllocateInfo salloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  salloc.descriptorPool = modern_pool_;
  salloc.descriptorSetCount = 3;
  salloc.pSetLayouts = ssao_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &salloc, ssao_sets_), "ssao descriptor sets");

  VkDescriptorSetLayout tex_layouts[19];
  for (int i = 0; i < 19; ++i) tex_layouts[i] = single_tex_set_layout_;
  VkDescriptorSet tex_sets[19] = {};
  VkDescriptorSetAllocateInfo talloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  talloc.descriptorPool = modern_pool_;
  talloc.descriptorSetCount = 19;
  talloc.pSetLayouts = tex_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &talloc, tex_sets), "modern tex descriptor sets");
  fxaa_set_scene_ = tex_sets[0];
  fxaa_set_standin_ = tex_sets[1];
  upscale_set_scene_ = tex_sets[2];
  upscale_set_fxaa_ = tex_sets[3];
  upscale_set_tmp_ = tex_sets[4];
  hdr_set_standin_ = tex_sets[5];
  hdr_set_fxaa_ = tex_sets[6];
  motion_set_ = tex_sets[7];
  upscale_set_hist_[0] = tex_sets[8];
  upscale_set_hist_[1] = tex_sets[9];
  for (int i = 0; i < 6; ++i) inspector_sets_[i] = tex_sets[10 + i];
  sscs_set_ = tex_sets[16];
  mblur_tile_set_ = tex_sets[17];
  mblur_neighbor_set_ = tex_sets[18];

  VkDescriptorSetLayout temp_layouts[4] = {temporal_set_layout_, temporal_set_layout_, temporal_set_layout_,
                                           temporal_set_layout_};
  VkDescriptorSetAllocateInfo tmp_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  tmp_alloc.descriptorPool = modern_pool_;
  tmp_alloc.descriptorSetCount = 4;
  tmp_alloc.pSetLayouts = temp_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &tmp_alloc, temporal_sets_), "temporal descriptor sets");

  auto make_fs_pipe = [&](const uint32_t* frag_code, size_t frag_bytes, VkPipelineLayout layout, VkFormat fmt,
                          VkPipeline& out, const char* what) -> bool {
    auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = bytes;
      info.pCode = code;
      return vkCreateShaderModule(device_, &info, nullptr, &module);
    };
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VK_TRY(make_module(kPost_vert_spv, sizeof(kPost_vert_spv), vert), "modern vert");
    VK_TRY(make_module(frag_code, frag_bytes, frag), what);
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main",
         nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bl.attachmentCount = 1;
    bl.pAttachments = &att;
    const VkDynamicState dstates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dstates;
    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rend;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &bl;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    const VkResult res = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pi, nullptr, &out);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    VK_TRY(res, what);
    return true;
  };
  if (!make_fs_pipe(kSsao_frag_spv, sizeof(kSsao_frag_spv), ssao_layout_, VK_FORMAT_R8G8B8A8_UNORM, ssao_pipe_,
                    "gtao pipeline") ||
      !make_fs_pipe(kSscs_frag_spv, sizeof(kSscs_frag_spv), sscs_layout_, VK_FORMAT_R8_UNORM, sscs_pipe_,
                    "sscs pipeline") ||
      !make_fs_pipe(kMblur_tile_frag_spv, sizeof(kMblur_tile_frag_spv), mblur_tile_layout_, VK_FORMAT_R16G16_SFLOAT,
                    mblur_tile_pipe_, "mblur tile pipeline") ||
      !make_fs_pipe(kFxaa_frag_spv, sizeof(kFxaa_frag_spv), fxaa_layout_, VK_FORMAT_R8G8B8A8_UNORM, fxaa_pipe_,
                    "fxaa pipeline") ||
      !make_fs_pipe(kUpscale_frag_spv, sizeof(kUpscale_frag_spv), upscale_layout_, VK_FORMAT_R8G8B8A8_UNORM,
                    upscale_pipe_, "upscale pipeline") ||
      !make_fs_pipe(kMotion_vectors_frag_spv, sizeof(kMotion_vectors_frag_spv), motion_layout_,
                    VK_FORMAT_R16G16B16A16_SFLOAT, motion_pipe_, "motion pipeline") ||
      !make_fs_pipe(kTemporal_upscale_frag_spv, sizeof(kTemporal_upscale_frag_spv), temporal_layout_,
                    VK_FORMAT_R8G8B8A8_UNORM, temporal_pipe_, "temporal pipeline") ||
      !make_fs_pipe(kInspector_preview_frag_spv, sizeof(kInspector_preview_frag_spv), inspector_layout_,
                    VK_FORMAT_R8G8B8A8_UNORM, inspector_pipe_, "inspector preview pipeline") ||
      !make_fs_pipe(kHdr_output_frag_spv, sizeof(kHdr_output_frag_spv), hdr_layout_, VK_FORMAT_R8G8B8A8_UNORM,
                    hdr_pipe_sdr_, "hdr sdr pipeline"))
    return false;
  if (color_format_ != VK_FORMAT_R8G8B8A8_UNORM) {
    if (!make_fs_pipe(kHdr_output_frag_spv, sizeof(kHdr_output_frag_spv), hdr_layout_, color_format_, hdr_pipe_swap_,
                      "hdr swap pipeline"))
      return false;
    hdr_swap_fmt_ = color_format_;
  } else {
    hdr_pipe_swap_ = hdr_pipe_sdr_;
    hdr_swap_fmt_ = color_format_;
  }
  modern_ready_ = true;
  return true;
}

void Renderer::destroy_modern_passes() {
  if (!device_) return;
  if (hdr_pipe_swap_ && hdr_pipe_swap_ != hdr_pipe_sdr_) vkDestroyPipeline(device_, hdr_pipe_swap_, nullptr);
  hdr_pipe_swap_ = VK_NULL_HANDLE;
  hdr_swap_fmt_ = VK_FORMAT_UNDEFINED;
  if (hdr_pipe_sdr_) vkDestroyPipeline(device_, hdr_pipe_sdr_, nullptr), hdr_pipe_sdr_ = VK_NULL_HANDLE;
  if (inspector_pipe_) vkDestroyPipeline(device_, inspector_pipe_, nullptr), inspector_pipe_ = VK_NULL_HANDLE;
  if (temporal_pipe_) vkDestroyPipeline(device_, temporal_pipe_, nullptr), temporal_pipe_ = VK_NULL_HANDLE;
  if (motion_pipe_) vkDestroyPipeline(device_, motion_pipe_, nullptr), motion_pipe_ = VK_NULL_HANDLE;
  if (mblur_tile_pipe_) vkDestroyPipeline(device_, mblur_tile_pipe_, nullptr), mblur_tile_pipe_ = VK_NULL_HANDLE;
  if (upscale_pipe_) vkDestroyPipeline(device_, upscale_pipe_, nullptr), upscale_pipe_ = VK_NULL_HANDLE;
  if (fxaa_pipe_) vkDestroyPipeline(device_, fxaa_pipe_, nullptr), fxaa_pipe_ = VK_NULL_HANDLE;
  if (sscs_pipe_) vkDestroyPipeline(device_, sscs_pipe_, nullptr), sscs_pipe_ = VK_NULL_HANDLE;
  if (ssao_pipe_) vkDestroyPipeline(device_, ssao_pipe_, nullptr), ssao_pipe_ = VK_NULL_HANDLE;
  if (inspector_layout_) vkDestroyPipelineLayout(device_, inspector_layout_, nullptr), inspector_layout_ = VK_NULL_HANDLE;
  if (temporal_layout_) vkDestroyPipelineLayout(device_, temporal_layout_, nullptr), temporal_layout_ = VK_NULL_HANDLE;
  if (motion_layout_) vkDestroyPipelineLayout(device_, motion_layout_, nullptr), motion_layout_ = VK_NULL_HANDLE;
  if (mblur_tile_layout_) vkDestroyPipelineLayout(device_, mblur_tile_layout_, nullptr), mblur_tile_layout_ = VK_NULL_HANDLE;
  if (hdr_layout_) vkDestroyPipelineLayout(device_, hdr_layout_, nullptr), hdr_layout_ = VK_NULL_HANDLE;
  if (upscale_layout_) vkDestroyPipelineLayout(device_, upscale_layout_, nullptr), upscale_layout_ = VK_NULL_HANDLE;
  if (fxaa_layout_) vkDestroyPipelineLayout(device_, fxaa_layout_, nullptr), fxaa_layout_ = VK_NULL_HANDLE;
  if (sscs_layout_) vkDestroyPipelineLayout(device_, sscs_layout_, nullptr), sscs_layout_ = VK_NULL_HANDLE;
  if (ssao_layout_) vkDestroyPipelineLayout(device_, ssao_layout_, nullptr), ssao_layout_ = VK_NULL_HANDLE;
  if (modern_pool_) vkDestroyDescriptorPool(device_, modern_pool_, nullptr), modern_pool_ = VK_NULL_HANDLE;
  if (temporal_set_layout_)
    vkDestroyDescriptorSetLayout(device_, temporal_set_layout_, nullptr), temporal_set_layout_ = VK_NULL_HANDLE;
  if (single_tex_set_layout_)
    vkDestroyDescriptorSetLayout(device_, single_tex_set_layout_, nullptr), single_tex_set_layout_ = VK_NULL_HANDLE;
  if (ssao_set_layout_)
    vkDestroyDescriptorSetLayout(device_, ssao_set_layout_, nullptr), ssao_set_layout_ = VK_NULL_HANDLE;
  destroy_image(inspector_thumb_);
  destroy_image(vel_zero_);
  destroy_image(ao_white_);
  modern_ready_ = false;
}

void Renderer::update_modern_descriptors() {
  if (!modern_ready_ || !depth_resolved_.view) return;
  const VkImageView fb = ao_white_.view;
  const VkImageView vel_fb = vel_zero_.view ? vel_zero_.view : fb;
  auto vv = [&](const Image& img) { return img.view ? img.view : fb; };
  auto vvel = [&](const Image& img) { return img.view ? img.view : vel_fb; };
  const VkImageView ao_view =
      (graphics_.ssao != modern::SsaoQuality::kOff && ao_blur_.view) ? ao_blur_.view : ao_white_.view;
  const VkImageView shadow_view = shadow_map_.view ? shadow_map_.view : depth_resolved_.view;
  VkDescriptorImageInfo ssao_imgs[6] = {
      {post_sampler_, ao_white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(ao_raw_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(ao_tmp_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
  };
  VkDescriptorImageInfo tex_imgs[19] = {
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(stand_in_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(upscale_tmp_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(stand_in_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(stand_in_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[0]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[1]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, ao_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, shadow_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      hiz_preview_view_
          ? VkDescriptorImageInfo{post_sampler_, hiz_preview_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
          : VkDescriptorImageInfo{post_depth_sampler_, depth_resolved_.view,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(mblur_tile_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkDescriptorSet tex_sets[19] = {fxaa_set_scene_,      fxaa_set_standin_,  upscale_set_scene_,   upscale_set_fxaa_,
                                  upscale_set_tmp_,     hdr_set_standin_,   hdr_set_fxaa_,        motion_set_,
                                  upscale_set_hist_[0], upscale_set_hist_[1], inspector_sets_[0], inspector_sets_[1],
                                  inspector_sets_[2],   inspector_sets_[3], inspector_sets_[4],   inspector_sets_[5],
                                  sscs_set_,            mblur_tile_set_,    mblur_neighbor_set_};
  VkDescriptorImageInfo temp_imgs[12] = {
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[1]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[0]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[1]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[0]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkWriteDescriptorSet writes[37]{};
  for (int i = 0; i < 6; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = ssao_sets_[i / 2];
    writes[i].dstBinding = uint32_t(i % 2);
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &ssao_imgs[i];
  }
  for (int i = 0; i < 19; ++i) {
    writes[6 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[6 + i].dstSet = tex_sets[i];
    writes[6 + i].dstBinding = 0;
    writes[6 + i].descriptorCount = 1;
    writes[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6 + i].pImageInfo = &tex_imgs[i];
  }
  for (int i = 0; i < 12; ++i) {
    writes[25 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[25 + i].dstSet = temporal_sets_[i / 3];
    writes[25 + i].dstBinding = uint32_t(i % 3);
    writes[25 + i].descriptorCount = 1;
    writes[25 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[25 + i].pImageInfo = &temp_imgs[i];
  }
  vkUpdateDescriptorSets(device_, 37, writes, 0, nullptr);
}

void Renderer::record_ssao(VkCommandBuffer cmd) {
  if (!modern_ready_ || graphics_.ssao == modern::SsaoQuality::kOff || !ao_raw_.view || !ao_blur_.view) return;
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  const float frame_jitter = float(frames_submitted_ & 63u) / 64.0f;
  auto draw_ao = [&](const Image& dst, VkDescriptorSet set, float mode) {
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = dst.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {ao_width_, ao_height_}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(ao_width_), float(ao_height_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {ao_width_, ao_height_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssao_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssao_layout_, 0, 1, &set, 0, nullptr);
    struct {
      float proj_params[4], ao_params[4], texel_step[4];
    } pc = {{cam_proj00_, cam_proj11_, cam_near_, cam_far_},
            {modern::ssao_radius_m(graphics_.ssao), float(modern::gtao_slices(graphics_.ssao)),
             float(modern::gtao_steps(graphics_.ssao)), mode},
            {1.0f / float(std::max(1u, ao_width_)), 1.0f / float(std::max(1u, ao_height_)),
             options_.reverse_z ? 1.0f : 0.0f, frame_jitter}};
    vkCmdPushConstants(cmd, ssao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  draw_ao(ao_raw_, ssao_sets_[0], 0.0f);
  draw_ao(ao_tmp_, ssao_sets_[1], 1.0f);
  draw_ao(ao_blur_, ssao_sets_[2], 2.0f);
  mark(cmd, "gtao");
}

void Renderer::record_sscs(VkCommandBuffer cmd) {
  if (!modern_ready_ || graphics_.contact_shadows == modern::ContactShadows::kOff || !sscs_pipe_ || !sscs_.view) return;
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  image_barrier(cmd, sscs_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = sscs_.view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {sscs_width_, sscs_height_}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(sscs_width_), float(sscs_height_), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {sscs_width_, sscs_height_}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sscs_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sscs_layout_, 0, 1, &sscs_set_, 0, nullptr);
  const PostSettings& s = options_.post;
  const float strength = graphics_.contact_shadows == modern::ContactShadows::kLow     ? 0.55f
                         : graphics_.contact_shadows == modern::ContactShadows::kHigh  ? 0.72f
                         : graphics_.contact_shadows == modern::ContactShadows::kUltra ? 0.80f
                                                                                       : 0.0f;
  struct {
    float proj_params[4], sun_view[4], sscs_params[4];
  } pc = {{cam_proj00_, cam_proj11_, cam_near_, cam_far_},
          {s.sun_view_dir[0], s.sun_view_dir[1], s.sun_view_dir[2], s.sun_elev_vis},
          {float(modern::contact_shadow_steps(graphics_.contact_shadows)), strength,
           options_.reverse_z ? 1.0f : 0.0f, float(frames_submitted_ & 63u) / 64.0f}};
  vkCmdPushConstants(cmd, sscs_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  image_barrier(cmd, sscs_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  mark(cmd, "sscs");
}

void Renderer::record_fxaa(VkCommandBuffer cmd, VkDescriptorSet set, VkImage dst_img, VkImageView dst_view, uint32_t w,
                           uint32_t h) {
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = dst_view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {w, h}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(w), float(h), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {w, h}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_layout_, 0, 1, &set, 0, nullptr);
  const float pc[4] = {1.0f / float(std::max(1u, w)), 1.0f / float(std::max(1u, h)), 0.75f, 0.125f};
  vkCmdPushConstants(cmd, fxaa_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  mark(cmd, "fxaa");
}

void Renderer::record_motion_vectors(VkCommandBuffer cmd) {
  if (!modern_ready_ || !motion_pipe_ || !motion_vec_.view) return;
  const uint32_t rw = render_width(), rh = render_height();
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

  image_barrier(cmd, motion_vec_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = motion_vec_.view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {rw, rh}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(rw), float(rh), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {rw, rh}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion_layout_, 0, 1, &motion_set_, 0, nullptr);

  struct {
    float prev_vp_inv_curr[16];
    float params[4];
  } pc{};
  for (int i = 0; i < 4; ++i) pc.prev_vp_inv_curr[i * 5] = 1.0f;
  float curr_inv[16];
  if (cam_vp_set_ && prev_cam_vp_valid_ && mat4_inv(cam_vp_, curr_inv)) {
    mat4_mul(prev_cam_vp_, curr_inv, pc.prev_vp_inv_curr);
  }
  pc.params[0] = cam_jitter_uv_x_ - prev_jitter_uv_x_;
  pc.params[1] = cam_jitter_uv_y_ - prev_jitter_uv_y_;
  pc.params[2] = options_.reverse_z ? 1.0f : 0.0f;
  pc.params[3] = cam_near_;
  vkCmdPushConstants(cmd, motion_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  image_barrier(cmd, motion_vec_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  if (cam_vp_set_) {
    std::memcpy(prev_cam_vp_, cam_vp_, sizeof(cam_vp_));
    prev_jitter_uv_x_ = cam_jitter_uv_x_;
    prev_jitter_uv_y_ = cam_jitter_uv_y_;
    prev_cam_vp_valid_ = true;
  }
  mark(cmd, "motion");
}

void Renderer::record_mblur_tiles(VkCommandBuffer cmd) {
  if (!modern_ready_ || !mblur_tile_pipe_ || !mblur_tile_.view || !mblur_neighbor_.view || !motion_vec_.view) return;
  const uint32_t rw = render_width(), rh = render_height();
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

  auto draw_tile = [&](const Image& dst, VkDescriptorSet set, float inv_w, float inv_h, float mode) {
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = dst.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {mblur_tile_w_, mblur_tile_h_}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(mblur_tile_w_), float(mblur_tile_h_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {mblur_tile_w_, mblur_tile_h_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mblur_tile_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mblur_tile_layout_, 0, 1, &set, 0, nullptr);
    const float pc[4] = {inv_w, inv_h, mode, options_.post.motion_blur_car_protect};
    vkCmdPushConstants(cmd, mblur_tile_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  draw_tile(mblur_tile_, mblur_tile_set_, 1.0f / float(std::max(1u, rw)), 1.0f / float(std::max(1u, rh)), 0.0f);
  draw_tile(mblur_neighbor_, mblur_neighbor_set_, 1.0f / float(std::max(1u, mblur_tile_w_)),
            1.0f / float(std::max(1u, mblur_tile_h_)), 1.0f);
  mark(cmd, "mblur_tiles");
}

void Renderer::record_inspector_preview(VkCommandBuffer cmd, VkBuffer readback_dst) {
  if (!modern_ready_ || !inspector_pipe_ || !inspector_thumb_.view || readback_dst == VK_NULL_HANDLE) return;
  const int idx = std::clamp(int(inspector_view_), 0, int(InspectorView::kCount) - 1);
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  image_barrier(cmd, inspector_thumb_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = inspector_thumb_.view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {kInspectorPreviewW, kInspectorPreviewH}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(kInspectorPreviewW), float(kInspectorPreviewH), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {kInspectorPreviewW, kInspectorPreviewH}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inspector_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inspector_layout_, 0, 1, &inspector_sets_[idx], 0,
                          nullptr);
  const float mode = (idx == 5 && !hiz_preview_view_) ? 1.0f : float(idx);
  const float params[4] = {mode, cam_near_, options_.reverse_z ? 1.0f : 0.0f, cam_far_};
  vkCmdPushConstants(cmd, inspector_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  image_barrier(cmd, inspector_thumb_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_stage, color_write, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {kInspectorPreviewW, kInspectorPreviewH, 1};
  vkCmdCopyImageToBuffer(cmd, inspector_thumb_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_dst, 1, &copy);
  VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &host;
  vkCmdPipelineBarrier2(cmd, &dep);
  mark(cmd, "inspect");
}

void Renderer::record_upscale(VkCommandBuffer cmd, VkImage /*src_img*/, VkDescriptorSet src_set, bool src_is_fxaa,
                              uint32_t src_w, uint32_t src_h, VkImage dst_img, VkImageView dst_view, uint32_t dst_w,
                              uint32_t dst_h) {
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  const float sharp = graphics_.sharpening / 100.0f;
  auto draw_up = [&](VkDescriptorSet set, VkImage target_img, VkImageView target_view, uint32_t sw, uint32_t sh,
                     uint32_t dw, uint32_t dh, float mode) {
    image_barrier(cmd, target_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = target_view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {dw, dh}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(dw), float(dh), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {dw, dh}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscale_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscale_layout_, 0, 1, &set, 0, nullptr);
    struct {
      float src_size[4], dst_size[4], params[4];
    } pc = {{float(sw), float(sh), 1.0f / float(std::max(1u, sw)), 1.0f / float(std::max(1u, sh))},
            {float(dw), float(dh), 1.0f / float(std::max(1u, dw)), 1.0f / float(std::max(1u, dh))},
            {mode, sharp, 0.0f, 0.0f}};
    vkCmdPushConstants(cmd, upscale_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, target_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  const auto choice = active_upscaler();
  if (choice.temporal && temporal_pipe_ && temporal_hist_[0].view && temporal_hist_[1].view) {
    if (temporal_reset_) {
      const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
      const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      for (int i = 0; i < 2; ++i) {
        image_barrier(cmd, temporal_hist_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, read_stage, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd, temporal_hist_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
        image_barrier(cmd, temporal_hist_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stage, read_acc);
      }
    }
    const uint32_t out_idx = temporal_idx_ & 1u;
    const Image& hist_out = temporal_hist_[out_idx];
    image_barrier(cmd, hist_out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, read_acc, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = hist_out.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {dst_w, dst_h}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(dst_w), float(dst_h), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {dst_w, dst_h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_pipe_);
    const uint32_t tset_idx = (src_is_fxaa ? 2u : 0u) + out_idx;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_layout_, 0, 1, &temporal_sets_[tset_idx], 0,
                            nullptr);
    struct {
      float render_size[4], output_size[4], params[4];
    } tpc = {{float(src_w), float(src_h), 1.0f / float(std::max(1u, src_w)), 1.0f / float(std::max(1u, src_h))},
             {float(dst_w), float(dst_h), 1.0f / float(std::max(1u, dst_w)), 1.0f / float(std::max(1u, dst_h))},
             {cam_jitter_uv_x_, cam_jitter_uv_y_, temporal_reset_ ? 1.0f : 0.0f, 0.90f}};
    vkCmdPushConstants(cmd, temporal_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tpc), &tpc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, hist_out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
    draw_up(upscale_set_hist_[out_idx], dst_img, dst_view, dst_w, dst_h, dst_w, dst_h, sharp > 0.0f ? 3.0f : 0.0f);
    temporal_reset_ = false;
    temporal_idx_ ^= 1u;
  } else if (choice.active == modern::Upscaler::kFsr1) {
    draw_up(src_set, upscale_tmp_.image, upscale_tmp_.view, src_w, src_h, dst_w, dst_h, 2.0f);
    draw_up(upscale_set_tmp_, dst_img, dst_view, dst_w, dst_h, dst_w, dst_h, 3.0f);
  } else if (choice.active == modern::Upscaler::kCas) {
    draw_up(src_set, dst_img, dst_view, src_w, src_h, dst_w, dst_h, 1.0f);
  } else {
    draw_up(src_set, dst_img, dst_view, src_w, src_h, dst_w, dst_h, 0.0f);
  }
  mark(cmd, "upscale");
}

void Renderer::record_hdr_output(VkCommandBuffer cmd, VkDescriptorSet src_set, VkImage dst_img, VkImageView dst_view,
                                 VkFormat dst_format, uint32_t w, uint32_t h) {
  VkPipeline pipe = (dst_format == color_format_) ? hdr_pipe_swap_ : hdr_pipe_sdr_;
  if (!pipe) pipe = hdr_pipe_sdr_;
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = dst_view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {w, h}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(w), float(h), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {w, h}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hdr_layout_, 0, 1, &src_set, 0, nullptr);
  const float paper_scale = modern::scrgb_scale(graphics_.paper_white);
  const float hl_factor = (graphics_.hdr || graphics_.hdr_compare)
                              ? std::max(graphics_.peak_nits / std::max(graphics_.paper_white, 80.0f) - 1.0f, 0.0f) *
                                    (graphics_.hdr_highlights / 100.0f)
                              : 0.0f;
  const uint32_t out_mode = (dst_format == color_format_ && hdr_mode_ == modern::HdrMode::kHdr10) ? 2u
                            : (dst_format == color_format_ && hdr_mode_ == modern::HdrMode::kScRgb) ? 1u
                                                                                                    : 0u;
  struct {
    float p0[4], p1[4];
    uint32_t p2[4];
  } pc = {{paper_scale, hl_factor, 0.5f, 2.0f},
          {0.0f, 0.5f, graphics_.hdr_compare ? float(w) * 0.5f : -1.0f, modern::scrgb_scale(hdr_pattern_nits_)},
          {out_mode, hdr_pattern_, w, h}};
  vkCmdPushConstants(cmd, hdr_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  (void)dst_img;
  mark(cmd, "hdr");
}

void Renderer::record_post(VkCommandBuffer cmd, VkImageView final_view, VkPipeline pipe, float out_alpha) {
  const PostSettings& s = options_.post;
  // Update optical post-FX UBO (binding 5): in the frame's commands, behind
  // a barrier, so a frame still in flight keeps its own values.
  if (post_optical_buf_.buffer) {
    struct PostOpticalUbo {
      float flags_near[4];
      float mblur[4];
      float dof[4];
      float sun[4];
      float sun_color[4];
      float vig_top_col[4];
      float vig_top_geom[4];
      float vig_top_rot[4];
      float vig_bot_col[4];
      float vig_bot_geom[4];
      float vig_bot_rot[4];
      float texel_size[4];
      float modern_p[4];
      float sota_p[4];
    };
    static_assert(sizeof(PostOpticalUbo) == 224);
    const uint32_t oflags = (s.motion_blur ? 1u : 0u) | (s.dof ? 2u : 0u) | (s.god_rays ? 4u : 0u) |
                            (s.lens_flare ? 8u : 0u) | (options_.reverse_z ? 16u : 0u) |
                            (s.paired_vignette ? 32u : 0u);
    PostOpticalUbo u{};
    u.flags_near[0] = float(oflags);
    u.flags_near[1] = s.near_plane;
    u.flags_near[2] = s.contrast;
    u.flags_near[3] = s.brightness;
    u.mblur[0] = s.motion_blur_center[0];
    u.mblur[1] = s.motion_blur_center[1];
    u.mblur[2] = s.motion_blur_strength;
    u.mblur[3] = s.motion_blur_car_protect;
    u.dof[0] = s.dof_focal_distance;
    u.dof[1] = s.dof_focal_range;
    u.dof[2] = s.dof_max_blur;
    u.dof[3] = s.sepia;
    u.sun[0] = s.sun_screen_uv[0];
    u.sun[1] = s.sun_screen_uv[1];
    u.sun[2] = s.sun_visibility;
    u.sun[3] = s.god_ray_strength;
    u.sun_color[0] = s.sun_scatter_color[0];
    u.sun_color[1] = s.sun_scatter_color[1];
    u.sun_color[2] = s.sun_scatter_color[2];
    u.sun_color[3] = s.lens_flare_strength;
    u.vig_top_col[0] = s.vig_top_color[0];
    u.vig_top_col[1] = s.vig_top_color[1];
    u.vig_top_col[2] = s.vig_top_color[2];
    u.vig_top_col[3] = s.vig_top_opacity;
    u.vig_top_geom[0] = s.vig_top_center[0];
    u.vig_top_geom[1] = s.vig_top_center[1];
    u.vig_top_geom[2] = s.vig_top_radius[0];
    u.vig_top_geom[3] = s.vig_top_radius[1];
    u.vig_top_rot[0] = std::cos(s.vig_top_rot);
    u.vig_top_rot[1] = std::sin(s.vig_top_rot);
    u.vig_top_rot[2] = s.vig_top_feather;
    u.vig_bot_col[0] = s.vig_bot_color[0];
    u.vig_bot_col[1] = s.vig_bot_color[1];
    u.vig_bot_col[2] = s.vig_bot_color[2];
    u.vig_bot_col[3] = s.vig_bot_opacity;
    u.vig_bot_geom[0] = s.vig_bot_center[0];
    u.vig_bot_geom[1] = s.vig_bot_center[1];
    u.vig_bot_geom[2] = s.vig_bot_radius[0];
    u.vig_bot_geom[3] = s.vig_bot_radius[1];
    u.vig_bot_rot[0] = std::cos(s.vig_bot_rot);
    u.vig_bot_rot[1] = std::sin(s.vig_bot_rot);
    u.vig_bot_rot[2] = s.vig_bot_feather;
    u.texel_size[0] = render_width_ ? 1.0f / float(render_width_) : 1.0f / 1280.0f;
    u.texel_size[1] = render_height_ ? 1.0f / float(render_height_) : 1.0f / 720.0f;
    u.texel_size[2] = s.time_sec;
    u.texel_size[3] = float(frames_submitted_ & 63u) / 64.0f;
    const float ao_strength = graphics_.ssao == modern::SsaoQuality::kLow      ? 0.50f
                              : graphics_.ssao == modern::SsaoQuality::kMedium ? 0.72f
                              : graphics_.ssao == modern::SsaoQuality::kHigh   ? 0.88f
                              : graphics_.ssao == modern::SsaoQuality::kUltra  ? 0.96f
                                                                               : 0.0f;
    u.modern_p[0] = ao_strength;
    u.modern_p[1] = float(lut_image_size_);
    u.modern_p[2] = lut_strength_;
    u.modern_p[3] = out_alpha;
    const float sscs_strength = graphics_.contact_shadows == modern::ContactShadows::kLow     ? 0.70f
                                : graphics_.contact_shadows == modern::ContactShadows::kHigh  ? 0.85f
                                : graphics_.contact_shadows == modern::ContactShadows::kUltra ? 0.95f
                                                                                              : 0.0f;
    const bool vel_mblur_ready =
        (graphics_.velocity_blur != modern::VelocityBlur::kOff) && mblur_neighbor_.view && motion_vec_.view;
    u.sota_p[0] = float(int(graphics_.tonemapper));
    u.sota_p[1] = sscs_.view ? sscs_strength : 0.0f;
    u.sota_p[2] = vel_mblur_ready ? float(modern::velocity_blur_taps(graphics_.velocity_blur)) : 0.0f;
    u.sota_p[3] = (graphics_.ssao == modern::SsaoQuality::kHigh || graphics_.ssao == modern::SsaoQuality::kUltra)
                      ? 1.0f
                      : (graphics_.ssao != modern::SsaoQuality::kOff ? 0.55f : 0.0f);
    VkBufferMemoryBarrier2 ub{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    ub.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ub.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    ub.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    ub.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ub.srcQueueFamilyIndex = ub.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ub.buffer = post_optical_buf_.buffer;
    ub.size = VK_WHOLE_SIZE;
    VkDependencyInfo udep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    udep.bufferMemoryBarrierCount = 1;
    udep.pBufferMemoryBarriers = &ub;
    vkCmdPipelineBarrier2(cmd, &udep);
    vkCmdUpdateBuffer(cmd, post_optical_buf_.buffer, 0, sizeof u, &u);
    ub.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    ub.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ub.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ub.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &udep);
  }
  // Upload updated N^3 3D LUT when dirty (zero pipeline stall).
  const size_t want_lut_bytes = size_t(lut_image_size_) * lut_image_size_ * lut_image_size_ * 4u;
  if (lut_dirty_ && lut_pending_size_ == lut_image_size_ && lut_pending_.size() == want_lut_bytes) {
    FrameSlot& fs = slot();
    if (fs.lut_staging.size < want_lut_bytes) {
      if (fs.lut_staging_mapped) vmaUnmapMemory(allocator_, fs.lut_staging.allocation);
      fs.lut_staging_mapped = nullptr;
      destroy_buffer(fs.lut_staging);
      if (create_buffer(want_lut_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, fs.lut_staging) &&
          vmaMapMemory(allocator_, fs.lut_staging.allocation, &fs.lut_staging_mapped) != VK_SUCCESS)
        fs.lut_staging_mapped = nullptr;
    }
  }
  if (lut_dirty_ && slot().lut_staging_mapped && lut_pending_size_ == lut_image_size_ &&
      lut_pending_.size() == want_lut_bytes) {
    lut_dirty_ = false;
    std::memcpy(slot().lut_staging_mapped, lut_pending_.data(), lut_pending_.size());
    image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {lut_image_size_, lut_image_size_, lut_image_size_};
    vkCmdCopyBufferToImage(cmd, slot().lut_staging.buffer, lut3d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }

  const uint32_t flags = (s.bloom ? 1u : 0u) | (s.lut_enabled ? 2u : 0u) | (s.auto_exposure ? 4u : 0u);
  struct {
    float mode[4], abc[4], def[4], desat[4], bloom[4], balance[4], exp_keys[4], exp_ev[4];
  } push = {{0, 0, 0, 0},
            {s.hable[0], s.hable[1], s.hable[2], s.white},
            {s.hable[3], s.hable[4], s.hable[5], s.exposure},
            {s.luminance[0], s.luminance[1], s.luminance[2], s.desaturate},
            {s.bloom_cutoff, s.bloom_scale, float(flags), s.bloom_lower_grad},
            {s.balance[0], s.balance[1], s.balance[2], s.bloom_upper_grad},
            {s.exp_keys[0], s.exp_keys[1], s.exp_keys[2], s.exp_keys[3]},
            {s.exp_ev[0], s.exp_ev[1], s.exp_ev[2], s.exp_ev[3]}};
  static_assert(sizeof(push) == 128);
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  auto to_read = [&](const Image& i) {
    image_barrier(cmd, i.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read);
  };
  auto to_write = [&](const Image& i) {
    image_barrier(cmd, i.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  };
  auto pass = [&](VkImageView target, uint32_t w, uint32_t h, VkPipeline pipeline, VkDescriptorSet set, float mode,
                  float dx, float dy, float mw = 0.0f) {
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {w, h}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0, 0, float(w), float(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &set, 0, nullptr);
    push.mode[0] = mode, push.mode[1] = dx, push.mode[2] = dy, push.mode[3] = mw;
    vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
  };

  to_read(hdr_);

  // 1. Adaptive auto-exposure: 16x16 centre-weighted reduction -> 1x1 temporal adaptation.
  if (s.auto_exposure) {
    to_write(lum_16_);
    pass(lum_16_.view, 16, 16, post_quarter_, post_sets_[0], 3, 0.25f / 16.0f, 0.25f / 16.0f);
    to_read(lum_16_);
    to_write(lum_adapt_[0]);
    const float alpha = lum_reset_ ? 1.0f : s.adapt_alpha;
    lum_reset_ = false;
    pass(lum_adapt_[0].view, 1, 1, post_quarter_, post_sets_[1], 4, 0, 0, alpha);
    // Copy adapted 1x1 luminance into lum_adapt_[1] for next frame, leave lum_adapt_[0] readable.
    image_barrier(cmd, lum_adapt_[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_stage, color_write, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(cmd, lum_adapt_[1].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, read_stage, read, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {1, 1, 1};
    vkCmdCopyImage(cmd, lum_adapt_[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lum_adapt_[1].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    image_barrier(cmd, lum_adapt_[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT, read_stage, read);
    image_barrier(cmd, lum_adapt_[1].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stage, read);
  }

  // 2. Physically-Based 6-level Jimenez / Dual-Kawase bloom pyramid:
  //    Mip 0: 13-tap Karis-averaged anti-firefly prefilter -> Mips 1..5: 13-tap progressive downsample
  //    Mips 5->0: 9-tap 3x3 tent-filter progressive upsample combine (11 passes vs 19 Gaussian passes).
  if (s.bloom && bloom_a_[0].image && bloom_b_[0].image && post_bloom_down_ && post_bloom_up_) {
    // Downsample mip 0 (Karis average + soft-knee prefilter from full HDR scene).
    to_write(bloom_a_[0]);
    pass(bloom_a_[0].view, bloom_w_[0], bloom_h_[0], post_bloom_down_, post_sets_[2], 0.0f,
         1.0f / float(std::max(1u, render_width_)), 1.0f / float(std::max(1u, render_height_)), 0.45f);
    to_read(bloom_a_[0]);

    // Progressive 13-tap downsamples mips 1..5.
    for (uint32_t k = 1; k < kBloomLevels; ++k) {
      to_write(bloom_a_[k]);
      pass(bloom_a_[k].view, bloom_w_[k], bloom_h_[k], post_bloom_down_, post_sets_[2 + k], float(k),
           1.0f / float(std::max(1u, bloom_w_[k - 1])), 1.0f / float(std::max(1u, bloom_h_[k - 1])), 0.0f);
      to_read(bloom_a_[k]);
    }

    // Progressive 3x3 tent-filter upsample mips 5->4->3->2->1->0 into bloom_b_[4..0].
    // Map FH1's 5 bloom_weights onto our 6 mips cleanly (mip 5 extends the widest halation tail).
    float mip_w[kBloomLevels];
    float w_sum = 0.0f;
    for (uint32_t k = 0; k < kBloomLevels; ++k) {
      mip_w[k] = std::max(k < 5 ? s.bloom_weights[k] : s.bloom_weights[4] * 0.85f, 0.0f);
      w_sum += mip_w[k];
    }
    const float w_norm = w_sum > 1e-4f ? 1.0f / w_sum : (1.0f / float(kBloomLevels));
    constexpr float kTentRadius = 1.15f;
    for (int k = int(kBloomLevels) - 2; k >= 0; --k) {
      const uint32_t set_idx = 8u + uint32_t((int(kBloomLevels) - 2) - k);
      const float w_cur = mip_w[k] * w_norm;
      const float w_prev = (k == int(kBloomLevels) - 2) ? (mip_w[k + 1] * w_norm) : 1.0f;
      to_write(bloom_b_[k]);
      pass(bloom_b_[k].view, bloom_w_[k], bloom_h_[k], post_bloom_up_, post_sets_[set_idx], w_cur,
           kTentRadius / float(std::max(1u, bloom_w_[k + 1])), kTentRadius / float(std::max(1u, bloom_h_[k + 1])),
           w_prev);
      to_read(bloom_b_[k]);
    }
    mark(cmd, "bloom");
  } else if (bloom_b_[0].image) {
    to_write(bloom_b_[0]);
    to_read(bloom_b_[0]);
  }

  // 3. Final composite (scene + McGuire MBlur + XeGTAO + SSCS + bloom -> exposure -> selected tonemapper -> LUT -> display).
  pass(final_view, render_width_, render_height_, pipe, post_sets_[13], 2, 0, 0, s.vignette_amount);
  mark(cmd, "post");
}

void Renderer::destroy_targets() {
  destroy_image(msaa_color_);
  destroy_image(depth_);
  destroy_image(depth_resolved_);
  destroy_image(hdr_);
  destroy_image(ao_raw_);
  destroy_image(ao_tmp_);
  destroy_image(ao_blur_);
  destroy_image(sscs_);
  destroy_image(scene_sdr_);
  destroy_image(scene_fxaa_);
  destroy_image(upscale_tmp_);
  destroy_image(stand_in_);
  destroy_image(stand_in_fxaa_);
  destroy_image(motion_vec_);
  destroy_image(mblur_tile_);
  destroy_image(mblur_neighbor_);
  destroy_image(temporal_hist_[0]);
  destroy_image(temporal_hist_[1]);
  destroy_image(inspector_thumb_);
  allocated_targets_ = {};
  temporal_reset_ = true;
  prev_cam_vp_valid_ = false;
  ao_width_ = ao_height_ = 0;
  sscs_width_ = sscs_height_ = 0;
  mblur_tile_w_ = mblur_tile_h_ = 0;
  for (uint32_t k = 0; k < kBloomLevels; ++k) {
    destroy_image(bloom_a_[k]);
    destroy_image(bloom_b_[k]);
    bloom_w_[k] = bloom_h_[k] = 0;
  }
  target_width_ = target_height_ = 0;
  render_width_ = render_height_ = 0;
}

void Renderer::record_scene(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view,
                            VkImageLayout final_layout, const FrameConstants& constants,
                            VkBuffer capture_dst, bool capture_before_ui, VkFormat final_format,
                            VkBuffer inspector_dst) {
  if (final_format == VK_FORMAT_UNDEFINED) final_format = color_format_;
  const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
  if (scene_.before) scene_.before(cmd);
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, VK_ACCESS_2_NONE,
                color_stage, color_write);
  if (msaa) {
    image_barrier(cmd, msaa_color_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, VK_ACCESS_2_NONE,
                  color_stage, color_write);
  }
  const uint32_t rw = render_width();
  const uint32_t rh = render_height();
  // Under the post chain the scene goes to the HDR image (resolved into
  // it with MSAA), and the composite writes the final image.
  const bool post = options_.post.enabled && hdr_.view;
  const VkImageView scene_view = post ? hdr_.view : final_view;
  if (post)
    image_barrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                  color_stage, color_write);
  const auto depth_stages =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_stages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depth_stages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

  const bool two_pass = bool(scene_.draw_particles) && depth_resolved_.view != VK_NULL_HANDLE &&
                        (!scene_.has_particles || scene_.has_particles());
  const bool need_hiz = bool(scene_.build_hiz) && depth_resolved_.view != VK_NULL_HANDLE;
  const bool can_msaa_depth_resolve = msaa && depth_resolve_supported_ && depth_resolved_.view != VK_NULL_HANDLE;
  if (two_pass || post || need_hiz) {
    image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  can_msaa_depth_resolve ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                         : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                  can_msaa_depth_resolve ? depth_stages : VK_PIPELINE_STAGE_2_COPY_BIT,
                  can_msaa_depth_resolve ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                         : VK_ACCESS_2_TRANSFER_WRITE_BIT);
  }

  auto set_vrs = [&](uint32_t w, uint32_t h) {
    if (!vrs_supported_ || !vkCmdSetFragmentShadingRateKHR) return;
    const VkExtent2D sz{w, h};
    const VkFragmentShadingRateCombinerOpKHR ops[2] = {
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
    vkCmdSetFragmentShadingRateKHR(cmd, &sz, ops);
  };

  const bool run_z_prepass = options_.z_prepass && bool(scene_.draw_z_prepass);
  if (run_z_prepass) {
    VkRenderingAttachmentInfo z_depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    z_depth.imageView = depth_.view;
    z_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    z_depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    z_depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    z_depth.clearValue.depthStencil = {options_.reverse_z ? 0.0f : 1.0f, 0};
    VkRenderingInfo z_rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    z_rend.renderArea = {{0, 0}, {rw, rh}};
    z_rend.layerCount = 1;
    z_rend.pDepthAttachment = &z_depth;
    vkCmdBeginRendering(cmd, &z_rend);
    VkViewport viewport{0, 0, float(rw), float(rh), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {rw, rh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    set_vrs(1, 1);
    scene_.draw_z_prepass(cmd);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_stages,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depth_stages,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    mark(cmd, "zpre");
  }

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = msaa ? msaa_color_.view : scene_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = (msaa && !two_pass) ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{scene_.clear[0], scene_.clear[1], scene_.clear[2], scene_.clear[3]}};
  if (post)
    for (int k = 0; k < 3; ++k) {
      // The clear colour is display (sRGB) encoded: linear for the HDR scene.
      const float c = scene_.clear[k];
      color.clearValue.color.float32[k] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
  if (msaa && !two_pass) {
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = scene_view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = run_z_prepass ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  const bool need_depth_store = depth_readable_ || two_pass || (!msaa && (post || need_hiz));
  depth.storeOp = need_depth_store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {options_.reverse_z ? 0.0f : 1.0f, 0};
  if (can_msaa_depth_resolve && (two_pass || post || need_hiz)) {
    depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depth.resolveImageView = depth_resolved_.view;
    depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  }
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {rw, rh}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  rendering.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rendering);
  VkViewport viewport{0, 0, float(rw), float(rh), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {rw, rh}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  set_vrs(graphics_.vrs ? 2u : 1u, graphics_.vrs ? 2u : 1u);
  if (scene_.draw) {
    scene_.draw(cmd);
  } else if (index_count_ > 0 && vertex_buffer_.buffer != VK_NULL_HANDLE &&
             index_buffer_.buffer != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_set_, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(FrameConstants), &constants);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
  }
  vkCmdEndRendering(cmd);

  if (two_pass || post || need_hiz) {
    const auto read_stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (can_msaa_depth_resolve) {
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    read_stages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else if (!msaa) {
      image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_stages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
      VkImageCopy ic{};
      ic.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      ic.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      ic.extent = {rw, rh, 1};
      vkCmdCopyImage(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_resolved_.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
      image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, depth_stages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stages,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else {
      // Fallback if GPU lacks MSAA depth resolve: clear resolved depth to far plane.
      const VkClearDepthStencilValue far_v{options_.reverse_z ? 0.0f : 1.0f, 0};
      const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      vkCmdClearDepthStencilImage(cmd, depth_resolved_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_v, 1, &rng);
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stages,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if (need_hiz) {
      scene_.build_hiz(cmd, depth_resolved_.view, rw, rh, options_.reverse_z);
      mark(cmd, "hiz_build");
    }
  }

  if (two_pass) {
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {
      color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      color.resolveImageView = scene_view;
      color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = depth_readable_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.resolveMode = VK_RESOLVE_MODE_NONE;
    depth.resolveImageView = VK_NULL_HANDLE;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    set_vrs(1, 1);
    scene_.draw_particles(cmd);
    vkCmdEndRendering(cmd);
    mark(cmd, "particles");
  }

  if (post) {
    record_ssao(cmd);
    record_sscs(cmd);
  }
  const bool need_mblur_tiles =
      post && options_.post.motion_blur && graphics_.velocity_blur != modern::VelocityBlur::kOff &&
      mblur_tile_.view != VK_NULL_HANDLE && mblur_neighbor_.view != VK_NULL_HANDLE;
  const bool need_motion =
      post && (active_upscaler().temporal || need_mblur_tiles ||
               (inspector_active_ && inspector_view_ == InspectorView::kMotion));
  if (need_motion) {
    record_motion_vectors(cmd);
  } else if (post && motion_vec_.image) {
    image_barrier(cmd, motion_vec_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }
  if (need_mblur_tiles) {
    record_mblur_tiles(cmd);
  } else if (post && mblur_neighbor_.image) {
    image_barrier(cmd, mblur_neighbor_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }

  const bool need_hdr_stage = (hdr_mode_ != modern::HdrMode::kOff) || graphics_.hdr_compare || (hdr_pattern_ != 0);
  const bool need_fxaa_world = (graphics_.aa == modern::AaMode::kFxaaWorld) && fxaa_pipe_ != VK_NULL_HANDLE;
  const bool need_fxaa_all = (graphics_.aa == modern::AaMode::kFxaaAll) && fxaa_pipe_ != VK_NULL_HANDLE;
  const bool use_modern_chain = uses_modern_chain();

  if (depth_readable_) {
    image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  }

  VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkPipelineStageFlags2 last_stage = color_stage;
  VkAccessFlags2 last_access = color_write;
  auto record_capture_copy = [&]() {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  last_stage, last_access, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {target_width_, target_height_, 1};
    vkCmdCopyImageToBuffer(cmd, final_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_dst, 1, &copy);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cmd, &dependency);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    last_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    last_access = VK_ACCESS_2_TRANSFER_READ_BIT;
  };
  auto copy_overlays = [&](VkImage dst_img, VkImageLayout& dst_layout, VkPipelineStageFlags2& dst_stage,
                           VkAccessFlags2& dst_access) {
    if (!overlay_staged_) return;
    image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, dst_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  dst_stage, dst_access, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy regions[kOverlayLayers]{};
    uint32_t count = 0;
    for (int li = 0; li < kOverlayLayers; ++li) {
      const auto& l = overlays_[li];
      if (!l.staged) continue;
      VkBufferImageCopy& region = regions[count++];
      region.bufferOffset = slot().overlay_off[li];
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {l.at.x, l.at.y, 0};
      region.imageExtent = {l.width, l.height, 1};
    }
    vkCmdCopyBufferToImage(cmd, slot().overlay.buffer, dst_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, regions);
    dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  };

  if (!use_modern_chain) {
    if (post) record_post(cmd, final_view, post_final_, 1.0f);
    if (capture_dst != VK_NULL_HANDLE && capture_before_ui) record_capture_copy();
    copy_overlays(final_image, layout, last_stage, last_access);
    record_ui(cmd, final_image, final_view, layout, last_stage, last_access, ui_pipeline_, target_width_,
              target_height_);
    if (capture_dst != VK_NULL_HANDLE && !capture_before_ui) record_capture_copy();
  } else {
    image_barrier(cmd, scene_sdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_NONE, color_stage, color_write);
    record_post(cmd, scene_sdr_.view, post_final_sdr_, 0.0f);
    image_barrier(cmd, scene_sdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    VkImage cur_img = scene_sdr_.image;
    VkDescriptorSet cur_up_set = upscale_set_scene_;
    bool cur_is_fxaa = false;
    if (need_fxaa_world) {
      record_fxaa(cmd, fxaa_set_scene_, scene_fxaa_.image, scene_fxaa_.view, rw, rh);
      cur_img = scene_fxaa_.image;
      cur_up_set = upscale_set_fxaa_;
      cur_is_fxaa = true;
    }
    if (inspector_active_ && inspector_dst != VK_NULL_HANDLE) {
      record_inspector_preview(cmd, inspector_dst);
    }
    record_upscale(cmd, cur_img, cur_up_set, cur_is_fxaa, rw, rh, stand_in_.image, stand_in_.view, target_width_,
                   target_height_);

    VkImageLayout si_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkPipelineStageFlags2 si_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkAccessFlags2 si_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

    auto resolve_standin_to_final = [&]() {
      VkDescriptorSet hdr_set = hdr_set_standin_;
      VkImage src_img = stand_in_.image;
      if (need_fxaa_all) {
        if (si_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
          image_barrier(cmd, stand_in_.image, VK_IMAGE_ASPECT_COLOR_BIT, si_layout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, si_stage, si_access,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        record_fxaa(cmd, fxaa_set_standin_, stand_in_fxaa_.image, stand_in_fxaa_.view, target_width_, target_height_);
        hdr_set = hdr_set_fxaa_;
        src_img = stand_in_fxaa_.image;
        si_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        si_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        si_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      }
      if (need_hdr_stage) {
        if (si_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
          image_barrier(cmd, src_img, VK_IMAGE_ASPECT_COLOR_BIT, si_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        si_stage, si_access, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      last_stage, last_access, color_stage, color_write);
        record_hdr_output(cmd, hdr_set, final_image, final_view, final_format, target_width_, target_height_);
        layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        last_stage = color_stage;
        last_access = color_write;
      } else {
        image_barrier(cmd, src_img, VK_IMAGE_ASPECT_COLOR_BIT, si_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      si_stage, si_access, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      last_stage, last_access, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(target_width_), int32_t(target_height_), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(target_width_), int32_t(target_height_), 1};
        vkCmdBlitImage(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, final_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
        last_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        si_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        si_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
        si_access = VK_ACCESS_2_TRANSFER_READ_BIT;
      }
    };

    if (capture_dst != VK_NULL_HANDLE && capture_before_ui) {
      resolve_standin_to_final();
      record_capture_copy();
    }
    copy_overlays(stand_in_.image, si_layout, si_stage, si_access);
    record_ui(cmd, stand_in_.image, stand_in_.view, si_layout, si_stage, si_access, ui_pipeline_sdr_, target_width_,
              target_height_);
    if (!(capture_dst != VK_NULL_HANDLE && capture_before_ui)) {
      resolve_standin_to_final();
    }
    if (capture_dst != VK_NULL_HANDLE && !capture_before_ui) record_capture_copy();
  }

  if (final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
  } else {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
  }
}

void Renderer::set_ui(std::span<const ui::Instance> items) { ui_items_.assign(items.begin(), items.end()); }

void Renderer::ui_atlas_put(uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::span<const uint8_t> rgba) {
  const uint32_t atlas_sz = options_.ui_atlas_size ? options_.ui_atlas_size : kUiAtlas;
  if (!w || !h || x + w > atlas_sz || y + h > atlas_sz || rgba.size() < size_t(w) * h * 4) return;
  ui_puts_.push_back({x, y, w, h, std::vector<uint8_t>(rgba.begin(), rgba.begin() + ptrdiff_t(size_t(w) * h * 4))});
}

bool Renderer::create_ui(std::string& error) {
  if (!ui_font_.buffer) {
    const auto font = ui::font_segments();
    if (!upload_buffer(font.data(), font.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ui_font_)) {
      error = "UI font buffer";
      return false;
    }
    const uint32_t atlas_sz = options_.ui_atlas_size ? options_.ui_atlas_size : kUiAtlas;
    if (!create_image(atlas_sz, atlas_sz, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ui_atlas_)) {
      error = "UI atlas";
      return false;
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_TRY(vkCreateSampler(device_, &sampler, nullptr, &ui_sampler_), "UI sampler");
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = 2;
    set.pBindings = bindings;
    VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &ui_set_layout_), "UI set layout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &ui_set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &ui_layout_), "UI pipeline layout");
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &ui_pool_), "UI descriptor pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = ui_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &ui_set_layout_;
    VK_TRY(vkAllocateDescriptorSets(device_, &alloc, &ui_set_), "UI descriptor set");
    VkDescriptorBufferInfo font_info{ui_font_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo atlas_info{ui_sampler_, ui_atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstSet = ui_set_, writes[0].dstBinding = 0, writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[0].pBufferInfo = &font_info;
    writes[1].dstSet = ui_set_, writes[1].dstBinding = 1, writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[1].pImageInfo = &atlas_info;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    // The atlas starts transparent, in its sampled layout.
    if (!submit_once([&](VkCommandBuffer cmd) {
          image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
          VkClearColorValue clear{};
          VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          vkCmdClearColorImage(cmd, ui_atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
          image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        })) {
      error = "UI atlas clear";
      return false;
    }
  }
  if (ui_pipeline_sdr_ && ui_pipeline_sdr_ != ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_sdr_, nullptr);
  ui_pipeline_sdr_ = VK_NULL_HANDLE;
  if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr), ui_pipeline_ = VK_NULL_HANDLE;

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
  VK_TRY(make_module(kUi_vert_spv, sizeof(kUi_vert_spv), vert), "UI vertex shader");
  VK_TRY(make_module(kUi_frag_spv, sizeof(kUi_frag_spv), frag), "UI fragment shader");
  const VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main",
       nullptr}};
  const VkVertexInputBindingDescription binding{0, sizeof(ui::Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
  const VkVertexInputAttributeDescription attributes[6] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                           {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
                                                           {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                                                           {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
                                                           {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 64},
                                                           {5, 0, VK_FORMAT_R32G32B32A32_UINT, 80}};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 6;
  vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.blendEnable = VK_TRUE;  // premultiplied
  attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  attachment.colorBlendOp = VK_BLEND_OP_ADD;
  attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  ui_format_ = color_format_;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &ui_format_;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = ui_layout_;
  VkResult result = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &ui_pipeline_);
  if (result == VK_SUCCESS) {
    if (ui_format_ == VK_FORMAT_R8G8B8A8_UNORM) {
      ui_pipeline_sdr_ = ui_pipeline_;
    } else {
      const VkFormat sdr_fmt = VK_FORMAT_R8G8B8A8_UNORM;
      rendering.pColorAttachmentFormats = &sdr_fmt;
      result = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &ui_pipeline_sdr_);
    }
  }
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  VK_TRY(result, "UI pipeline");
  return true;
}

void Renderer::record_ui(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view, VkImageLayout& layout,
                         VkPipelineStageFlags2& last_stage, VkAccessFlags2& last_access, VkPipeline pipeline,
                         uint32_t width, uint32_t height) {
  if (ui_items_.empty() && ui_puts_.empty()) return;
  if (!ui_ready_) {
    if (ui_failed_) return;
    std::string e;
    if (!create_ui(e)) {
      std::fprintf(stderr, "carview: UI layer off: %s\n", e.c_str());
      ui_failed_ = true;
      return;
    }
    ui_ready_ = true;
  }
  if (!pipeline) pipeline = (ui_format_ == VK_FORMAT_R8G8B8A8_UNORM) ? ui_pipeline_sdr_ : ui_pipeline_;
  // Atlas uploads: staged, copied, back to sampled.
  if (!ui_puts_.empty()) {
    VkDeviceSize bytes = 0;
    for (const auto& p : ui_puts_) bytes += p.rgba.size();
    FrameSlot& fs = slot();
    if (fs.ui_staging.size < bytes) {
      // This slot's last frame is done (its fence): the buffer is free to replace.
      if (fs.ui_staging_mapped) vmaUnmapMemory(allocator_, fs.ui_staging.allocation);
      fs.ui_staging_mapped = nullptr;
      destroy_buffer(fs.ui_staging);
      if (create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, fs.ui_staging) &&
          vmaMapMemory(allocator_, fs.ui_staging.allocation, &fs.ui_staging_mapped) != VK_SUCCESS)
        fs.ui_staging_mapped = nullptr;
    }
    if (fs.ui_staging_mapped) {
      std::vector<VkBufferImageCopy> regions;
      VkDeviceSize at = 0;
      for (const auto& p : ui_puts_) {
        std::memcpy(static_cast<uint8_t*>(fs.ui_staging_mapped) + at, p.rgba.data(), p.rgba.size());
        VkBufferImageCopy r{};
        r.bufferOffset = at;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {int32_t(p.x), int32_t(p.y), 0};
        r.imageExtent = {p.w, p.h, 1};
        regions.push_back(r);
        at += p.rgba.size();
      }
      image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      vkCmdCopyBufferToImage(cmd, fs.ui_staging.buffer, ui_atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(regions.size()), regions.data());
      image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    ui_puts_.clear();
  } else {
    // Release oversized transient UI staging buffers (> 2 MiB) once this slot's fence has passed.
    FrameSlot& fs = slot();
    if (fs.ui_staging.size > (2ULL << 20)) {
      if (fs.ui_staging_mapped) vmaUnmapMemory(allocator_, fs.ui_staging.allocation);
      fs.ui_staging_mapped = nullptr;
      destroy_buffer(fs.ui_staging);
    }
  }
  if (ui_items_.empty() || !pipeline) return;
  const VkDeviceSize bytes = ui_items_.size() * sizeof(ui::Instance);
  FrameSlot& us = slot();
  if (us.ui_instances.size < bytes) {
    if (us.ui_instances_mapped) vmaUnmapMemory(allocator_, us.ui_instances.allocation);
    us.ui_instances_mapped = nullptr;
    destroy_buffer(us.ui_instances);
    const VkDeviceSize cap = std::max<VkDeviceSize>(bytes * 2, 64 * 1024);
    if (!create_buffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, us.ui_instances) ||
        vmaMapMemory(allocator_, us.ui_instances.allocation, &us.ui_instances_mapped) != VK_SUCCESS) {
      us.ui_instances_mapped = nullptr;
      return;
    }
  }
  std::memcpy(us.ui_instances_mapped, ui_items_.data(), size_t(bytes));
  image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                last_stage, last_access, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  last_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  last_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = final_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {width, height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vkCmdBeginRendering(cmd, &rendering);
  VkViewport viewport{0, 0, float(width), float(height), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {width, height}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_layout_, 0, 1, &ui_set_, 0, nullptr);
  const float inv_w = 2.0f / float(std::max(1u, width));
  const float inv_h = 2.0f / float(std::max(1u, height));
  const float hud_scale = hud_layout_.virtual_w > 0 ? hud_layout_.scale : 1.0f;
  const float off_x = hud_layout_.virtual_w > 0 ? hud_layout_.origin_x : 0.0f;
  const float off_y = hud_layout_.virtual_w > 0 ? hud_layout_.origin_y : 0.0f;
  const float xform[4] = {hud_scale * inv_w, hud_scale * inv_h, off_x * inv_w - 1.0f, off_y * inv_h - 1.0f};
  vkCmdPushConstants(cmd, ui_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof xform, xform);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &us.ui_instances.buffer, &zero);
  vkCmdDraw(cmd, 6, uint32_t(ui_items_.size()), 0, 0);
  vkCmdEndRendering(cmd);
  mark(cmd, "ui");
}

void Renderer::set_overlay(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, int32_t x, int32_t y,
                           int layer) {
  if (layer < 0 || layer >= kOverlayLayers) return;
  if (rgba.size() != size_t(width) * height * 4) rgba.clear();
  OverlayLayer& l = overlays_[layer];
  l.pixels = std::move(rgba);
  l.width = l.pixels.empty() ? 0 : width;
  l.height = l.pixels.empty() ? 0 : height;
  l.x = x;
  l.y = y;
  l.dirty = true;
  ++l.version;
}

bool Renderer::uses_modern_chain() const {
  const bool post = options_.post.enabled && hdr_.view != VK_NULL_HANDLE;
  if (!post || !modern_ready_ || !scene_sdr_.view || !stand_in_.view) return false;
  const uint32_t rw = render_width();
  const uint32_t rh = render_height();
  const bool need_scale = (rw != target_width_ || rh != target_height_);
  const bool need_upscaler = need_scale || graphics_.upscaler != modern::Upscaler::kBilinear;
  const bool need_hdr_stage = (hdr_mode_ != modern::HdrMode::kOff) || graphics_.hdr_compare || (hdr_pattern_ != 0);
  const bool need_fxaa_world = (graphics_.aa == modern::AaMode::kFxaaWorld) && fxaa_pipe_ != VK_NULL_HANDLE;
  const bool need_fxaa_all = (graphics_.aa == modern::AaMode::kFxaaAll) && fxaa_pipe_ != VK_NULL_HANDLE;
  return need_upscaler || need_hdr_stage || need_fxaa_world || need_fxaa_all || inspector_active_;
}

bool Renderer::stage_overlay(VkFormat final_format) {
  overlay_staged_ = false;
  for (auto& l : overlays_) l.staged = false;
  const bool modern_chain = uses_modern_chain();
  if (!overlay_supported_ && !modern_chain) return false;
  // Where each layer that fits goes, packed one after another (16-byte
  // aligned offsets, as copies want a multiple of the texel size).
  VkDeviceSize bytes = 0, offsets[kOverlayLayers]{};
  for (int i = 0; i < kOverlayLayers; ++i) {
    OverlayLayer& l = overlays_[i];
    if (l.pixels.empty()) continue;
    const int64_t x = l.x < 0 ? int64_t(target_width_) - l.width + l.x + 1 : l.x;
    const int64_t y = l.y < 0 ? int64_t(target_height_) - l.height + l.y + 1 : l.y;
    if (x < 0 || y < 0 || x + l.width > target_width_ || y + l.height > target_height_) continue;
    l.at = {int32_t(x), int32_t(y)};
    l.staged = true;
    offsets[i] = bytes;
    bytes += (VkDeviceSize(l.pixels.size()) + 15) & ~VkDeviceSize(15);
  }
  if (bytes == 0) return false;
  bool all = false;  // every staged layer is copied again (a new buffer, moved offsets)
  FrameSlot& os = slot();
  if (os.overlay.size < bytes) {
    if (os.overlay_mapped) vmaUnmapMemory(allocator_, os.overlay.allocation);
    os.overlay_mapped = nullptr;
    destroy_buffer(os.overlay);
    if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, os.overlay))
      return false;
    if (vmaMapMemory(allocator_, os.overlay.allocation, &os.overlay_mapped) != VK_SUCCESS) {
      os.overlay_mapped = nullptr;
      return false;
    }
    all = true;
  }
  // Overlays copy into stand_in_ (VK_FORMAT_R8G8B8A8_UNORM) when the modern
  // chain is active, and directly into final_image otherwise. Match the exact
  // destination format so R/B channels are never double-swapped!
  const VkFormat eff_final = (final_format == VK_FORMAT_UNDEFINED) ? color_format_ : final_format;
  const VkFormat dst_fmt = modern_chain ? VK_FORMAT_R8G8B8A8_UNORM : eff_final;
  const bool bgra = dst_fmt == VK_FORMAT_B8G8R8A8_UNORM || dst_fmt == VK_FORMAT_B8G8R8A8_SRGB;
  for (int i = 0; i < kOverlayLayers; ++i) {
    OverlayLayer& l = overlays_[i];
    if (!l.staged) continue;
    // Each slot's buffer remembers which version & channel order of a layer it holds.
    if (!all && os.overlay_ver[i] == l.version && os.overlay_off[i] == offsets[i] && os.overlay_bgra[i] == bgra) continue;
    os.overlay_off[i] = offsets[i];
    os.overlay_ver[i] = l.version;
    os.overlay_bgra[i] = bgra;
    auto* out = static_cast<uint8_t*>(os.overlay_mapped) + os.overlay_off[i];
    for (size_t k = 0; k < l.pixels.size(); k += 4) {
      out[k + 0] = l.pixels[k + (bgra ? 2 : 0)];
      out[k + 1] = l.pixels[k + 1];
      out[k + 2] = l.pixels[k + (bgra ? 0 : 2)];
      out[k + 3] = l.pixels[k + 3];
    }
  }
  overlay_staged_ = true;
  return true;
}

bool Renderer::render_offscreen(const FrameConstants& constants, uint32_t width, uint32_t height,
                                std::vector<uint8_t>& rgba, std::string& error,
                                std::vector<float>* depth) {
  if (depth && (samples_ != VK_SAMPLE_COUNT_1_BIT || depth_format_ != VK_FORMAT_D32_SFLOAT)) {
    error = "depth readback needs one sample and a D32_SFLOAT depth buffer";
    return false;
  }
  depth_readable_ = depth != nullptr;
  if (options_.window) wait_idle();  // the targets below are shared with frames in flight
  if (!create_targets(width, height, error)) return false;
  if (lut_dirty_ && lut_pending_size_ != lut_image_size_ && post_final_) {
    if (!ensure_lut_image(lut_pending_size_, error)) return false;
  }
  const VkFormat out_fmt = (hdr_mode_ == modern::HdrMode::kOff) ? color_format_ : VK_FORMAT_R8G8B8A8_UNORM;
  Image output;
  if (!create_image(width, height, 1, out_fmt, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, output)) {
    error = "offscreen colour image";
    return false;
  }
  Buffer readback;
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
  const VkDeviceSize readback_bytes = depth ? bytes * 2 : bytes;  // RGBA8, then f32 depth
  if (!create_buffer(readback_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readback)) {
    destroy_image(output);
    error = "readback buffer";
    return false;
  }
  VkBuffer inspector_dst = VK_NULL_HANDLE;
  const VkDeviceSize insp_bytes = VkDeviceSize(kInspectorPreviewW) * kInspectorPreviewH * 4;
  if (inspector_active_) {
    if (!inspector_offscreen_buf_.buffer) {
      create_buffer(insp_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    inspector_offscreen_buf_);
    }
    inspector_dst = inspector_offscreen_buf_.buffer;
  }
  const bool time_offscreen = gpu_timing_ && timestamp_ns_ > 0;
  if (time_offscreen && !timestamps_) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kTimestamps;
    if (vkCreateQueryPool(device_, &info, nullptr, &timestamps_) != VK_SUCCESS) timestamp_ns_ = 0;
  }
  stage_overlay(out_fmt);  // submit_once waits for the queue: no frame is using the buffer
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    mark_labels_.clear();
    ts_pool_ = timestamps_;
    if (time_offscreen && timestamps_) {
      vkCmdResetQueryPool(cmd, timestamps_, 0, kTimestamps);
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, 0);
      timing_ = true;
    }
    record_scene(cmd, output.image, output.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, constants,
                 VK_NULL_HANDLE, false, out_fmt, inspector_dst);
    if (timing_) {
      mark_end_ = uint32_t(mark_labels_.size()) + 1;
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, mark_end_);
      timing_ = false;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);
    if (depth) {
      VkBufferImageCopy depth_copy{};
      depth_copy.bufferOffset = bytes;
      depth_copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      depth_copy.imageExtent = {width, height, 1};
      vkCmdCopyImageToBuffer(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &depth_copy);
    }
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });
  bool readback_ok = ok;
  if (ok) {
    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, readback.allocation, &mapped) == VK_SUCCESS) {
      rgba.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + bytes);
      if (depth) {
        depth->resize(size_t(width) * height);
        std::memcpy(depth->data(), static_cast<uint8_t*>(mapped) + bytes, size_t(bytes));
      }
      vmaUnmapMemory(allocator_, readback.allocation);
    } else {
      error = "offscreen readback map failed";
      readback_ok = false;
    }
    if (inspector_active_ && inspector_dst != VK_NULL_HANDLE) {
      void* imapped = nullptr;
      if (vmaMapMemory(allocator_, inspector_offscreen_buf_.allocation, &imapped) == VK_SUCCESS) {
        inspector_rgba_.assign(static_cast<uint8_t*>(imapped), static_cast<uint8_t*>(imapped) + insp_bytes);
        vmaUnmapMemory(allocator_, inspector_offscreen_buf_.allocation);
        inspector_has_rgba_ = true;
      }
    }
    if (time_offscreen && timestamps_) {
      uint64_t ticks[kTimestamps] = {};
      if (vkGetQueryPoolResults(device_, timestamps_, 0, mark_end_ + 1, sizeof ticks, ticks, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        auto ms = [&](uint32_t a, uint32_t b) { return double(ticks[b] - ticks[a]) * timestamp_ns_ / 1e6; };
        gpu_frame_ms_ = ms(0, mark_end_);
        slot_stages_.clear();
        for (uint32_t i = 0; i < mark_labels_.size(); ++i) slot_stages_.push_back({mark_labels_[i], ms(i, i + 1)});
        slot_stages_.push_back({"rest", ms(uint32_t(mark_labels_.size()), mark_end_)});
      }
    }
  } else {
    error = "offscreen render submission failed";
  }
  destroy_buffer(readback);
  destroy_image(output);
  if (depth_readable_) {
    destroy_targets();
    depth_readable_ = false;
  }
  return readback_ok;
}

bool Renderer::render_timed(const FrameConstants& constants, uint32_t width, uint32_t height, double& record_ms,
                            double& gpu_ms, std::string& error) {
  if ((targets_dirty_ || target_width_ != width || target_height_ != height) && !create_targets(width, height, error))
    return false;
  if (lut_dirty_ && lut_pending_size_ != lut_image_size_ && post_final_) {
    if (!ensure_lut_image(lut_pending_size_, error)) return false;
  }
  if (!bench_output_.image &&
      !create_image(width, height, 1, color_format_, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                    bench_output_)) {
    error = "benchmark colour image";
    return false;
  }
  if (timestamp_ns_ > 0 && !timestamps_) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kTimestamps;
    if (vkCreateQueryPool(device_, &info, nullptr, &timestamps_) != VK_SUCCESS) timestamp_ns_ = 0;
  }
  stage_overlay();
  double recorded = 0;
  const auto submit_t0 = std::chrono::steady_clock::now();
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    const auto t0 = std::chrono::steady_clock::now();
    mark_labels_.clear();
    ts_pool_ = timestamps_;
    if (timestamps_) {
      vkCmdResetQueryPool(cmd, timestamps_, 0, kTimestamps);
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, 0);
      timing_ = true;
    }
    record_scene(cmd, bench_output_.image, bench_output_.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, constants);
    timing_ = false;
    if (timestamps_) mark_end_ = uint32_t(mark_labels_.size()) + 1;
    if (timestamps_) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, mark_end_);
    recorded = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  });
  if (!ok) {
    error = "benchmark submission failed";
    return false;
  }
  record_ms = recorded;
  gpu_ms = -1;
  uint64_t ticks[kTimestamps] = {};
  stage_ms_.clear();
  if (timestamps_ && vkGetQueryPoolResults(device_, timestamps_, 0, mark_end_ + 1, sizeof ticks, ticks,
                                           sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
    auto ms = [&](uint32_t a, uint32_t b) { return double(ticks[b] - ticks[a]) * timestamp_ns_ / 1e6; };
    gpu_ms = ms(0, mark_end_);
    for (uint32_t i = 0; i < mark_labels_.size(); ++i) stage_ms_.push_back({mark_labels_[i], ms(i, i + 1)});
    stage_ms_.push_back({"rest", ms(uint32_t(mark_labels_.size()), mark_end_)});
    gpu_frame_ms_ = gpu_ms;
    slot_stages_ = stage_ms_;
  }
  const double total_submit_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_t0).count();
  last_submit_wait_ms_ = std::max(0.0, total_submit_ms - recorded);
  return true;
}

void Renderer::mark(VkCommandBuffer cmd, const char* label) {
  if (!timing_ || !ts_pool_ || mark_labels_.size() + 2 >= kTimestamps) return;
  mark_labels_.push_back(label);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts_pool_, uint32_t(mark_labels_.size()));
}

bool Renderer::create_swapchain(std::string& error) {
  VkSurfaceCapabilitiesKHR caps;
  VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
         "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
    hdr10_supported_ = false;
    scrgb_supported_ = false;
    VkFormat sdr_fmt = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    for (const auto& f : formats) {
      if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
          f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        sdr_fmt = f.format;
      }
      if (hdr_colorspace_ext_ && f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
          f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        hdr10_supported_ = true;
      }
      if (hdr_colorspace_ext_ && f.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
          f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
        scrgb_supported_ = true;
      }
    }
    hdr_mode_ = modern::resolve_hdr(graphics_.hdr, hdr10_supported_, scrgb_supported_);
    VkFormat wanted_fmt = sdr_fmt;
    VkColorSpaceKHR wanted_cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (hdr_mode_ == modern::HdrMode::kScRgb) {
      wanted_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
      wanted_cs = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    } else if (hdr_mode_ == modern::HdrMode::kHdr10) {
      wanted_fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      wanted_cs = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    }
    if (color_format_ != VK_FORMAT_UNDEFINED && wanted_fmt != color_format_) {
      if (ui_pipeline_ && ui_pipeline_ != ui_pipeline_sdr_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
      ui_pipeline_ = VK_NULL_HANDLE;
      ui_ready_ = false;
      destroy_modern_passes();
    }
    color_format_ = wanted_fmt;
    swapchain_colorspace_ = wanted_cs;
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(options_.window, &w, &h);
    extent = {uint32_t(std::max(w, 1)), uint32_t(std::max(h, 1))};
  }
  extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  if (extent.width == 0 || extent.height == 0) {
    swapchain_dirty_ = true;  // minimised: try again later
    return true;
  }
  uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount) image_count = std::min(image_count, caps.maxImageCount);
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = surface_;
  info.minImageCount = image_count;
  info.imageFormat = color_format_;
  info.imageColorSpace = swapchain_colorspace_;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  // Transfers into the image carry the overlay panel (set_overlay); transfers
  // out of the image enable F8/Photo Mode swapchain PNG capture.
  overlay_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
  capture_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    (overlay_supported_ ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0) |
                    (capture_supported_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  {
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
    uint32_t mask = modern::vsync_bit(modern::VSync::kFifo);
    for (VkPresentModeKHR m : modes) {
      if (m == VK_PRESENT_MODE_MAILBOX_KHR) mask |= modern::vsync_bit(modern::VSync::kMailbox);
      if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mask |= modern::vsync_bit(modern::VSync::kImmediate);
    }
    vsync_active_ = modern::resolve_vsync(vsync_, mask);
    info.presentMode = vsync_active_ == modern::VSync::kImmediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                       : vsync_active_ == modern::VSync::kMailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                   : VK_PRESENT_MODE_FIFO_KHR;
    // Mailbox and immediate want a spare image.
    if (vsync_active_ != modern::VSync::kFifo && (!caps.maxImageCount || info.minImageCount < caps.maxImageCount))
      info.minImageCount = std::max(info.minImageCount, std::min(caps.minImageCount + 2, caps.maxImageCount ? caps.maxImageCount : caps.minImageCount + 2));
  }
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;
  VkSwapchainKHR created = VK_NULL_HANDLE;
  VK_TRY(vkCreateSwapchainKHR(device_, &info, nullptr, &created), "vkCreateSwapchainKHR");
  destroy_swapchain(false);
  swapchain_ = created;
  swapchain_extent_ = extent;
  uint32_t count = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
  swapchain_images_.resize(count);
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
  for (VkImage image : swapchain_images_) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = color_format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView handle;
    VK_TRY(vkCreateImageView(device_, &view, nullptr, &handle), "swapchain image view");
    swapchain_views_.push_back(handle);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore done;
    VK_TRY(vkCreateSemaphore(device_, &semaphore, nullptr, &done), "vkCreateSemaphore");
    render_done_.push_back(done);
  }
  swapchain_dirty_ = false;
  ++swapchain_recreate_count_;
  // Skip rebuilding offscreen render targets and post descriptors when swapchain
  // pixel dimensions and color format are unchanged (fast alt-tab / focus recovery).
  const bool has_targets = depth_.image != VK_NULL_HANDLE && (!options_.post.enabled || hdr_.image != VK_NULL_HANDLE);
  if (needs_target_recreate(targets_dirty_, has_targets, extent.width, extent.height, target_width_, target_height_,
                            color_format_ != target_color_format_)) {
    return create_targets(extent.width, extent.height, error);
  }
  return true;
}

void Renderer::destroy_swapchain(bool keep_handle) {
  for (VkImageView view : swapchain_views_) vkDestroyImageView(device_, view, nullptr);
  for (VkSemaphore s : render_done_) vkDestroySemaphore(device_, s, nullptr);
  swapchain_views_.clear();
  render_done_.clear();
  swapchain_images_.clear();
  if (!keep_handle && swapchain_) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

void Renderer::request_capture(bool include_ui) {
  capture_requested_ = true;
  capture_include_ui_ = include_ui;
  capture_ready_ = false;
}

bool Renderer::take_captured_frame(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  if (!capture_ready_) return false;
  rgba = std::move(capture_rgba_);
  width = capture_w_;
  height = capture_h_;
  capture_ready_ = false;
  capture_w_ = 0;
  capture_h_ = 0;
  return !rgba.empty() && width > 0 && height > 0;
}

// Reads the timestamps of a slot's last frame (its fence is done).
void Renderer::collect_slot_timing(FrameSlot& s) {
  if (!s.timed || !s.timestamps) return;
  s.timed = false;
  uint64_t ticks[kTimestamps] = {};
  if (vkGetQueryPoolResults(device_, s.timestamps, 0, s.mark_end + 1, sizeof ticks, ticks, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  auto ms = [&](uint32_t a, uint32_t b) { return double(ticks[b] - ticks[a]) * timestamp_ns_ / 1e6; };
  gpu_frame_ms_ = ms(0, s.mark_end);
  slot_stages_.clear();
  for (uint32_t i = 0; i < s.labels.size(); ++i) slot_stages_.push_back({s.labels[i], ms(i, i + 1)});
  slot_stages_.push_back({"rest", ms(uint32_t(s.labels.size()), s.mark_end)});
}

bool Renderer::draw_frame(const FrameConstants& constants, std::string& error) {
  FrameSlot& fs = slot();
  // This slot's last frame (two submissions ago) must be done before its
  // command buffer, staging buffers and queries are reused; the other
  // slot's frame may still be drawing.
  vkWaitForFences(device_, 1, &fs.fence, VK_TRUE, UINT64_MAX);
  if (fs.submit_id > frames_done_) frames_done_ = fs.submit_id;
  collect_slot_timing(fs);
  if (fs.inspector_ready && fs.inspector_readback_mapped) {
    const size_t insp_bytes = size_t(kInspectorPreviewW) * kInspectorPreviewH * 4;
    inspector_rgba_.assign(static_cast<const uint8_t*>(fs.inspector_readback_mapped),
                           static_cast<const uint8_t*>(fs.inspector_readback_mapped) + insp_bytes);
    inspector_has_rgba_ = true;
    fs.inspector_ready = false;
  }
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(options_.window, &w, &h);
  if (w <= 0 || h <= 0) return true;  // minimised
  if (needs_swapchain_recreate(swapchain_dirty_, swapchain_ != VK_NULL_HANDLE, uint32_t(w), uint32_t(h),
                               swapchain_extent_.width, swapchain_extent_.height)) {
    wait_idle();
    if (!create_swapchain(error)) return false;
    if (!swapchain_) return true;
  }
  const bool has_targets = depth_.image != VK_NULL_HANDLE && (!options_.post.enabled || hdr_.image != VK_NULL_HANDLE);
  if (needs_target_recreate(targets_dirty_, has_targets, swapchain_extent_.width, swapchain_extent_.height,
                            target_width_, target_height_, color_format_ != target_color_format_)) {
    wait_idle();
    if (!create_targets(swapchain_extent_.width, swapchain_extent_.height, error)) return false;
  }
  if (lut_dirty_ && lut_pending_size_ != lut_image_size_ && post_final_) {
    if (!ensure_lut_image(lut_pending_size_, error)) return false;
  }
  uint32_t index = 0;
  // Use a finite acquire timeout so occluded/unfocused MoltenVK or DXGI surfaces
  // never stall the SDL event loop for 1+ seconds during alt-tab / Cmd-Tab.
  const uint64_t acquire_timeout_ns = window_focused_ ? 50'000'000ULL : 16'000'000ULL;
  VkResult result =
      vkAcquireNextImageKHR(device_, swapchain_, acquire_timeout_ns, fs.image_ready, VK_NULL_HANDLE, &index);
  if (result == VK_TIMEOUT || result == VK_NOT_READY) {
    return true;
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty_ = true;
    return true;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    error = "vkAcquireNextImageKHR failed (" + std::to_string(int(result)) + ")";
    return false;
  }
  const bool capture_direct_fmt =
      color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_R8G8B8A8_UNORM ||
      color_format_ == VK_FORMAT_B8G8R8A8_SRGB || color_format_ == VK_FORMAT_R8G8B8A8_SRGB;
  bool do_capture = false;
  const VkDeviceSize cap_bytes = VkDeviceSize(swapchain_extent_.width) * swapchain_extent_.height * 4;
  if (capture_requested_ && capture_supported_ && capture_direct_fmt && cap_bytes > 0) {
    if (capture_buffer_.size < cap_bytes) {
      destroy_buffer(capture_buffer_);
      create_buffer(cap_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, capture_buffer_);
    }
    do_capture = capture_buffer_.buffer != VK_NULL_HANDLE;
  }
  vkResetFences(device_, 1, &fs.fence);
  vkResetCommandBuffer(fs.cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(fs.cmd, &begin);
  mark_labels_.clear();
  ts_pool_ = fs.timestamps;
  timing_ = gpu_timing_ && fs.timestamps != VK_NULL_HANDLE;
  if (timing_) {
    vkCmdResetQueryPool(fs.cmd, fs.timestamps, 0, kTimestamps);
    vkCmdWriteTimestamp2(fs.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, fs.timestamps, 0);
  }
  VkBuffer inspector_dst = VK_NULL_HANDLE;
  if (inspector_active_) {
    const VkDeviceSize insp_bytes = VkDeviceSize(kInspectorPreviewW) * kInspectorPreviewH * 4;
    if (!fs.inspector_readback.buffer) {
      if (create_buffer(insp_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        fs.inspector_readback)) {
        vmaMapMemory(allocator_, fs.inspector_readback.allocation, &fs.inspector_readback_mapped);
      }
    }
    inspector_dst = fs.inspector_readback.buffer;
  }
  stage_overlay();  // the slot's fence above: its last frame is done with its buffers
  record_scene(fs.cmd, swapchain_images_[index], swapchain_views_[index],
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, constants,
               do_capture ? capture_buffer_.buffer : VK_NULL_HANDLE, !capture_include_ui_,
               VK_FORMAT_UNDEFINED, inspector_dst);
  if (inspector_dst != VK_NULL_HANDLE) fs.inspector_ready = true;
  if (timing_) {
    fs.labels = mark_labels_;
    fs.mark_end = uint32_t(fs.labels.size()) + 1;
    vkCmdWriteTimestamp2(fs.cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, fs.timestamps, fs.mark_end);
    fs.timed = true;
    timing_ = false;
  }
  vkEndCommandBuffer(fs.cmd);
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &fs.image_ready;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &fs.cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_done_[index];
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    VK_TRY(vkQueueSubmit(queue_, 1, &submit, fs.fence), "vkQueueSubmit");
  }
  fs.submit_id = ++frames_submitted_;
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &render_done_[index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &index;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    result = vkQueuePresentKHR(queue_, &present);
  }
  slot_ = (slot_ + 1) % frames_in_flight_;
  if (do_capture) {
    vkWaitForFences(device_, 1, &fs.fence, VK_TRUE, UINT64_MAX);
    if (fs.submit_id > frames_done_) frames_done_ = fs.submit_id;
    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, capture_buffer_.allocation, &mapped) == VK_SUCCESS) {
      capture_w_ = swapchain_extent_.width;
      capture_h_ = swapchain_extent_.height;
      capture_rgba_.resize(size_t(cap_bytes));
      const auto* src = static_cast<const uint8_t*>(mapped);
      const bool bgra = color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_B8G8R8A8_SRGB;
      for (size_t i = 0; i < size_t(cap_bytes); i += 4) {
        capture_rgba_[i + 0] = src[i + (bgra ? 2 : 0)];
        capture_rgba_[i + 1] = src[i + 1];
        capture_rgba_[i + 2] = src[i + (bgra ? 0 : 2)];
        capture_rgba_[i + 3] = 255;
      }
      vmaUnmapMemory(allocator_, capture_buffer_.allocation);
      capture_ready_ = true;
    }
    capture_requested_ = false;
  } else if (capture_requested_ && (!capture_supported_ || !capture_direct_fmt)) {
    // Fallback when swapchain does not support TRANSFER_SRC_BIT: render offscreen.
    wait_idle();  // render_offscreen rebuilds the targets the other frame may still use
    std::string cap_err;
    std::vector<uint8_t> saved_ui_items_guard;
    const auto saved_ui = ui_items_;
    if (!capture_include_ui_) ui_items_.clear();
    if (render_offscreen(constants, swapchain_extent_.width, swapchain_extent_.height, capture_rgba_, cap_err)) {
      const bool bgra = color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_B8G8R8A8_SRGB;
      for (size_t i = 0; i < capture_rgba_.size(); i += 4) {
        if (bgra) std::swap(capture_rgba_[i + 0], capture_rgba_[i + 2]);
        capture_rgba_[i + 3] = 255;
      }
      capture_w_ = swapchain_extent_.width;
      capture_h_ = swapchain_extent_.height;
      capture_ready_ = true;
    }
    if (!capture_include_ui_) ui_items_ = saved_ui;
    capture_requested_ = false;
  }
  int cur_w = 0, cur_h = 0;
  SDL_GetWindowSizeInPixels(options_.window, &cur_w, &cur_h);
  if (should_mark_dirty_on_present(result, uint32_t(std::max(0, cur_w)), uint32_t(std::max(0, cur_h)),
                                   swapchain_extent_.width, swapchain_extent_.height)) {
    swapchain_dirty_ = true;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    error = "vkQueuePresentKHR failed (" + std::to_string(int(result)) + ")";
    return false;
  }
  return true;
}

}  // namespace carview

#if !defined(SHIFT_RHI_PROVIDES_VMA)
#define VMA_IMPLEMENTATION
#endif
#include "renderer_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

#include "platform_support.h"
#include "rhi/rhi.h"

namespace carview {

namespace {

const char* backend_display_name(shift::rhi::BackendKind k) {
  switch (k) {
    case shift::rhi::BackendKind::Vulkan: return "Vulkan 1.3";
    case shift::rhi::BackendKind::Metal: return "Metal 3";
    case shift::rhi::BackendKind::D3D12: return "DirectX 12";
    case shift::rhi::BackendKind::Auto: return "Auto";
  }
  return "Vulkan 1.3";
}

}  // namespace

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
    if (shadow_raw_sampler_) vkDestroySampler(device_, shadow_raw_sampler_, nullptr);
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

  // Resolve requested RHI backend (CLI / settings / SHIFT_BACKEND env) and probe native
  // RHI device availability for startup diagnostics with clean Vulkan 1.3 frame-graph fallback.
  requested_backend_ = options.requested_backend;
  if (requested_backend_ == shift::rhi::BackendKind::Auto) {
    if (const char* env_be = std::getenv("SHIFT_BACKEND"); env_be && *env_be) {
      requested_backend_ = shift::rhi::parse_backend_kind(env_be, shift::rhi::BackendKind::Auto);
    }
  }
  probed_rhi_backend_ = shift::rhi::BackendKind::Vulkan;
  probed_rhi_device_name_.clear();
  active_backend_label_ = "Vulkan 1.3";
  {
    const auto order = shift::rhi::resolve_backend_order(requested_backend_);
    const shift::rhi::BackendKind target_probe =
        (requested_backend_ != shift::rhi::BackendKind::Auto)
            ? requested_backend_
            : (!order.empty() ? order.front() : shift::rhi::BackendKind::Vulkan);
    if (target_probe == shift::rhi::BackendKind::D3D12 || target_probe == shift::rhi::BackendKind::Metal) {
      // Fast adapter-only probe (avoids spinning up & tearing down a full second GPU device at startup).
      const auto adapters = shift::rhi::enumerate_adapters(target_probe);
      if (!adapters.empty()) {
        probed_rhi_backend_ = adapters.front().backend;
        probed_rhi_device_name_ = adapters.front().name;
        const bool rt_ok = adapters.front().supports_ray_query;
        if (requested_backend_ == shift::rhi::BackendKind::D3D12 ||
            requested_backend_ == shift::rhi::BackendKind::Metal) {
          active_backend_label_ = std::string(backend_display_name(probed_rhi_backend_)) + " (RHI) / Vulkan 1.3";
        } else {
          active_backend_label_ = std::string("Vulkan 1.3 (") + backend_display_name(probed_rhi_backend_) + " ready)";
        }
        std::fprintf(stderr, "[rhi] probed %s on %s (%zu adapter(s), RT=%s); active frame graph: Vulkan 1.3\n",
                     backend_display_name(probed_rhi_backend_), probed_rhi_device_name_.c_str(), adapters.size(),
                     rt_ok ? "yes" : "no");
      } else if (requested_backend_ != shift::rhi::BackendKind::Auto) {
        std::fprintf(stderr, "[rhi] requested %s unavailable (no adapters); falling back to Vulkan 1.3\n",
                     backend_display_name(requested_backend_));
      }
    }
  }

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
  bool pipe_ok = create_pipeline(scene_format(), error);
  // Eagerly pre-compile ALL Renderer pipelines during startup (post, bloom, GTAO,
  // SSCS, SSR, volumetrics, motion blur, FXAA, upscale, temporal, HDR, UI) so
  // Frame 0 never stalls on lazy pipeline compilation or one-shot LUT/atlas init.
  if (pipe_ok && options_.post.enabled) {
    pipe_ok = create_post(error) && create_modern_passes(error);
  }
  if (pipe_ok && !ui_ready_) {
    std::string ui_err;
    if (create_ui(ui_err)) {
      ui_ready_ = true;
    } else {
      std::fprintf(stderr, "carview: UI pre-compile note: %s\n", ui_err.c_str());
    }
  }
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
    if (offered.textureCompressionBC) {
      f2.features.textureCompressionBC = VK_TRUE;
      bc_compression_supported_ = true;
    }
  }
  std::vector<const char*> extensions;
  if (surface_) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
#ifdef SHIFT_ENABLE_RAY_TRACING
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR rq_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtm1_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
  VkPhysicalDeviceOpacityMicromapFeaturesEXT omm_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
#ifdef VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME
  VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV ser_nv_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV};
#endif
#endif
  rt_caps_ = {};
  rt_caps_.is_moltenvk = device_info_.is_moltenvk;
  rt_caps_.is_turnip = device_info_.is_turnip;
  rt_caps_.device_local_bytes = uint64_t(device_local_bytes());
  bool has_portability_subset = false;
  {
    uint32_t dev_ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, nullptr);
    std::vector<VkExtensionProperties> dev_exts(dev_ext_count);
    if (dev_ext_count > 0) vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, dev_exts.data());
    bool has_vrs_ext = false;
    bool has_as_ext = false, has_rq_ext = false, has_rtp_ext = false, has_dho_ext = false;
    bool has_rtm1_ext = false, has_omm_ext = false, has_ser_nv_ext = false, has_ser_ext = false;
    for (const auto& e : dev_exts) {
      if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
        extensions.push_back("VK_KHR_portability_subset");
        has_portability_subset = true;
        device_info_.portability_subset = true;
      } else if (std::strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0) {
        has_vrs_ext = true;
      } else if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) {
        has_as_ext = true;
      } else if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) {
        has_rq_ext = true;
      } else if (std::strcmp(e.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) {
        has_rtp_ext = true;
      } else if (std::strcmp(e.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) {
        has_dho_ext = true;
      } else if (std::strcmp(e.extensionName, VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME) == 0) {
        has_rtm1_ext = true;
      } else if (std::strcmp(e.extensionName, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME) == 0) {
        has_omm_ext = true;
#ifdef VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME
      } else if (std::strcmp(e.extensionName, VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) == 0) {
        has_ser_nv_ext = true;
#endif
#ifdef VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME
      } else if (std::strcmp(e.extensionName, VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) == 0) {
        has_ser_ext = true;
#endif
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
#ifdef SHIFT_ENABLE_RAY_TRACING
    const char* rt_off_env = std::getenv("SHIFT_RT_FORCE_OFF");
    const bool rt_force_off = rt_off_env && *rt_off_env == '1';
    rt_caps_.compiled_in = true;
    if (!rt_force_off && has_as_ext && has_rq_ext && has_dho_ext) {
      VkPhysicalDeviceVulkan12Features o12_rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      VkPhysicalDeviceAccelerationStructureFeaturesKHR o_as{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
      VkPhysicalDeviceRayQueryFeaturesKHR o_rq{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
      VkPhysicalDeviceRayTracingPipelineFeaturesKHR o_rtp{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
      VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR o_rtm1{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
      VkPhysicalDeviceOpacityMicromapFeaturesEXT o_omm{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
#ifdef VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME
      VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV o_ser_nv{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV};
#endif
      VkPhysicalDeviceFeatures2 qrt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      qrt.pNext = &o12_rt;
      void** tail = &o12_rt.pNext;
      *tail = &o_as;
      tail = &o_as.pNext;
      *tail = &o_rq;
      tail = &o_rq.pNext;
      if (has_rtp_ext) {
        *tail = &o_rtp;
        tail = &o_rtp.pNext;
      }
      if (has_rtm1_ext) {
        *tail = &o_rtm1;
        tail = &o_rtm1.pNext;
      }
      if (has_omm_ext) {
        *tail = &o_omm;
        tail = &o_omm.pNext;
      }
#ifdef VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME
      if (has_ser_nv_ext) {
        *tail = &o_ser_nv;
        tail = &o_ser_nv.pNext;
      }
#endif
      vkGetPhysicalDeviceFeatures2(physical_, &qrt);

      if (o12_rt.bufferDeviceAddress && o12_rt.descriptorIndexing && o12_rt.runtimeDescriptorArray &&
          o12_rt.shaderStorageBufferArrayNonUniformIndexing && o12_rt.descriptorBindingPartiallyBound &&
          o_as.accelerationStructure && o_rq.rayQuery) {
        f12.bufferDeviceAddress = VK_TRUE;
        f12.descriptorIndexing = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        if (o12_rt.scalarBlockLayout) f12.scalarBlockLayout = VK_TRUE;
        if (qrt.features.shaderInt64) f2.features.shaderInt64 = VK_TRUE;
        as_feat.accelerationStructure = VK_TRUE;
        as_feat.pNext = f2.pNext;
        f2.pNext = &as_feat;
        rq_feat.rayQuery = VK_TRUE;
        rq_feat.pNext = f2.pNext;
        f2.pNext = &rq_feat;
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        rt_caps_.buffer_device_address = true;
        rt_caps_.scalar_block_layout = o12_rt.scalarBlockLayout == VK_TRUE;
        rt_caps_.shader_int64 = qrt.features.shaderInt64 == VK_TRUE;
        rt_caps_.acceleration_structure = true;
        rt_caps_.ray_query = true;
        rt_caps_.deferred_host_operations = true;

        if (has_rtp_ext && o_rtp.rayTracingPipeline && !device_info_.is_moltenvk && !device_info_.is_turnip) {
          rtp_feat.rayTracingPipeline = VK_TRUE;
          rtp_feat.pNext = f2.pNext;
          f2.pNext = &rtp_feat;
          extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
          rt_caps_.ray_tracing_pipeline = true;
        }
        if (has_rtm1_ext && o_rtm1.rayTracingMaintenance1) {
          rtm1_feat.rayTracingMaintenance1 = VK_TRUE;
          rtm1_feat.pNext = f2.pNext;
          f2.pNext = &rtm1_feat;
          extensions.push_back(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
          rt_caps_.rt_maintenance1 = true;
        }
        if (has_omm_ext && o_omm.micromap && !device_info_.is_moltenvk && !device_info_.is_turnip) {
          omm_feat.micromap = VK_TRUE;
          omm_feat.pNext = f2.pNext;
          f2.pNext = &omm_feat;
          extensions.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
          rt_caps_.opacity_micromap = true;
        }
#ifdef VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME
        if (has_ser_nv_ext && o_ser_nv.rayTracingInvocationReorder && rt_caps_.ray_tracing_pipeline) {
          ser_nv_feat.rayTracingInvocationReorder = VK_TRUE;
          ser_nv_feat.pNext = f2.pNext;
          f2.pNext = &ser_nv_feat;
          extensions.push_back(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
          rt_caps_.invocation_reorder_nv = true;
        }
#endif
        rt_caps_.invocation_reorder_ext = has_ser_ext && rt_caps_.ray_tracing_pipeline;

        // Query RT properties (scratch alignment, SBT handle sizes, recursion depth).
        VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp_props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 prt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        prt.pNext = &as_props;
        if (rt_caps_.ray_tracing_pipeline) as_props.pNext = &rtp_props;
        vkGetPhysicalDeviceProperties2(physical_, &prt);
        rt_caps_.min_as_scratch_alignment = std::max(as_props.minAccelerationStructureScratchOffsetAlignment, 256u);
        if (rt_caps_.ray_tracing_pipeline) {
          rt_caps_.max_ray_recursion_depth = rtp_props.maxRayRecursionDepth;
          rt_caps_.shader_group_handle_size = rtp_props.shaderGroupHandleSize;
          rt_caps_.shader_group_base_alignment = rtp_props.shaderGroupBaseAlignment;
        }
      }
    }
#else
    (void)has_as_ext;
    (void)has_rq_ext;
    (void)has_rtp_ext;
    (void)has_dho_ext;
    (void)has_rtm1_ext;
    (void)has_omm_ext;
    (void)has_ser_nv_ext;
    (void)has_ser_ext;
#endif
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
    // Default locking: stage_world() and reaper_ run off the main thread.
    // Enable BDA allocation flag whenever VK_KHR_buffer_device_address is active.
    alloc_info.flags = rt_caps_.buffer_device_address ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0;
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
    if (ec2) {
      std::filesystem::remove(tmp_path, ec2);
      return;
    }
  }
  pipeline_cache_loaded_ = true;
  pipeline_cache_loaded_bytes_ = data.size();
}

bool Renderer::compile_pipelines_parallel(const std::vector<std::function<VkResult(VkPipelineCache)>>& tasks,
                                          std::string& error) {
  if (tasks.empty()) return true;
  if (tasks.size() == 1) {
    const VkResult r = tasks[0](pipeline_cache_);
    if (r != VK_SUCCESS) {
      error = "pipeline creation failed (" + std::to_string(int(r)) + ")";
      return false;
    }
    return true;
  }

  const uint32_t hw_threads = std::max(2u, std::thread::hardware_concurrency());
  const size_t num_threads = std::min<size_t>(tasks.size(), size_t(hw_threads));
  const bool use_private_caches = !pipeline_cache_loaded_ && device_ != VK_NULL_HANDLE &&
                                  pipeline_cache_ != VK_NULL_HANDLE;

  std::vector<VkPipelineCache> worker_caches;
  if (use_private_caches) {
    worker_caches.assign(num_threads, VK_NULL_HANDLE);
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    for (size_t t = 0; t < num_threads; ++t) {
      if (vkCreatePipelineCache(device_, &pci, nullptr, &worker_caches[t]) != VK_SUCCESS) {
        worker_caches[t] = VK_NULL_HANDLE;
      }
    }
  }

  std::atomic<size_t> next_task{0};
  std::atomic<int> first_error{int(VK_SUCCESS)};

  auto worker_fn = [&](size_t tid) {
    const VkPipelineCache cache =
        (use_private_caches && worker_caches[tid] != VK_NULL_HANDLE) ? worker_caches[tid] : pipeline_cache_;
    while (true) {
      const size_t idx = next_task.fetch_add(1, std::memory_order_relaxed);
      if (idx >= tasks.size()) break;
      const VkResult r = tasks[idx](cache);
      if (r != VK_SUCCESS) {
        int expected = int(VK_SUCCESS);
        first_error.compare_exchange_strong(expected, int(r), std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads - 1);
  for (size_t t = 1; t < num_threads; ++t) {
    threads.emplace_back(worker_fn, t);
  }
  worker_fn(0);
  for (std::thread& th : threads) th.join();

  if (use_private_caches) {
    std::vector<VkPipelineCache> valid_caches;
    valid_caches.reserve(worker_caches.size());
    for (VkPipelineCache c : worker_caches) {
      if (c != VK_NULL_HANDLE) valid_caches.push_back(c);
    }
    if (!valid_caches.empty()) {
      vkMergePipelineCaches(device_, pipeline_cache_, uint32_t(valid_caches.size()), valid_caches.data());
      for (VkPipelineCache c : valid_caches) {
        vkDestroyPipelineCache(device_, c, nullptr);
      }
    }
  }

  const int err_code = first_error.load(std::memory_order_relaxed);
  if (err_code != int(VK_SUCCESS)) {
    error = "parallel pipeline creation failed (" + std::to_string(err_code) + ")";
    return false;
  }
  return true;
}

bool Renderer::prewarm_targets(uint32_t width, uint32_t height, std::string& error) {
  if (!device_ || width == 0 || height == 0) return true;
  if (target_width_ == width && target_height_ == height && depth_.image != VK_NULL_HANDLE &&
      (!options_.post.enabled || hdr_.image != VK_NULL_HANDLE)) {
    return true;
  }
  return create_targets(width, height, error);
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


void Renderer::wait_idle() {
  if (!device_) return;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    vkDeviceWaitIdle(device_);
  }
  frames_done_ = frames_submitted_;
}


}  // namespace carview

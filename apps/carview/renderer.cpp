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
#include <cstring>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "car_frag_spv.h"
#include "car_model.h"
#include "car_vert_spv.h"
#include "post_frag_spv.h"
#include "post_vert_spv.h"
#include "ui_frag_spv.h"
#include "ui_vert_spv.h"

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

}  // namespace

Renderer::~Renderer() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    destroy_swapchain(false);
    destroy_targets();
    destroy_buffer(vertex_buffer_);
    destroy_buffer(index_buffer_);
    if (overlay_mapped_) {
      vmaUnmapMemory(allocator_, overlay_buffer_.allocation);
      overlay_mapped_ = nullptr;
    }
    destroy_buffer(overlay_buffer_);
    destroy_buffer(ui_font_);
    if (ui_instances_mapped_) {
      vmaUnmapMemory(allocator_, ui_instances_.allocation);
      ui_instances_mapped_ = nullptr;
    }
    destroy_buffer(ui_instances_);
    if (ui_staging_mapped_) {
      vmaUnmapMemory(allocator_, ui_staging_.allocation);
      ui_staging_mapped_ = nullptr;
    }
    destroy_buffer(ui_staging_);
    destroy_image(ui_atlas_);
    if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
    if (ui_layout_) vkDestroyPipelineLayout(device_, ui_layout_, nullptr);
    if (ui_set_layout_) vkDestroyDescriptorSetLayout(device_, ui_set_layout_, nullptr);
    if (ui_pool_) vkDestroyDescriptorPool(device_, ui_pool_, nullptr);
    if (ui_sampler_) vkDestroySampler(device_, ui_sampler_, nullptr);
    destroy_image(texture_);
    destroy_image(bench_output_);
    if (post_quarter_) vkDestroyPipeline(device_, post_quarter_, nullptr);
    if (post_final_) vkDestroyPipeline(device_, post_final_, nullptr);
    if (post_layout_) vkDestroyPipelineLayout(device_, post_layout_, nullptr);
    if (post_set_layout_) vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
    if (post_pool_) vkDestroyDescriptorPool(device_, post_pool_, nullptr);
    if (post_sampler_) vkDestroySampler(device_, post_sampler_, nullptr);
    if (lut_staging_mapped_) {
      vmaUnmapMemory(allocator_, lut_staging_.allocation);
      lut_staging_mapped_ = nullptr;
    }
    destroy_buffer(lut_staging_);
    destroy_image(lut3d_);
    destroy_image(lum_16_);
    destroy_image(lum_adapt_[0]);
    destroy_image(lum_adapt_[1]);
    destroy_image(shadow_map_);
    if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
    if (timestamps_) vkDestroyQueryPool(device_, timestamps_, nullptr);
    if (image_ready_) vkDestroySemaphore(device_, image_ready_, nullptr);
    if (frame_fence_) vkDestroyFence(device_, frame_fence_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
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
  if (volkInitialize() != VK_SUCCESS) {
    error = "no Vulkan loader found (libvulkan / vulkan-1.dll)";
    return false;
  }
  if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
    error = "the Vulkan loader is older than 1.3";
    return false;
  }
  if (!create_instance(error) || !pick_device(error) || !create_device(error)) return false;
  if (options_.window) {
    if (!create_swapchain(error)) return false;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_TRY(vkCreateSemaphore(device_, &semaphore, nullptr, &image_ready_), "vkCreateSemaphore");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_TRY(vkCreateFence(device_, &fence, nullptr, &frame_fence_), "vkCreateFence");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VK_TRY(vkAllocateCommandBuffers(device_, &alloc, &frame_cmd_), "vkAllocateCommandBuffers");
  } else {
    color_format_ = VK_FORMAT_R8G8B8A8_UNORM;
  }
  return create_pipeline(scene_format(), error);
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
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
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
      }
      break;
    }
  }
  if (!physical_) {
    error = "no Vulkan 1.3 device with dynamic rendering and synchronization2";
    return false;
  }
  vkGetPhysicalDeviceMemoryProperties(physical_, &memory_properties_);
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical_, &props);
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
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props2.pNext = &ds_resolve;
  vkGetPhysicalDeviceProperties2(physical_, &props2);
  depth_resolve_supported_ = (ds_resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0;
  VkFormatProperties format_props;
  vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_D32_SFLOAT, &format_props);
  depth_format_ = (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                      ? VK_FORMAT_D32_SFLOAT
                      : VK_FORMAT_D16_UNORM;
  return true;
}

bool Renderer::create_device(std::string& error) {
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = queue_family_;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  // The car shaders' alpha-tested discards compile to demote (a Vulkan 1.3
  // required feature, but it must still be enabled).
  f13.shaderDemoteToHelperInvocation = VK_TRUE;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;
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
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  info.pNext = &f2;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queue;
  info.enabledExtensionCount = uint32_t(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  VK_TRY(vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
  volkLoadDevice(device_);
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
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = queue_family_;
  VK_TRY(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
  return true;
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

bool Renderer::submit_once(const std::function<void(VkCommandBuffer)>& record) {
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd;
  if (vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  record(cmd);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  const bool ok = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                  vkQueueWaitIdle(queue_) == VK_SUCCESS;
  if (ok) frames_done_ = ++frames_submitted_;  // the queue is idle: every frame is done
  vkFreeCommandBuffers(device_, pool_, 1, &cmd);
  return ok;
}

bool Renderer::upload_buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                             Buffer& out) {
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
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
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
      vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  VK_TRY(result, "vkCreateGraphicsPipelines");
  return true;
}

bool Renderer::create_targets(uint32_t width, uint32_t height, std::string& error) {
  destroy_targets();
  if (samples_ != VK_SAMPLE_COUNT_1_BIT &&
      !create_image(width, height, 1, scene_format(), samples_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, msaa_color_)) {
    error = "MSAA colour target";
    return false;
  }
  const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (!create_image(width, height, 1, depth_format_, samples_, depth_usage,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_)) {
    error = "depth target";
    return false;
  }
  if (!create_image(width, height, 1, depth_format_, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_resolved_)) {
    error = "resolved depth target";
    return false;
  }
  if (options_.post.enabled) {
    // Resolved HDR scene + 5-level bloom pyramid (1/4 down to 1/64).
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!create_image(width, height, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                      hdr_)) {
      error = "post hdr target";
      return false;
    }
    uint32_t bw = std::max(1u, width / 4), bh = std::max(1u, height / 4);
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
    if (!create_post(error)) return false;
    // Wire the 22 post descriptor sets:
    //   0: lum16 (hdr, hdr)
    //   1: lum_adapt (lum_16, lum_adapt[1])
    //   2: bright (hdr, hdr)
    //   3..7: blurH[0..4] (bloom_a[k], bloom_a[k])
    //   8..12: blurV[0..4] (bloom_b[k], bloom_b[k])
    //   13..16: down[0..3] (bloom_a[k], bloom_a[k])
    //   17: up3 (bloom_a[3], bloom_a[4])
    //   18: up2 (bloom_a[2], bloom_b[3])
    //   19: up1 (bloom_a[1], bloom_b[2])
    //   20: up0 (bloom_a[0], bloom_b[1])
    //   21: composite (hdr, bloom_b[0])
    VkImageView sources[kPostSetCount][2] = {};
    sources[0][0] = hdr_.view; sources[0][1] = hdr_.view;
    sources[1][0] = lum_16_.view; sources[1][1] = lum_adapt_[1].view;
    sources[2][0] = hdr_.view; sources[2][1] = hdr_.view;
    for (uint32_t k = 0; k < kBloomLevels; ++k) {
      sources[3 + k][0] = bloom_a_[k].view; sources[3 + k][1] = bloom_a_[k].view;
      sources[8 + k][0] = bloom_b_[k].view; sources[8 + k][1] = bloom_b_[k].view;
    }
    for (uint32_t k = 0; k < kBloomLevels - 1; ++k) {
      sources[13 + k][0] = bloom_a_[k].view; sources[13 + k][1] = bloom_a_[k].view;
    }
    sources[17][0] = bloom_a_[3].view; sources[17][1] = bloom_a_[4].view;
    sources[18][0] = bloom_a_[2].view; sources[18][1] = bloom_b_[3].view;
    sources[19][0] = bloom_a_[1].view; sources[19][1] = bloom_b_[2].view;
    sources[20][0] = bloom_a_[0].view; sources[20][1] = bloom_b_[1].view;
    sources[21][0] = hdr_.view;        sources[21][1] = bloom_b_[0].view;

    std::vector<VkDescriptorImageInfo> images(kPostSetCount * 4);
    std::vector<VkWriteDescriptorSet> writes(kPostSetCount * 4);
    for (uint32_t s = 0; s < kPostSetCount; ++s) {
      const VkImageView views[4] = {sources[s][0], sources[s][1], lut3d_.view, lum_adapt_[0].view};
      for (uint32_t b = 0; b < 4; ++b) {
        const size_t idx = size_t(s) * 4 + b;
        images[idx] = {post_sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[idx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[idx];
      }
    }
    vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);
  }
  target_width_ = width;
  target_height_ = height;
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

void Renderer::set_post_lut(std::span<const uint8_t> rgba16) {
  if (rgba16.size() < 16u * 16u * 16u * 4u) return;
  lut_pending_.assign(rgba16.begin(), rgba16.begin() + 16 * 16 * 16 * 4);
  lut_dirty_ = true;
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
    if (!create_buffer(kLutBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, lut_staging_)) {
      error = "3D LUT staging buffer";
      return false;
    }
    VK_TRY(vmaMapMemory(allocator_, lut_staging_.allocation, &lut_staging_mapped_), "map 3D LUT staging");
    // Fill identity ramp by default.
    auto* dst = static_cast<uint8_t*>(lut_staging_mapped_);
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
      vkCmdCopyBufferToImage(cmd, lut_staging_.buffer, lut3d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
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
  const VkDescriptorSetLayoutBinding bindings[4] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 4;
  set.pBindings = bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &post_set_layout_), "post set layout");
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &post_set_layout_;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &post_layout_), "post pipeline layout");
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPostSetCount * 4};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = kPostSetCount;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
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
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
  VK_TRY(make_module(kPost_vert_spv, sizeof(kPost_vert_spv), vert), "post vertex shader");
  VK_TRY(make_module(kPost_frag_spv, sizeof(kPost_frag_spv), frag), "post fragment shader");
  const VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main",
       nullptr}};
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
  VkResult result = VK_SUCCESS;
  for (int k = 0; k < 2 && result == VK_SUCCESS; ++k) {
    const VkFormat format = k == 0 ? scene_format() : color_format_;
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
    result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, k == 0 ? &post_quarter_ : &post_final_);
  }
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  VK_TRY(result, "post pipelines");
  return true;
}

void Renderer::record_post(VkCommandBuffer cmd, VkImageView final_view) {
  const PostSettings& s = options_.post;
  // Upload updated 16x16x16 3D LUT when dirty (zero pipeline stall).
  if (lut_dirty_ && lut_staging_mapped_ && lut_pending_.size() == 16 * 16 * 16 * 4) {
    lut_dirty_ = false;
    std::memcpy(lut_staging_mapped_, lut_pending_.data(), lut_pending_.size());
    image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {16, 16, 16};
    vkCmdCopyBufferToImage(cmd, lut_staging_.buffer, lut3d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
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

  // 2. 5-level multi-mip bloom pyramid (bright pass -> downsample + Gaussian blur -> weighted upsample).
  if (s.bloom) {
    to_write(bloom_a_[0]);
    pass(bloom_a_[0].view, bloom_w_[0], bloom_h_[0], post_quarter_, post_sets_[2], 0,
         1.0f / float(target_width_), 1.0f / float(target_height_));
    to_read(bloom_a_[0]);

    for (uint32_t k = 0; k < kBloomLevels; ++k) {
      if (k > 0) {
        to_write(bloom_a_[k]);
        // Downsample from bloom_a_[k-1] with light blur kernel.
        pass(bloom_a_[k].view, bloom_w_[k], bloom_h_[k], post_quarter_, post_sets_[13 + k - 1], 1,
             0.5f / float(bloom_w_[k - 1]), 0.5f / float(bloom_h_[k - 1]));
        to_read(bloom_a_[k]);
      }
      to_write(bloom_b_[k]);
      pass(bloom_b_[k].view, bloom_w_[k], bloom_h_[k], post_quarter_, post_sets_[3 + k], 1,
           1.0f / float(bloom_w_[k]), 0);
      to_read(bloom_b_[k]);
      image_barrier(cmd, bloom_a_[k].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, read, color_stage, color_write);
      pass(bloom_a_[k].view, bloom_w_[k], bloom_h_[k], post_quarter_, post_sets_[8 + k], 1,
           0, 1.0f / float(bloom_h_[k]));
      to_read(bloom_a_[k]);
    }
    // Upsample combine: 4+3 -> b[3], b[3]+2 -> b[2], b[2]+1 -> b[1], b[1]+0 -> b[0].
    float w_sum = 0.0f;
    for (uint32_t k = 0; k < kBloomLevels; ++k) w_sum += std::max(s.bloom_weights[k], 0.0f);
    const float w_norm = w_sum > 1e-4f ? 1.0f / w_sum : 0.2f;
    for (int k = int(kBloomLevels) - 2; k >= 0; --k) {
      const uint32_t set_idx = 17u + uint32_t(3 - k);
      const float w_cur = std::max(s.bloom_weights[k], 0.0f) * w_norm;
      const float w_prev = (k == int(kBloomLevels) - 2) ? (std::max(s.bloom_weights[k + 1], 0.0f) * w_norm) : 1.0f;
      image_barrier(cmd, bloom_b_[k].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, read, color_stage, color_write);
      pass(bloom_b_[k].view, bloom_w_[k], bloom_h_[k], post_quarter_, post_sets_[set_idx], 5, w_cur, w_prev);
      to_read(bloom_b_[k]);
    }
  } else {
    to_write(bloom_b_[0]);
    to_read(bloom_b_[0]);
  }

  // 3. Final composite (scene + bloom -> exposure -> Hable -> 3D LUT -> vignette -> desat -> sRGB).
  pass(final_view, target_width_, target_height_, post_final_, post_sets_[21], 2, 0, 0, s.vignette_amount);
  mark(cmd, "post");
}

void Renderer::destroy_targets() {
  destroy_image(msaa_color_);
  destroy_image(depth_);
  destroy_image(depth_resolved_);
  destroy_image(hdr_);
  for (uint32_t k = 0; k < kBloomLevels; ++k) {
    destroy_image(bloom_a_[k]);
    destroy_image(bloom_b_[k]);
    bloom_w_[k] = bloom_h_[k] = 0;
  }
  target_width_ = target_height_ = 0;
}

void Renderer::record_scene(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view,
                            VkImageLayout final_layout, const FrameConstants& constants) {
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

  const bool two_pass = bool(scene_.draw_particles) && depth_resolved_.view != VK_NULL_HANDLE;
  const bool can_msaa_depth_resolve = msaa && depth_resolve_supported_ && depth_resolved_.view != VK_NULL_HANDLE;
  if (two_pass || post) {
    image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  can_msaa_depth_resolve ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                         : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                  can_msaa_depth_resolve ? depth_stages : VK_PIPELINE_STAGE_2_COPY_BIT,
                  can_msaa_depth_resolve ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                         : VK_ACCESS_2_TRANSFER_WRITE_BIT);
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
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = (depth_readable_ || two_pass || !msaa) ? VK_ATTACHMENT_STORE_OP_STORE
                                                         : VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {options_.reverse_z ? 0.0f : 1.0f, 0};
  if (can_msaa_depth_resolve && (two_pass || post)) {
    depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depth.resolveImageView = depth_resolved_.view;
    depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  }
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {target_width_, target_height_}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  rendering.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rendering);
  VkViewport viewport{0, 0, float(target_width_), float(target_height_), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {target_width_, target_height_}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  if (scene_.draw) {
    scene_.draw(cmd);
  } else {
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

  if (two_pass || post) {
    if (can_msaa_depth_resolve) {
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else if (!msaa) {
      image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_stages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
      VkImageCopy ic{};
      ic.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      ic.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      ic.extent = {target_width_, target_height_, 1};
      vkCmdCopyImage(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_resolved_.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
      image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, depth_stages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else {
      // Fallback if GPU lacks MSAA depth resolve: clear resolved depth to far plane.
      const VkClearDepthStencilValue far_v{options_.reverse_z ? 0.0f : 1.0f, 0};
      const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      vkCmdClearDepthStencilImage(cmd, depth_resolved_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_v, 1, &rng);
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
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
    scene_.draw_particles(cmd);
    vkCmdEndRendering(cmd);
  }
  if (post) record_post(cmd, final_view);
  if (depth_readable_) {
    image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  }

  // The overlay panel, copied over the resolved frame.
  VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkPipelineStageFlags2 last_stage = color_stage;
  VkAccessFlags2 last_access = color_write;
  if (overlay_staged_) {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, color_stage, color_write,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy regions[kOverlayLayers]{};
    uint32_t count = 0;
    for (const auto& l : overlays_) {
      if (!l.staged) continue;
      VkBufferImageCopy& region = regions[count++];
      region.bufferOffset = l.offset;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {l.at.x, l.at.y, 0};
      region.imageExtent = {l.width, l.height, 1};
    }
    vkCmdCopyBufferToImage(cmd, overlay_buffer_.buffer, final_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count,
                           regions);
    layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    last_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    last_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  }
  record_ui(cmd, final_image, final_view, layout, last_stage, last_access);
  if (final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  } else {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
  }
}

void Renderer::set_ui(std::span<const ui::Instance> items) { ui_items_.assign(items.begin(), items.end()); }

void Renderer::ui_atlas_put(uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::span<const uint8_t> rgba) {
  if (!w || !h || x + w > kUiAtlas || y + h > kUiAtlas || rgba.size() < size_t(w) * h * 4) return;
  ui_puts_.push_back({x, y, w, h, std::vector<uint8_t>(rgba.begin(), rgba.begin() + ptrdiff_t(size_t(w) * h * 4))});
}

bool Renderer::create_ui(std::string& error) {
  const auto font = ui::font_segments();
  if (!upload_buffer(font.data(), font.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ui_font_)) {
    error = "UI font buffer";
    return false;
  }
  if (!create_image(kUiAtlas, kUiAtlas, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
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
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
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
  const VkVertexInputAttributeDescription attributes[4] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                           {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
                                                           {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                                                           {3, 0, VK_FORMAT_R32G32B32A32_UINT, 48}};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 4;
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
  const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &ui_pipeline_);
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  VK_TRY(result, "UI pipeline");
  return true;
}

void Renderer::record_ui(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view, VkImageLayout& layout,
                         VkPipelineStageFlags2& last_stage, VkAccessFlags2& last_access) {
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
  // Atlas uploads: staged, copied, back to sampled.
  if (!ui_puts_.empty()) {
    VkDeviceSize bytes = 0;
    for (const auto& p : ui_puts_) bytes += p.rgba.size();
    if (ui_staging_.size < bytes) {
      if (ui_staging_mapped_) vmaUnmapMemory(allocator_, ui_staging_.allocation);
      ui_staging_mapped_ = nullptr;
      destroy_buffer(ui_staging_);
      if (create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ui_staging_) &&
          vmaMapMemory(allocator_, ui_staging_.allocation, &ui_staging_mapped_) != VK_SUCCESS)
        ui_staging_mapped_ = nullptr;
    }
    if (ui_staging_mapped_) {
      std::vector<VkBufferImageCopy> regions;
      VkDeviceSize at = 0;
      for (const auto& p : ui_puts_) {
        std::memcpy(static_cast<uint8_t*>(ui_staging_mapped_) + at, p.rgba.data(), p.rgba.size());
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
      vkCmdCopyBufferToImage(cmd, ui_staging_.buffer, ui_atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(regions.size()), regions.data());
      image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    ui_puts_.clear();
  }
  if (ui_items_.empty() || ui_format_ != color_format_) return;
  const VkDeviceSize bytes = ui_items_.size() * sizeof(ui::Instance);
  if (ui_instances_.size < bytes) {
    if (ui_instances_mapped_) vmaUnmapMemory(allocator_, ui_instances_.allocation);
    ui_instances_mapped_ = nullptr;
    destroy_buffer(ui_instances_);
    const VkDeviceSize cap = std::max<VkDeviceSize>(bytes * 2, 64 * 1024);
    if (!create_buffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ui_instances_) ||
        vmaMapMemory(allocator_, ui_instances_.allocation, &ui_instances_mapped_) != VK_SUCCESS) {
      ui_instances_mapped_ = nullptr;
      return;
    }
  }
  std::memcpy(ui_instances_mapped_, ui_items_.data(), size_t(bytes));
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
  rendering.renderArea = {{0, 0}, {target_width_, target_height_}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vkCmdBeginRendering(cmd, &rendering);
  VkViewport viewport{0, 0, float(target_width_), float(target_height_), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {target_width_, target_height_}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_layout_, 0, 1, &ui_set_, 0, nullptr);
  const float inv[2] = {2.0f / float(target_width_), 2.0f / float(target_height_)};
  vkCmdPushConstants(cmd, ui_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof inv, inv);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &ui_instances_.buffer, &zero);
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
}

bool Renderer::stage_overlay() {
  overlay_staged_ = false;
  for (auto& l : overlays_) l.staged = false;
  if (!overlay_supported_) return false;
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
  if (overlay_buffer_.size < bytes) {
    if (overlay_mapped_) vmaUnmapMemory(allocator_, overlay_buffer_.allocation);
    overlay_mapped_ = nullptr;
    destroy_buffer(overlay_buffer_);
    if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, overlay_buffer_))
      return false;
    if (vmaMapMemory(allocator_, overlay_buffer_.allocation, &overlay_mapped_) != VK_SUCCESS) {
      overlay_mapped_ = nullptr;
      return false;
    }
    all = true;
  }
  const bool bgra = color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_B8G8R8A8_SRGB;
  for (int i = 0; i < kOverlayLayers; ++i) {
    OverlayLayer& l = overlays_[i];
    if (!l.staged) continue;
    if (!all && !l.dirty && l.offset == offsets[i]) continue;
    l.offset = offsets[i];
    l.dirty = false;
    auto* out = static_cast<uint8_t*>(overlay_mapped_) + l.offset;
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
  if (!create_targets(width, height, error)) return false;
  Image output;
  if (!create_image(width, height, 1, color_format_, VK_SAMPLE_COUNT_1_BIT,
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
  stage_overlay();  // submit_once waits for the queue: no frame is using the buffer
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    record_scene(cmd, output.image, output.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, constants);
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
  if ((target_width_ != width || target_height_ != height) && !create_targets(width, height, error)) return false;
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
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    const auto t0 = std::chrono::steady_clock::now();
    mark_labels_.clear();
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
  }
  return true;
}

void Renderer::mark(VkCommandBuffer cmd, const char* label) {
  if (!timing_ || mark_labels_.size() + 2 >= kTimestamps) return;
  mark_labels_.push_back(label);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, uint32_t(mark_labels_.size()));
}

bool Renderer::create_swapchain(std::string& error) {
  VkSurfaceCapabilitiesKHR caps;
  VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
         "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  if (color_format_ == VK_FORMAT_UNDEFINED) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
    color_format_ = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    for (const auto& f : formats) {
      if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
          f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        color_format_ = f.format;
        break;
      }
    }
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
  info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  // Transfers into the image carry the overlay panel (set_overlay).
  overlay_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (overlay_supported_ ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
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
  return create_targets(extent.width, extent.height, error);
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

bool Renderer::draw_frame(const FrameConstants& constants, std::string& error) {
  vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, UINT64_MAX);
  frames_done_ = frames_submitted_;
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(options_.window, &w, &h);
  if (w <= 0 || h <= 0) return true;  // minimised
  if (swapchain_dirty_ || !swapchain_ || uint32_t(w) != swapchain_extent_.width ||
      uint32_t(h) != swapchain_extent_.height) {
    vkDeviceWaitIdle(device_);
    if (!create_swapchain(error)) return false;
    if (!swapchain_) return true;
  }
  uint32_t index = 0;
  VkResult result =
      vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_ready_, VK_NULL_HANDLE, &index);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty_ = true;
    return true;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    error = "vkAcquireNextImageKHR failed (" + std::to_string(int(result)) + ")";
    return false;
  }
  vkResetFences(device_, 1, &frame_fence_);
  vkResetCommandBuffer(frame_cmd_, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(frame_cmd_, &begin);
  stage_overlay();  // the fence above: the last frame is done with its buffer
  record_scene(frame_cmd_, swapchain_images_[index], swapchain_views_[index],
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, constants);
  vkEndCommandBuffer(frame_cmd_);
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &image_ready_;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame_cmd_;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_done_[index];
  VK_TRY(vkQueueSubmit(queue_, 1, &submit, frame_fence_), "vkQueueSubmit");
  ++frames_submitted_;
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &render_done_[index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &index;
  result = vkQueuePresentKHR(queue_, &present);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty_ = true;
  } else if (result != VK_SUCCESS) {
    error = "vkQueuePresentKHR failed (" + std::to_string(int(result)) + ")";
    return false;
  }
  return true;
}

}  // namespace carview

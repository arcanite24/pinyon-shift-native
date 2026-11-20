#include "renderer.h"

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
    destroy_buffer(overlay_buffer_);
    destroy_image(texture_);
    destroy_image(bench_output_);
    if (post_quarter_) vkDestroyPipeline(device_, post_quarter_, nullptr);
    if (post_final_) vkDestroyPipeline(device_, post_final_, nullptr);
    if (post_layout_) vkDestroyPipelineLayout(device_, post_layout_, nullptr);
    if (post_set_layout_) vkDestroyDescriptorSetLayout(device_, post_set_layout_, nullptr);
    if (post_pool_) vkDestroyDescriptorPool(device_, post_pool_, nullptr);
    if (post_sampler_) vkDestroySampler(device_, post_sampler_, nullptr);
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
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = queue_family_;
  VK_TRY(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
  return true;
}

uint32_t Renderer::memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const {
  for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (memory_properties_.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  }
  return UINT32_MAX;
}

bool Renderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags flags, Buffer& out) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  if (vkCreateBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, out.buffer, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, flags);
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS)
    return false;
  vkBindBufferMemory(device_, out.buffer, out.memory, 0);
  out.size = size;
  return true;
}

void Renderer::destroy_buffer(Buffer& b) {
  if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
  if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
  b = Buffer{};
}

bool Renderer::create_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                            VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                            VkImageAspectFlags aspect, Image& out) {
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
  if (vkCreateImage(device_, &info, nullptr, &out.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, out.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS)
    return false;
  vkBindImageMemory(device_, out.image, out.memory, 0);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {aspect, 0, levels, 0, 1};
  return vkCreateImageView(device_, &view, nullptr, &out.view) == VK_SUCCESS;
}

void Renderer::destroy_image(Image& i) {
  if (i.view) vkDestroyImageView(device_, i.view, nullptr);
  if (i.image) vkDestroyImage(device_, i.image, nullptr);
  if (i.memory) vkFreeMemory(device_, i.memory, nullptr);
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
  vkMapMemory(device_, staging.memory, 0, size, 0, &mapped);
  std::memcpy(mapped, data, size_t(size));
  vkUnmapMemory(device_, staging.memory);
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
  vkMapMemory(device_, staging.memory, 0, total, 0, reinterpret_cast<void**>(&mapped));
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
  vkUnmapMemory(device_, staging.memory);
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
  depth.depthCompareOp = VK_COMPARE_OP_LESS;
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
      !create_image(width, height, 1, scene_format(), samples_,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, msaa_color_)) {
    error = "MSAA colour target";
    return false;
  }
  const VkImageUsageFlags depth_usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      (depth_readable_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
  if (!create_image(width, height, 1, depth_format_, samples_, depth_usage,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_)) {
    error = "depth target";
    return false;
  }
  if (options_.post.enabled) {
    // The resolved HDR scene and the bloom pair at a quarter of the size.
    bloom_width_ = std::max(1u, width / 4);
    bloom_height_ = std::max(1u, height / 4);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!create_image(width, height, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                      hdr_) ||
        !create_image(bloom_width_, bloom_height_, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, bloom_a_) ||
        !create_image(bloom_width_, bloom_height_, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, bloom_b_)) {
      error = "post targets";
      return false;
    }
    if (!create_post(error)) return false;
    const VkImageView sources[4][2] = {
        {hdr_.view, hdr_.view}, {bloom_a_.view, bloom_a_.view}, {bloom_b_.view, bloom_b_.view}, {hdr_.view, bloom_a_.view}};
    VkDescriptorImageInfo images[4][2];
    VkWriteDescriptorSet writes[8];
    for (int s = 0; s < 4; ++s)
      for (int b = 0; b < 2; ++b) {
        images[s][b] = {post_sampler_, sources[s][b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[s * 2 + b];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = uint32_t(b);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[s][b];
      }
    vkUpdateDescriptorSets(device_, 8, writes, 0, nullptr);
  }
  target_width_ = width;
  target_height_ = height;
  return true;
}

bool Renderer::ensure_shadow_map(std::string& error) {
  if (shadow_map_.view) return true;
  const uint32_t size = options_.shadow.size ? options_.shadow.size : 1;
  if (!create_image(size, size, 1, kShadowFormat, VK_SAMPLE_COUNT_1_BIT,
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

bool Renderer::create_post(std::string& error) {
  if (post_final_) return true;
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(device_, &sampler, nullptr, &post_sampler_), "post sampler");
  const VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 2;
  set.pBindings = bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &post_set_layout_), "post set layout");
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &post_set_layout_;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &post_layout_), "post pipeline layout");
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 4;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &post_pool_), "post descriptor pool");
  const VkDescriptorSetLayout layouts[4] = {post_set_layout_, post_set_layout_, post_set_layout_, post_set_layout_};
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = post_pool_;
  alloc.descriptorSetCount = 4;
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
  struct {
    float mode[4], abc[4], def[4], desat[4], bloom[4];
  } push = {{0, 0, 0, 0},
            {s.hable[0], s.hable[1], s.hable[2], s.white},
            {s.hable[3], s.hable[4], s.hable[5], s.exposure},
            {s.luminance[0], s.luminance[1], s.luminance[2], s.desaturate},
            {s.bloom_cutoff, s.bloom_scale, s.bloom ? 1.0f : 0.0f, 0}};
  static_assert(sizeof(push) == 80);
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
                  float dx, float dy) {
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
    push.mode[0] = mode, push.mode[1] = dx, push.mode[2] = dy;
    vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
  };
  to_read(hdr_);
  if (s.bloom) {
    to_write(bloom_a_);
    pass(bloom_a_.view, bloom_width_, bloom_height_, post_quarter_, post_sets_[0], 0, 1.0f / float(target_width_),
         1.0f / float(target_height_));
    to_read(bloom_a_);
    to_write(bloom_b_);
    pass(bloom_b_.view, bloom_width_, bloom_height_, post_quarter_, post_sets_[1], 1, 1.0f / float(bloom_width_), 0);
    to_read(bloom_b_);
    image_barrier(cmd, bloom_a_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, read, color_stage, color_write);
    pass(bloom_a_.view, bloom_width_, bloom_height_, post_quarter_, post_sets_[2], 1, 0, 1.0f / float(bloom_height_));
    to_read(bloom_a_);
  } else {
    // The composite's set still names the bloom image: give it a defined layout.
    to_write(bloom_a_);
    to_read(bloom_a_);
  }
  pass(final_view, target_width_, target_height_, post_final_, post_sets_[3], 2, 0, 0);
  mark(cmd, "post");
}

void Renderer::destroy_targets() {
  destroy_image(msaa_color_);
  destroy_image(depth_);
  destroy_image(hdr_);
  destroy_image(bloom_a_);
  destroy_image(bloom_b_);
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

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = msaa ? msaa_color_.view : scene_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{scene_.clear[0], scene_.clear[1], scene_.clear[2], scene_.clear[3]}};
  if (post)
    for (int k = 0; k < 3; ++k) {
      // The clear colour is display (sRGB) encoded: linear for the HDR scene.
      const float c = scene_.clear[k];
      color.clearValue.color.float32[k] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
  if (msaa) {
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = scene_view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = depth_readable_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {1.0f, 0};
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
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {overlay_at_.x, overlay_at_.y, 0};
    region.imageExtent = {overlay_width_, overlay_height_, 1};
    vkCmdCopyBufferToImage(cmd, overlay_buffer_.buffer, final_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    last_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    last_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  }
  if (final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  } else {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
  }
}

void Renderer::set_overlay(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, int32_t x, int32_t y) {
  if (rgba.size() != size_t(width) * height * 4) rgba.clear();
  overlay_pixels_ = std::move(rgba);
  overlay_width_ = overlay_pixels_.empty() ? 0 : width;
  overlay_height_ = overlay_pixels_.empty() ? 0 : height;
  overlay_x_ = x;
  overlay_y_ = y;
}

bool Renderer::stage_overlay() {
  overlay_staged_ = false;
  if (overlay_pixels_.empty() || !overlay_supported_) return false;
  const int64_t x = overlay_x_ < 0 ? int64_t(target_width_) - overlay_width_ + overlay_x_ + 1 : overlay_x_;
  const int64_t y = overlay_y_ < 0 ? int64_t(target_height_) - overlay_height_ + overlay_y_ + 1 : overlay_y_;
  if (x < 0 || y < 0 || x + overlay_width_ > target_width_ || y + overlay_height_ > target_height_) return false;
  const VkDeviceSize bytes = overlay_pixels_.size();
  if (overlay_buffer_.size < bytes) {
    if (overlay_mapped_) vkUnmapMemory(device_, overlay_buffer_.memory);
    overlay_mapped_ = nullptr;
    destroy_buffer(overlay_buffer_);
    if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, overlay_buffer_))
      return false;
    if (vkMapMemory(device_, overlay_buffer_.memory, 0, bytes, 0, &overlay_mapped_) != VK_SUCCESS) {
      overlay_mapped_ = nullptr;
      return false;
    }
  }
  auto* out = static_cast<uint8_t*>(overlay_mapped_);
  const bool bgra = color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_B8G8R8A8_SRGB;
  for (size_t i = 0; i < overlay_pixels_.size(); i += 4) {
    out[i + 0] = overlay_pixels_[i + (bgra ? 2 : 0)];
    out[i + 1] = overlay_pixels_[i + 1];
    out[i + 2] = overlay_pixels_[i + (bgra ? 0 : 2)];
    out[i + 3] = overlay_pixels_[i + 3];
  }
  overlay_at_ = {int32_t(x), int32_t(y)};
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
  if (ok) {
    void* mapped = nullptr;
    vkMapMemory(device_, readback.memory, 0, readback_bytes, 0, &mapped);
    rgba.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + bytes);
    if (depth) {
      depth->resize(size_t(width) * height);
      std::memcpy(depth->data(), static_cast<uint8_t*>(mapped) + bytes, size_t(bytes));
    }
    vkUnmapMemory(device_, readback.memory);
  } else {
    error = "offscreen render submission failed";
  }
  destroy_buffer(readback);
  destroy_image(output);
  if (depth_readable_) {
    destroy_targets();
    depth_readable_ = false;
  }
  return ok;
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

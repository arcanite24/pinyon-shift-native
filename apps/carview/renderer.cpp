#include "renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "car_frag_spv.h"
#include "car_model.h"
#include "car_vert_spv.h"

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
    destroy_image(texture_);
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
  return create_pipeline(color_format_, error);
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
      !create_image(width, height, 1, color_format_, samples_,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, msaa_color_)) {
    error = "MSAA colour target";
    return false;
  }
  if (!create_image(width, height, 1, depth_format_, samples_,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_)) {
    error = "depth target";
    return false;
  }
  target_width_ = width;
  target_height_ = height;
  return true;
}

void Renderer::destroy_targets() {
  destroy_image(msaa_color_);
  destroy_image(depth_);
  target_width_ = target_height_ = 0;
}

void Renderer::record_scene(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view,
                            VkImageLayout final_layout, const FrameConstants& constants) {
  const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
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
  const auto depth_stages =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_stages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depth_stages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = msaa ? msaa_color_.view : final_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{0.16f, 0.18f, 0.21f, 1.0f}};
  if (msaa) {
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = final_view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
  vkCmdEndRendering(cmd);

  if (final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout, color_stage, color_write,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  } else {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout, color_stage, color_write,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
  }
}

bool Renderer::render_offscreen(const FrameConstants& constants, uint32_t width, uint32_t height,
                                std::vector<uint8_t>& rgba, std::string& error) {
  if (!create_targets(width, height, error)) return false;
  Image output;
  if (!create_image(width, height, 1, color_format_, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, output)) {
    error = "offscreen colour image";
    return false;
  }
  Buffer readback;
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
  if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readback)) {
    destroy_image(output);
    error = "readback buffer";
    return false;
  }
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    record_scene(cmd, output.image, output.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, constants);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);
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
    vkMapMemory(device_, readback.memory, 0, bytes, 0, &mapped);
    rgba.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + bytes);
    vkUnmapMemory(device_, readback.memory);
  } else {
    error = "offscreen render submission failed";
  }
  destroy_buffer(readback);
  destroy_image(output);
  return ok;
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
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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

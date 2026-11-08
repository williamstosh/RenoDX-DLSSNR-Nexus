#pragma once

#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "vulkan_test_api_stubs.hpp"

namespace nr::test::vk {

inline void Require(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(operation) + " Vulkan result="
                             + std::to_string(static_cast<int32_t>(result)));
  }
}

template <typename T>
T NativeHandle(uint64_t value) {
  return reinterpret_cast<T>(static_cast<uintptr_t>(value));
}

template <typename T>
uint64_t ApiHandle(T value) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
}

class VulkanContext {
 public:
  VulkanContext() { Initialize(); }
  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;
  ~VulkanContext() { Shutdown(); }

  template <typename T>
  T InstanceProc(const char* name) const {
    return reinterpret_cast<T>(get_instance_proc(instance, name));
  }

  template <typename T>
  T DeviceProc(const char* name) const {
    return reinterpret_cast<T>(get_device_proc(device, name));
  }

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags required) const {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) != 0
          && (memory_properties.memoryTypes[i].propertyFlags & required) == required) {
        return i;
      }
    }
    throw std::runtime_error("compatible Vulkan memory type unavailable");
  }

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  uint32_t queue_family = UINT32_MAX;
  VkPhysicalDeviceMemoryProperties memory_properties{};
  uint32_t vendor_id = 0;
  VkPhysicalDeviceFeatures enabled_features{};

 private:
  HMODULE loader = nullptr;
  PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc = nullptr;

  void Initialize() {
    loader = LoadLibraryW(L"vulkan-1.dll");
    if (loader == nullptr) throw std::runtime_error("vulkan-1.dll unavailable");
    get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (get_instance_proc == nullptr) throw std::runtime_error("vkGetInstanceProcAddr unavailable");

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "NR Nexus Vulkan ControlMask test";
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        GetProcAddress(loader, "vkCreateInstance"));
    Require(create_instance(&instance_info, nullptr, &instance), "vkCreateInstance");
    get_device_proc = InstanceProc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    if (get_device_proc == nullptr) throw std::runtime_error("vkGetDeviceProcAddr unavailable");

    uint32_t physical_count = 0;
    Require(InstanceProc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(
                instance, &physical_count, nullptr), "vkEnumeratePhysicalDevices(count)");
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    Require(InstanceProc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(
                instance, &physical_count, physical_devices.data()), "vkEnumeratePhysicalDevices");
    for (VkPhysicalDevice candidate : physical_devices) {
      VkPhysicalDeviceProperties properties{};
      InstanceProc<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(
          candidate, &properties);
      if (properties.vendorID == 0x10de) {
        physical = candidate;
        vendor_id = properties.vendorID;
        break;
      }
    }
    if (physical == VK_NULL_HANDLE) throw std::runtime_error("NVIDIA Vulkan adapter unavailable");

    InstanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        physical, &memory_properties);
    uint32_t family_count = 0;
    InstanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(
        physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    InstanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(
        physical, &family_count, families.data());
    for (uint32_t i = 0; i < family_count; ++i) {
      if (families[i].queueCount != 0
          && (families[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
                 == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
        queue_family = i;
        break;
      }
    }
    if (queue_family == UINT32_MAX) throw std::runtime_error("Vulkan compute/transfer queue unavailable");

    uint32_t extension_count = 0;
    Require(InstanceProc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(
                physical, nullptr, &extension_count, nullptr), "vkEnumerateDeviceExtensionProperties(count)");
    std::vector<VkExtensionProperties> extensions(extension_count);
    Require(InstanceProc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(
                physical, nullptr, &extension_count, extensions.data()), "vkEnumerateDeviceExtensionProperties");
    const auto has_extension = [&](const char* name) {
      return std::any_of(extensions.begin(), extensions.end(), [name](const auto& entry) {
        return std::strcmp(entry.extensionName, name) == 0;
      });
    };
    std::vector<const char*> enabled_extensions;
    for (const char* required : {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                 VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
                                 VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME}) {
      if (!has_extension(required)) {
        throw std::runtime_error(std::string("required Vulkan device extension unavailable: ") + required);
      }
      enabled_extensions.push_back(required);
    }
    for (const char* optional : {"VK_KHR_external_memory_win32",
                                 "VK_KHR_external_semaphore_win32"}) {
      if (has_extension(optional)) enabled_extensions.push_back(optional);
    }

    VkPhysicalDeviceFeatures supported{};
    InstanceProc<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical, &supported);
    if (supported.shaderStorageImageWriteWithoutFormat != VK_TRUE) {
      throw std::runtime_error("shaderStorageImageWriteWithoutFormat unavailable");
    }
    enabled_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    const float priority = 1.f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
    device_info.ppEnabledExtensionNames = enabled_extensions.data();
    device_info.pEnabledFeatures = &enabled_features;
    Require(InstanceProc<PFN_vkCreateDevice>("vkCreateDevice")(physical, &device_info, nullptr, &device),
            "vkCreateDevice");
    InstanceProc<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, queue_family, 0, &queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    Require(DeviceProc<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pool_info, nullptr,
                                                                           &command_pool),
            "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    Require(DeviceProc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                device, &allocation, &command_buffer), "vkAllocateCommandBuffers");
  }

  void Shutdown() {
    if (device != VK_NULL_HANDLE) {
      if (const auto wait = DeviceProc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")) wait(device);
      if (command_pool != VK_NULL_HANDLE) {
        DeviceProc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, command_pool, nullptr);
      }
      InstanceProc<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
      device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
      InstanceProc<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
      instance = VK_NULL_HANDLE;
    }
    if (loader != nullptr) {
      FreeLibrary(loader);
      loader = nullptr;
    }
  }
};

inline VkFormat ToVkFormat(reshade::api::format value) {
  using reshade::api::format;
  switch (value) {
    case format::r8g8b8a8_unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case format::r16g16_float: return VK_FORMAT_R16G16_SFLOAT;
    case format::r16g16b16a16_float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case format::r32_float: return VK_FORMAT_R32_SFLOAT;
    case format::r32g32_float: return VK_FORMAT_R32G32_SFLOAT;
    case format::r32g32b32a32_float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case format::r32g32b32a32_uint: return VK_FORMAT_R32G32B32A32_UINT;
    default: throw std::runtime_error("test Vulkan adapter format unsupported: "
                                      + std::to_string(static_cast<uint32_t>(value)));
  }
}

class Device final : public DeviceApiStubs {
 public:
  explicit Device(VulkanContext* context) : context(context) {}

  uint64_t get_native() const override { return ApiHandle(context->device); }
  void get_private_data(const uint8_t guid[16], uint64_t* data) const override {
    *data = 0;
    for (const auto& entry : private_data) {
      if (std::memcmp(entry.first.data(), guid, 16) == 0) {
        *data = entry.second;
        return;
      }
    }
  }
  void set_private_data(const uint8_t guid[16], const uint64_t data) override {
    std::array<uint8_t, 16> key{};
    std::memcpy(key.data(), guid, key.size());
    for (auto& entry : private_data) {
      if (entry.first == key) {
        entry.second = data;
        return;
      }
    }
    private_data.emplace_back(key, data);
  }
  reshade::api::device_api get_api() const override { return reshade::api::device_api::vulkan; }

  bool create_resource(const reshade::api::resource_desc& desc,
                       const reshade::api::subresource_data*, reshade::api::resource_usage,
                       reshade::api::resource* output, void** = nullptr) override {
    if (desc.type != reshade::api::resource_type::texture_2d) return false;
    const VkFormat format = ToVkFormat(desc.texture.format);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {desc.texture.width, desc.texture.height, 1};
    info.mipLevels = std::max(1u, static_cast<uint32_t>(desc.texture.levels));
    info.arrayLayers = std::max(1u, static_cast<uint32_t>(desc.texture.depth_or_layers));
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (context->DeviceProc<PFN_vkCreateImage>("vkCreateImage")(
            context->device, &info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    context->DeviceProc<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        context->device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context->MemoryType(requirements.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = context->DeviceProc<PFN_vkAllocateMemory>("vkAllocateMemory")(
        context->device, &allocation, nullptr, &memory);
    if (result == VK_SUCCESS) {
      result = context->DeviceProc<PFN_vkBindImageMemory>("vkBindImageMemory")(
          context->device, image, memory, 0);
    }
    if (result != VK_SUCCESS) {
      if (memory != VK_NULL_HANDLE) {
        context->DeviceProc<PFN_vkFreeMemory>("vkFreeMemory")(context->device, memory, nullptr);
      }
      context->DeviceProc<PFN_vkDestroyImage>("vkDestroyImage")(context->device, image, nullptr);
      return false;
    }
    const uint64_t handle = ApiHandle(image);
    images.emplace(handle, Image{image, memory, desc});
    output->handle = handle;
    return true;
  }

  void destroy_resource(reshade::api::resource resource) override {
    const auto found = images.find(resource.handle);
    if (found == images.end()) return;
    context->DeviceProc<PFN_vkDestroyImage>("vkDestroyImage")(context->device, found->second.image, nullptr);
    context->DeviceProc<PFN_vkFreeMemory>("vkFreeMemory")(context->device, found->second.memory, nullptr);
    images.erase(found);
  }

  reshade::api::resource_desc get_resource_desc(reshade::api::resource resource) const override {
    const auto found = images.find(resource.handle);
    return found == images.end() ? reshade::api::resource_desc{} : found->second.desc;
  }

  VkImage NativeImage(reshade::api::resource resource) const {
    const auto found = images.find(resource.handle);
    return found == images.end() ? VK_NULL_HANDLE : found->second.image;
  }

  uint64_t ResourceForView(reshade::api::resource_view view) const {
    const auto found = view_resources.find(view.handle);
    return found == view_resources.end() ? 0 : found->second;
  }

  bool create_resource_view(reshade::api::resource resource, reshade::api::resource_usage,
                            const reshade::api::resource_view_desc& desc,
                            reshade::api::resource_view* output) override {
    const auto found = images.find(resource.handle);
    if (found == images.end()) return false;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = found->second.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = ToVkFormat(desc.format == reshade::api::format::unknown
                                 ? found->second.desc.texture.format : desc.format);
    info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, desc.texture.first_level,
                             std::max(1u, desc.texture.level_count), desc.texture.first_layer,
                             std::max(1u, desc.texture.layer_count)};
    VkImageView view = VK_NULL_HANDLE;
    if (context->DeviceProc<PFN_vkCreateImageView>("vkCreateImageView")(
            context->device, &info, nullptr, &view) != VK_SUCCESS) return false;
    const uint64_t handle = ApiHandle(view);
    views.emplace(handle, view);
    view_resources.emplace(handle, resource.handle);
    output->handle = handle;
    return true;
  }

  void destroy_resource_view(reshade::api::resource_view view) override {
    const auto found = views.find(view.handle);
    if (found == views.end()) return;
    context->DeviceProc<PFN_vkDestroyImageView>("vkDestroyImageView")(
        context->device, found->second, nullptr);
    views.erase(found);
    view_resources.erase(view.handle);
  }

  bool create_pipeline_layout(uint32_t count, const reshade::api::pipeline_layout_param* params,
                              reshade::api::pipeline_layout* output) override {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkPushConstantRange> push_ranges;
    for (uint32_t param_index = 0; param_index < count; ++param_index) {
      const auto& param = params[param_index];
      if (param.type == reshade::api::pipeline_layout_param_type::push_descriptors_with_ranges) {
        for (uint32_t range_index = 0; range_index < param.descriptor_table.count; ++range_index) {
          const auto& range = param.descriptor_table.ranges[range_index];
          VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          if (range.type == reshade::api::descriptor_type::texture_unordered_access_view) {
            type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          }
          bindings.push_back({range.binding, type, range.count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
      } else if (param.type == reshade::api::pipeline_layout_param_type::push_constants) {
        push_ranges.push_back({VK_SHADER_STAGE_COMPUTE_BIT, param.push_constants.binding * 4,
                               param.push_constants.count * 4});
      } else {
        throw std::runtime_error("unsupported Vulkan test pipeline layout parameter");
      }
    }
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (!bindings.empty()) {
      VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
      set_info.bindingCount = static_cast<uint32_t>(bindings.size());
      set_info.pBindings = bindings.data();
      Require(context->DeviceProc<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(
                  context->device, &set_info, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
    }
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = set_layout == VK_NULL_HANDLE ? 0u : 1u;
    info.pSetLayouts = set_layout == VK_NULL_HANDLE ? nullptr : &set_layout;
    info.pushConstantRangeCount = static_cast<uint32_t>(push_ranges.size());
    info.pPushConstantRanges = push_ranges.data();
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const VkResult result = context->DeviceProc<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
        context->device, &info, nullptr, &layout);
    if (result != VK_SUCCESS) {
      if (set_layout != VK_NULL_HANDLE) {
        context->DeviceProc<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(
            context->device, set_layout, nullptr);
      }
      return false;
    }
    const uint64_t handle = ApiHandle(layout);
    layouts.emplace(handle, Layout{layout, set_layout});
    output->handle = handle;
    return true;
  }

  void destroy_pipeline_layout(reshade::api::pipeline_layout layout) override {
    const auto found = layouts.find(layout.handle);
    if (found == layouts.end()) return;
    context->DeviceProc<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(
        context->device, found->second.layout, nullptr);
    if (found->second.set_layout != VK_NULL_HANDLE) {
      context->DeviceProc<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(
          context->device, found->second.set_layout, nullptr);
    }
    layouts.erase(found);
  }

  bool create_pipeline(reshade::api::pipeline_layout layout, uint32_t count,
                       const reshade::api::pipeline_subobject* subobjects,
                       reshade::api::pipeline* output) override {
    const auto found = layouts.find(layout.handle);
    if (found == layouts.end()) return false;
    const reshade::api::shader_desc* shader = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
      if (subobjects[i].type == reshade::api::pipeline_subobject_type::compute_shader) {
        shader = static_cast<const reshade::api::shader_desc*>(subobjects[i].data);
      }
    }
    if (shader == nullptr) return false;
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = shader->code_size;
    shader_info.pCode = static_cast<const uint32_t*>(shader->code);
    VkShaderModule module = VK_NULL_HANDLE;
    if (context->DeviceProc<PFN_vkCreateShaderModule>("vkCreateShaderModule")(
            context->device, &shader_info, nullptr, &module) != VK_SUCCESS) return false;
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = shader->entry_point == nullptr ? "main" : shader->entry_point;
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = stage;
    pipeline_info.layout = found->second.layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = context->DeviceProc<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        context->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    context->DeviceProc<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(
        context->device, module, nullptr);
    if (result != VK_SUCCESS) return false;
    const uint64_t handle = ApiHandle(pipeline);
    pipelines.emplace(handle, pipeline);
    output->handle = handle;
    return true;
  }

  void destroy_pipeline(reshade::api::pipeline pipeline) override {
    const auto found = pipelines.find(pipeline.handle);
    if (found == pipelines.end()) return;
    context->DeviceProc<PFN_vkDestroyPipeline>("vkDestroyPipeline")(
        context->device, found->second, nullptr);
    pipelines.erase(found);
  }

  ~Device() {
    for (const auto& entry : pipelines) {
      context->DeviceProc<PFN_vkDestroyPipeline>("vkDestroyPipeline")(
          context->device, entry.second, nullptr);
    }
    for (const auto& entry : layouts) {
      context->DeviceProc<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(
          context->device, entry.second.layout, nullptr);
      if (entry.second.set_layout != VK_NULL_HANDLE) {
        context->DeviceProc<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(
            context->device, entry.second.set_layout, nullptr);
      }
    }
    for (const auto& entry : views) {
      context->DeviceProc<PFN_vkDestroyImageView>("vkDestroyImageView")(
          context->device, entry.second, nullptr);
    }
    for (const auto& entry : images) {
      context->DeviceProc<PFN_vkDestroyImage>("vkDestroyImage")(
          context->device, entry.second.image, nullptr);
      context->DeviceProc<PFN_vkFreeMemory>("vkFreeMemory")(
          context->device, entry.second.memory, nullptr);
    }
  }

 private:
  struct Image { VkImage image; VkDeviceMemory memory; reshade::api::resource_desc desc; };
  struct Layout { VkPipelineLayout layout; VkDescriptorSetLayout set_layout; };
  VulkanContext* context;
  std::vector<std::pair<std::array<uint8_t, 16>, uint64_t>> private_data;
  std::unordered_map<uint64_t, Image> images;
  std::unordered_map<uint64_t, VkImageView> views;
  std::unordered_map<uint64_t, uint64_t> view_resources;
  std::unordered_map<uint64_t, Layout> layouts;
  std::unordered_map<uint64_t, VkPipeline> pipelines;
};

class CommandList final : public CommandListApiStubs {
 public:
  CommandList(VulkanContext* context, Device* device) : context(context), device(device) {}
  uint64_t get_native() const override { return ApiHandle(context->command_buffer); }
  void get_private_data(const uint8_t guid[16], uint64_t* data) const override {
    device->get_private_data(guid, data);
  }
  void set_private_data(const uint8_t guid[16], const uint64_t data) override {
    device->set_private_data(guid, data);
  }
  reshade::api::device* get_device() override { return device; }
  void bind_pipeline(reshade::api::pipeline_stage,
                     reshade::api::pipeline pipeline) override {
    context->DeviceProc<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(
        context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, NativeHandle<VkPipeline>(pipeline.handle));
  }
  void push_descriptors(reshade::api::shader_stage, reshade::api::pipeline_layout layout,
                        uint32_t param, const reshade::api::descriptor_table_update& update) override {
    const auto* views = static_cast<const reshade::api::resource_view*>(update.descriptors);
    std::vector<VkDescriptorImageInfo> image_infos(update.count);
    for (uint32_t i = 0; i < update.count; ++i) {
      image_infos[i].imageView = NativeHandle<VkImageView>(views[i].handle);
      image_infos[i].imageLayout = update.type == reshade::api::descriptor_type::texture_unordered_access_view
          ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstBinding = update.binding;
    write.dstArrayElement = update.array_offset;
    write.descriptorCount = update.count;
    write.descriptorType = update.type == reshade::api::descriptor_type::texture_unordered_access_view
        ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = image_infos.data();
    const auto push = context->DeviceProc<PFN_vkCmdPushDescriptorSetKHR>("vkCmdPushDescriptorSetKHR");
    push(context->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
         NativeHandle<VkPipelineLayout>(layout.handle), param, 1, &write);
  }
  void push_constants(reshade::api::shader_stage, reshade::api::pipeline_layout layout,
                      uint32_t param, uint32_t first, uint32_t count, const void* values) override {
    context->DeviceProc<PFN_vkCmdPushConstants>("vkCmdPushConstants")(
        context->command_buffer, NativeHandle<VkPipelineLayout>(layout.handle),
        VK_SHADER_STAGE_COMPUTE_BIT, first * 4, count * 4, values);
  }
  void dispatch(uint32_t x, uint32_t y, uint32_t z) override {
    context->DeviceProc<PFN_vkCmdDispatch>("vkCmdDispatch")(context->command_buffer, x, y, z);
  }
  void barrier(uint32_t count, const reshade::api::resource* resources,
               const reshade::api::resource_usage* old_states,
               const reshade::api::resource_usage* new_states) override {
    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(count);
    VkMemoryBarrier global_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    bool has_global_barrier = false;
    VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dest_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    for (uint32_t i = 0; i < count; ++i) {
      const auto [old_layout, old_access, old_stage] = State(old_states[i]);
      const auto [new_layout, new_access, new_stage] = State(new_states[i]);
      source_stage |= old_stage;
      dest_stage |= new_stage;
      if (resources[i].handle == 0) {
        has_global_barrier = true;
        global_barrier.srcAccessMask |= old_access;
        global_barrier.dstAccessMask |= new_access;
        continue;
      }
      barriers.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
          old_access, new_access, old_layout, new_layout, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, NativeHandle<VkImage>(resources[i].handle),
          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
      image_layouts[resources[i].handle] = new_layout;
    }
    context->DeviceProc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        context->command_buffer, source_stage, dest_stage, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(barriers.size()), barriers.data());
    if (has_global_barrier) {
      context->DeviceProc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
          context->command_buffer, source_stage, dest_stage, 0, 1, &global_barrier, 0, nullptr,
          0, nullptr);
    }
  }

  void copy_texture_region(reshade::api::resource source, uint32_t,
                           const reshade::api::subresource_box* source_box,
                           reshade::api::resource dest, uint32_t,
                           const reshade::api::subresource_box* dest_box,
                           reshade::api::filter_mode = reshade::api::filter_mode::min_mag_mip_point) override {
    const auto source_desc = device->get_resource_desc(source);
    const auto extent = source_box != nullptr
        ? VkExtent3D{source_box->right - source_box->left,
                     source_box->bottom - source_box->top,
                     source_box->back - source_box->front}
        : VkExtent3D{source_desc.texture.width, source_desc.texture.height, 1};
    const auto source_offset = source_box != nullptr
        ? VkOffset3D{static_cast<int32_t>(source_box->left), static_cast<int32_t>(source_box->top),
                     static_cast<int32_t>(source_box->front)} : VkOffset3D{0, 0, 0};
    const auto dest_offset = dest_box != nullptr
        ? VkOffset3D{static_cast<int32_t>(dest_box->left), static_cast<int32_t>(dest_box->top),
                     static_cast<int32_t>(dest_box->front)} : VkOffset3D{0, 0, 0};
    const VkImageCopy copy{
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, source_offset,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, dest_offset, extent};
    context->DeviceProc<PFN_vkCmdCopyImage>("vkCmdCopyImage")(
        context->command_buffer, device->NativeImage(source), ImageLayout(source),
        device->NativeImage(dest), ImageLayout(dest), 1, &copy);
  }

  void clear_unordered_access_view_float(reshade::api::resource_view view,
                                         const float values[4], uint32_t,
                                         const reshade::api::rect*) override {
    const reshade::api::resource resource{device->ResourceForView(view)};
    const VkClearColorValue color{{values[0], values[1], values[2], values[3]}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context->DeviceProc<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(
        context->command_buffer, device->NativeImage(resource), ImageLayout(resource),
        &color, 1, &range);
  }

 private:
  struct ImageState { VkImageLayout layout; VkAccessFlags access; VkPipelineStageFlags stage; };
  static ImageState State(reshade::api::resource_usage usage) {
    using reshade::api::resource_usage;
    if (usage == resource_usage::undefined) {
      return {VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    }
    if (usage == resource_usage::general) {
      return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    }
    if ((usage & resource_usage::unordered_access) != resource_usage::undefined) {
      return {VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    }
    if ((usage & resource_usage::shader_resource) != resource_usage::undefined) {
      return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    }
    if ((usage & resource_usage::copy_dest) != resource_usage::undefined) {
      return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT};
    }
    if ((usage & resource_usage::copy_source) != resource_usage::undefined) {
      return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT};
    }
    throw std::runtime_error("test Vulkan adapter resource usage unsupported");
  }
  VkImageLayout ImageLayout(reshade::api::resource resource) const {
    const auto found = image_layouts.find(resource.handle);
    return found == image_layouts.end() ? VK_IMAGE_LAYOUT_UNDEFINED : found->second;
  }
  VulkanContext* context;
  Device* device;
  std::unordered_map<uint64_t, VkImageLayout> image_layouts;
};

}  // namespace nr::test::vk

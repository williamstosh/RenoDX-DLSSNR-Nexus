#define VK_NO_PROTOTYPES
#define DEBUG_LEVEL_0
#include <windows.h>
#include <vulkan/vulkan.h>
#include <dlssnr.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include "vulkan_test_device.hpp"

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void*, uint32_t) {
  return true;
}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(void*) {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}

namespace addon = renodx::addons::dlss5::internal;
namespace vk_test = nr::test::vk;

namespace {
struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
};

Buffer CreateHostBuffer(vk_test::VulkanContext& context, VkDeviceSize size,
                        VkBufferUsageFlags usage) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  Buffer result{.size = size};
  vk_test::Require(context.DeviceProc<PFN_vkCreateBuffer>("vkCreateBuffer")(
                       context.device, &info, nullptr, &result.buffer), "vkCreateBuffer");
  VkMemoryRequirements requirements{};
  context.DeviceProc<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(
      context.device, result.buffer, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = context.MemoryType(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vk_test::Require(context.DeviceProc<PFN_vkAllocateMemory>("vkAllocateMemory")(
                       context.device, &allocation, nullptr, &result.memory), "vkAllocateMemory(buffer)");
  vk_test::Require(context.DeviceProc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
                       context.device, result.buffer, result.memory, 0), "vkBindBufferMemory");
  return result;
}

void DestroyBuffer(vk_test::VulkanContext& context, Buffer* buffer) {
  if (buffer->buffer != VK_NULL_HANDLE) {
    context.DeviceProc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer->buffer, nullptr);
    buffer->buffer = VK_NULL_HANDLE;
  }
  if (buffer->memory != VK_NULL_HANDLE) {
    context.DeviceProc<PFN_vkFreeMemory>("vkFreeMemory")(context.device, buffer->memory, nullptr);
    buffer->memory = VK_NULL_HANDLE;
  }
}

void Begin(vk_test::VulkanContext& context) {
  vk_test::Require(context.DeviceProc<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(
                       context.command_buffer, 0), "vkResetCommandBuffer");
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vk_test::Require(context.DeviceProc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(
                       context.command_buffer, &info), "vkBeginCommandBuffer");
}

void SubmitRecordedAndWait(vk_test::VulkanContext& context);

void SubmitAndWait(vk_test::VulkanContext& context) {
  vk_test::Require(context.DeviceProc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(
                       context.command_buffer), "vkEndCommandBuffer");
  SubmitRecordedAndWait(context);
}

void SubmitRecordedAndWait(vk_test::VulkanContext& context) {
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vk_test::Require(context.DeviceProc<PFN_vkCreateFence>("vkCreateFence")(
                       context.device, &fence_info, nullptr, &fence), "vkCreateFence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &context.command_buffer;
  const VkResult submitted = context.DeviceProc<PFN_vkQueueSubmit>("vkQueueSubmit")(
      context.queue, 1, &submit, fence);
  if (submitted == VK_SUCCESS) {
    vk_test::Require(context.DeviceProc<PFN_vkWaitForFences>("vkWaitForFences")(
                         context.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
  }
  context.DeviceProc<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
  vk_test::Require(submitted, "vkQueueSubmit");
}

NVSDK_NGX_Resource_VK MakeImageResource(VkImage image, VkImageView view, uint32_t width,
                                        uint32_t height, VkFormat format, bool read_write) {
  NVSDK_NGX_Resource_VK result{};
  result.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
  result.ReadWrite = read_write;
  result.Resource.ImageViewInfo = {view, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                   format, width, height};
  return result;
}

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

addon::vk::EvaluateFn native_real_evaluate = nullptr;
NVSDK_NGX_Resource_VK* native_observed_mask = nullptr;
NVSDK_NGX_Result native_observed_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;

NVSDK_NGX_Result NVSDK_CONV ObserveNativeEvaluate(
    VkCommandBuffer command_buffer, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  void* value = nullptr;
  if (parameters != nullptr
      && NVSDK_NGX_SUCCEED(const_cast<NVSDK_NGX_Parameter*>(parameters)->Get(
          "DLSSNR.ControlMask", &value))) {
    native_observed_mask = static_cast<NVSDK_NGX_Resource_VK*>(value);
  }
  native_observed_result = native_real_evaluate(command_buffer, handle, parameters, callback);
  return native_observed_result;
}

Buffer UploadImage(vk_test::VulkanContext& context, vk_test::CommandList& command_list,
                   reshade::api::resource image, VkImage native_image,
                   const void* data, VkDeviceSize size, uint32_t width, uint32_t height) {
  Buffer upload = CreateHostBuffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  void* mapped = nullptr;
  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, upload.memory, 0, upload.size, 0, &mapped), "map upload");
  std::memcpy(mapped, data, static_cast<size_t>(size));
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, upload.memory);
  const reshade::api::resource resources[] = {image};
  const reshade::api::resource_usage before[] = {reshade::api::resource_usage::undefined};
  const reshade::api::resource_usage copy_dest[] = {reshade::api::resource_usage::copy_dest};
  command_list.barrier(1, resources, before, copy_dest);
  const VkBufferImageCopy region{0, 0, 0,
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {width, height, 1}};
  context.DeviceProc<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
      context.command_buffer, upload.buffer, native_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      1, &region);
  const reshade::api::resource_usage shader_resource[] = {
      reshade::api::resource_usage::shader_resource};
  command_list.barrier(1, resources, copy_dest, shader_resource);
  return upload;
}

void RunNativeNgx() {
  vk_test::VulkanContext context;
  vk_test::Device device(&context);
  vk_test::CommandList command_list(&context, &device);
  reshade::api::resource color{};
  reshade::api::resource output{};
  reshade::api::resource depth{};
  reshade::api::resource motion{};
  reshade::api::resource_view color_view{};
  reshade::api::resource_view output_view{};
  reshade::api::resource_view depth_view{};
  reshade::api::resource_view motion_view{};
  std::vector<Buffer> uploads;
  Buffer mask_readback{};
  const auto cleanup = [&] {
    if (addon::vk::game_api_device.load(std::memory_order_relaxed) == &device) {
      std::lock_guard lock(addon::runtime_mutex);
      addon::vk::ShutdownNr(context.device);
      addon::vk::OnDestroyGameDevice();
    }
    addon::vk::OnDestroyDeviceContext(&device);
    for (Buffer& buffer : uploads) DestroyBuffer(context, &buffer);
    DestroyBuffer(context, &mask_readback);
    device.destroy_resource_view(motion_view);
    device.destroy_resource_view(depth_view);
    device.destroy_resource_view(output_view);
    device.destroy_resource_view(color_view);
    device.destroy_resource(motion);
    device.destroy_resource(depth);
    device.destroy_resource(output);
    device.destroy_resource(color);
  };

  try {
    addon::addon_module = GetModuleHandleW(nullptr);
    addon::hooks_enabled = false;
    Begin(context);
    addon::vk::OnResetCommandList(&command_list);
    addon::vk::game_instance.store(context.instance, std::memory_order_relaxed);
    addon::vk::game_physical_device.store(context.physical, std::memory_order_relaxed);
    addon::vk::game_init_device.store(context.device, std::memory_order_relaxed);
    {
      std::lock_guard lock(addon::vk::context_mutex);
      addon::vk::device_contexts[context.device] = {context.instance, context.physical};
    }
    {
      std::lock_guard lock(addon::vk::context_mutex);
      addon::vk::api_devices[context.device] = &device;
    }
    addon::hooks_enabled = true;
    addon::enabled = true;
    addon::control_mask_enabled = true;
    addon::control_mask_mode = addon::kControlMaskDepthFar;
    addon::control_mask_depth_near = 0.7f;
    addon::control_mask_depth_far = 0.9f;
    addon::control_mask_blend_mode = 0;
    addon::stack_passes = 1;
    addon::hook_point = addon::kHookUpscaled;
    addon::preset = 1;
    addon::intensity = 1.f;
    addon::transfer_strength = 1.f;
    addon::color_strength = 1.f;
    addon::look_mode = false;
    addon::detail_stability = 1;
    addon::PreloadNrRuntime(nullptr);
    Check(addon::direct_api.module != nullptr && addon::direct_api.init != nullptr,
          "production Vulkan loader preloads the selected NR Runtime");
    Check(addon::direct_runtime_requirements.file_version == "310.9.16.0",
          "production Vulkan loader identifies the selected 310.9.16.0 Runtime");

    constexpr uint32_t width = 960;
    constexpr uint32_t height = 540;
    constexpr uint32_t depth_base_x = 4;
    constexpr uint32_t depth_base_y = 6;
    constexpr uint32_t depth_width = width + depth_base_x;
    constexpr uint32_t depth_height = height + depth_base_y;
    const auto usages = reshade::api::resource_usage::shader_resource
        | reshade::api::resource_usage::unordered_access
        | reshade::api::resource_usage::copy_dest
        | reshade::api::resource_usage::copy_source;
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16b16a16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &color),
          "native color image");
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16b16a16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &output),
          "native output image");
    Check(device.create_resource({depth_width, depth_height, 1, 1, reshade::api::format::r32_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &depth),
          "native depth image");
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &motion),
          "native motion image");
    Check(device.create_resource_view(color, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float),
                                     &color_view), "native color view");
    Check(device.create_resource_view(output, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float),
                                     &output_view), "native output view");
    Check(device.create_resource_view(depth, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r32_float),
                                     &depth_view), "native depth view");
    Check(device.create_resource_view(motion, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16_float),
                                     &motion_view), "native motion view");

    std::vector<uint16_t> color_data(width * height * 4, 0x3c00);
    std::vector<uint16_t> output_data(width * height * 4, 0);
    std::vector<float> depth_data(depth_width * depth_height);
    for (uint32_t y = 0; y < depth_height; ++y) {
      const uint32_t relative_y = y >= depth_base_y ? y - depth_base_y : UINT32_MAX;
      const float value = relative_y < 32 ? 0.5f : (relative_y < 56 ? 0.8f : 1.0f);
      for (uint32_t x = 0; x < depth_width; ++x) depth_data[y * depth_width + x] = value;
    }
    std::vector<uint16_t> motion_data(width * height * 2, 0);
    Begin(context);
    addon::hooks_enabled = false;
    addon::vk::OnResetCommandList(&command_list);
    addon::hooks_enabled = true;
    uploads.push_back(UploadImage(context, command_list, color,
                                  vk_test::NativeHandle<VkImage>(color.handle), color_data.data(),
                                  color_data.size() * sizeof(uint16_t), width, height));
    uploads.push_back(UploadImage(context, command_list, output,
                                  vk_test::NativeHandle<VkImage>(output.handle), output_data.data(),
                                  output_data.size() * sizeof(uint16_t), width, height));
    uploads.push_back(UploadImage(context, command_list, depth,
                                  vk_test::NativeHandle<VkImage>(depth.handle), depth_data.data(),
                                  depth_data.size() * sizeof(float), depth_width, depth_height));
    uploads.push_back(UploadImage(context, command_list, motion,
                                  vk_test::NativeHandle<VkImage>(motion.handle), motion_data.data(),
                                  motion_data.size() * sizeof(uint16_t), width, height));

    const auto color_vk = addon::vk::ImageResource(color_view, color, width, height, false,
                                                   VK_FORMAT_R16G16B16A16_SFLOAT);
    const auto output_vk = addon::vk::ImageResource(output_view, output, width, height, true,
                                                    VK_FORMAT_R16G16B16A16_SFLOAT);
    const auto depth_vk = addon::vk::ImageResource(depth_view, depth, depth_width, depth_height, false,
                                                   VK_FORMAT_R32_SFLOAT);
    const auto motion_vk = addon::vk::ImageResource(motion_view, motion, width, height, false,
                                                    VK_FORMAT_R16G16_SFLOAT);
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    addon::SetNrCreateParameters(&parameters, width, height, width, height, 0, false, 1);
    parameters.Set(NVSDK_NGX_Parameter_Color, const_cast<NVSDK_NGX_Resource_VK*>(&color_vk));
    parameters.Set(NVSDK_NGX_Parameter_Output, const_cast<NVSDK_NGX_Resource_VK*>(&output_vk));
    parameters.Set(NVSDK_NGX_Parameter_Depth, const_cast<NVSDK_NGX_Resource_VK*>(&depth_vk));
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, const_cast<NVSDK_NGX_Resource_VK*>(&motion_vk));
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, depth_base_x);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depth_base_y);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC0A71003});
    addon::vk::Feature feature{};
    feature.id = addon::kFeatureDlss;
    feature.device = &device;
    addon::vk::features.emplace(owner, std::move(feature));

    const auto first = addon::vk::ProcessNr(context.command_buffer, owner, &parameters);
    Check(first == renodx::addons::dlss5::NrDeclineReason::kSlotWarmupMaturation,
          "production Vulkan Feature 18 create reaches warmup maturation");
    SubmitAndWait(context);

    Begin(context);
    addon::hooks_enabled = false;
    addon::vk::OnResetCommandList(&command_list);
    addon::hooks_enabled = true;
    const reshade::api::resource_usage color_before[] = {reshade::api::resource_usage::shader_resource};
    const reshade::api::resource_usage color_copy[] = {reshade::api::resource_usage::copy_source};
    const reshade::api::resource_usage output_before[] = {reshade::api::resource_usage::shader_resource};
    const reshade::api::resource_usage output_copy[] = {reshade::api::resource_usage::copy_dest};
    const reshade::api::resource_usage output_write[] = {reshade::api::resource_usage::unordered_access};
    command_list.barrier(1, &color, color_before, color_copy);
    command_list.barrier(1, &output, output_before, output_copy);
    command_list.copy_texture_region(color, 0, nullptr, output, 0, nullptr);
    command_list.barrier(1, &color, color_copy, color_before);
    command_list.barrier(1, &output, output_copy, output_write);

    native_real_evaluate = addon::vk::runtime.evaluate;
    Check(native_real_evaluate != nullptr, "production Vulkan Runtime exposes Feature 18 Evaluate");
    native_observed_mask = nullptr;
    addon::vk::runtime.evaluate = ObserveNativeEvaluate;
    const auto second = addon::vk::ProcessNr(context.command_buffer, owner, &parameters);
    addon::vk::runtime.evaluate = native_real_evaluate;
    Check(second == renodx::addons::dlss5::NrDeclineReason::kCount,
          "production Vulkan Feature 18 Evaluate completes with ControlMask enabled");
    Check(NVSDK_NGX_SUCCEED(native_observed_result),
          "real Vulkan Runtime Feature 18 Evaluate returns success");
    const auto& workset = addon::vk::features.at(owner).worksets.front();
    Check(native_observed_mask != nullptr,
          "real Vulkan Runtime Evaluate receives a ControlMask resource");
    Check(native_observed_mask->Resource.ImageViewInfo.Image
              == vk_test::NativeHandle<VkImage>(workset.images[addon::vk::kControlMask].handle),
          "real Vulkan Runtime receives the generated production mask image");

    mask_readback = CreateHostBuffer(context, width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const reshade::api::resource mask_resource = workset.images[addon::vk::kControlMask];
    const reshade::api::resource_usage mask_before[] = {reshade::api::resource_usage::unordered_access};
    const reshade::api::resource_usage mask_copy[] = {reshade::api::resource_usage::copy_source};
    command_list.barrier(1, &mask_resource, mask_before, mask_copy);
    const VkBufferImageCopy mask_region{0, 0, 0,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {width, height, 1}};
    context.DeviceProc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        context.command_buffer, vk_test::NativeHandle<VkImage>(mask_resource.handle),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mask_readback.buffer, 1, &mask_region);
    SubmitAndWait(context);
    void* mapped = nullptr;
    vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                         context.device, mask_readback.memory, 0, mask_readback.size, 0, &mapped),
                     "map native production mask");
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    Check(pixels[0] > 190 && pixels[0] <= 255, "native production mask retains near depth weight");
    Check(pixels[32 * width * 4] > 90 && pixels[32 * width * 4] < 230,
          "native production mask contains the depth-far transition");
    Check(pixels[63 * width * 4] == 0, "native production mask fades far depth to zero");
    context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, mask_readback.memory);
    SubmitRecordedAndWait(context);
    Check(!addon::vk::RecordedUseComplete(workset.use_id),
          "completed production Vulkan recording remains live while replayable");
    Begin(context);
    addon::vk::OnResetCommandList(&command_list);
    Check(addon::vk::RecordedUseComplete(workset.use_id),
          "production reset retires the replayed workset use");
    cleanup();
    std::cout << "PASS NATIVE_GPU+NATIVE_NGX: production Vulkan Feature 18 Create/Evaluate consumed"
                 " the generated depth-far ControlMask, replayed the completed command buffer,"
                 " and completed reset/readback\n";
  } catch (...) {
    cleanup();
    throw;
  }
}

void RunNativeNgxEvaluateOnly() {
  vk_test::VulkanContext context;
  vk_test::Device device(&context);
  vk_test::CommandList command_list(&context, &device);
  reshade::api::resource color{};
  reshade::api::resource output{};
  reshade::api::resource depth{};
  reshade::api::resource motion{};
  reshade::api::resource_view color_view{};
  reshade::api::resource_view output_view{};
  reshade::api::resource_view depth_view{};
  reshade::api::resource_view motion_view{};
  std::vector<Buffer> uploads;
  NVSDK_NGX_Handle* handle = nullptr;
  const auto cleanup = [&] {
    if (handle != nullptr && addon::vk::runtime.release != nullptr) {
      addon::vk::runtime.release(handle);
      handle = nullptr;
    }
    if (addon::vk::runtime.device == context.device ||
        addon::vk::game_api_device.load(std::memory_order_relaxed) == &device) {
      std::lock_guard lock(addon::runtime_mutex);
      if (addon::vk::runtime.device == context.device) {
        addon::vk::ShutdownNr(context.device);
      }
      if (addon::vk::game_api_device.load(std::memory_order_relaxed) == &device) {
        addon::vk::OnDestroyGameDevice();
      }
    }
    addon::vk::OnDestroyDeviceContext(&device);
    for (Buffer& buffer : uploads) DestroyBuffer(context, &buffer);
    device.destroy_resource_view(motion_view);
    device.destroy_resource_view(depth_view);
    device.destroy_resource_view(output_view);
    device.destroy_resource_view(color_view);
    device.destroy_resource(motion);
    device.destroy_resource(depth);
    device.destroy_resource(output);
    device.destroy_resource(color);
  };

  try {
    addon::addon_module = GetModuleHandleW(nullptr);
    addon::hooks_enabled = false;
    Begin(context);
    addon::vk::OnResetCommandList(&command_list);
    addon::vk::game_instance.store(context.instance, std::memory_order_relaxed);
    addon::vk::game_physical_device.store(context.physical, std::memory_order_relaxed);
    addon::vk::game_init_device.store(context.device, std::memory_order_relaxed);
    {
      std::lock_guard lock(addon::vk::context_mutex);
      addon::vk::device_contexts[context.device] = {context.instance, context.physical};
      addon::vk::api_devices[context.device] = &device;
    }
    addon::hooks_enabled = true;
    addon::enabled = true;
    addon::control_mask_enabled = false;
    addon::stack_passes = 1;
    addon::preset = 1;
    addon::intensity = 1.f;
    addon::transfer_strength = 1.f;
    addon::color_strength = 1.f;
    addon::look_mode = false;
    addon::detail_stability = 1;
    constexpr uint32_t width = 960;
    constexpr uint32_t height = 540;
    constexpr uint32_t depth_width = width;
    constexpr uint32_t depth_height = height;
    const auto usages = reshade::api::resource_usage::shader_resource
        | reshade::api::resource_usage::unordered_access
        | reshade::api::resource_usage::copy_dest
        | reshade::api::resource_usage::copy_source;
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16b16a16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &color),
          "evaluate-only color image");
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16b16a16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &output),
          "evaluate-only output image");
    Check(device.create_resource({depth_width, depth_height, 1, 1, reshade::api::format::r32_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &depth),
          "evaluate-only depth image");
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &motion),
          "evaluate-only motion image");
    Check(device.create_resource_view(color, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float),
                                     &color_view), "evaluate-only color view");
    Check(device.create_resource_view(output, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float),
                                     &output_view), "evaluate-only output view");
    Check(device.create_resource_view(depth, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r32_float),
                                     &depth_view), "evaluate-only depth view");
    Check(device.create_resource_view(motion, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16_float),
                                     &motion_view), "evaluate-only motion view");

    std::vector<uint16_t> color_data(width * height * 4, 0x3c00);
    std::vector<uint16_t> output_data(width * height * 4, 0);
    std::vector<float> depth_data(depth_width * depth_height, 0.5f);
    std::vector<uint16_t> motion_data(width * height * 2, 0);
    Begin(context);
    addon::vk::OnResetCommandList(&command_list);
    uploads.push_back(UploadImage(context, command_list, color,
                                  vk_test::NativeHandle<VkImage>(color.handle), color_data.data(),
                                  color_data.size(), width, height));
    uploads.push_back(UploadImage(context, command_list, output,
                                  vk_test::NativeHandle<VkImage>(output.handle), output_data.data(),
                                  output_data.size(), width, height));
    uploads.push_back(UploadImage(context, command_list, depth,
                                  vk_test::NativeHandle<VkImage>(depth.handle), depth_data.data(),
                                  depth_data.size() * sizeof(float), depth_width, depth_height));
    uploads.push_back(UploadImage(context, command_list, motion,
                                  vk_test::NativeHandle<VkImage>(motion.handle), motion_data.data(),
                                  motion_data.size() * sizeof(uint16_t), width, height));
    SubmitAndWait(context);

    const auto color_vk = addon::vk::ImageResource(color_view, color, width, height, false,
                                                   VK_FORMAT_R16G16B16A16_SFLOAT);
    const auto output_vk = addon::vk::ImageResource(output_view, output, width, height, true,
                                                    VK_FORMAT_R16G16B16A16_SFLOAT);
    const auto depth_vk = addon::vk::ImageResource(depth_view, depth, depth_width, depth_height,
                                                   false, VK_FORMAT_R32_SFLOAT);
    const auto motion_vk = addon::vk::ImageResource(motion_view, motion, width, height, false,
                                                    VK_FORMAT_R16G16_SFLOAT);
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    addon::SetNrCreateParameters(&parameters, width, height, width, height, 0, false, 1);
    parameters.Set(NVSDK_NGX_Parameter_Color, const_cast<NVSDK_NGX_Resource_VK*>(&color_vk));
    parameters.Set(NVSDK_NGX_Parameter_Output, const_cast<NVSDK_NGX_Resource_VK*>(&output_vk));
    parameters.Set(NVSDK_NGX_Parameter_Depth, const_cast<NVSDK_NGX_Resource_VK*>(&depth_vk));
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, const_cast<NVSDK_NGX_Resource_VK*>(&motion_vk));
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    addon::vk::SetControlMaskParameters(&parameters, nullptr, 0, 0, 0);
    parameters.Set("DLSSNR.Color", const_cast<NVSDK_NGX_Resource_VK*>(&color_vk));
    parameters.Set("DLSSNR.Output", const_cast<NVSDK_NGX_Resource_VK*>(&output_vk));
    parameters.Set("DLSSNR.Depth", const_cast<NVSDK_NGX_Resource_VK*>(&depth_vk));
    parameters.Set("DLSSNR.MVec", const_cast<NVSDK_NGX_Resource_VK*>(&motion_vk));
    for (const auto& [key, value] : std::initializer_list<std::pair<const char*, uint32_t>>{
             {NVSDK_NGX_Parameter_Width, width},
             {NVSDK_NGX_Parameter_Height, height},
             {NVSDK_NGX_Parameter_OutWidth, width},
             {NVSDK_NGX_Parameter_OutHeight, height},
             {"DLSSNR.ColorSubrectBaseX", 0},
             {"DLSSNR.ColorSubrectBaseY", 0},
             {"DLSSNR.ColorSubrectWidth", width},
             {"DLSSNR.ColorSubrectHeight", height},
             {"DLSSNR.DepthSubrectBaseX", 0},
             {"DLSSNR.DepthSubrectBaseY", 0},
             {"DLSSNR.DepthSubrectWidth", width},
             {"DLSSNR.DepthSubrectHeight", height},
             {"DLSSNR.MVecSubrectBaseX", 0},
             {"DLSSNR.MVecSubrectBaseY", 0},
             {"DLSSNR.MVecSubrectWidth", width},
             {"DLSSNR.MVecSubrectHeight", height},
             {"DLSSNR.OutputSubrectBaseX", 0},
             {"DLSSNR.OutputSubrectBaseY", 0},
             {"DLSSNR.OutputSubrectWidth", width},
             {"DLSSNR.OutputSubrectHeight", height},
             {"DLSSNR.InputWidth", width},
             {"DLSSNR.InputHeight", height},
             {"DLSSNR.OutputWidth", width},
             {"DLSSNR.OutputHeight", height},
         }) {
      parameters.Set(key, value);
    }
    parameters.Set("DLSSNR.ScalingRatio", 1.f);
    parameters.Set("DLSSNR.Scale", 1.f);
    parameters.Set("DLSSNR.MVecScaleX", 1.f);
    parameters.Set("DLSSNR.MVecScaleY", 1.f);

    addon::PreloadNrRuntime(nullptr);
    const auto runtime_status = addon::vk::EnsureRuntime(context.device);
    if (runtime_status != renodx::addons::dlss5::NrDeclineReason::kCount) {
      throw std::runtime_error(
          "evaluate-only Runtime Init failed decline="
          + std::to_string(static_cast<int>(runtime_status))
          + " last=0x" + std::to_string(addon::last_result)
          + " module=" + std::to_string(addon::direct_api.module != nullptr)
          + " init=" + std::to_string(addon::direct_api.init != nullptr));
    }
    addon::vk::CheckExtensions();
    Check(addon::vk::extension_state.load(std::memory_order_relaxed) != 2,
          "evaluate-only Runtime extension requirements are enabled");

    Begin(context);
    const auto create_result = addon::vk::runtime.create(
        context.device, context.command_buffer, addon::kFeatureDlssNr, &parameters, &handle);
    Check(NVSDK_NGX_SUCCEED(create_result) && handle != nullptr,
          "evaluate-only Feature 18 Create succeeds");
    SubmitAndWait(context);

    Begin(context);
    const reshade::api::resource output_resource[] = {output};
    const reshade::api::resource_usage output_before[] = {
        reshade::api::resource_usage::shader_resource};
    const reshade::api::resource_usage output_write[] = {
        reshade::api::resource_usage::unordered_access};
    command_list.barrier(1, output_resource, output_before, output_write);
    addon::NrFeatureSlot model_slot{};
    model_slot.parameters = &parameters;
    model_slot.nr_preset = 1;
    model_slot.pending_reset = true;
    addon::SetModelParameters(model_slot, 0,
                              addon::history_reset_epoch.load(std::memory_order_relaxed),
                              0, 1, false);
    const auto evaluate_result = addon::vk::runtime.evaluate(
        context.command_buffer, handle, &parameters, nullptr);
    if (NVSDK_NGX_FAILED(evaluate_result)) {
      throw std::runtime_error(
          "evaluate-only Feature 18 Evaluate failed result="
          + std::to_string(static_cast<uint32_t>(evaluate_result)));
    }
    SubmitAndWait(context);
    std::cout << "PASS NATIVE_NGX: Feature 18 Evaluate-only command buffer completed"
                 " without production codec or ControlMask dispatch\n";
    cleanup();
  } catch (...) {
    cleanup();
    throw;
  }
}

void RunProductionMaskDispatchMock() {
  vk_test::VulkanContext context;
  vk_test::Device device(&context);
  vk_test::CommandList command_list(&context, &device);
  constexpr uint32_t width = 16;
  constexpr uint32_t height = 16;
  constexpr uint32_t depth_base_x = 3;
  constexpr uint32_t depth_base_y = 6;
  constexpr uint32_t depth_width = 8;
  constexpr uint32_t depth_height = 8;

  reshade::api::resource depth{};
  reshade::api::resource mask{};
  reshade::api::resource_view depth_view{};
  reshade::api::resource_view mask_view{};
  const auto usages = reshade::api::resource_usage::shader_resource
      | reshade::api::resource_usage::unordered_access
      | reshade::api::resource_usage::copy_dest
      | reshade::api::resource_usage::copy_source;
  Check(device.create_resource({16, 16, 1, 1, reshade::api::format::r32_float, 1,
                                reshade::api::memory_heap::gpu_only, usages},
                               nullptr, reshade::api::resource_usage::undefined, &depth),
        "create depth image");
  Check(device.create_resource({width, height, 1, 1, reshade::api::format::r8g8b8a8_unorm, 1,
                                reshade::api::memory_heap::gpu_only, usages},
                               nullptr, reshade::api::resource_usage::undefined, &mask),
        "create RGBA8 mask image");
  Check(device.create_resource_view(depth, reshade::api::resource_usage::undefined,
                                   reshade::api::resource_view_desc(reshade::api::format::r32_float),
                                   &depth_view), "create depth image view");
  Check(device.create_resource_view(mask, reshade::api::resource_usage::undefined,
                                   reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm),
                                   &mask_view), "create mask image view");
  Check(addon::vk::EnsureControlMaskPipeline(&device), "create production ControlMask pipeline");

  std::array<float, 16 * 16> depths{};
  for (uint32_t y = 0; y < depth_height; ++y) {
    for (uint32_t x = 0; x < depth_width; ++x) {
      depths[(depth_base_y + y) * 16 + depth_base_x + x] =
          x < 2 ? 0.5f : (x < 5 ? 0.8f : 1.0f);
    }
  }
  Buffer upload = CreateHostBuffer(context, sizeof(depths), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  Buffer readback = CreateHostBuffer(context, width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  void* mapped = nullptr;
  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, upload.memory, 0, upload.size, 0, &mapped), "map depth upload");
  std::memcpy(mapped, depths.data(), sizeof(depths));
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, upload.memory);

  Begin(context);
  const reshade::api::resource depth_resource[] = {depth};
  const reshade::api::resource_usage depth_before[] = {reshade::api::resource_usage::undefined};
  const reshade::api::resource_usage depth_copy[] = {reshade::api::resource_usage::copy_dest};
  command_list.barrier(1, depth_resource, depth_before, depth_copy);
  const VkBufferImageCopy upload_region{0, 0, 0,
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {16, 16, 1}};
  context.DeviceProc<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
      context.command_buffer, upload.buffer, vk_test::NativeHandle<VkImage>(depth.handle),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
  const reshade::api::resource_usage depth_after[] = {reshade::api::resource_usage::shader_resource};
  command_list.barrier(1, depth_resource, depth_copy, depth_after);
  const reshade::api::resource mask_resource[] = {mask};
  const reshade::api::resource_usage mask_before[] = {reshade::api::resource_usage::undefined};
  const reshade::api::resource_usage mask_write[] = {reshade::api::resource_usage::unordered_access};
  command_list.barrier(1, mask_resource, mask_before, mask_write);

  const auto depth_vk = MakeImageResource(vk_test::NativeHandle<VkImage>(depth.handle),
      vk_test::NativeHandle<VkImageView>(depth_view.handle), 16, 16, VK_FORMAT_R32_SFLOAT, false);
  const addon::vk::ControlMaskConstants constants{
      width, height, addon::kControlMaskDepthFar, width / 2, height / 2,
      0.75f, 0.25f, 0.5f, depth_base_x, depth_base_y, depth_width, depth_height,
      0, 0.7f, 0.9f, 0};
  addon::vk::RecordControlMaskDispatch(&command_list, &depth_vk, depth_view, mask_view, constants);
  const reshade::api::resource_usage mask_read[] = {reshade::api::resource_usage::shader_resource};
  command_list.barrier(1, mask_resource, mask_write, mask_read);
  const reshade::api::resource_usage mask_copy[] = {reshade::api::resource_usage::copy_source};
  command_list.barrier(1, mask_resource, mask_read, mask_copy);
  const VkBufferImageCopy read_region{0, 0, 0,
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {width, height, 1}};
  context.DeviceProc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
      context.command_buffer, vk_test::NativeHandle<VkImage>(mask.handle),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &read_region);

  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  addon::vk::SetControlMaskParameters(&parameters, nullptr, 0, 0, 0);
  auto mask_resource_vk = addon::vk::ImageResource(mask_view, mask, width, height, false,
                                                   VK_FORMAT_R8G8B8A8_UNORM);
  addon::vk::SetControlMaskParameters(&parameters, &mask_resource_vk, width, height, 3);
  void* bound = nullptr;
  Check(NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.ControlMask", &bound)) && bound == &mask_resource_vk,
        "production parameter block binds generated Vulkan image to Feature 18 Evaluate");
  Check(addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectWidth") == width
            && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectHeight") == height
            && addon::GetUInt(&parameters, "DLSSNR.MaskBlendMode") == 3,
        "production parameter block carries full mask subrect and blend mode");

  constexpr uint64_t use_id = 0xC0A71001;
  addon::vk::TrackRecordedUse(&command_list, use_id);
  Check(!addon::vk::RecordedUseComplete(use_id), "recorded mask use remains live before GPU submission");
  SubmitAndWait(context);
  Check(!addon::vk::RecordedUseComplete(use_id),
        "completed Vulkan submission remains replayable until command-buffer reset");

  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, readback.memory, 0, readback.size, 0, &mapped), "map mask readback");
  const auto* pixels = static_cast<const uint8_t*>(mapped);
  const auto channel = [&](uint32_t x, uint32_t y, uint32_t c) {
    return pixels[(y * width + x) * 4 + c];
  };
  Check(channel(0, 0, 0) >= 190 && channel(0, 0, 0) <= 192,
        "production Vulkan shader preserves configured near-depth red weight");
  Check(channel(6, 0, 0) > 90 && channel(6, 0, 0) < 100,
        "production Vulkan shader produces the soft depth-far transition");
  Check(channel(14, 0, 0) == 0, "production Vulkan shader fades far-depth red weight to zero");
  if (channel(6, 0, 1) != 64 || channel(6, 0, 2) < 127 || channel(6, 0, 2) > 128
      || channel(6, 0, 3) != 255) {
    throw std::runtime_error("production Vulkan mask G/B/A readback was "
        + std::to_string(channel(6, 0, 1)) + "/" + std::to_string(channel(6, 0, 2))
        + "/" + std::to_string(channel(6, 0, 3)));
  }
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, readback.memory);

  SubmitRecordedAndWait(context);
  Check(!addon::vk::RecordedUseComplete(use_id),
        "replayed production Vulkan mask use remains live until reset");
  Begin(context);
  addon::vk::OnResetCommandList(&command_list);
  Check(addon::vk::RecordedUseComplete(use_id),
        "production reset retirement releases completed mask use");

  constexpr uint64_t discarded_use = 0xC0A71002;
  Begin(context);
  addon::vk::OnResetCommandList(&command_list);
  addon::vk::TrackRecordedUse(&command_list, discarded_use);
  Check(!addon::vk::RecordedUseComplete(discarded_use), "unsubmitted recording retains its mask use");
  Begin(context);
  addon::vk::OnResetCommandList(&command_list);
  Check(addon::vk::RecordedUseComplete(discarded_use), "discarded recording retires mask use on reset");

  DestroyBuffer(context, &upload);
  DestroyBuffer(context, &readback);
  device.destroy_resource_view(mask_view);
  device.destroy_resource_view(depth_view);
  device.destroy_resource(mask);
  device.destroy_resource(depth);
  device.destroy_pipeline(addon::vk::control_mask_program);
  device.destroy_pipeline_layout(addon::vk::control_mask_layout);
  addon::vk::control_mask_program = {};
  addon::vk::control_mask_layout = {};
  std::cout << "PASS NATIVE_GPU+MOCK: production Vulkan ControlMask pipeline, depth subrect,"
               " Evaluate binding, submission replay, and reset/discard retirement\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    addon::addon_module = GetModuleHandleW(nullptr);
    if (argc > 1 && std::wstring(argv[1]) == L"--native-ngx") {
      RunNativeNgx();
    } else if (argc > 1 && std::wstring(argv[1]) == L"--native-ngx-evaluate-only") {
      RunNativeNgxEvaluateOnly();
    } else {
      RunProductionMaskDispatchMock();
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL Vulkan ControlMask: " << error.what() << '\n';
    return 1;
  }
}

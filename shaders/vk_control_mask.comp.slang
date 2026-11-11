// Standalone ControlMask layout: 28 DWORDs, depth at 0, motion at 1, output at 4.
#define RENODX_VULKAN 1
#include "vk_binding.hlsli"

VK_BINDING(0) Texture2D<float> input_depth;
VK_BINDING(1) Texture2D<float2> input_motion;
VK_BINDING(4) RWTexture2D<float4> output_mask;

PUSH_CONSTANTS(ControlMaskConstants) {
  uint width;
  uint height;
  uint mode;
  uint rect_width;
  uint rect_height;
  float red;
  float green;
  float blue;
  uint depth_base_x;
  uint depth_base_y;
  uint depth_width;
  uint depth_height;
  uint depth_inverted;
  float depth_near;
  float depth_far;
  uint motion_enabled;
  uint motion_base_x;
  uint motion_base_y;
  uint motion_width;
  uint motion_height;
  float motion_scale_x;
  float motion_scale_y;
  float motion_start;
  float motion_end;
  uint4 padding;
};

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  if (dispatch_id.x >= width || dispatch_id.y >= height) return;

  float4 value = float4(red, green, blue, 1.0);
  float depth_keep = 1.0;
  float motion_keep = 1.0;
  if (mode == 1) {
    value = 1.0;
  } else if (mode == 3 || mode == 4) {
    value = 1.0;
    if (dispatch_id.x < width / 2 && dispatch_id.y < height / 2) {
      if (mode == 3) {
        value.g = 0.0;
      } else {
        value.b = 0.0;
      }
    }
  } else if (mode == 2 && dispatch_id.x < rect_width && dispatch_id.y < rect_height) {
    value = float4(0.0, 0.0, 0.0, 1.0);
  } else if (mode == 5 && depth_width != 0 && depth_height != 0 && depth_far > depth_near) {
    const uint depth_x = depth_base_x + min(
        (uint)(((float)dispatch_id.x + 0.5) * (float)depth_width / (float)width), depth_width - 1);
    const uint depth_y = depth_base_y + min(
        (uint)(((float)dispatch_id.y + 0.5) * (float)depth_height / (float)height), depth_height - 1);
    float depth_value = input_depth.Load(int3(depth_x, depth_y, 0));
    if (depth_inverted != 0) {
      depth_value = 1.0 - depth_value;
    }
    if (depth_value >= 0.0 && depth_value <= 1.0) {
      depth_keep = 1.0 - smoothstep(depth_near, depth_far, depth_value);
    }
  }
  if (motion_enabled != 0 && motion_width != 0 && motion_height != 0 && motion_end > motion_start) {
    const uint motion_x = motion_base_x + min(
        (uint)(((float)dispatch_id.x + 0.5) * (float)motion_width / (float)width), motion_width - 1);
    const uint motion_y = motion_base_y + min(
        (uint)(((float)dispatch_id.y + 0.5) * (float)motion_height / (float)height), motion_height - 1);
    const float2 motion_vector = input_motion.Load(int3(motion_x, motion_y, 0));
    const float speed = length(motion_vector * float2(motion_scale_x, motion_scale_y));
    if (all(isfinite(motion_vector)) && isfinite(speed)) {
      motion_keep = 1.0 - smoothstep(motion_start, motion_end, speed);
    }
  }
  value.r *= min(depth_keep, motion_keep);
  output_mask[dispatch_id.xy] = value;
}

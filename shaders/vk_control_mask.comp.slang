// Vulkan ControlMask generator (v8.1). The standalone layout mirrors the
// D3D12 shader's 16 DWORD constants, with depth at binding 0 and mask output
// at binding 4 so the optional pipeline cannot affect the codec layout.
#define RENODX_VULKAN 1
#include "vk_binding.hlsli"

VK_BINDING(0) Texture2D<float> input_depth;
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
  uint padding;
};

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  if (dispatch_id.x >= width || dispatch_id.y >= height) return;

  float4 value = float4(red, green, blue, 1.0);
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
      value.r = red * (1.0 - smoothstep(depth_near, depth_far, depth_value));
    }
  }
  output_mask[dispatch_id.xy] = value;
}

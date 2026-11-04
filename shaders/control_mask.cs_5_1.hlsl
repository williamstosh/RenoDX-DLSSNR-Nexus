cbuffer Constants : register(b0) {
  uint width;
  uint height;
  uint mode;
  uint rect_width;
  uint rect_height;
  float red;
  float green;
  float blue;
};

RWTexture2D<float4> output_mask : register(u0);

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
  }
  output_mask[dispatch_id.xy] = value;
}

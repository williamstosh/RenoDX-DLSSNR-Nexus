#include "v6_common.hlsli"

VK_BINDING(5) RWTexture2D<float4> BlockMean : register(u1);

#ifdef DLSS5_HDR_CAPTURE
#include "hdr_commit_capture.hlsli"
#endif

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const int2 block_max = int2((Size + 31u) / 32u) - int2(1, 1);
  const float2 block_pos = (float2(pixel) + 0.5) / 32.0 - 0.5;
  const int2 b0 = clamp(int2(floor(block_pos)), int2(0, 0), block_max);
  const int2 b1 = min(b0 + int2(1, 1), block_max);
  const float2 f = saturate(block_pos - float2(b0));
  const float4 m00 = BlockMean.Load((int2)b0);
  const float4 m10 = BlockMean.Load((int2)int2(b1.x, b0.y));
  const float4 m01 = BlockMean.Load((int2)int2(b0.x, b1.y));
  const float4 m11 = BlockMean.Load((int2)b1);
  const float final_mean =
      m00.x * (1.0 - f.x) * (1.0 - f.y) + m10.x * f.x * (1.0 - f.y)
      + m01.x * (1.0 - f.x) * f.y + m11.x * f.x * f.y;
  const float untouched_mean =
      m00.y * (1.0 - f.x) * (1.0 - f.y) + m10.y * f.x * (1.0 - f.y)
      + m01.y * (1.0 - f.x) * f.y + m11.y * f.x * f.y;
  // Bilinearly interpolated untouched block max. The legacy hard gate on
  // this value still steps at the cutoff; DarkGateMode 2 tapers inside the
  // admitted dark range so crossing the gate cannot add a pedestal seam.
  const float untouched_max =
      m00.z * (1.0 - f.x) * (1.0 - f.y) + m10.z * f.x * (1.0 - f.y)
      + m01.z * (1.0 - f.x) * f.y + m11.z * f.x * f.y;
  const float4 value = Original.Load(int3(pixel, 0));
  float3 result = value.rgb;
#ifdef DLSS5_HDR_CAPTURE
  const bool capture_inside_roi = all(pixel >= CaptureRoi.xy)
      && all(pixel - CaptureRoi.xy < CaptureRoi.zw);
  const uint2 capture_roi_pixel = pixel - CaptureRoi.xy;
  const uint2 capture_linear_pixel = (CaptureFullPlanes != 0u ? pixel : capture_roi_pixel);
  float capture_legacy_estimate = 0.0;
  float capture_applied = 0.0;
  float capture_clip_flags = 0.0;
#endif
#ifdef GATE_IN_INPUT_UNITS
  // Feed v2: the NR input scale (t2, the norm_scale texel) is the game's
  // exposure and can sit many stops from 1, so "near-black" and the cap are
  // stated for what NR sees and carried back to source units here.
  const float unit = max(Neural.Load(int3(0, 0, 0)).r, 1e-8);
#else
  const float unit = 1.0;
#endif
  // Gate on the block's brightest untouched pixel: a mean-only gate admits
  // mixed dark/bright blocks whose mean-lift estimate is contaminated by the
  // bright feature (the blobbing class; see v6_pedestal_reduce).
  const float weight = BlackRestoreWeight(untouched_max, DarkGate * unit, DarkGateMode == 2u);
#ifdef DLSS5_HDR_CAPTURE
  capture_legacy_estimate = clamp(final_mean - untouched_mean, 0.0, PedestalCap * unit)
      * TransferStrength * CaptureLegacyPedestalGate
      * BlackRestoreWeight(untouched_max, DarkGate * unit, CaptureLegacyDarkGateMode == 2u);
#endif
  if (weight > 0.0) {
    const float pedestal =
        clamp(final_mean - untouched_mean, 0.0, PedestalCap * unit)
        * TransferStrength * PedestalGate * weight;
#ifdef DLSS5_HDR_CAPTURE
    capture_applied = pedestal;
#endif
    if (pedestal > 0.0) {
      result -= pedestal;
      // A BT.2020 source's BT.709 negatives are real colours (Encoding 4):
      // its clamp waits for the conversion back below.
#ifdef DLSS5_HDR_CAPTURE
      if (Encoding != 4u && any(result < 0.0)) capture_clip_flags = 1.0;
#endif
      if (Encoding != 4u) result = max(result, 0.0);
    }
  }
#ifdef DLSS5_HDR_CAPTURE
  if (CaptureFullPlanes != 0u || capture_inside_roi) {
    CapturePostPedestal[capture_linear_pixel] = float4(result, value.a);
  }
#endif
  if (Encoding == 4u) result = Bt709ToBt2020(result);
#ifdef DLSS5_HDR_CAPTURE
  if (capture_inside_roi) {
    if (Encoding == 4u) CapturePostGamut[capture_roi_pixel] = float4(result, value.a);
    if ((Encoding == 2u || Encoding == 4u) && any(result < 0.0)) capture_clip_flags += 2.0;
    CapturePedestalField[capture_roi_pixel] =
        float4(capture_legacy_estimate, capture_applied, weight, capture_clip_flags);
  }
#endif
  if (Encoding == 2u || Encoding == 4u) {
#ifdef DLSS5_HDR_CAPTURE
    HDR_CAPTURE_ENCODED(float4(LinearToPQ(max(result, 0.0)), value.a));
#else
    Output[VK_OUTPUT_PIXEL(pixel)] = float4(LinearToPQ(max(result, 0.0)), value.a);
#endif
  } else if (Encoding == 3u) {
#ifdef DLSS5_HDR_CAPTURE
    HDR_CAPTURE_ENCODED(float4(SrgbEncodeExtended(result), value.a));
#else
    Output[VK_OUTPUT_PIXEL(pixel)] = float4(SrgbEncodeExtended(result), value.a);
#endif
  } else {
#ifdef DLSS5_HDR_CAPTURE
    HDR_CAPTURE_ENCODED(float4(result, value.a));
#else
    Output[VK_OUTPUT_PIXEL(pixel)] = float4(result, value.a);
#endif
  }
}

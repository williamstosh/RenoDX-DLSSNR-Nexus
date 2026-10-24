#ifndef DLSS5_HDR_COMMIT_CAPTURE_HLSLI
#define DLSS5_HDR_COMMIT_CAPTURE_HLSLI

RWTexture2D<float4> CapturePostPedestal : register(u2);
RWTexture2D<float4> CapturePostGamut : register(u3);
RWTexture2D<float4> CaptureEncoded : register(u4);
RWTexture2D<float4> CapturePedestalField : register(u5);
cbuffer HdrCaptureConstants : register(b1) {
  uint4 CaptureRoi;
  uint CaptureFullPlanes;
  float CaptureLegacyPedestalGate;
  uint CaptureLegacyDarkGateMode;
  uint CaptureReserved;
};

// CaptureFullPlanes selects full-size u2/u4 in CommitCore; u3/u5 always
// retain the full-frame grid origin while storing only the selected ROI.
#define HDR_CAPTURE_ENCODED(encoded_expression) \
  { \
    const float4 capture_encoded = (encoded_expression); \
    if (CaptureFullPlanes != 0u || capture_inside_roi) { \
      CaptureEncoded[capture_linear_pixel] = capture_encoded; \
    } \
    Output[VK_OUTPUT_PIXEL(pixel)] = capture_encoded; \
  }

#endif

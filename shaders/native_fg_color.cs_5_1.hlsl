// Preserve the final NR image. Distribute its edit into the auxiliary FG
// colors while keeping B = U + (1-a)H and valid premultiplied UI values.
// All values stay in the caller's normalized, encoded color domain.
Texture2D<float4> EnhancedBack : register(t0);
Texture2D<float4> OriginalBack : register(t1);
Texture2D<float4> OriginalHudless : register(t2);
Texture2D<float4> OriginalUi : register(t3);
RWTexture2D<float4> HudlessOutput : register(u0);
RWTexture2D<float4> UiOutput : register(u1);

cbuffer ColorConstants : register(b0) {
  uint2 Size;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  const float3 b = EnhancedBack.Load(int3(id.xy, 0)).rgb;
  const float3 original = OriginalBack.Load(int3(id.xy, 0)).rgb;
  const float4 h = OriginalHudless.Load(int3(id.xy, 0));
  const float a = saturate(OriginalUi.Load(int3(id.xy, 0)).a);
  float3 scene;
  float3 ui;
  if (a == 0.0) {
    scene = round(b * 65535.0) / 65535.0;
    ui = 0.0;
  } else {
    float3 lower = 0.0;
    float3 upper = 1.0;
    if (a < 1.0) {
      lower = max(0.0, (b - a) / (1.0 - a));
      upper = min(1.0, b / (1.0 - a));
    }
    // Choose representable scene codes before deriving UI. Clipping a UI
    // residual instead would change the final NR color at translucent UI.
    scene = clamp(round((h.rgb + b - original) * 65535.0),
                  ceil(lower * 65535.0), floor(upper * 65535.0)) / 65535.0;
    ui = b - (1.0 - a) * scene;
  }
  HudlessOutput[id.xy] = float4(scene, h.a);
  UiOutput[id.xy] = float4(ui, a);
}

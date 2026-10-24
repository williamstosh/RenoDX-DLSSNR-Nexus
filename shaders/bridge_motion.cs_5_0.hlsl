// RG32_FLOAT is not shareable on every D3D11 driver. Keep both components
// at float32 precision while expanding to a shareable four-channel twin.
Texture2D<float2> Motion : register(t0);
RWTexture2D<float4> Twin : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  Twin[id.xy] = float4(Motion.Load(int3(id.xy, 0)), 0.0, 0.0);
}

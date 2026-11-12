// X264Encoder's input conversion (x264_encoder.h): BGRA -> NV12, BT.709 limited range, drawn with
// scale.hlsl's fullscreen-triangle VS. Two passes: Y into an R8 target the size of the encode,
// interleaved CbCr into an R8G8 target half that size. The encode size is even (ResolveOutputSize);
// reads still clamp to the last texel.
//
// Chroma is the plain 2x2 average (centre-sited), not H.264's default left-siting: a quarter-texel
// shift in chroma only, invisible at recording bitrates. Built by fxc at build time.

cbuffer Nv12Params : register(b0) {
    int2 src_max;  // last valid texel of the source (width - 1, height - 1)
};

Texture2D<float4> src : register(t0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float3 Texel(int2 p) {
    return src.Load(int3(min(p, src_max), 0)).rgb;
}

float PSLuma(float4 pos : SV_Position) : SV_Target {
    const float y = dot(Texel(int2(pos.xy)), kLuma);
    return (16.0 + 219.0 * y) / 255.0;
}

float2 PSChroma(float4 pos : SV_Position) : SV_Target {
    const int2   p   = int2(pos.xy) * 2;
    const float3 rgb = 0.25 * (Texel(p) + Texel(p + int2(1, 0)) + Texel(p + int2(0, 1)) + Texel(p + int2(1, 1)));
    const float  y   = dot(rgb, kLuma);
    const float  cb  = (rgb.b - y) / 1.8556;
    const float  cr  = (rgb.r - y) / 1.5748;
    return (128.0 + 224.0 * float2(cb, cr)) / 255.0;
}

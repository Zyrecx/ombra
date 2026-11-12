// FrameScaler's downscale (frame_scaler.h): one fullscreen triangle, area filter.
//
// Each output pixel is the exact average of the source texels its footprint covers, weighted by
// the overlap -- OBS's "Area" scale filter (libobs/data/area.effect, 30.2.3), with the footprint
// taken from SV_Position and the constant buffer rather than ddx/ddy. A box over the footprint
// doesn't alias at any ratio, so there is no separate below-half-size path (OBS switches to
// bilinear_lowres there because its bicubic samples a fixed 4x4 in source space).
//
// Built by fxc at build time (CMakeLists.txt), so the engine never loads d3dcompiler.

cbuffer ScaleParams : register(b0) {
    float2 src_size;  // source texture size in texels
    float2 ratio;     // source texels per output pixel, per axis (> 1 when downscaling)
};

Texture2D<float4> src : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    // (-1,1), (3,1), (-1,-3): clockwise, covers the whole viewport.
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    // pos.xy is the output pixel's centre (x + 0.5); its footprint in source texel units:
    const float2 lo = (pos.xy - 0.5) * ratio;
    const float2 hi = min((pos.xy + 0.5) * ratio, src_size);
    const int2   first = int2(floor(lo));
    const int2   last  = int2(ceil(hi)) - 1;

    float4 total = 0.0;
    [loop] for (int y = first.y; y <= last.y; ++y) {
        const float h = min(hi.y, y + 1.0) - max(lo.y, (float)y);
        [loop] for (int x = first.x; x <= last.x; ++x) {
            const float w = min(hi.x, x + 1.0) - max(lo.x, (float)x);
            total += src.Load(int3(x, y, 0)) * (w * h);
        }
    }
    return total / ((hi.x - lo.x) * (hi.y - lo.y));
}

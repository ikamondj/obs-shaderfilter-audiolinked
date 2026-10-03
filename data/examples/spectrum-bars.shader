// Select an Audio source in the filter properties.
// This declaration opts into the shared FFT and enables spectrum().
uniform texture2d spec;
uniform float falloff <
    string label = "Spectrum falloff";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.01;
> = 0.0;

float4 mainImage(VertData v_in) : TARGET
{
    float x = saturate(v_in.uv.x);
    float band = min(floor(x * 64.0), 63.0);
    // 0 is instant (same as omitting falloff); 1 is the long window.
    float level = spectrum((band + 0.5) / 64.0, falloff);
    float gap = step(0.06, frac(x * 64.0)) * (1.0 - step(0.94, frac(x * 64.0)));
    float bar = step(1.0 - v_in.uv.y, level) * gap;
    float3 tint = lerp(float3(0.1, 0.8, 1.0), float3(1.0, 0.2, 0.5), x);
    return float4(lerp(float3(0.015, 0.02, 0.03), tint, bar), 1.0);
}

// Controller slot 0: A cycles noise modes; left-stick X integrates hue.
// Still animates without a controller. All four noise coordinates are continuous.
float4 mainImage(VertData v_in) : TARGET
{
    float phase = gamepad_axis_interval(0, 0, 360.0);
    float4 p = float4(v_in.uv * 6.0, elapsed_time * 0.07, elapsed_time * 0.13);
    int mode = gamepad_press_count(0, 0) % 4;
    float n = simplex_noise(p);
    if (mode == 1) n = perlin_noise_fbm(p, 4, 1.0, 2.0, 0.5);
    else if (mode == 2) n = 2.0 * voronoi_noise(p) - 1.0;
    else if (mode == 3) n = curl_noise(p).x * 0.25;
    float3 color = hsl2rgb(float3(frac(phase + elapsed_time * 0.02), 0.75, saturate(0.5 + n * 0.5)));
    return float4(color, 1.0);
}

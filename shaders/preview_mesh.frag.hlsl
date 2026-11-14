struct PSIn {
    float4 clip_pos : SV_Position;
    [[vk::location(0)]] float3 world_normal : NORMAL0;
    [[vk::location(1)]] float3 world_proxy : NORMAL1;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 color : COLOR0;
    [[vk::location(4)]] float3 world_pos : TEXCOORD1;
};

[[vk::binding(0, 0)]] Texture2D<float4> uBaseTex;
[[vk::binding(1, 0)]] SamplerState uLinearSampler;

[[vk::push_constant]]
cbuffer PreviewPush {
    float4x4 mvp;
    float4 base_color;
    float4 shade_color;
    float4 rim_and_shift;
    float4 params;
};

float4 main(PSIn input) : SV_Target0 {
    float4 tex_col = uBaseTex.Sample(uLinearSampler, input.uv0);
    float4 lit_base = tex_col * base_color * input.color;

    if (params.z > 0.5 && lit_base.a < params.y) {
        discard;
    }

    if (params.w > 0.5) {
        return float4(lit_base.rgb, 1.0);
    }

    // Two-tone MToon preview shading using baked proxy_normal + studio key light
    float3 N = normalize(dot(input.world_proxy, input.world_proxy) > 0.001 ? input.world_proxy : input.world_normal);
    float3 L = normalize(float3(0.45, 0.72, 0.52));
    float ndl = dot(N, L);

    float toony = clamp(params.x, 0.05, 0.99);
    float feather = max(0.02, 1.0 - toony);
    float shift = rim_and_shift.w;
    float lit_w = smoothstep(shift - feather, shift + feather, ndl);

    float3 shade_tint = lit_base.rgb * shade_color.rgb;
    float3 toon_rgb = lerp(shade_tint, lit_base.rgb, lit_w);

    // Subtle rim highlight along true geometric normal
    float3 V = normalize(float3(0.0, 0.25, 1.0));
    float fresnel = pow(saturate(1.0 - abs(dot(normalize(input.world_normal), V))), 4.0);
    toon_rgb += rim_and_shift.rgb * (fresnel * 0.22);

    return float4(toon_rgb, lit_base.a);
}

struct VSIn {
    [[vk::location(0)]] float3 pos : POSITION0;
    [[vk::location(1)]] float3 true_normal : NORMAL0;
    [[vk::location(2)]] float3 proxy_normal : NORMAL1;
    [[vk::location(4)]] float2 uv0 : TEXCOORD0;
    [[vk::location(5)]] float4 color : COLOR0;
};

struct VSOut {
    float4 clip_pos : SV_Position;
    [[vk::location(0)]] float3 world_normal : NORMAL0;
    [[vk::location(1)]] float3 world_proxy : NORMAL1;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 color : COLOR0;
    [[vk::location(4)]] float3 world_pos : TEXCOORD1;
};

[[vk::push_constant]]
cbuffer PreviewPush {
    float4x4 mvp;
    float4 base_color;
    float4 shade_color;
    float4 rim_and_shift;
    float4 params;
};

VSOut main(VSIn input) {
    VSOut o;
    o.clip_pos = mul(mvp, float4(input.pos, 1.0));
    o.world_pos = input.pos;
    o.world_normal = input.true_normal;
    o.world_proxy = input.proxy_normal;
    o.uv0 = input.uv0;
    o.color = input.color;
    return o;
}

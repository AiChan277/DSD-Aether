struct BoneVSIn {
    [[vk::location(0)]] float3 pos : POSITION0;
    [[vk::location(1)]] float4 color : COLOR0;
};

struct BoneVSOut {
    float4 clip_pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

[[vk::push_constant]]
cbuffer BonePush {
    float4x4 mvp;
};

BoneVSOut main(BoneVSIn input) {
    BoneVSOut o;
    o.clip_pos = mul(mvp, float4(input.pos, 1.0));
    o.color = input.color;
    return o;
}

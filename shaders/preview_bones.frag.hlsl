struct BonePSIn {
    float4 clip_pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

float4 main(BonePSIn input) : SV_Target0 {
    return input.color;
}

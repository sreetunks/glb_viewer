struct ROOT_CONSTANTS
{
    float4x4 world;
    float4x4 view_proj;
};
[[vk::push_constant]] ROOT_CONSTANTS root_constants;
struct VS_INPUT
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
};
struct VS_OUTPUT
{
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
};
VS_OUTPUT vert_main(VS_INPUT input)
{
    VS_OUTPUT output = (VS_OUTPUT)0;
    output.pos= mul(root_constants.view_proj, mul(root_constants.world, float4(input.pos, 1.0)));
    output.nrm = input.nrm;
    return output;
}
struct PS_INPUT
{
    float3 nrm : NORMAL;
};
float4 frag_main(PS_INPUT input) : SV_Target
{
    float3 light_dir = float3(0.0, 1.0, -1.0);
    float light_ratio = max(dot(input.nrm, normalize(light_dir)), 0.2);
    return float4(1.0, 0.0, 1.0, 1.0)* light_ratio;
}

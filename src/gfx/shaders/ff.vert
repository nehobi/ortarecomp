#version 450
// Fixed-function vertex stage. Positions arrive already in clip space
// (transformed on the CPU from the game's matrices or screen coordinates);
// colours arrive as D3DCOLOR bytes (B, G, R, A in memory).

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_diffuse;
layout(location = 2) in vec4 in_specular;
layout(location = 3) in vec4 in_tc0;
layout(location = 4) in vec4 in_tc1;
layout(location = 5) in vec4 in_tc2;
layout(location = 6) in vec4 in_tc3;
layout(location = 7) in float in_fog;

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) out vec4 v_tc0;
layout(location = 3) out vec4 v_tc1;
layout(location = 4) out vec4 v_tc2;
layout(location = 5) out vec4 v_tc3;
layout(location = 6) out float v_fog;

void main()
{
    gl_Position = in_pos;
    v_diffuse = in_diffuse.bgra;
    v_specular = in_specular.bgra;
    v_tc0 = in_tc0;
    v_tc1 = in_tc1;
    v_tc2 = in_tc2;
    v_tc3 = in_tc3;
    v_fog = in_fog;
}

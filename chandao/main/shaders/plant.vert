#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vPos;

void main()
{
    vColor = inColor;
    vPos = inPos;
    gl_Position = vec4(inPos, 0.0, 1.0);
}

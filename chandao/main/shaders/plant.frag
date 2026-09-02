#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vPos;

layout(location = 0) out vec4 outColor;

void main()
{
    if (dot(vPos, vPos) > 1.0) {
        discard;
    }
    outColor = vColor;
}

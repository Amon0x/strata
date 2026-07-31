#version 330

layout(std140) uniform DynamicTransforms {
    mat4 ModelViewMat;
    vec4 ColorModulator;
    vec3 ModelOffset;
    mat4 TextureMat;
};
layout(std140) uniform StrataMaterial {
    vec4 Data0;
    vec4 Data1;
    vec4 Data2;
    vec4 Data3;
};
uniform sampler2D Sampler0;

in vec2 texCoord0;
in vec4 vertexColor;
out vec4 fragColor;

void main() {
    vec4 color = texture(Sampler0, texCoord0) * vertexColor;
    if (color.a == 0.0) {
        discard;
    }
    fragColor = color * ColorModulator;
}

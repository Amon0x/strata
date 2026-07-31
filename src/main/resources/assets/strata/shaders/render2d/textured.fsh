#version 330

layout(std140) uniform DynamicTransforms {
    mat4 ModelViewMat;
    vec4 ColorModulator;
    vec3 ModelOffset;
    mat4 TextureMat;
};
uniform sampler2D Sampler0;

in vec2 texCoord0;
in vec4 vertexColor;
in vec4 drawData0;
in vec4 drawData1;
in vec4 drawData2;
in vec4 drawData3;

out vec4 fragColor;

void main() {
    vec4 color = texture(Sampler0, texCoord0) * vertexColor;
    color.a *= drawData3.w;
    if (color.a == 0.0) {
        discard;
    }
    fragColor = color * ColorModulator;
}

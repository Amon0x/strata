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
    vec2 texel = Data0.yz;
    const int maxRadius = 32;
    float radius = clamp(round(max(Data0.x, 1.0)), 1.0, float(maxRadius));
    vec4 color = vec4(0.0);
    float weight = 0.0;
    for (int sampleOffset = -maxRadius; sampleOffset <= maxRadius; sampleOffset++) {
        float offset = float(sampleOffset);
        if (abs(offset) > radius) {
            continue;
        }
        color += texture(Sampler0, texCoord0 + vec2(0.0, texel.y * offset));
        weight += 1.0;
    }
    fragColor = (color / max(weight, 1.0)) * vertexColor * ColorModulator;
}

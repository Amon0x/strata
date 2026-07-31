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

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 sampleColor = texture(Sampler0, texCoord0).rgb;
    float signedDistance = median(sampleColor.r, sampleColor.g, sampleColor.b) - 0.5;
    vec2 textureSizePixels = vec2(textureSize(Sampler0, 0));
    vec2 unitRange = vec2(max(drawData0.x, 0.0001)) / max(textureSizePixels, vec2(1.0));
    vec2 screenTexSize = vec2(1.0) / max(fwidth(texCoord0), vec2(0.000001));
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
    float alpha = clamp(signedDistance * screenPxRange + 0.5, 0.0, 1.0);
    vec4 color = vertexColor;
    color.a *= alpha * drawData3.w;
    if (color.a == 0.0) {
        discard;
    }
    fragColor = color * ColorModulator;
}

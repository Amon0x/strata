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

float roundedBoxSdf(vec2 p, vec2 halfSize, vec4 radii) {
    vec2 quadrant = step(vec2(0.0), p);
    float radius = mix(
        mix(radii.x, radii.w, quadrant.y),
        mix(radii.y, radii.z, quadrant.y),
        quadrant.x
    );
    vec2 q = abs(p) - halfSize + vec2(radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float median3(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    int mode = int(floor(drawData3.z + 0.5));
    vec4 color = vertexColor;

    if (mode == 1) {
        color *= texture(Sampler0, texCoord0);
    } else if (mode == 2) {
        vec2 size = max(drawData0.xy, vec2(1.0));
        vec4 radii = max(drawData1, vec4(0.0));
        float softness = max(drawData0.z, 0.5);
        float borderWidth = max(drawData0.w, 0.0);
        vec2 p = (texCoord0 - vec2(0.5)) * size;
        float distance = roundedBoxSdf(p, size * 0.5, radii);
        float alpha = 1.0 - smoothstep(-softness, softness, distance);
        float borderMix = 1.0 - smoothstep(borderWidth - softness, borderWidth + softness, -distance);
        color = mix(color, drawData2, clamp(borderMix * step(0.001, borderWidth), 0.0, 1.0));
        color.a *= alpha;
    } else if (mode == 3) {
        vec2 size = max(drawData0.xy, vec2(1.0));
        vec4 radii = max(drawData1, vec4(0.0));
        float width = max(drawData0.z, 0.0);
        float softness = max(drawData0.w, 0.5);
        vec2 p = (texCoord0 - vec2(0.5)) * size;
        float distance = roundedBoxSdf(p, size * 0.5, radii);
        float outer = 1.0 - smoothstep(-softness, softness, distance);
        float inner = 1.0 - smoothstep(-softness, softness, distance + width);
        color.a *= clamp(outer - inner, 0.0, 1.0);
    } else if (mode == 4) {
        color.a *= texture(Sampler0, texCoord0).r;
    } else if (mode == 5) {
        vec3 sampleColor = texture(Sampler0, texCoord0).rgb;
        float signedDistance = median3(sampleColor.r, sampleColor.g, sampleColor.b) - 0.5;
        vec2 textureSizePixels = vec2(textureSize(Sampler0, 0));
        vec2 unitRange = vec2(max(drawData0.x, 0.0001)) / max(textureSizePixels, vec2(1.0));
        vec2 screenTexSize = vec2(1.0) / max(fwidth(texCoord0), vec2(0.000001));
        float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
        color.a *= clamp(signedDistance * screenPxRange + 0.5, 0.0, 1.0);
    } else if (mode == 6) {
        vec2 shapeSize = max(drawData0.xy, vec2(1.0));
        vec2 quadSize = max(drawData2.xy, shapeSize);
        float blurRadius = max(drawData0.z, 0.5);
        float spread = drawData0.w;
        vec4 radii = max(drawData1, vec4(0.0));
        vec2 p = (texCoord0 - vec2(0.5)) * quadSize;
        float distance = roundedBoxSdf(p, shapeSize * 0.5, radii);
        float outside = smoothstep(-1.0, 1.0, distance);
        float falloff = 1.0 - smoothstep(
            0.0,
            blurRadius,
            max(distance - spread, 0.0)
        );
        color.a *= outside * falloff;
    }

    color.a *= drawData3.w;
    if (color.a == 0.0) {
        discard;
    }
    fragColor = color * ColorModulator;
}

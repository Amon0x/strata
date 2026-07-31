#version 330

layout(std140) uniform DynamicTransforms {
    mat4 ModelViewMat;
    vec4 ColorModulator;
    vec3 ModelOffset;
    mat4 TextureMat;
};
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

void main() {
    vec2 size = max(drawData0.xy, vec2(1.0));
    float blurRadius = max(drawData0.z, 0.5);
    float spread = max(drawData0.w, 0.0);
    vec4 radii = max(drawData1 + vec4(spread), vec4(0.0));
    vec2 p = (texCoord0 - vec2(0.5)) * size;
    float distance = roundedBoxSdf(p, size * 0.5 - vec2(blurRadius), radii);
    float alpha = 1.0 - smoothstep(-blurRadius, blurRadius, distance);
    vec4 color = vertexColor;
    color.a *= alpha * drawData3.w;
    if (color.a == 0.0) {
        discard;
    }
    fragColor = color * ColorModulator;
}

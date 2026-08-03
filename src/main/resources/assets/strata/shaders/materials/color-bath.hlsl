float2 rotatePoint(float2 value, float angle) {
    float sine = sin(angle);
    float cosine = cos(angle);
    return float2(
        value.x * cosine - value.y * sine,
        value.x * sine + value.y * cosine
    );
}

float colorPool(float2 position, float2 center, float2 radius, float angle) {
    float2 local = rotatePoint(position - center, angle) / max(radius, 0.0001);
    return exp(-dot(local, local) * 1.65);
}

float hash21(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float silkFold(float value, float width) {
    float distance = abs(value);
    float broad = exp(-distance * distance / (width * width));
    float edge = exp(-distance * distance / (width * width * 0.075));
    return broad * 0.56 + edge * 0.44;
}

float4 material(PixelInput input) {
    float2 size = max(materialSize(input), 1.0);
    float aspect = size.x / size.y;
    float2 uv = input.uv;
    float2 position = (uv - 0.5) * float2(aspect, 1.0);
    float time = materialTime();

    // Animate the coordinate field itself so even the space between color pools keeps moving.
    float2 flow = position;
    flow.x += sin(position.y * 2.7 + time * 0.83) * 0.18;
    flow.y += sin(position.x * 2.2 - time * 0.69) * 0.15;
    flow += float2(
        sin(position.y * 6.3 - time * 0.57),
        cos(position.x * 5.6 + time * 0.64)
    ) * 0.038;

    float cyan = colorPool(
        flow,
        float2(
            -0.34 * aspect + sin(time * 0.47) * aspect * 0.22,
            -0.17 + cos(time * 0.59) * 0.27
        ),
        float2(0.58, 0.40),
        -0.28 + sin(time * 0.31) * 0.35
    );
    float coral = colorPool(
        flow,
        float2(
            0.31 * aspect + cos(time * 0.41 + 1.3) * aspect * 0.24,
            -0.18 + sin(time * 0.53 + 0.8) * 0.29
        ),
        float2(0.55, 0.37),
        0.24 + cos(time * 0.29) * 0.38
    );
    float magenta = colorPool(
        flow,
        float2(
            -0.30 * aspect + cos(time * 0.37 + 2.8) * aspect * 0.27,
            0.24 + sin(time * 0.46 + 2.0) * 0.25
        ),
        float2(0.61, 0.41),
        0.31 + sin(time * 0.34 + 1.0) * 0.32
    );
    float violet = colorPool(
        flow,
        float2(
            0.30 * aspect + sin(time * 0.43 + 3.7) * aspect * 0.25,
            0.22 + cos(time * 0.49 + 2.4) * 0.28
        ),
        float2(0.64, 0.43),
        -0.34 + cos(time * 0.27 + 0.7) * 0.36
    );

    // The base current changes continuously across the whole surface, rather than leaving a
    // static dark plate behind moving blobs.
    float current = 0.5 + 0.5 * sin(
        flow.x * 1.18 - flow.y * 1.53 + time * 0.76 +
        sin(flow.y * 2.4 + time * 0.48) * 0.82
    );
    float3 color = lerp(
        float3(0.012, 0.018, 0.085),
        float3(0.11, 0.025, 0.20),
        current
    );
    color = lerp(color, float3(0.00, 0.72, 0.91), cyan * 0.91);
    color = lerp(color, float3(1.00, 0.14, 0.055), coral * 0.90);
    color = lerp(color, float3(0.94, 0.018, 0.43), magenta * 0.88);
    color = lerp(color, float3(0.39, 0.035, 0.93), violet * 0.87);

    // Moving satin folds provide fine, high-contrast features for the glass to refract without
    // turning the backdrop into a diagnostic grid.
    float foldFieldA =
        sin(flow.x * 2.12 + flow.y * 1.31 + time * 0.94) +
        sin(flow.x * 4.31 - flow.y * 1.68 - time * 0.61) * 0.34;
    float foldFieldB =
        sin(flow.y * 3.26 - flow.x * 1.16 - time * 0.81 + 1.7) +
        sin(flow.x * 2.66 + flow.y * 4.08 + time * 0.69) * 0.29;
    float foldA = silkFold(foldFieldA, 0.15);
    float foldB = silkFold(foldFieldB, 0.13);

    color += float3(0.04, 0.83, 0.92) * foldA * (0.14 + cyan * 0.22);
    color += float3(1.00, 0.31, 0.25) * foldB * (0.11 + coral * 0.20);
    color += float3(1.00, 0.78, 0.62) * foldA * foldB * 0.22;
    color *= 0.84 + current * 0.29;

    // Fine caustic filaments. A smooth gradient gives a glass surface nothing to work with: the
    // blur has no detail to destroy, the edge lens has no line to visibly bend, and the rim has no
    // contrast to reflect. These ride the same flow field, so they stay part of the same material
    // rather than reading as an overlaid texture.
    float causticField =
        sin(flow.x * 13.7 + flow.y * 9.1 + time * 1.24) *
        sin(flow.y * 11.3 - flow.x * 7.6 - time * 0.97);
    float caustic = pow(saturate(causticField * 0.5 + 0.5), 7.0);
    float causticMask = saturate(cyan + coral + magenta + violet);
    color += float3(0.72, 0.90, 1.00) * caustic * causticMask * 0.30;

    // Drifting speculars: small, near-hard points that survive as recognisable shapes under
    // refraction, so lens compression and rim reflection are legible against them.
    float2 sparkFlow = flow * 3.4 + float2(time * 0.21, -time * 0.16);
    float2 sparkCell = frac(sparkFlow) - 0.5;
    float spark = exp(-dot(sparkCell, sparkCell) * 62.0) *
        step(0.62, hash21(floor(sparkFlow)));
    color += float3(1.00, 0.93, 0.86) * spark * 0.55;

    // Darker lanes between the pools widen the luminance range inside any one element's footprint,
    // which is what makes the surface's tone compression visible at all.
    float lanes = saturate(cyan + coral + magenta + violet);
    color *= lerp(0.62, 1.06, smoothstep(0.04, 0.55, lanes));

    float vignette = saturate(1.0 - dot(uv - 0.5, uv - 0.5) * 0.72);
    color *= 0.85 + vignette * 0.15;
    color = color / (1.0 + color * 0.37) * 1.10;

    return float4(
        saturate(color),
        input.color.a * materialCoverage(input) * materialOpacity(input)
    );
}

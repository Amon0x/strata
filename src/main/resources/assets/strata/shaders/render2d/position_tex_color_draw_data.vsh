#version 330

layout(std140) uniform DynamicTransforms {
    mat4 ModelViewMat;
    vec4 ColorModulator;
    vec3 ModelOffset;
    mat4 TextureMat;
};
layout(std140) uniform Projection {
    mat4 ProjMat;
};

in vec3 Position;
in vec2 UV0;
in vec4 Color;
in vec4 DrawData0;
in vec4 DrawData1;
in vec4 DrawData2;
in vec4 DrawData3;

out vec2 texCoord0;
out vec4 vertexColor;
out vec4 drawData0;
out vec4 drawData1;
out vec4 drawData2;
out vec4 drawData3;

void main() {
    gl_Position = ProjMat * ModelViewMat * vec4(Position, 1.0);
    texCoord0 = UV0;
    vertexColor = Color;
    drawData0 = DrawData0;
    drawData1 = DrawData1;
    drawData2 = DrawData2;
    drawData3 = DrawData3;
}

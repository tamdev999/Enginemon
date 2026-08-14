#version 450

// Vertex input
layout(location = 0) in vec2 inPosition;   // Position in NDC (-1 to 1)
layout(location = 1) in vec2 inTexCoord;   // UV coordinates
layout(location = 2) in vec4 inColor;      // RGBA color

// Output to fragment shader
layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
}

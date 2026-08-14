#version 450

// Tileset texture with nearest-neighbor sampling
layout(set = 0, binding = 0) uniform sampler2D tilesetAtlas;

// Input from vertex shader
layout(location = 0) in vec2 fragTexCoord;

// Output color
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tilesetAtlas, fragTexCoord);
}

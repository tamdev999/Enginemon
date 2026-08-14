#version 450

// Font texture
layout(set = 0, binding = 0) uniform sampler2D fontAtlas;

// Input from vertex shader
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

// Output color
layout(location = 0) out vec4 outColor;

void main() {
    // Vertex alpha discriminates quad type:
    //   fragColor.a = 1.0 → Solid background quad: output vertex color directly
    //   fragColor.a = 0.0 → Glyph quad: sample texture, transparent stays transparent
    
    if (fragColor.a > 0.5) {
        // Solid background: output vertex color
        outColor = vec4(fragColor.rgb, 1.0);
    } else {
        // Glyph: use texture directly (black ink on transparent)
        outColor = texture(fontAtlas, fragTexCoord);
    }
}

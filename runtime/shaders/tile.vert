#version 450

// Push constants for camera transform
layout(push_constant) uniform PushConstants {
    vec2 camera;      // Camera position in pixels (top-left of view)
    vec2 screenSize;  // Logical render size in pixels (160x144)
} pc;

// Vertex input
layout(location = 0) in vec2 inPosition;  // Position in map pixels (0,0 = top-left)
layout(location = 1) in vec2 inTexCoord;  // UV coordinates

// Output to fragment shader
layout(location = 0) out vec2 fragTexCoord;

void main() {
    // Transform from map pixel coordinates to Vulkan NDC
    // Map coordinate system: (0,0) = top-left, Y increases downward
    // Vulkan NDC: (-1,-1) = top-left, Y increases downward
    // These conventions match, so NO Y flip is needed.
    
    // 1. Subtract camera position to get view-relative position
    // 2. Normalize to 0..1 range by dividing by screen size
    // 3. Scale to -1..1 NDC range
    vec2 viewPos = inPosition - pc.camera;
    vec2 ndc = viewPos / pc.screenSize * 2.0 - 1.0;
    
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragTexCoord = inTexCoord;
}

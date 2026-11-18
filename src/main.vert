#version 120

uniform sampler1D transforms;
uniform int numTransforms;

attribute vec2 position;
attribute vec2 texCoords;
attribute float spriteNumber;

varying vec2 texCoordsOut;

// Given a world position (for this lab, bottom-left is (0, 0), top-right of window is (1280, 720)),
// convert it to be in [-1, 1]^2, where y = -1 is the bottom and x = -1 is the left side.
vec2 cameraPos(vec2 worldPos) {
    const vec2 worldSize = vec2(1280.0, 720.0);
    return worldPos / worldSize * 2.0 - 1.0;
}

void main() {
    // Sample the centre of this sprite's texel: transform = (scale.x, scale.y, trans.x, trans.y)
    vec4 transform = texture1D(transforms, (spriteNumber + 0.5) / float(numTransforms));
    // first scale, then translate (same as BasicTransform::applyPoint)
    vec2 worldPos = position * transform.xy + transform.zw;
    gl_Position = vec4(cameraPos(worldPos), 0.0, 1.0);
    // Our UVs use a bottom-left origin, but SFML uploads images top row first (i.e., row 0 of the
    // image is at t = 0), so flip v to sample the right rows.
    texCoordsOut = vec2(texCoords.x, 1.0 - texCoords.y);
}

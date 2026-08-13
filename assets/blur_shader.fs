#version 330

// One pass of a separable Gaussian, with a darkening folded into it.
//
// Separable because a two-dimensional Gaussian is the product of two
// one-dimensional ones: nine taps across and nine taps down give the same result
// as the eighty-one of the square kernel, for eighteen samples instead. The pass
// is told which way to run through `direction` and is invoked twice.
//
// The darkening rides along on the second pass rather than being a black
// rectangle drawn over the top. It is one less thing over the frame, and it is
// exact — a rectangle at some alpha and a multiply are the same arithmetic only
// while the alpha is right, and the multiply cannot be wrong.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

// One texel of the source, so the taps step by a whole texel whichever way they
// are running and whatever size the window has been dragged to.
uniform vec2 texelSize;

// {1,0} across, {0,1} down.
uniform vec2 direction;

// What the frame is multiplied by. One leaves it alone.
uniform float dim;

out vec4 finalColor;

// Nine taps at sigma three, normalised. Written out rather than computed: they
// are constants of the kernel and never change, and a loop working them out per
// fragment would recompute the same nine numbers for every pixel of the frame.
const float kWeight[5] = float[](0.20418, 0.18017, 0.12383, 0.06628, 0.02763);

void main() {
    vec2 step = texelSize * direction;

    vec3 sum = texture(texture0, fragTexCoord).rgb * kWeight[0];

    for (int i = 1; i < 5; i++) {
        vec2 offset = step * float(i);

        sum += texture(texture0, fragTexCoord + offset).rgb * kWeight[i];
        sum += texture(texture0, fragTexCoord - offset).rgb * kWeight[i];
    }

    finalColor = vec4(sum * dim, 1.0) * colDiffuse * fragColor;
}

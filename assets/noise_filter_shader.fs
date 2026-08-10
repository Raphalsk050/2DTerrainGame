#version 330

// Entradas enviadas pelo Raylib automaticamente
in vec2 fragTexCoord;
in vec4 fragColor;

// A textura que está sendo desenhada
uniform sampler2D texture0;

// Valores customizados para o controle do Smoothstep (enviados pelo C++)
uniform float edge0;
uniform float edge1;

out vec4 finalColor;

void main() {
    // Pega a cor original do pixel da textura
    vec4 texColor = texture(texture0, fragTexCoord);

    // Aplica o smoothstep nos canais de cor R, G e B
    vec3 smoothedRGB = smoothstep(edge0, edge1, texColor.rgb);

    // Retorna a nova cor mantendo o Alpha (transparência) original
    finalColor = vec4(smoothedRGB, texColor.a) * fragColor;
}

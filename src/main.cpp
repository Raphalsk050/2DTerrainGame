#include "raylib.h"
#include <cstddef>

#define RECS_WIDTH    8
#define RECS_HEIGHT   8
#define FLOOR_HEIGHT  10
#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 600
#define RESOLUTION    20
// Os parenteses sao obrigatorios: sem eles a macro e colada crua no ponto de
// uso e a precedencia dos operadores quebra a conta (COLS * ROWS viraria
// 800/20 + 1*600/20 + 1 = 71 em vez de 41*31 = 1271).
#define COLS             (SCREEN_WIDTH / RESOLUTION + 1)
#define ROWS             (SCREEN_HEIGHT / RESOLUTION + 1)
#define AMOUNT_OF_POINTS (COLS * ROWS)

Texture2D noiseTexture;
Shader shader;

// Raio (em pixels) em volta de um ponto da grade onde o clique conta como sendo
// daquele ponto. Metade do espacamento faz cada ponto dominar a sua vizinhanca.
#define PICK_RADIUS (RESOLUTION / 2)

void FillPoints(Vector2 points[], int cols, int rows, int x_size, int y_size) {
    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            points[i * rows + j] = {static_cast<float>(i * x_size), static_cast<float>(j * y_size)};
        }
    }
}

void LoadShader() {
    // Caminho relativo ao diretorio de trabalho, entao o programa precisa ser
    // rodado a partir da raiz do projeto (./build/CppGame).
    shader         = LoadShader(0, "assets/noise_filter_shader.fs");
    int edge0Loc   = GetShaderLocation(shader, "edge0");
    int edge1Loc   = GetShaderLocation(shader, "edge1");
    float edge0Val = 0.55f;
    float edge1Val = 0.6f;

    SetShaderValue(shader, edge0Loc, &edge0Val, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, edge1Loc, &edge1Val, SHADER_UNIFORM_FLOAT);
}

void SpawnPoints(Vector2 points[], Rectangle recs[], int length) {
    for (int i = 0; i < length; i++) {
        recs[i].x      = points[i].x;
        recs[i].y      = points[i].y;
        recs[i].width  = RECS_WIDTH;
        recs[i].height = RECS_HEIGHT;
    }
}

static Vector2 Midpoint(Vector2 a, Vector2 b) {
    return {(a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f};
}

// Marching squares: cada celula da grade tem 4 cantos ligados/desligados, o que
// da 16 configuracoes possiveis. O indice e montado lendo os cantos no sentido
// horario a partir do superior esquerdo (a=8, b=4, c=2, d=1) e cada caso diz
// por quais arestas o contorno atravessa. Como os valores sao binarios, o corte
// cai sempre no meio da aresta - sem interpolacao.
static void DrawCell(Vector2 a, Vector2 b, Vector2 c, Vector2 d, bool va, bool vb, bool vc, bool vd, Color color) {
    const int state = (va << 3) | (vb << 2) | (vc << 1) | (vd);
    if (state == 0 || state == 15) return; // celula toda fora ou toda dentro

    const Vector2 top    = Midpoint(a, b);
    const Vector2 right  = Midpoint(b, c);
    const Vector2 bottom = Midpoint(d, c);
    const Vector2 left   = Midpoint(a, d);

    const float thickness = 2.0f;

    switch (state) {
    case 1:
    case 14: DrawLineEx(left, bottom, thickness, color); break;
    case 2:
    case 13: DrawLineEx(bottom, right, thickness, color); break;
    case 3:
    case 12: DrawLineEx(left, right, thickness, color); break;
    case 4:
    case 11: DrawLineEx(top, right, thickness, color); break;
    case 6:
    case 9: DrawLineEx(top, bottom, thickness, color); break;
    case 7:
    case 8: DrawLineEx(left, top, thickness, color); break;

    // Casos ambiguos: os dois cantos ligados sao diagonais, entao cabem duas
    // ligacoes diferentes. A escolha aqui e sempre separar as diagonais.
    case 5:
        DrawLineEx(left, top, thickness, color);
        DrawLineEx(bottom, right, thickness, color);
        break;
    case 10:
        DrawLineEx(left, bottom, thickness, color);
        DrawLineEx(top, right, thickness, color);
        break;
    }
}

// Terreno procedural: gera um ruido Perlin com um pixel por ponto da grade e
// liga o ponto quando a intensidade passa do limiar. 'scale' controla o tamanho
// das ilhas (quanto maior, mais recortado) e 'threshold' quanto de terra sobra.
static void GenerateTerrain(bool values[], int cols, int rows, float scale, unsigned char threshold) {
    Image noise = GenImagePerlinNoise(cols, rows, 0, 0, scale);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            // A imagem sai em tons de cinza, entao qualquer canal serve.

            values[i * rows + j] = GetImageColor(noise, i, j).r > threshold && j > FLOOR_HEIGHT;
        }
    }

    noiseTexture = LoadTextureFromImage(noise);
    UnloadImage(noise);
}

int main() {
    const int cols = SCREEN_WIDTH / RESOLUTION + 1;
    const int rows = SCREEN_HEIGHT / RESOLUTION + 1;

    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "marching squares");
    SetTargetFPS(60);

    Rectangle recs[AMOUNT_OF_POINTS];
    Vector2 screenPoints[AMOUNT_OF_POINTS];
    bool values[AMOUNT_OF_POINTS] = {false}; // canto ligado (dentro) ou nao

    GenerateTerrain(values, cols, rows, 2.0f, 128);
    LoadShader(); // uma vez so: compilar o shader a cada frame vazaria programas
                  // GL

    FillPoints(screenPoints, cols, rows, RESOLUTION, RESOLUTION);
    SpawnPoints(screenPoints, recs, AMOUNT_OF_POINTS);

    while (!WindowShouldClose()) // Detect window close button or ESC key
    {
        // --- entrada -------------------------------------------------------
        // Botao esquerdo liga os pontos, direito desliga. Ficar com o botao
        // pressionado pinta continuamente, o que torna o desenho mais fluido do
        // que um clique por ponto.
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            const Vector2 mouse = GetMousePosition();
            const bool paint    = IsMouseButtonDown(MOUSE_BUTTON_LEFT);

            // Em vez de varrer todos os pontos, converte a posicao do mouse
            // direto no indice do ponto mais proximo.
            const int i = static_cast<int>((mouse.x + RESOLUTION / 2.0f) / RESOLUTION);
            const int j = static_cast<int>((mouse.y + RESOLUTION / 2.0f) / RESOLUTION);

            if (i >= 0 && i < cols && j >= 0 && j < rows) {
                const int index = i * rows + j;
                if (CheckCollisionPointCircle(mouse, screenPoints[index], PICK_RADIUS)) {
                    values[index] = paint;
                }
            }
        }

        if (IsKeyPressed(KEY_C)) {
            for (int i = 0; i < AMOUNT_OF_POINTS; i++) values[i] = false;
        }

        // --- desenho -------------------------------------------------------
        BeginDrawing();

        ClearBackground(RAYWHITE);

        for (int i = 0; i < AMOUNT_OF_POINTS; i++) {
            const Color color = values[i] ? RED : LIGHTGRAY;
            DrawRectanglePro(recs[i], (Vector2){recs[i].width / 2, recs[i].height / 2}, 0, color);
        }

        // Uma celula por par de colunas/linhas vizinhas - dai o cols-1.
        for (int i = 0; i < cols - 1; i++) {
            for (int j = 0; j < rows - 1; j++) {
                const int a = (i)*rows + (j);           // superior esquerdo
                const int b = (i + 1) * rows + (j);     // superior direito
                const int c = (i + 1) * rows + (j + 1); // inferior direito
                const int d = (i)*rows + (j + 1);       // inferior esquerdo

                DrawCell(screenPoints[a], screenPoints[b], screenPoints[c], screenPoints[d], values[a], values[b],
                         values[c], values[d], DARKBLUE);
            }
        }

        BeginShaderMode(shader);

        DrawTextureEx(noiseTexture, {static_cast<float>(COLS), static_cast<float>(ROWS)}, 0.0f, 5.0f, WHITE);

        EndShaderMode();

        DrawText("esquerdo: liga  |  direito: desliga  |  C: limpa", 10, SCREEN_HEIGHT - 24, 14, GRAY);

        EndDrawing();
    }

    CloseWindow();

    return 0;
}

#include "raylib.h"
// Header do stb_perlin que a propria raylib usa por dentro. Sem definir
// STB_PERLIN_IMPLEMENTATION ele traz so as declaracoes - a implementacao ja esta
// compilada dentro da libraylib, entao basta linkar. O caminho vem do
// target_include_directories no CMakeLists.txt.
#include "stb_perlin.h"
#include <cstddef>

#define RECS_WIDTH    8
#define RECS_HEIGHT   8
#define FLOOR_HEIGHT  18
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
    float edge0Val = 0.2f;
    float edge1Val = 0.8f;

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

// Parametros do gerador. A GenImagePerlinNoise da raylib fixa oitavas em 6,
// lacunarity em 2.0, gain em 0.5 e nao aceita semente - nada disso da para
// ajustar de fora. Aqui cada um vira um campo.
struct NoiseSettings {
    float frequency;  // quantas "ilhas" cabem na largura do mapa. Baixo = poucas
                      // formas grandes; alto = muitas formas pequenas.
    int octaves;      // quantas camadas de ruido sao somadas. 1 = suave e liso;
                      // 4-6 = contorno rugoso, com detalhe fino por cima.
    float lacunarity; // quanto a frequencia cresce a cada oitava. ~2.0 e o usual.
    float gain;       // quanto a amplitude cai a cada oitava. Abaixo de 0.5 o
                      // detalhe fino quase some; acima, domina e vira ruido.
    float offsetX;    // desloca a amostragem. Serve para "rolar" o mapa sem
    float offsetY;    // mudar o formato do terreno.
    int seed;         // mesma semente = mesmo terreno. Troque para sortear outro.
    float threshold;  // [0,1] - acima disso o ponto conta como terra. Sobe o
                      // valor, sobra menos terra.
    int floorRows;    // quantas linhas do topo ficam sempre vazias (ceu).
};

// fBm (fractal brownian motion): soma varias oitavas de Perlin, cada uma com a
// frequencia multiplicada por 'lacunarity' e a amplitude por 'gain'. E a soma
// dessas camadas que da o aspecto natural - uma oitava sozinha fica lisa demais.
// O retorno e normalizado de volta para [-1,1] dividindo pela soma das
// amplitudes, senao mudar o numero de oitavas mudaria tambem o contraste.
static float FbmNoise(float x, float y, const NoiseSettings &s) {
    float sum          = 0.0f;
    float amplitude    = 1.0f;
    float frequency    = 1.0f;
    float maxAmplitude = 0.0f;

    for (int o = 0; o < s.octaves; o++) {
        // A semente muda por oitava para as camadas nao ficarem correlacionadas
        // (com a mesma semente elas repetiriam o mesmo padrao em escalas
        // diferentes, e o resultado fica artificial).
        sum += stb_perlin_noise3_seed(x * frequency, y * frequency, 0.0f, 0, 0, 0, s.seed + o) * amplitude;

        maxAmplitude += amplitude;
        frequency *= s.lacunarity;
        amplitude *= s.gain;
    }

    return (maxAmplitude > 0.0f) ? (sum / maxAmplitude) : 0.0f;
}

// Terreno procedural: avalia o fBm num ponto por vertice da grade e liga o
// vertice quando o valor passa do limiar. Tambem monta a textura de debug com o
// campo continuo, antes do corte - por isso ela mostra tons de cinza e nao so
// preto e branco.
static void GenerateTerrain(bool values[], int cols, int rows, const NoiseSettings &s) {
    Image noise = GenImageColor(cols, rows, BLACK);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            // Os dois eixos sao divididos por 'cols' (e nao cada um pelo seu
            // tamanho) para a celula do ruido continuar quadrada. Dividir y por
            // 'rows' esticaria o terreno na vertical, ja que cols != rows.
            const float nx = (i + s.offsetX) * s.frequency / cols;
            const float ny = (j + s.offsetY) * s.frequency / cols;

            const float n = (FbmNoise(nx, ny, s) + 1.0f) * 0.5f; // [-1,1] -> [0,1]

            values[i * rows + j] = (n > s.threshold) && (j > s.floorRows);

            const unsigned char v = static_cast<unsigned char>(n * 255.0f);
            ImageDrawPixel(&noise, i, j, {v, v, v, 255});
        }
    }

    // Se o terreno for regerado, a textura anterior precisa sair da VRAM antes.
    if (noiseTexture.id != 0) UnloadTexture(noiseTexture);

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

    NoiseSettings terrain = {
        .frequency  = 4.0f,
        .octaves    = 4,
        .lacunarity = 2.0f,
        .gain       = 0.5f,
        .offsetX    = 0.0f,
        .offsetY    = 0.0f,
        .seed       = 1337,
        .threshold  = 0.45f,
        .floorRows  = FLOOR_HEIGHT,
    };

    GenerateTerrain(values, cols, rows, terrain);
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

        // BeginShaderMode(shader);

        // DrawTextureEx(noiseTexture, {static_cast<float>(COLS), static_cast<float>(ROWS)}, 0.0f, 5.0f, WHITE);

        // EndShaderMode();

        DrawText("esquerdo: liga  |  direito: desliga  |  C: limpa", 10, SCREEN_HEIGHT - 24, 14, GRAY);

        EndDrawing();
    }

    CloseWindow();

    return 0;
}

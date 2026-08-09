#include "raylib.h"

#define RECS_WIDTH              8
#define RECS_HEIGHT             8

// Raio (em pixels) em volta de um ponto da grade onde o clique conta como sendo
// daquele ponto. Metade do espacamento faz cada ponto dominar a sua vizinhanca.
#define PICK_RADIUS             (resolution / 2)

void FillPoints(Vector2 points[], int cols, int rows, int x_size, int y_size){
    for(int i = 0; i < cols; i++){
        for(int j = 0; j < rows; j++){
            points[i * rows + j] = {static_cast<float>(i * x_size), static_cast<float>(j * y_size)};
        }
    }
}

void SpawnPoints(Vector2 points[], Rectangle recs[], int length){
    for(int i = 0; i < length; i++){
        recs[i].x = points[i].x;
        recs[i].y = points[i].y;
        recs[i].width  = RECS_WIDTH;
        recs[i].height = RECS_HEIGHT;
    }
}

static Vector2 Midpoint(Vector2 a, Vector2 b){
    return { (a.x + b.x)/2.0f, (a.y + b.y)/2.0f };
}

// Marching squares: cada celula da grade tem 4 cantos ligados/desligados, o que
// da 16 configuracoes possiveis. O indice e montado lendo os cantos no sentido
// horario a partir do superior esquerdo (a=8, b=4, c=2, d=1) e cada caso diz
// por quais arestas o contorno atravessa. Como os valores sao binarios, o corte
// cai sempre no meio da aresta - sem interpolacao.
static void DrawCell(Vector2 a, Vector2 b, Vector2 c, Vector2 d,
                     bool va, bool vb, bool vc, bool vd, Color color)
{
    const int state = (va << 3) | (vb << 2) | (vc << 1) | (vd);
    if (state == 0 || state == 15) return;   // celula toda fora ou toda dentro

    const Vector2 top    = Midpoint(a, b);
    const Vector2 right  = Midpoint(b, c);
    const Vector2 bottom = Midpoint(d, c);
    const Vector2 left   = Midpoint(a, d);

    const float thickness = 2.0f;

    switch (state)
    {
        case 1:  case 14: DrawLineEx(left, bottom, thickness, color); break;
        case 2:  case 13: DrawLineEx(bottom, right, thickness, color); break;
        case 3:  case 12: DrawLineEx(left, right, thickness, color); break;
        case 4:  case 11: DrawLineEx(top, right, thickness, color); break;
        case 6:  case  9: DrawLineEx(top, bottom, thickness, color); break;
        case 7:  case  8: DrawLineEx(left, top, thickness, color); break;

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

int main(){
    const int screenWidth = 800;
    const int screenHeight = 450;
    const int resolution = 20;   // espacamento em pixels entre pontos da grade
    const int cols = screenWidth / resolution + 1;
    const int rows = screenHeight / resolution + 1;
    const int amountOfPoints = cols * rows;

    InitWindow(screenWidth, screenHeight, "marching squares");
    SetTargetFPS(60);

    Rectangle recs[amountOfPoints];
    Vector2 screenPoints[amountOfPoints];
    bool values[amountOfPoints] = { false };   // canto ligado (dentro) ou nao

    FillPoints(screenPoints, cols, rows, resolution, resolution);
    SpawnPoints(screenPoints, recs, amountOfPoints);

    while (!WindowShouldClose())    // Detect window close button or ESC key
    {
        // --- entrada -------------------------------------------------------
        // Botao esquerdo liga os pontos, direito desliga. Ficar com o botao
        // pressionado pinta continuamente, o que torna o desenho mais fluido do
        // que um clique por ponto.
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
        {
            const Vector2 mouse = GetMousePosition();
            const bool paint = IsMouseButtonDown(MOUSE_BUTTON_LEFT);

            // Em vez de varrer todos os pontos, converte a posicao do mouse
            // direto no indice do ponto mais proximo.
            const int i = static_cast<int>((mouse.x + resolution/2.0f) / resolution);
            const int j = static_cast<int>((mouse.y + resolution/2.0f) / resolution);

            if (i >= 0 && i < cols && j >= 0 && j < rows)
            {
                const int index = i * rows + j;
                if (CheckCollisionPointCircle(mouse, screenPoints[index], PICK_RADIUS))
                {
                    values[index] = paint;
                }
            }
        }

        if (IsKeyPressed(KEY_C))
        {
            for (int i = 0; i < amountOfPoints; i++) values[i] = false;
        }

        // --- desenho -------------------------------------------------------
        BeginDrawing();

            ClearBackground(RAYWHITE);

            for (int i = 0; i < amountOfPoints; i++)
            {
                const Color color = values[i] ? RED : LIGHTGRAY;
                DrawRectanglePro(recs[i], (Vector2){ recs[i].width/2, recs[i].height/2 }, 0, color);
            }

            // Uma celula por par de colunas/linhas vizinhas - dai o cols-1.
            for (int i = 0; i < cols - 1; i++)
            {
                for (int j = 0; j < rows - 1; j++)
                {
                    const int a = (i    ) * rows + (j    );   // superior esquerdo
                    const int b = (i + 1) * rows + (j    );   // superior direito
                    const int c = (i + 1) * rows + (j + 1);   // inferior direito
                    const int d = (i    ) * rows + (j + 1);   // inferior esquerdo

                    DrawCell(screenPoints[a], screenPoints[b], screenPoints[c], screenPoints[d],
                             values[a], values[b], values[c], values[d], DARKBLUE);
                }
            }

            DrawText("esquerdo: liga  |  direito: desliga  |  C: limpa", 10, screenHeight - 24, 14, GRAY);

        EndDrawing();
    }

    CloseWindow();

    return 0;
}

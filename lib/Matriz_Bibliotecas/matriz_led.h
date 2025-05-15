#ifndef MATRIZ_LED_H
#define MATRIZ_LED_H

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "generated/ws2812.pio.h"

#define PINO_WS2812   7  // Pino GPIO para comunicação com WS2812
#define NUM_LINHAS    5  // Número de linhas da matriz
#define NUM_COLUNAS   5  // Número de colunas da matriz
#define NUM_PIXELS    (NUM_LINHAS * NUM_COLUNAS)  // Total de LEDs (25)
#define RGBW_ATIVO    false  // Define protocolo RGB (não RGBW)

/* ---------- Utilidades de cor ---------- */
#define GRB(r,g,b)   ( ((uint32_t)(g) << 16) | ((uint32_t)(r) << 8) | (b) )  // Converte RGB para formato GRB do WS2812

/* ---------- Estrutura de cor RGB ---------- */
typedef struct {
    const char* nome;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} CorRGB;

/* ---------- Paleta de cores ---------- */
extern const CorRGB PALETA_CORES[];

/* ---------- Cores básicas para padrões ---------- */
#define COR_BRANCO    GRB(255, 255, 255)  // Branco
#define COR_PRATA     GRB(192, 192, 192)  // Prata
#define COR_CINZA     GRB(40, 35, 35)     // Cinza
#define COR_VIOLETA   GRB(130, 0, 130)    // Violeta
#define COR_AZUL      GRB(0, 0, 200)      // Azul
#define COR_MARROM    GRB(30, 10, 10)     // Marrom
#define COR_VERDE     GRB(0, 150, 0)      // Verde
#define COR_OURO      GRB(218, 165, 32)   // Ouro
#define COR_LARANJA   GRB(255, 65, 0)     // Laranja
#define COR_AMARELO   GRB(255, 140, 0)    // Amarelo
#define COR_VERMELHO  GRB(190, 0, 0)     // Vermelho
#define COR_OFF       GRB(0, 0, 0)        // Desliga LEDs

/* ---------- Padrões 5 × 5 (✓, !, X) ---------- */
extern const uint8_t PAD_OK[5];   // Padrão "✓" para verde
extern const uint8_t PAD_EXC[5];  // Padrão "!" para amarelo
extern const uint8_t PAD_X[5];    // Padrão "X" para vermelho

/* ---------- Padrões para dígitos 0-9 (novo formato) ---------- */
extern const bool padrao_numeros[10][25];  // Array 2D com padrões dos números 0-9

/* ---------- API ---------- */
void inicializar_matriz_led(void);  // Inicializa PIO para WS2812
void matriz_draw_pattern(const uint8_t pad[5], uint32_t cor_on);  // Desenha padrão na matriz
void matriz_draw_number(uint8_t numero, uint32_t cor_on);  // Desenha número (0-9) na matriz
void matriz_draw_rain_animation(uint32_t cor_on);  // Desenha animação de chuva
void matriz_clear(void);  // Limpa todos os LEDs

#endif /* MATRIZ_LED_H */
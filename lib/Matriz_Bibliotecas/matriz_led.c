#include "matriz_led.h"
#include <stdlib.h>

const CorRGB PALETA_CORES[] = {
    {"Branco",  255, 255, 255},
    {"Prata",   192, 192, 192},
    {"Cinza",    40,  35,  35},
    {"Violeta", 130,   0, 130},
    {"Azul",      0,   0, 200},
    {"Marrom",   30,  10,  10},
    {"Verde",     0, 150,   0},
    {"Ouro",    218, 165,  32},
    {"Laranja", 255,  65,   0},
    {"Amarelo", 255, 140,   0},
    {"Vermelho",190,   0,   0},
    {"---",       0,   0,   0}
};

const uint8_t PAD_OK[5]  = {0b00001,0b00010,0b00100,0b11000,0b10000};  // Padrão "✓" para verde
const uint8_t PAD_EXC[5] = {0b00100,0b00100,0b00100,0b00000,0b00100};  // Padrão "!" para amarelo
const uint8_t PAD_X[5]   = {0b10001,0b01010,0b00100,0b01010,0b10001};  // Padrão "X" para vermelho

// Novos padrões de números (invertidos)
const bool padrao_numeros[10][25] = {
    // 0 (invertido)
    {
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 1,
        1, 0, 0, 0, 1,
        1, 0, 0, 0, 1,
        1, 1, 1, 1, 1
    },
    // 1 (invertido)
    {
        1, 1, 1, 1, 1,
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 1, 1, 0, 0,
        0, 0, 1, 0, 0
    },
    // 2 (invertido)
    {
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 0,
        1, 1, 1, 1, 1,
        0, 0, 0, 0, 1,
        1, 1, 1, 1, 1
    },
    // 3 (invertido)
    {
        1, 1, 1, 1, 1,
        0, 0, 0, 0, 1,
        1, 1, 1, 1, 1,
        0, 0, 0, 0, 1,
        1, 1, 1, 1, 1
    },
    // 4 (invertido)
    {
        1, 0, 0, 0, 0,
        0, 0, 0, 0, 1,
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 1,
        1, 0, 0, 0, 1
    },
    // 5 (invertido)
    {
        1, 1, 1, 1, 1,
        0, 0, 0, 0, 1,
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 0,
        1, 1, 1, 1, 1
    },
    // 6 (invertido)
    {
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 1,
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 0,
        1, 1, 1, 1, 1
    },
    // 7 (invertido)
    {
        1, 0, 0, 0, 0,
        0, 0, 0, 0, 1,
        1, 1, 1, 0, 0,
        0, 0, 0, 0, 1,
        1, 1, 1, 1, 1
    },
    // 8 (invertido)
    {
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 1,
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 1,
        1, 1, 1, 1, 1
    },
    // 9 (invertido)
    {
        1, 1, 1, 1, 1,
        0, 0, 0, 0, 1,
        1, 1, 1, 1, 1,
        1, 0, 0, 0, 1,
        1, 1, 1, 1, 1
    }
};

static inline void ws2812_put(uint32_t grb) {  // Envia dados GRB para um LED
    pio_sm_put_blocking(pio0, 0, grb << 8u);  // Desloca 8 bits para alinhar protocolo WS2812
}

void inicializar_matriz_led(void) {  // Configura PIO para controlar WS2812
    PIO pio = pio0;
    uint off = pio_add_program(pio, &ws2812_program);  // Carrega programa PIO
    ws2812_program_init(pio, 0, off, PINO_WS2812, 800000, RGBW_ATIVO);  // Inicia PIO a 800kHz
    srand(to_us_since_boot(get_absolute_time()));  // Inicializa semente para rand()
}

void matriz_draw_pattern(const uint8_t pad[5], uint32_t cor_on) {  // Desenha padrão na matriz
    /* placa montada "de cabeça-para-baixo" → linha 4 primeiro */
    for (int lin = 4; lin >= 0; --lin) {
        for (int col = 0; col < 5; ++col) {
            bool aceso = pad[lin] & (1 << (4 - col));  // Verifica bit do padrão
            ws2812_put(aceso ? cor_on : COR_OFF);  // Aplica cor ou desliga LED
        }
    }
    sleep_us(60);  // Latência para atualizar matriz
}

void matriz_draw_number(uint8_t numero, uint32_t cor_on) {  // Desenha um número na matriz
    if (numero > 9) {
        matriz_draw_pattern(PAD_X, COR_VERMELHO);  // Desenha "X" vermelho se o número for maior que 9
    } else {
        /* O formato da matriz boolean requer uma lógica diferente para desenhar */
        for (int lin = 0; lin < 5; ++lin) {
            for (int col = 0; col < 5; ++col) {
                bool aceso = padrao_numeros[numero][lin * 5 + col];
                ws2812_put(aceso ? cor_on : COR_OFF);
            }
        }
        sleep_us(60);  // Latência para atualizar matriz
    }
}

void matriz_draw_rain_animation(uint32_t cor_on) {
    static uint8_t gotas[5] = {0};  // Posição Y de cada gota por coluna (0 a 4, 0=desligada)
    static uint32_t ultimo_tempo = 0;
    uint32_t tempo_atual = to_ms_since_boot(get_absolute_time());

    // Atualiza a cada 50ms para movimento mais rápido
    if (tempo_atual - ultimo_tempo >= 50) {
        // Limpa a matriz
        matriz_clear();

        // Atualiza posição das gotas
        for (int col = 0; col < 5; col++) {
            if (gotas[col] > 0) {
                gotas[col]++;  // Move gota para baixo
                if (gotas[col] > 4) gotas[col] = 0;  // Reseta se atingir o fundo
            } else {
                // Sempre criar uma nova gota quando a coluna estiver vazia
                gotas[col] = 1;  // Começa na linha superior
            }
        }

        // Desenha gotas
        for (int col = 0; col < 5; col++) {
            if (gotas[col] > 0) {
                // Calcula índice do LED (matriz invertida: linha 4 - (gotas[col] - 1))
                int lin = 4 - (gotas[col] - 1);
                for (int i = 0; i < NUM_PIXELS; i++) {
                    if (i == lin * 5 + col) {
                        ws2812_put(cor_on);  // Acende o LED da gota
                    } else {
                        ws2812_put(COR_OFF);  // Desliga os outros LEDs
                    }
                }
            }
        }
        sleep_us(60);  // Latência para atualizar matriz
        ultimo_tempo = tempo_atual;
    }
}

void matriz_clear(void) {  // Limpa todos os LEDs
    for (int i = 0; i < NUM_PIXELS; ++i)
        ws2812_put(COR_OFF);  // Desliga cada LED
    sleep_us(60);  // Latência para confirmar limpeza
}
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "ssd1306.h"
#include "matriz_led.h"

// DEFINIÇÕES DO DISPLAY OLED
#define I2C_PORT i2c1
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 15
#define SSD1306_I2C_ADDR   0x3C
#define SSD1306_WIDTH      128
#define SSD1306_HEIGHT     64
#define BUTTON_A_PIN 5
#define ADC_JOYSTICK_X_PIN 27 // GPIO 27 → ADC1 (nível de água)
#define ADC_JOYSTICK_Y_PIN 26 // GPIO 26 → ADC0 (volume de chuva)
#define LED_RED_PIN 13        // GPIO 13 → LED vermelho

// Estrutura para armazenar dados dos sensores
typedef struct {
    uint16_t nivelAguaRaw;         // Valor bruto do nível de água
    uint16_t volumeChuvaRaw;       // Valor bruto do volume de chuva
    float nivelAguaPercent;        // Nível de água em porcentagem
    float volumeChuvaPercent;      // Volume de chuva em porcentagem
    float volumeChuvaMmH;          // Volume de chuva em mm/h
    bool alertaRiscoEnchente;      // Indica se há risco de enchente
} dadosSensores_t;

// Estrutura para dados de previsão
typedef struct {
    float nivelAguaPrevisto;       // Previsão do nível de água
} dadosPrevisao_t;

// Filas para comunicação entre tarefas
static QueueHandle_t filaDadosSensores = NULL;
static QueueHandle_t filaDadosExibicao = NULL;

// Instância do display OLED
static ssd1306_t display;

// Histórico para previsão (buffer circular)
#define TAMANHO_HISTORICO 5
static float historicoNivelAgua[TAMANHO_HISTORICO];
static float historicoVolumeChuva[TAMANHO_HISTORICO];
static int indiceHistorico = 0;    // Índice atual no histórico
static int contagemHistorico = 0;  // Quantidade de dados no histórico

// Buffers para dados dos gráficos (percentual de chuva e nível a cada 2 segundos)
#define TAMANHO_GRAFICO 10 // 20 segundos / 2 segundos por ponto = 10 pontos
static float dadosGraficoChuva[TAMANHO_GRAFICO];
static float dadosGraficoNivel[TAMANHO_GRAFICO]; // Buffer para nível de água
static int indiceGrafico = 0;      // Índice atual nos buffers dos gráficos
static int contagemGrafico = 0;    // Quantidade de dados nos buffers dos gráficos
static uint32_t ultimoTempoGrafico = 0; // Último tempo de atualização dos gráficos

// FUNÇÕES AUXILIARES

// Converte percentual de chuva para mm/h
float percentualParaMmH(float percentual) {
    if (percentual <= 0.0f) return 0.0f;
    else if (percentual < 30.0f) return (percentual / 30.0f) * 5.0f;
    else if (percentual < 60.0f) return 5.0f + ((percentual - 30.0f) / 30.0f) * 10.0f;
    else if (percentual < 80.0f) return 15.0f + ((percentual - 60.0f) / 20.0f) * 15.0f;
    else if (percentual < 95.0f) return 30.0f + ((percentual - 80.0f) / 15.0f) * 5.0f;
    else return 35.0f;
}

// Calcula a inclinação para previsão (regressão linear simples)
float calcularInclinacao() {
    if (contagemHistorico < 2) return 0.0f; // Precisa de pelo menos 2 pontos

    float somaX = 0.0f, somaY = 0.0f, somaXY = 0.0f, somaX2 = 0.0f;
    int n = (contagemHistorico < TAMANHO_HISTORICO) ? contagemHistorico : TAMANHO_HISTORICO;

    for (int i = 0; i < n; i++) {
        float x = (float)i; // Índice como tempo
        float y = historicoNivelAgua[(indiceHistorico - n + i + TAMANHO_HISTORICO) % TAMANHO_HISTORICO];
        somaX += x;
        somaY += y;
        somaXY += x * y;
        somaX2 += x * x;
    }

    float denominador = n * somaX2 - somaX * somaX;
    if (denominador == 0.0f) return 0.0f; // Evita divisão por zero

    return (n * somaXY - somaX * somaY) / denominador;
}

// TASKS

// Task que lê os sensores (simulados por joystick)
void TaskMedicao(void *pvParameters) {
    adc_init();
    adc_gpio_init(ADC_JOYSTICK_X_PIN);
    adc_gpio_init(ADC_JOYSTICK_Y_PIN);

    dadosSensores_t dados;

    while (true) {
        // Lê os sensores
        adc_select_input(1);
        dados.nivelAguaRaw = adc_read();
        dados.nivelAguaPercent = (dados.nivelAguaRaw / 4095.0f) * 100.0f;

        adc_select_input(0);
        dados.volumeChuvaRaw = adc_read();
        dados.volumeChuvaPercent = (dados.volumeChuvaRaw / 4095.0f) * 100.0f;

        dados.volumeChuvaMmH = percentualParaMmH(dados.volumeChuvaPercent);
        dados.alertaRiscoEnchente = (dados.nivelAguaPercent >= 70.0f || dados.volumeChuvaPercent >= 80.0f);

        // Envia os dados para a fila
        xQueueSend(filaDadosSensores, &dados, pdMS_TO_TICKS(10));
        printf("Medicao: Nivel=%.1f%%, Chuva=%.1f%%\n", dados.nivelAguaPercent, dados.volumeChuvaPercent);

        // Atualiza os buffers dos gráficos a cada 2 segundos
        uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());
        if ((tempoAtual - ultimoTempoGrafico) >= 2000) {
            dadosGraficoChuva[indiceGrafico] = dados.volumeChuvaPercent;
            dadosGraficoNivel[indiceGrafico] = dados.nivelAguaPercent;
            indiceGrafico = (indiceGrafico + 1) % TAMANHO_GRAFICO;
            if (contagemGrafico < TAMANHO_GRAFICO) contagemGrafico++;
            ultimoTempoGrafico = tempoAtual;
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// Task que faz a previsão do nível de água
void TaskPrevisao(void *pvParameters) {
    dadosSensores_t dadosRecebidos;
    dadosPrevisao_t dadosEnviar;

    while (true) {
        if (xQueueReceive(filaDadosSensores, &dadosRecebidos, pdMS_TO_TICKS(100)) == pdPASS) {
            // Atualiza o histórico
            historicoNivelAgua[indiceHistorico] = dadosRecebidos.nivelAguaPercent;
            historicoVolumeChuva[indiceHistorico] = dadosRecebidos.volumeChuvaMmH;
            indiceHistorico = (indiceHistorico + 1) % TAMANHO_HISTORICO;
            if (contagemHistorico < TAMANHO_HISTORICO) contagemHistorico++;

            // Calcula a previsão
            float inclinacao = calcularInclinacao();
            float intervalosFuturos = 10.0f; // Previsão para 2.5s à frente (10 * 250ms)
            float nivelPrevisto = dadosRecebidos.nivelAguaPercent + inclinacao * intervalosFuturos;

            // Limita a previsão entre 0% e 100%
            if (nivelPrevisto < 0.0f) nivelPrevisto = 0.0f;
            else if (nivelPrevisto > 100.0f) nivelPrevisto = 100.0f;
            dadosEnviar.nivelAguaPrevisto = nivelPrevisto;

            // Envia os dados de previsão
            xQueueSend(filaDadosExibicao, &dadosEnviar, pdMS_TO_TICKS(10));
        }
    }
}

// Task que exibe os dados no display OLED
void TaskDisplay(void *pvParameters) {
    dadosSensores_t dadosSensores;
    dadosPrevisao_t dadosPrevisao;
    char buffer[32];
    uint8_t telaAtual = 0; // 0: Principal, 1: Secundária, 2: Gráfico de chuva, 3: Gráfico de nível

    // Configura o botão
    gpio_init(BUTTON_A_PIN);
    gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_A_PIN);

    bool estadoBotaoAnterior = gpio_get(BUTTON_A_PIN);
    uint32_t tempoUltimoPressionamento = 0;
    const uint32_t delayDebounceMs = 200;

    while (true) {
        // Verifica o botão com debounce
        bool estadoBotaoAtual = gpio_get(BUTTON_A_PIN);
        uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());
        if (estadoBotaoAnterior && !estadoBotaoAtual) {
            if ((tempoAtual - tempoUltimoPressionamento) > delayDebounceMs) {
                telaAtual = (telaAtual + 1) % 4; // Alterna entre 0, 1, 2, 3
                tempoUltimoPressionamento = tempoAtual;
                ssd1306_fill(&display, false); // Limpa a tela ao trocar
                printf("Botao pressionado: Tela=%d\n", telaAtual);
            }
        }
        estadoBotaoAnterior = estadoBotaoAtual;

        // Espia os dados dos sensores sem consumir
        if (xQueuePeek(filaDadosSensores, &dadosSensores, pdMS_TO_TICKS(50)) == pdPASS) {
            printf("Display: Nivel=%.1f%%, Tela=%d\n", dadosSensores.nivelAguaPercent, telaAtual);
            ssd1306_fill(&display, false); // Limpa a tela

            if (telaAtual == 0) {
                // Tela principal: dados detalhados
                snprintf(buffer, sizeof(buffer), "QntChuva:%.2fmm", dadosSensores.volumeChuvaMmH);
                ssd1306_draw_string(&display, buffer, 0, 0, false);

                snprintf(buffer, sizeof(buffer), "Chuva: %.1f%%", dadosSensores.volumeChuvaPercent);
                ssd1306_draw_string(&display, buffer, 0, 13, false);

                snprintf(buffer, sizeof(buffer), "Nivel: %.1f%%", dadosSensores.nivelAguaPercent);
                ssd1306_draw_string(&display, buffer, 0, 26, false);

                snprintf(buffer, sizeof(buffer), "Status: %s", dadosSensores.alertaRiscoEnchente ? "ALERTA!" : "Normal");
                ssd1306_draw_string(&display, buffer, 0, 39, false);

                const char* cor = dadosSensores.alertaRiscoEnchente ? "Cor: Vermelho" : "Cor: Verde";
                ssd1306_draw_string(&display, cor, 0, 52, false);
            } else if (telaAtual == 1) {
                // Tela secundária: barras e previsão
                ssd1306_draw_string(&display, "Barra Chuva:", 0, 0, false);
                uint8_t barYChuva = 10, barWidth = SSD1306_WIDTH - 20, barHeight = 8;
                ssd1306_rect(&display, barYChuva, 0, barWidth, barHeight, true, false);
                uint8_t chuvaFill = (uint8_t)(dadosSensores.volumeChuvaPercent * (barWidth - 2) / 100.0f);
                if (chuvaFill > 0) ssd1306_rect(&display, barYChuva + 1, 1, chuvaFill, barHeight - 2, true, true);

                ssd1306_draw_string(&display, "Barra Nivel:", 0, 25, false);
                uint8_t barYNivel = 35;
                ssd1306_rect(&display, barYNivel, 0, barWidth, barHeight, true, false);
                uint8_t nivelFill = (uint8_t)(dadosSensores.nivelAguaPercent * (barWidth - 2) / 100.0f);
                if (ashboard_fill > 0) ssd1306_rect(&display, barYNivel + 1, 1, nivelFill, barHeight - 2, true, true);

                if (xQueueReceive(filaDadosExibicao, &dadosPrevisao, 0) == pdPASS) {
                    snprintf(buffer, sizeof(buffer), "Previsao:%.1f%%", dadosPrevisao.nivelAguaPrevisto);
                } else {
                    snprintf(buffer, sizeof(buffer), "Previsao: N/A");
                }
                ssd1306_draw_string(&display, buffer, 0, 50, false);
            } else if (telaAtual == 2) {
                // Tela de gráfico: percentual de chuva vs. tempo
                const uint8_t graficoX = 15; // Início do eixo X
                const uint8_t graficoY = 54; // Base do eixo Y
                const uint8_t alturaGrafico = 45; // Altura do gráfico
                const uint8_t larguraGrafico = 100; // Largura do gráfico (20 segundos)

                // Desenha o título centralizado
                const char* titulo = "Chuva %";
                uint8_t tituloWidth = strlen(titulo) * 5; // Aproximado com números pequenos
                uint8_t tituloX = (SSD1306_WIDTH - tituloWidth) / 2; // Centraliza
                ssd1306_draw_string(&display, titulo, tituloX, 5, true); // Título na linha y=5

                // Desenha os eixos
                ssd1306_line(&display, graficoX, graficoY, graficoX + larguraGrafico, graficoY, true);
                ssd1306_line(&display, graficoX, graficoY, graficoX, graficoY - alturaGrafico, true);

                // Plota os pontos do gráfico
                int n = (contagemGrafico < TAMANHO_GRAFICO) ? contagemGrafico : TAMANHO_GRAFICO;
                for (int i = 0; i < n - 1; i++) {
                    int idxAtual = (indiceGrafico - n + i + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    int idxProximo = (indiceGrafico - n + i + 1 + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;

                    float chuvaAtual = dadosGraficoChuva[idxAtual];
                    float chuvaProxima = dadosGraficoChuva[idxProximo];

                    uint8_t yAtual = graficoY - (uint8_t)(chuvaAtual * alturaGrafico / 100.0f);
                    uint8_t yProximo = graficoY - (uint8_t)(chuvaProxima * alturaGrafico / 100.0f);

                    uint8_t xAtual = graficoX + (i * larguraGrafico / (TAMANHO_GRAFICO - 1));
                    uint8_t xProximo = graficoX + ((i + 1) * larguraGrafico / (TAMANHO_GRAFICO - 1));

                    ssd1306_line(&display, xAtual, yAtual, xProximo, yProximo, true);
                }

                // Marcações no eixo Y (0, 20, 40, 60, 80, 100%)
                for (int i = 0; i <= 5; i++) {
                    uint8_t y = graficoY - (i * alturaGrafico / 5);
                    ssd1306_line(&display, graficoX - 3, y, graficoX, y, true);
                    if (i % 2 == 0) { // Rótulos em 0, 40, 80
                        snprintf(buffer, sizeof(buffer), "%d", i * 20);
                        ssd1306_draw_string(&display, buffer, 0, y - 3, true); // Números pequenos
                    }
                }

                // Marcações no eixo X (0, 5, 10, 15, 20 segundos)
                for (int i = 0; i <= 4; i++) {
                    uint8_t x = graficoX + (i * larguraGrafico / 4);
                    ssd1306_line(&display, x, graficoY, x, graficoY + 2, true);
                    snprintf(buffer, sizeof(buffer), "%d", i * 5);
                    ssd1306_draw_string(&display, buffer, x - 8, graficoY + 2, true); // Números pequenos
                }
            } else if (telaAtual == 3) {
                // Tela de gráfico: percentual de nível de água vs. tempo
                const uint8_t graficoX = 15; // Início do eixo X
                const uint8_t graficoY = 54; // Base do eixo Y
                const uint8_t alturaGrafico = 45; // Altura do gráfico
                const uint8_t larguraGrafico = 100; // Largura do gráfico (20 segundos)

                // Desenha o título centralizado
                const char* titulo = "Nivel %";
                uint8_t tituloWidth = strlen(titulo) * 5; // Aproximado com números pequenos
                uint8_t tituloX = (SSD1306_WIDTH - tituloWidth) / 2; // Centraliza
                ssd1306_draw_string(&display, titulo, tituloX, 5, true); // Título na linha y=5

                // Desenha os eixos
                ssd1306_line(&display, graficoX, graficoY, graficoX + larguraGrafico, graficoY, true);
                ssd1306_line(&display, graficoX, graficoY, graficoX, graficoY - alturaGrafico, true);

                // Plota os pontos do gráfico
                int n = (contagemGrafico < TAMANHO_GRAFICO) ? contagemGrafico : TAMANHO_GRAFICO;
                for (int i = 0; i < n - 1; i++) {
                    int idxAtual = (indiceGrafico - n + i + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    int idxProximo = (indiceGrafico - n + i + 1 + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;

                    float nivelAtual = dadosGraficoNivel[idxAtual];
                    float nivelProximo = dadosGraficoNivel[idxProximo];

                    uint8_t yAtual = graficoY - (uint8_t)(nivelAtual * alturaGrafico / 100.0f);
                    uint8_t yProximo = graficoY - (uint8_t)(nivelProximo * alturaGrafico / 100.0f);

                    uint8_t xAtual = graficoX + (i * larguraGrafico / (TAMANHO_GRAFICO - 1));
                    uint8_t xProximo = graficoX + ((i + 1) * larguraGrafico / (TAMANHO_GRAFICO - 1));

                    ssd1306_line(&display, xAtual, yAtual, xProximo, yProximo, true);
                }

                // Marcações no eixo Y (0, 20, 40, 60, 80, 100%)
                for (int i = 0; i <= 5; i++) {
                    uint8_t y = graficoY - (i * alturaGrafico / 5);
                    ssd1306_line(&display, graficoX - 3, y, graficoX, y, true);
                    if (i % 2 == 0) { // Rótulos em 0, 40, 80
                        snprintf(buffer, sizeof(buffer), "%d", i * 20);
                        ssd1306_draw_string(&display, buffer, 0, y - 3, true); // Números pequenos
                    }
                }

                // Marcações no eixo X (0, 5, 10, 15, 20 segundos)
                for (int i = 0; i <= 4; i++) {
                    uint8_t x = graficoX + (i * larguraGrafico / 4);
                    ssd1306_line(&display, x, graficoY, x, graficoY + 2, true);
                    snprintf(buffer, sizeof(buffer), "%d", i * 5);
                    ssd1306_draw_string(&display, buffer, x - 8, graficoY + 2, true); // Números pequenos
                }
            }
            ssd1306_send_data(&display); // Atualiza o display
        } else {
            printf("Display: Falha ao espiar filaDadosSensores\n");
        }
    }
}

// Task que controla a matriz de LEDs e o LED vermelho
void TaskMatrizLED(void *pvParameters) {
    dadosSensores_t dados;
    // Cores em formato GRB
    const uint32_t cor_vermelho = COR_VERMELHO; // R=190, G=0, B=0
    const uint32_t cor_azul = COR_AZUL;         // R=0, G=0, B=200
    const uint32_t cor_amarelo = COR_AMARELO;   // R=255, G=140, B=0
    bool estadoXAtivo = false;                  // Estado do "X" vermelho
    bool estadoChuvaAtivo = false;              // Estado da animação de chuva
    bool estadoExclamacaoAtivo = false;         // Estado do "!" amarelo
    bool estadoLedVermelho = false;             // Estado do LED vermelho
    uint8_t estadoExibicao = 0;                 // 0: Chuva, 1: Exclamação, 2: X
    uint32_t ultimoTempoAlternancia = 0;        // Última alternância (ms)

    while (true) {
        if (xQueuePeek(filaDadosSensores, &dados, pdMS_TO_TICKS(100)) == pdPASS) {
            bool chuvaAlta = (dados.volumeChuvaPercent > 80.0f);
            bool nivelAlto = (dados.nivelAguaPercent > 70.0f);
            bool nivelCritico = (dados.nivelAguaPercent > 80.0f); // Condição para LED vermelho
            uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());

            printf("MatrizLED: Nivel=%.1f%%, Chuva=%.1f%%, ChuvaAlta=%d, NivelAlto=%d, LedVermelho=%d\n",
                   dados.nivelAguaPercent, dados.volumeChuvaPercent, chuvaAlta, nivelAlto, nivelCritico);

            // Controle do LED vermelho
            if (nivelCritico && !estadoLedVermelho) {
                gpio_put(LED_RED_PIN, 1); // Liga o LED
                estadoLedVermelho = true;
                printf("LED Vermelho: LIGADO\n");
            } else if (!nivelCritico && estadoLedVermelho) {
                gpio_put(LED_RED_PIN, 0); // Desliga o LED
                estadoLedVermelho = false;
                printf("LED Vermelho: DESLIGADO\n");
            }

            // Lógica de alternância a cada 4 segundos para chuva > 80%
            if (chuvaAlta && (tempoAtual - ultimoTempoAlternancia >= 4000)) {
                estadoExibicao = (estadoExibicao + 1) % 3; // Alterna entre 0 (chuva), 1 (exclamação), 2 (X)
                ultimoTempoAlternancia = tempoAtual;
                printf("MatrizLED: Alternando para %s\n",
                       estadoExibicao == 0 ? "Animação Chuva" :
                       estadoExibicao == 1 ? "Exclamação Amarela" : "X Vermelho");
            }

            // Estado 1: Chuva > 80% → Alternar animação de chuva, "!" amarelo e "X" vermelho
            if (chuvaAlta) {
                if (estadoExibicao == 0) {
                    matriz_draw_rain_animation(cor_azul);
                    estadoChuvaAtivo = true;
                    estadoExclamacaoAtivo = false;
                    estadoXAtivo = false;
                } else if (estadoExibicao == 1) {
                    matriz_draw_pattern(PAD_EXC, cor_amarelo);
                    estadoExclamacaoAtivo = true;
                    estadoChuvaAtivo = false;
                    estadoXAtivo = false;
                } else {
                    matriz_draw_pattern(PAD_X, cor_vermelho);
                    estadoXAtivo = true;
                    estadoChuvaAtivo = false;
                    estadoExclamacaoAtivo = false;
                }
            }
            // Estado 2: Nível > 70% → Apenas "X" vermelho
            else if (nivelAlto && !estadoXAtivo) {
                matriz_draw_pattern(PAD_X, cor_vermelho);
                estadoXAtivo = true;
                estadoChuvaAtivo = false;
                estadoExclamacaoAtivo = false;
            }
            // Estado 3: Nenhuma condição → Limpar matriz
            else if (!nivelAlto && (estadoXAtivo || estadoChuvaAtivo || estadoExclamacaoAtivo)) {
                matriz_clear();
                estadoXAtivo = false;
                estadoChuvaAtivo = false;
                estadoExclamacaoAtivo = false;
            }
        } else {
            printf("MatrizLED: Falha ao espiar filaDadosSensores\n");
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Atraso para suportar animação (100ms por quadro)
    }
}

// FUNÇÃO PRINCIPAL
int main() {
    stdio_init_all();
    sleep_ms(2000); // Aguarda o terminal serial
    printf("\n=== Sistema de Alerta de Enchente - Inicializando ===\n\n");

    // Inicializa o GPIO do LED vermelho
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_put(LED_RED_PIN, 0); // Garante que o LED comece desligado
    printf("LED vermelho inicializado em GPIO=%d.\n", LED_RED_PIN);

    // Configura o I2C
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    printf("I2C inicializado em SDA=%d, SCL=%d.\n", I2C_SDA_PIN, I2C_SCL_PIN);

    // Inicializa o display OLED
    ssd1306_init(&display, SSD1306_WIDTH, SSD1306_HEIGHT, false, SSD1306_I2C_ADDR, I2C_PORT);
    ssd1306_config(&display);
    ssd1306_fill(&display, false);
    ssd1306_draw_string(&display, "Iniciando...", 0, 28, false);
    ssd1306_send_data(&display);
    printf("Display OLED inicializado.\n");

    // Inicializa a matriz de LEDs
    inicializar_matriz_led();
    printf("Matriz de LEDs inicializada.\n");

    // Cria as filas
    filaDadosSensores = xQueueCreate(10, sizeof(dadosSensores_t));
    filaDadosExibicao = xQueueCreate(5, sizeof(dadosPrevisao_t));
    if (filaDadosSensores == NULL || filaDadosExibicao == NULL) {
        printf("Erro: falha ao criar filas!\n");
        while(1);
    }
    printf("Filas criadas.\n");

    // Cria as tasks
    xTaskCreate(TaskMedicao, "Leitura", 512, NULL, 2, NULL);
    xTaskCreate(TaskPrevisao, "Previsao", configMINIMAL_STACK_SIZE + 256, NULL, 1, NULL);
    xTaskCreate(TaskDisplay, "Exibicao", configMINIMAL_STACK_SIZE + 512, NULL, 1, NULL);
    xTaskCreate(TaskMatrizLED, "MatrizLED", configMINIMAL_STACK_SIZE + 768, NULL, 1, NULL);
    printf("Tasks criadas.\n");

    // Inicia o sistema
    printf("Iniciando o FreeRTOS...\n");
    vTaskStartScheduler();

    // Se chegar aqui, houve erro
    while (true) {
        printf("Erro: FreeRTOS não iniciou!\n");
        sleep_ms(1000);
    }

    return 0;
}
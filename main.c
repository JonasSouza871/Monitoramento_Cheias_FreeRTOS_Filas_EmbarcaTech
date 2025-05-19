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
#define LED_PIN 13 // Pino do LED vermelho (nome mantido, mas representa o VERMELHO)
#define LED_VERDE_PIN 11 // Pino do LED verde

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
static QueueHandle_t filaEstadoAlerta = NULL; // Fila para o estado de alerta

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

// TASKS

// Task que lê os sensores (simulados por joystick)
void TaskMedicao(void *pvParameters) {
adc_init();
adc_gpio_init(ADC_JOYSTICK_X_PIN);
adc_gpio_init(ADC_JOYSTICK_Y_PIN);

// Configura os pinos dos LEDs como saída
gpio_init(LED_PIN); // LED Vermelho
gpio_set_dir(LED_PIN, GPIO_OUT);
gpio_put(LED_PIN, 0); // Começa apagado

gpio_init(LED_VERDE_PIN); // LED Verde
gpio_set_dir(LED_VERDE_PIN, GPIO_OUT);
gpio_put(LED_VERDE_PIN, 0); // Começa apagado

dadosSensores_t dados;
static uint32_t ultimoTempoPiscoLedVermelho = 0;
static bool estadoPiscoLedVermelho = false;

while (true) {
    // Lê os sensores
    adc_select_input(1);
    dados.nivelAguaRaw = adc_read();
    dados.nivelAguaPercent = (dados.nivelAguaRaw / 4095.0f) * 100.0f;

    adc_select_input(0);
    dados.volumeChuvaRaw = adc_read();
    dados.volumeChuvaPercent = (dados.volumeChuvaRaw / 4095.0f) * 100.0f;

    dados.volumeChuvaMmH = percentualParaMmH(dados.volumeChuvaPercent);
    // A definição de alertaRiscoEnchente pode ou não alinhar com a nova lógica dos LEDs.
    // Por enquanto, manteremos a definição original de alertaRiscoEnchente para o display e matriz.
    // A lógica dos LEDs físicos será independente e mais granular.
    dados.alertaRiscoEnchente = (dados.nivelAguaPercent >= 70.0f || dados.volumeChuvaPercent >= 80.0f);


    // Envia os dados para a fila
    xQueueSend(filaDadosSensores, &dados, pdMS_TO_TICKS(10));
    // Envia/Sobrescreve o estado de alerta na filaEstadoAlerta
    if (filaEstadoAlerta != NULL) {
        xQueueOverwrite(filaEstadoAlerta, &dados.alertaRiscoEnchente);
    }


    // Atualiza os buffers dos gráficos a cada 2 segundos
    uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());
    if ((tempoAtual - ultimoTempoGrafico) >= 2000) {
        dadosGraficoChuva[indiceGrafico] = dados.volumeChuvaPercent;
        dadosGraficoNivel[indiceGrafico] = dados.nivelAguaPercent;
        indiceGrafico = (indiceGrafico + 1) % TAMANHO_GRAFICO;
        if (contagemGrafico < TAMANHO_GRAFICO) contagemGrafico++;
        ultimoTempoGrafico = tempoAtual;
    }

    // Lógica de controle dos LEDs Verde e Vermelho (nova ordem de prioridade)

    // Prioridade 1: Nível > 95% (Vermelho Piscando)
    if (dados.nivelAguaPercent > 95.0f) {
        gpio_put(LED_VERDE_PIN, 0); // Verde sempre apagado
        if ((tempoAtual - ultimoTempoPiscoLedVermelho) >= 500) {
            estadoPiscoLedVermelho = !estadoPiscoLedVermelho;
            gpio_put(LED_PIN, estadoPiscoLedVermelho); // LED_PIN é o Vermelho
            ultimoTempoPiscoLedVermelho = tempoAtual;
        }
    }
    // Prioridade 2: Nível < 70% E Chuva > 80% (Amarelo = Verde + Vermelho acesos)
    else if (dados.nivelAguaPercent < 70.0f && dados.volumeChuvaPercent > 80.0f) {
        gpio_put(LED_VERDE_PIN, 1); // Verde aceso
        gpio_put(LED_PIN, 1);     // Vermelho aceso
        estadoPiscoLedVermelho = false; // Garante que o estado de pisco seja resetado
    }
    // Prioridade 3: Nível >= 70% e < 95% E Chuva > 80% (Vermelho Fixo)
    else if (dados.nivelAguaPercent >= 70.0f && dados.nivelAguaPercent < 95.0f && dados.volumeChuvaPercent > 80.0f) {
        gpio_put(LED_VERDE_PIN, 0); // Verde apagado
        gpio_put(LED_PIN, 1);     // Vermelho aceso fixo
        estadoPiscoLedVermelho = false; // Garante que o estado de pisco seja resetado
    }
    // Prioridade 4: Nível < 70% E Chuva <= 80% (Verde Fixo)
    else if (dados.nivelAguaPercent < 70.0f && dados.volumeChuvaPercent <= 80.0f) { // <= 80% para cobrir o normal
        gpio_put(LED_VERDE_PIN, 1); // Verde aceso
        gpio_put(LED_PIN, 0);     // Vermelho apagado
        estadoPiscoLedVermelho = false; // Garante que o estado de pisco seja resetado
    }
    // Outras combinações (ambos apagados)
    else {
        gpio_put(LED_VERDE_PIN, 0); // Verde apagado
        gpio_put(LED_PIN, 0);     // Vermelho apagado
        estadoPiscoLedVermelho = false; // Garante que o estado de pisco seja resetado
    }

    vTaskDelay(pdMS_TO_TICKS(250)); // Delay da TaskMedicao
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

        // Calcula a inclinação (lógica de calcularInclinacao() movida para cá)
        float inclinacao = 0.0f;
        if (contagemHistorico < 2) {
            inclinacao = 0.0f; // Precisa de pelo menos 2 pontos
        } else {
            float somaX = 0.0f, somaY = 0.0f, somaXY = 0.0f, somaX2 = 0.0f;
            int n = (contagemHistorico < TAMANHO_HISTORICO) ? contagemHistorico : TAMANHO_HISTORICO;

            for (int i = 0; i < n; i++) {
                float x_val = (float)i;
                float y_val = historicoNivelAgua[(indiceHistorico - n + i + TAMANHO_HISTORICO) % TAMANHO_HISTORICO];
                somaX += x_val;
                somaY += y_val;
                somaXY += x_val * y_val;
                somaX2 += x_val * x_val;
            }

            float denominador = n * somaX2 - somaX * somaX;
            if (denominador == 0.0f) {
                inclinacao = 0.0f; // Evita divisão por zero
            } else {
                inclinacao = (n * somaXY - somaX * somaY) / denominador;
            }
        }

        float intervalosFuturos = 10.0f; // Previsão para 2.5s à frente (10 * 250ms)
        float nivelPrevisto = dadosRecebidos.nivelAguaPercent + inclinacao * intervalosFuturos;

        if (nivelPrevisto < 0.0f) nivelPrevisto = 0.0f;
        else if (nivelPrevisto > 100.0f) nivelPrevisto = 100.0f;
        dadosEnviar.nivelAguaPrevisto = nivelPrevisto;

        xQueueSend(filaDadosExibicao, &dadosEnviar, pdMS_TO_TICKS(10));
    }
}
}

// Task que exibe os dados no display OLED
void TaskDisplay(void *pvParameters) {
dadosSensores_t dadosSensores;
dadosPrevisao_t dadosPrevisao;
bool estadoAlertaAtual = false; // Default para Normal
char buffer[32];
uint8_t telaAtual = 0;

gpio_init(BUTTON_A_PIN);
gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
gpio_pull_up(BUTTON_A_PIN);

bool estadoBotaoAnterior = gpio_get(BUTTON_A_PIN);
uint32_t tempoUltimoPressionamento = 0;
const uint32_t delayDebounceMs = 200;

while (true) {
    bool estadoBotaoAtual = gpio_get(BUTTON_A_PIN);
    uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());
    if (estadoBotaoAnterior && !estadoBotaoAtual) {
        if ((tempoAtual - tempoUltimoPressionamento) > delayDebounceMs) {
            telaAtual = (telaAtual + 1) % 4;
            tempoUltimoPressionamento = tempoAtual;
            ssd1306_fill(&display, false);
        }
    }
    estadoBotaoAnterior = estadoBotaoAtual;

    if (filaEstadoAlerta != NULL) {
        xQueuePeek(filaEstadoAlerta, &estadoAlertaAtual, 0);
    }

    if (xQueuePeek(filaDadosSensores, &dadosSensores, pdMS_TO_TICKS(50)) == pdPASS) {
        ssd1306_fill(&display, false);

        if (telaAtual == 0) {
            snprintf(buffer, sizeof(buffer), "QntChuva:%.2fmm", dadosSensores.volumeChuvaMmH);
            ssd1306_draw_string(&display, buffer, 0, 0, false);
            snprintf(buffer, sizeof(buffer), "Chuva: %.1f%%", dadosSensores.volumeChuvaPercent);
            ssd1306_draw_string(&display, buffer, 0, 13, false);
            snprintf(buffer, sizeof(buffer), "Nivel: %.1f%%", dadosSensores.nivelAguaPercent);
            ssd1306_draw_string(&display, buffer, 0, 26, false);
            snprintf(buffer, sizeof(buffer), "Status: %s", estadoAlertaAtual ? "ALERTA!" : "Normal"); // Baseado na filaEstadoAlerta
            ssd1306_draw_string(&display, buffer, 0, 39, false);

            // Determina a cor a ser exibida no display baseado na lógica dos LEDs físicos
            // Isso pode ser um pouco redundante se o "Status: ALERTA!" já indicar perigo.
            // Mas para ser explícito sobre a cor simulada:
            const char* corDisplay;
            if (dadosSensores.nivelAguaPercent > 95.0f) {
                corDisplay = "Cor: V. Pisc."; // Vermelho Piscando
            } else if (dadosSensores.nivelAguaPercent < 70.0f && dadosSensores.volumeChuvaPercent > 80.0f) {
                corDisplay = "Cor: Amarelo"; // Amarelo
            } else if (dadosSensores.nivelAguaPercent >= 70.0f && dadosSensores.nivelAguaPercent < 95.0f && dadosSensores.volumeChuvaPercent > 80.0f) {
                corDisplay = "Cor: Vermelho"; // Vermelho Fixo
            } else if (dadosSensores.nivelAguaPercent < 70.0f && dadosSensores.volumeChuvaPercent <= 80.0f) {
                corDisplay = "Cor: Verde";    // Verde
            } else {
                corDisplay = "Cor: Apagado";  // Apagado
            }
            ssd1306_draw_string(&display, corDisplay, 0, 52, false);

        } else if (telaAtual == 1) {
            ssd1306_draw_string(&display, "Barra Chuva:", 0, 0, false);
            uint8_t barYChuva = 10, barWidth = SSD1306_WIDTH - 20, barHeight = 8;
            ssd1306_rect(&display, barYChuva, 0, barWidth, barHeight, true, false);
            uint8_t chuvaFill = (uint8_t)(dadosSensores.volumeChuvaPercent * (barWidth - 2) / 100.0f);
            if (chuvaFill > 0) ssd1306_rect(&display, barYChuva + 1, 1, chuvaFill, barHeight - 2, true, true);

            ssd1306_draw_string(&display, "Barra Nivel:", 0, 25, false);
            uint8_t barYNivel = 35;
            ssd1306_rect(&display, barYNivel, 0, barWidth, barHeight, true, false);
            uint8_t nivelFill = (uint8_t)(dadosSensores.nivelAguaPercent * (barWidth - 2) / 100.0f);
            if (nivelFill > 0) ssd1306_rect(&display, barYNivel + 1, 1, nivelFill, barHeight - 2, true, true);

            if (xQueueReceive(filaDadosExibicao, &dadosPrevisao, 0) == pdPASS) {
                snprintf(buffer, sizeof(buffer), "Previsao:%.1f%%", dadosPrevisao.nivelAguaPrevisto);
            } else {
                snprintf(buffer, sizeof(buffer), "Previsao: N/A");
            }
            ssd1306_draw_string(&display, buffer, 0, 50, false);
        } else if (telaAtual == 2) {
            const uint8_t graficoX = 15;
            const uint8_t graficoY = 54;
            const uint8_t alturaGrafico = 45;
            const uint8_t larguraGrafico = 100;
            const char* titulo = "Chuva %";
            uint8_t tituloWidth = strlen(titulo) * 5;
            uint8_t tituloX_pos = (SSD1306_WIDTH - tituloWidth) / 2;
            ssd1306_draw_string(&display, titulo, tituloX_pos, 5, true);
            ssd1306_line(&display, graficoX, graficoY, graficoX + larguraGrafico, graficoY, true);
            ssd1306_line(&display, graficoX, graficoY, graficoX, graficoY - alturaGrafico, true);
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
            for (int i = 0; i <= 5; i++) {
                uint8_t y_mark = graficoY - (i * alturaGrafico / 5);
                ssd1306_line(&display, graficoX - 3, y_mark, graficoX, y_mark, true);
                if (i % 2 == 0) {
                    snprintf(buffer, sizeof(buffer), "%d", i * 20);
                    ssd1306_draw_string(&display, buffer, 0, y_mark - 3, true);
                }
            }
            for (int i = 0; i <= 4; i++) {
                uint8_t x_mark = graficoX + (i * larguraGrafico / 4);
                ssd1306_line(&display, x_mark, graficoY, x_mark, graficoY + 2, true);
                snprintf(buffer, sizeof(buffer), "%d", i * 5);
                ssd1306_draw_string(&display, buffer, x_mark - 8, graficoY + 2, true);
            }
        } else if (telaAtual == 3) {
            const uint8_t graficoX = 15;
            const uint8_t graficoY = 54;
            const uint8_t alturaGrafico = 45;
            const uint8_t larguraGrafico = 100;
            const char* titulo = "Nivel %";
            uint8_t tituloWidth = strlen(titulo) * 5;
            uint8_t tituloX_pos = (SSD1306_WIDTH - tituloWidth) / 2;
            ssd1306_draw_string(&display, titulo, tituloX_pos, 5, true);
            ssd1306_line(&display, graficoX, graficoY, graficoX + larguraGrafico, graficoY, true);
            ssd1306_line(&display, graficoX, graficoY, graficoX, graficoY - alturaGrafico, true);
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
            for (int i = 0; i <= 5; i++) {
                uint8_t y_mark = graficoY - (i * alturaGrafico / 5);
                ssd1306_line(&display, graficoX - 3, y_mark, graficoX, y_mark, true);
                if (i % 2 == 0) {
                    snprintf(buffer, sizeof(buffer), "%d", i * 20);
                    ssd1306_draw_string(&display, buffer, 0, y_mark - 3, true);
                }
            }
            for (int i = 0; i <= 4; i++) {
                uint8_t x_mark = graficoX + (i * larguraGrafico / 4);
                ssd1306_line(&display, x_mark, graficoY, x_mark, graficoY + 2, true);
                snprintf(buffer, sizeof(buffer), "%d", i * 5);
                ssd1306_draw_string(&display, buffer, x_mark - 8, graficoY + 2, true);
            }
        }
        ssd1306_send_data(&display);
    }
}
}

// Task que controla a matriz de LEDs
void TaskMatrizLED(void *pvParameters) {
dadosSensores_t dados;
bool estadoAlertaRecebido = false; // Default para Normal
const uint32_t cor_vermelho_matriz = COR_VERMELHO; // Renomeado para evitar conflito com cor_vermelho no display
const uint32_t cor_azul_matriz = COR_AZUL;       // Renomeado
const uint32_t cor_amarelo_matriz = COR_AMARELO;   // Renomeado
uint8_t estadoExibicao = 0;
uint32_t ultimoTempoAlternancia = 0;
static bool first_entry_chuva_alta_after_no_chuva = true;

while (true) {
    if (filaEstadoAlerta != NULL) {
        xQueuePeek(filaEstadoAlerta, &estadoAlertaRecebido, 0);
    }

    if (xQueuePeek(filaDadosSensores, &dados, pdMS_TO_TICKS(100)) == pdPASS) {
        bool chuvaAlta = (dados.volumeChuvaPercent > 80.0f); // Usado para lógica da matriz
        uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());

        // A lógica da matriz de LEDs é baseada no estadoAlertaRecebido e chuvaAlta.
        // O estadoAlertaRecebido é definido por (dados.nivelAguaPercent >= 70.0f || dados.volumeChuvaPercent >= 80.0f)
        if (estadoAlertaRecebido) {
            if (chuvaAlta) { // Se alerta e especificamente chuva alta
                if (first_entry_chuva_alta_after_no_chuva) {
                    estadoExibicao = 0;
                    ultimoTempoAlternancia = tempoAtual;
                    first_entry_chuva_alta_after_no_chuva = false;
                } else if ((tempoAtual - ultimoTempoAlternancia >= 4000)) {
                    estadoExibicao = (estadoExibicao + 1) % 3;
                    ultimoTempoAlternancia = tempoAtual;
                }

                if (estadoExibicao == 0) {
                    matriz_draw_rain_animation(cor_azul_matriz);
                } else if (estadoExibicao == 1) {
                    matriz_draw_pattern(PAD_EXC, cor_amarelo_matriz);
                } else {
                    matriz_draw_pattern(PAD_X, cor_vermelho_matriz);
                }
            } else { // Alerta, mas não chuvaAlta (implica nivelAguaPercent >= 70%)
                matriz_draw_pattern(PAD_X, cor_vermelho_matriz);
                first_entry_chuva_alta_after_no_chuva = true;
                estadoExibicao = 0;
            }
        } else { // Sem alerta
            matriz_clear();
            first_entry_chuva_alta_after_no_chuva = true;
            estadoExibicao = 0;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}
}

// FUNÇÃO PRINCIPAL
int main() {
stdio_init_all();
sleep_ms(2000);

i2c_init(I2C_PORT, 100 * 1000);
gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
gpio_pull_up(I2C_SDA_PIN);
gpio_pull_up(I2C_SCL_PIN);

ssd1306_init(&display, SSD1306_WIDTH, SSD1306_HEIGHT, false, SSD1306_I2C_ADDR, I2C_PORT);
ssd1306_config(&display);
ssd1306_fill(&display, false);
ssd1306_draw_string(&display, "Iniciando...", 0, 28, false);
ssd1306_send_data(&display);

inicializar_matriz_led();

filaDadosSensores = xQueueCreate(10, sizeof(dadosSensores_t));
filaDadosExibicao = xQueueCreate(5, sizeof(dadosPrevisao_t));
filaEstadoAlerta = xQueueCreate(1, sizeof(bool));
if (filaDadosSensores == NULL || filaDadosExibicao == NULL || filaEstadoAlerta == NULL) {
    while(1);
}

xTaskCreate(TaskMedicao, "Leitura", 512, NULL, 2, NULL);
xTaskCreate(TaskPrevisao, "Previsao", configMINIMAL_STACK_SIZE + 256, NULL, 1, NULL);
xTaskCreate(TaskDisplay, "Exibicao", configMINIMAL_STACK_SIZE + 512, NULL, 1, NULL);
xTaskCreate(TaskMatrizLED, "MatrizLED", configMINIMAL_STACK_SIZE + 768, NULL, 1, NULL);

vTaskStartScheduler();

while (true) {
    sleep_ms(1000);
}

return 0;
}
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"  // Added for PWM support
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
#define LED_PIN 13 // Pino do LED vermelho
#define LED_VERDE_PIN 11 // Pino do LED verde
#define BUZZER_PIN 10 // Pino do Buzzer

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
static QueueHandle_t filaEstadoAlerta = NULL;

// Instância do display OLED
static ssd1306_t display;

// Histórico para previsão (buffer circular)
#define TAMANHO_HISTORICO 5
static float historicoNivelAgua[TAMANHO_HISTORICO];
static float historicoVolumeChuva[TAMANHO_HISTORICO];
static int indiceHistorico = 0;
static int contagemHistorico = 0;

// Buffers para dados dos gráficos
#define TAMANHO_GRAFICO 10
static float dadosGraficoChuva[TAMANHO_GRAFICO];
static float dadosGraficoNivel[TAMANHO_GRAFICO];
static int indiceGrafico = 0;
static int contagemGrafico = 0;
static uint32_t ultimoTempoGrafico = 0;

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

// Funções para controlar o buzzer com PWM
void buzzer_on(uint frequency) {
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    uint channel = pwm_gpio_to_channel(BUZZER_PIN);
    
    // Configura a frequência do PWM
    pwm_set_clkdiv(slice_num, 125.0f); // Base clock divider (adjust as needed)
    uint32_t top = 1000000 / frequency - 1; // Calculate TOP value for desired frequency
    pwm_set_wrap(slice_num, top);
    pwm_set_chan_level(slice_num, channel, top / 15); //  duty cycle dividido por 15 para diminuir som
    
    // Habilita o PWM no pino
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    pwm_set_enabled(slice_num, true);
}

void buzzer_off() {
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_enabled(slice_num, false); // Desliga o PWM
    gpio_init(BUZZER_PIN); // Restaura o pino como GPIO
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0); // Garante que o buzzer esteja desligado
}

// TASKS

// Task que lê os sensores
void TaskMedicao(void *pvParameters) {
    adc_init();
    adc_gpio_init(ADC_JOYSTICK_X_PIN);
    adc_gpio_init(ADC_JOYSTICK_Y_PIN);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    gpio_init(LED_VERDE_PIN);
    gpio_set_dir(LED_VERDE_PIN, GPIO_OUT);
    gpio_put(LED_VERDE_PIN, 0);

    dadosSensores_t dados;
    static uint32_t ultimoTempoPiscoLedVermelho = 0;
    static bool estadoPiscoLedVermelho = false;

    while (true) {
        adc_select_input(1);
        dados.nivelAguaRaw = adc_read();
        dados.nivelAguaPercent = (dados.nivelAguaRaw / 4095.0f) * 100.0f;

        adc_select_input(0);
        dados.volumeChuvaRaw = adc_read();
        dados.volumeChuvaPercent = (dados.volumeChuvaRaw / 4095.0f) * 100.0f;

        dados.volumeChuvaMmH = percentualParaMmH(dados.volumeChuvaPercent);
        dados.alertaRiscoEnchente = (dados.nivelAguaPercent >= 70.0f || dados.volumeChuvaPercent >= 80.0f);

        xQueueSend(filaDadosSensores, &dados, pdMS_TO_TICKS(10));
        if (filaEstadoAlerta != NULL) {
            xQueueOverwrite(filaEstadoAlerta, &dados.alertaRiscoEnchente);
        }

        uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());
        if ((tempoAtual - ultimoTempoGrafico) >= 2000) {
            dadosGraficoChuva[indiceGrafico] = dados.volumeChuvaPercent;
            dadosGraficoNivel[indiceGrafico] = dados.nivelAguaPercent;
            indiceGrafico = (indiceGrafico + 1) % TAMANHO_GRAFICO;
            if (contagemGrafico < TAMANHO_GRAFICO) contagemGrafico++;
            ultimoTempoGrafico = tempoAtual;
        }

        if (dados.nivelAguaPercent > 95.0f) {
            gpio_put(LED_VERDE_PIN, 0);
            if ((tempoAtual - ultimoTempoPiscoLedVermelho) >= 500) {
                estadoPiscoLedVermelho = !estadoPiscoLedVermelho;
                gpio_put(LED_PIN, estadoPiscoLedVermelho);
                ultimoTempoPiscoLedVermelho = tempoAtual;
            }
        }
        else if (dados.nivelAguaPercent < 70.0f && dados.volumeChuvaPercent > 80.0f) {
            gpio_put(LED_VERDE_PIN, 1);
            gpio_put(LED_PIN, 1);
            estadoPiscoLedVermelho = false;
        }
        else if (dados.nivelAguaPercent >= 70.0f && dados.nivelAguaPercent < 95.0f && dados.volumeChuvaPercent > 80.0f) {
            gpio_put(LED_VERDE_PIN, 0);
            gpio_put(LED_PIN, 1);
            estadoPiscoLedVermelho = false;
        }
        else if (dados.nivelAguaPercent < 70.0f && dados.volumeChuvaPercent <= 80.0f) {
            gpio_put(LED_VERDE_PIN, 1);
            gpio_put(LED_PIN, 0);
            estadoPiscoLedVermelho = false;
        }
        else {
            gpio_put(LED_VERDE_PIN, 0);
            gpio_put(LED_PIN, 0);
            estadoPiscoLedVermelho = false;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// Task que faz a previsão
void TaskPrevisao(void *pvParameters) {
    dadosSensores_t dadosRecebidos;
    dadosPrevisao_t dadosEnviar;
    const float fatorChuva = 0.1f; // Coeficiente ajustável para o impacto da chuva

    while (true) {
        if (xQueueReceive(filaDadosSensores, &dadosRecebidos, pdMS_TO_TICKS(100)) == pdPASS) {
            // Atualiza o histórico
            historicoNivelAgua[indiceHistorico] = dadosRecebidos.nivelAguaPercent;
            historicoVolumeChuva[indiceHistorico] = dadosRecebidos.volumeChuvaMmH;
            indiceHistorico = (indiceHistorico + 1) % TAMANHO_HISTORICO;
            if (contagemHistorico < TAMANHO_HISTORICO) contagemHistorico++;

            // Calcula a inclinação (tendência) do nível de água
            float inclinacao = 0.0f;
            if (contagemHistorico >= 2) {
                float somaX = 0.0f, somaY = 0.0f, somaXY = 0.0f, somaX2 = 0.0f;
                int n = contagemHistorico;
                for (int i = 0; i < n; i++) {
                    float x_val = (float)i;
                    float y_val = historicoNivelAgua[(indiceHistorico - n + i + TAMANHO_HISTORICO) % TAMANHO_HISTORICO];
                    somaX += x_val;
                    somaY += y_val;
                    somaXY += x_val * y_val;
                    somaX2 += x_val * x_val;
                }
                float denominador = n * somaX2 - somaX * somaX;
                if (denominador != 0.0f) {
                    inclinacao = (n * somaXY - somaX * somaY) / denominador;
                }
            }

            // Previsão do nível de água com ajuste pela chuva
            float intervalosFuturos = 10.0f;
            float nivelPrevisto = dadosRecebidos.nivelAguaPercent + (inclinacao * intervalosFuturos);
            float chuvaAtual = dadosRecebidos.volumeChuvaMmH;
            nivelPrevisto += fatorChuva * chuvaAtual;

            // Limita a previsão entre 0% e 100%
            if (nivelPrevisto < 0.0f) nivelPrevisto = 0.0f;
            else if (nivelPrevisto > 100.0f) nivelPrevisto = 100.0f;

            dadosEnviar.nivelAguaPrevisto = nivelPrevisto;
            xQueueSend(filaDadosExibicao, &dadosEnviar, pdMS_TO_TICKS(10));
        }
    }
}

// Task que exibe no display OLED
void TaskDisplay(void *pvParameters) {
    dadosSensores_t dadosSensores;
    dadosPrevisao_t dadosPrevisao;
    bool estadoAlertaAtual = false;
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
                snprintf(buffer, sizeof(buffer), "Status: %s", estadoAlertaAtual ? "ALERTA!" : "Normal");
                ssd1306_draw_string(&display, buffer, 0, 39, false);
                const char* corDisplay;
                if (dadosSensores.nivelAguaPercent > 95.0f) {
                    corDisplay = "Cor: V. Pisc.";
                } else if (dadosSensores.nivelAguaPercent < 70.0f && dadosSensores.volumeChuvaPercent > 80.0f) {
                    corDisplay = "Cor: Amarelo";
                } else if (dadosSensores.nivelAguaPercent >= 70.0f && dadosSensores.nivelAguaPercent < 95.0f && dadosSensores.volumeChuvaPercent > 80.0f) {
                    corDisplay = "Cor: Vermelho";
                } else if (dadosSensores.nivelAguaPercent < 70.0f && dadosSensores.volumeChuvaPercent <= 80.0f) {
                    corDisplay = "Cor: Verde";
                } else {
                    corDisplay = "Cor: Apagado";
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
                const uint8_t graficoX = 15; const uint8_t graficoY = 54;
                const uint8_t alturaGrafico = 45; const uint8_t larguraGrafico = 100;
                const char* titulo = "Chuva %"; uint8_t tituloWidth = strlen(titulo) * 5;
                uint8_t tituloX_pos = (SSD1306_WIDTH - tituloWidth) / 2;
                ssd1306_draw_string(&display, titulo, tituloX_pos, 5, true);
                ssd1306_line(&display, graficoX, graficoY, graficoX + larguraGrafico, graficoY, true);
                ssd1306_line(&display, graficoX, graficoY, graficoX, graficoY - alturaGrafico, true);
                int n = (contagemGrafico < TAMANHO_GRAFICO) ? contagemGrafico : TAMANHO_GRAFICO;
                for (int i = 0; i < n - 1; i++) {
                    int idxAtual = (indiceGrafico - n + i + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    int idxProximo = (indiceGrafico - n + i + 1 + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    float chuvaAtual = dadosGraficoChuva[idxAtual]; float chuvaProxima = dadosGraficoChuva[idxProximo];
                    uint8_t yAtual = graficoY - (uint8_t)(chuvaAtual * alturaGrafico / 100.0f);
                    uint8_t yProximo = graficoY - (uint8_t)(chuvaProxima * alturaGrafico / 100.0f);
                    uint8_t xAtual = graficoX + (i * larguraGrafico / (TAMANHO_GRAFICO - 1));
                    uint8_t xProximo = graficoX + ((i + 1) * larguraGrafico / (TAMANHO_GRAFICO - 1));
                    ssd1306_line(&display, xAtual, yAtual, xProximo, yProximo, true);
                }
                for (int i = 0; i <= 5; i++) {
                    uint8_t y_mark = graficoY - (i * alturaGrafico / 5);
                    ssd1306_line(&display, graficoX - 3, y_mark, graficoX, y_mark, true);
                    if (i % 2 == 0) { snprintf(buffer, sizeof(buffer), "%d", i * 20); ssd1306_draw_string(&display, buffer, 0, y_mark - 3, true); }
                }
                for (int i = 0; i <= 4; i++) {
                    uint8_t x_mark = graficoX + (i * larguraGrafico / 4);
                    ssd1306_line(&display, x_mark, graficoY, x_mark, graficoY + 2, true);
                    snprintf(buffer, sizeof(buffer), "%d", i * 5); ssd1306_draw_string(&display, buffer, x_mark - 8, graficoY + 2, true);
                }
            } else if (telaAtual == 3) {
                const uint8_t graficoX = 15; const uint8_t graficoY = 54;
                const uint8_t alturaGrafico = 45; const uint8_t larguraGrafico = 100;
                const char* titulo = "Nivel %"; uint8_t tituloWidth = strlen(titulo) * 5;
                uint8_t tituloX_pos = (SSD1306_WIDTH - tituloWidth) / 2;
                ssd1306_draw_string(&display, titulo, tituloX_pos, 5, true);
                ssd1306_line(&display, graficoX, graficoY, graficoX + larguraGrafico, graficoY, true);
                ssd1306_line(&display, graficoX, graficoY, graficoX, graficoY - alturaGrafico, true);
                int n = (contagemGrafico < TAMANHO_GRAFICO) ? contagemGrafico : TAMANHO_GRAFICO;
                for (int i = 0; i < n - 1; i++) {
                    int idxAtual = (indiceGrafico - n + i + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    int idxProximo = (indiceGrafico - n + i + 1 + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    float nivelAtual = dadosGraficoNivel[idxAtual]; float nivelProximo = dadosGraficoNivel[idxProximo];
                    uint8_t yAtual = graficoY - (uint8_t)(nivelAtual * alturaGrafico / 100.0f);
                    uint8_t yProximo = graficoY - (uint8_t)(nivelProximo * alturaGrafico / 100.0f);
                    uint8_t xAtual = graficoX + (i * larguraGrafico / (TAMANHO_GRAFICO - 1));
                    uint8_t xProximo = graficoX + ((i + 1) * larguraGrafico / (TAMANHO_GRAFICO - 1));
                    ssd1306_line(&display, xAtual, yAtual, xProximo, yProximo, true);
                }
                for (int i = 0; i <= 5; i++) {
                    uint8_t y_mark = graficoY - (i * alturaGrafico / 5);
                    ssd1306_line(&display, graficoX - 3, y_mark, graficoX, y_mark, true);
                    if (i % 2 == 0) { snprintf(buffer, sizeof(buffer), "%d", i * 20); ssd1306_draw_string(&display, buffer, 0, y_mark - 3, true); }
                }
                for (int i = 0; i <= 4; i++) {
                    uint8_t x_mark = graficoX + (i * larguraGrafico / 4);
                    ssd1306_line(&display, x_mark, graficoY, x_mark, graficoY + 2, true);
                    snprintf(buffer, sizeof(buffer), "%d", i * 5); ssd1306_draw_string(&display, buffer, x_mark - 8, graficoY + 2, true);
                }
            }
            ssd1306_send_data(&display);
        }
    }
}

// Task que controla a matriz de LEDs
void TaskMatrizLED(void *pvParameters) {
    dadosSensores_t dados;
    bool estadoAlertaRecebido = false;
    const uint32_t cor_vermelho_matriz = COR_VERMELHO;
    const uint32_t cor_azul_matriz = COR_AZUL;
    const uint32_t cor_amarelo_matriz = COR_AMARELO;
    uint8_t estadoExibicao = 0;
    uint32_t ultimoTempoAlternancia = 0;
    static bool first_entry_chuva_alta_after_no_chuva = true;

    while (true) {
        if (filaEstadoAlerta != NULL) {
            xQueuePeek(filaEstadoAlerta, &estadoAlertaRecebido, 0);
        }
        if (xQueuePeek(filaDadosSensores, &dados, pdMS_TO_TICKS(100)) == pdPASS) {
            bool chuvaAlta = (dados.volumeChuvaPercent > 80.0f);
            uint32_t tempoAtual = to_ms_since_boot(get_absolute_time());
            if (estadoAlertaRecebido) {
                if (chuvaAlta) {
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
                } else {
                    matriz_draw_pattern(PAD_X, cor_vermelho_matriz);
                    first_entry_chuva_alta_after_no_chuva = true;
                    estadoExibicao = 0;
                }
            } else {
                matriz_clear();
                first_entry_chuva_alta_after_no_chuva = true;
                estadoExibicao = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Task que controla o buzzer
void TaskBuzzer(void *pvParameters) {
    dadosSensores_t dadosAtuais;

    while (true) {
        if (xQueuePeek(filaDadosSensores, &dadosAtuais, pdMS_TO_TICKS(50)) == pdPASS) {
            float nivel = dadosAtuais.nivelAguaPercent;
            float chuva = dadosAtuais.volumeChuvaPercent;

            // Prioridade 1: Nível > 70% E Chuva > 80%
            if (nivel > 70.0f && chuva > 80.0f) {
                buzzer_on(1000); // "BEEP" de 1000 Hz
                vTaskDelay(pdMS_TO_TICKS(1000));
                buzzer_off();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            // Prioridade 2: Chuva > 80% (e Nível <= 70%)
            else if (chuva > 80.0f) {
                buzzer_on(1000);
                vTaskDelay(pdMS_TO_TICKS(150));
                buzzer_off();
                vTaskDelay(pdMS_TO_TICKS(150));
                buzzer_on(1000);
                vTaskDelay(pdMS_TO_TICKS(150));
                buzzer_off();
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            // Prioridade 3: Nível > 70% (e Chuva <= 80%)
            else if (nivel > 70.0f) {
                buzzer_on(1000);
                vTaskDelay(pdMS_TO_TICKS(200));
                buzzer_off();
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            // Nenhuma condição de alerta para o buzzer
            else {
                buzzer_off();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else {
            buzzer_off();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
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
    xTaskCreate(TaskBuzzer, "Buzzer", configMINIMAL_STACK_SIZE + 256, NULL, 1, NULL);

    vTaskStartScheduler();

    while (true) {
        sleep_ms(1000);
    }

    return 0;
}
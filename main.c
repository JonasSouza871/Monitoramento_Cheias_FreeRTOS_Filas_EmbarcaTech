#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"  // Biblioteca para suporte a PWM
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "ssd1306.h"
#include "matriz_led.h"

// --- DEFINIÇÕES DE PINOS E CONSTANTES ---
#define I2C_PORT i2c1
#define I2C_SDA_PIN 14              // Pino SDA para comunicação I2C
#define I2C_SCL_PIN 15              // Pino SCL para comunicação I2C
#define SSD1306_I2C_ADDR 0x3C       // Endereço I2C do display OLED
#define SSD1306_WIDTH 128           // Largura do display OLED
#define SSD1306_HEIGHT 64           // Altura do display OLED
#define BUTTON_A_PIN 5              // Pino do botão A
#define ADC_JOYSTICK_X_PIN 27       // GPIO 27 → ADC1 (nível de água)
#define ADC_JOYSTICK_Y_PIN 26       // GPIO 26 → ADC0 (volume de chuva)
#define LED_PIN 13                  // Pino do LED vermelho
#define LED_VERDE_PIN 11            // Pino do LED verde
#define BUZZER_PIN 10               // Pino do buzzer

// --- ESTRUTURAS DE DADOS ---
typedef struct {
    uint16_t nivel_agua_raw;        // Valor bruto do nível de água lido pelo ADC
    uint16_t volume_chuva_raw;      // Valor bruto do volume de chuva lido pelo ADC
    float nivel_agua_percent;       // Nível de água em porcentagem (0-100%)
    float volume_chuva_percent;     // Volume de chuva em porcentagem (0-100%)
    float volume_chuva_mmh;         // Volume de chuva convertido para mm/h
    bool alerta_risco_enchente;     // Indica se há risco de enchente
} dados_sensores_t;

typedef struct {
    float nivel_agua_previsto;      // Previsão do nível de água em porcentagem
} dados_previsao_t;

// --- FILAS PARA COMUNICAÇÃO ENTRE TAREFAS ---
static QueueHandle_t fila_dados_sensores = NULL;   // Fila para dados dos sensores
static QueueHandle_t fila_dados_exibicao = NULL;   // Fila para dados de previsão
static QueueHandle_t fila_estado_alerta = NULL;    // Fila para estado de alerta

// --- VARIÁVEIS GLOBAIS ---
static ssd1306_t display;                          // Instância do display OLED

// Histórico para previsão (buffer circular)
#define TAMANHO_HISTORICO 5
static float historico_nivel_agua[TAMANHO_HISTORICO];    // Histórico de níveis de água
static float historico_volume_chuva[TAMANHO_HISTORICO];  // Histórico de volumes de chuva
static int indice_historico = 0;                         // Índice atual do histórico
static int contagem_historico = 0;                       // Contagem de entradas no histórico

// Buffers para gráficos no display
#define TAMANHO_GRAFICO 10
static float dados_grafico_chuva[TAMANHO_GRAFICO];   // Dados de chuva para gráfico
static float dados_grafico_nivel[TAMANHO_GRAFICO];   // Dados de nível para gráfico
static int indice_grafico = 0;                       // Índice atual do gráfico
static int contagem_grafico = 0;                     // Contagem de entradas no gráfico
static uint32_t ultimo_tempo_grafico = 0;            // Última atualização do gráfico

// --- FUNÇÕES AUXILIARES ---

// Converte percentual de chuva para mm/h baseado em faixas predefinidas
float percentual_para_mmh(float percentual) {
    if (percentual <= 0.0f) return 0.0f;
    else if (percentual < 30.0f) return (percentual / 30.0f) * 5.0f;         // 0-5 mm/h
    else if (percentual < 60.0f) return 5.0f + ((percentual - 30.0f) / 30.0f) * 10.0f; // 5-15 mm/h
    else if (percentual < 80.0f) return 15.0f + ((percentual - 60.0f) / 20.0f) * 15.0f; // 15-30 mm/h
    else if (percentual < 95.0f) return 30.0f + ((percentual - 80.0f) / 15.0f) * 5.0f;  // 30-35 mm/h
    else return 35.0f; // Máximo de 35 mm/h
}

// Liga o buzzer com uma frequência específica usando PWM
void ligar_buzzer(uint frequency) {
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    uint channel = pwm_gpio_to_channel(BUZZER_PIN);

    pwm_set_clkdiv(slice_num, 125.0f); // Define divisor de clock para PWM
    uint32_t top = 1000000 / frequency - 1; // Calcula valor TOP para a frequência
    pwm_set_wrap(slice_num, top);
    pwm_set_chan_level(slice_num, channel, top / 15); // Ciclo de trabalho reduzido

    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM); // Configura pino para PWM
    pwm_set_enabled(slice_num, true); // Habilita PWM
}

// Desliga o buzzer e restaura o pino como GPIO
void desligar_buzzer() {
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_enabled(slice_num, false); // Desativa PWM
    gpio_init(BUZZER_PIN); // Reinicia o pino
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0); // Garante que o buzzer esteja desligado
}

// --- TAREFAS ---

// Tarefa responsável por ler os sensores e atualizar LEDs
void tarefa_medicao(void *pvParameters) {
    // Inicializa o ADC e os pinos correspondentes
    adc_init();
    adc_gpio_init(ADC_JOYSTICK_X_PIN);
    adc_gpio_init(ADC_JOYSTICK_Y_PIN);

    // Configura os LEDs como saídas
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
    gpio_init(LED_VERDE_PIN);
    gpio_set_dir(LED_VERDE_PIN, GPIO_OUT);
    gpio_put(LED_VERDE_PIN, 0);

    dados_sensores_t dados;
    static uint32_t ultimo_tempo_pisco_led_vermelho = 0;
    static bool estado_pisco_led_vermelho = false;

    while (true) {
        // Lê o nível de água (ADC1)
        adc_select_input(1);
        dados.nivel_agua_raw = adc_read();
        dados.nivel_agua_percent = (dados.nivel_agua_raw / 4095.0f) * 100.0f;

        // Lê o volume de chuva (ADC0)
        adc_select_input(0);
        dados.volume_chuva_raw = adc_read();
        dados.volume_chuva_percent = (dados.volume_chuva_raw / 4095.0f) * 100.0f;

        // Converte percentual de chuva para mm/h
        dados.volume_chuva_mmh = percentual_para_mmh(dados.volume_chuva_percent);

        // Define condição de alerta de enchente
        dados.alerta_risco_enchente = (dados.nivel_agua_percent >= 70.0f || dados.volume_chuva_percent >= 80.0f);

        // Envia dados para as filas
        xQueueSend(fila_dados_sensores, &dados, pdMS_TO_TICKS(10));
        if (fila_estado_alerta != NULL) {
            xQueueOverwrite(fila_estado_alerta, &dados.alerta_risco_enchente);
        }

        // Atualiza os dados do gráfico a cada 2 segundos
        uint32_t tempo_atual = to_ms_since_boot(get_absolute_time());
        if ((tempo_atual - ultimo_tempo_grafico) >= 2000) {
            dados_grafico_chuva[indice_grafico] = dados.volume_chuva_percent;
            dados_grafico_nivel[indice_grafico] = dados.nivel_agua_percent;
            indice_grafico = (indice_grafico + 1) % TAMANHO_GRAFICO;
            if (contagem_grafico < TAMANHO_GRAFICO) contagem_grafico++;
            ultimo_tempo_grafico = tempo_atual;
        }

        // Controle dos LEDs com base nas condições
        if (dados.nivel_agua_percent > 95.0f) {
            gpio_put(LED_VERDE_PIN, 0);
            if ((tempo_atual - ultimo_tempo_pisco_led_vermelho) >= 500) {
                estado_pisco_led_vermelho = !estado_pisco_led_vermelho;
                gpio_put(LED_PIN, estado_pisco_led_vermelho);
                ultimo_tempo_pisco_led_vermelho = tempo_atual;
            }
        } else if (dados.nivel_agua_percent < 70.0f && dados.volume_chuva_percent > 80.0f) {
            gpio_put(LED_VERDE_PIN, 1);
            gpio_put(LED_PIN, 1);
            estado_pisco_led_vermelho = false;
        } else if (dados.nivel_agua_percent >= 70.0f && dados.nivel_agua_percent < 95.0f && dados.volume_chuva_percent > 80.0f) {
            gpio_put(LED_VERDE_PIN, 0);
            gpio_put(LED_PIN, 1);
            estado_pisco_led_vermelho = false;
        } else if (dados.nivel_agua_percent < 70.0f && dados.volume_chuva_percent <= 80.0f) {
            gpio_put(LED_VERDE_PIN, 1);
            gpio_put(LED_PIN, 0);
            estado_pisco_led_vermelho = false;
        } else {
            gpio_put(LED_VERDE_PIN, 0);
            gpio_put(LED_PIN, 0);
            estado_pisco_led_vermelho = false;
        }
        vTaskDelay(pdMS_TO_TICKS(250)); // Aguarda 250ms antes da próxima leitura
    }
}

// Tarefa que realiza a previsão do nível de água
void tarefa_previsao(void *pvParameters) {
    dados_sensores_t dados_recebidos;
    dados_previsao_t dados_enviar;
    const float fator_chuva = 0.1f; // Fator de impacto da chuva na previsão

    while (true) {
        if (xQueueReceive(fila_dados_sensores, &dados_recebidos, pdMS_TO_TICKS(100)) == pdPASS) {
            // Atualiza o histórico de dados
            historico_nivel_agua[indice_historico] = dados_recebidos.nivel_agua_percent;
            historico_volume_chuva[indice_historico] = dados_recebidos.volume_chuva_mmh;
            indice_historico = (indice_historico + 1) % TAMANHO_HISTORICO;
            if (contagem_historico < TAMANHO_HISTORICO) contagem_historico++;

            // Calcula a tendência (inclinação) do nível de água
            float inclinacao = 0.0f;
            if (contagem_historico >= 2) {
                float soma_x = 0.0f, soma_y = 0.0f, soma_xy = 0.0f, soma_x2 = 0.0f;
                int n = contagem_historico;
                for (int i = 0; i < n; i++) {
                    float x_val = (float)i;
                    float y_val = historico_nivel_agua[(indice_historico - n + i + TAMANHO_HISTORICO) % TAMANHO_HISTORICO];
                    soma_x += x_val;
                    soma_y += y_val;
                    soma_xy += x_val * y_val;
                    soma_x2 += x_val * x_val;
                }
                float denominador = n * soma_x2 - soma_x * soma_x;
                if (denominador != 0.0f) {
                    inclinacao = (n * soma_xy - soma_x * soma_y) / denominador;
                }
            }

            // Calcula previsão considerando tendência e impacto da chuva
            float intervalos_futuros = 10.0f;
            float nivel_previsto = dados_recebidos.nivel_agua_percent + (inclinacao * intervalos_futuros);
            nivel_previsto += fator_chuva * dados_recebidos.volume_chuva_mmh;

            // Limita a previsão entre 0% e 100%
            if (nivel_previsto < 0.0f) nivel_previsto = 0.0f;
            else if (nivel_previsto > 100.0f) nivel_previsto = 100.0f;

            dados_enviar.nivel_agua_previsto = nivel_previsto;
            xQueueSend(fila_dados_exibicao, &dados_enviar, pdMS_TO_TICKS(10));
        }
    }
}

// Tarefa que exibe informações no display OLED
void tarefa_exibicao(void *pvParameters) {
    dados_sensores_t dados_sensores;
    dados_previsao_t dados_previsao;
    bool estado_alerta_atual = false;
    char buffer[32];
    uint8_t tela_atual = 0;

    // Configura o botão para alternar telas
    gpio_init(BUTTON_A_PIN);
    gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_A_PIN);
    bool estado_botao_anterior = gpio_get(BUTTON_A_PIN);
    uint32_t tempo_ultimo_pressionamento = 0;
    const uint32_t delay_debounce_ms = 200;

    while (true) {
        // Detecta pressionamento do botão com debounce
        bool estado_botao_atual = gpio_get(BUTTON_A_PIN);
        uint32_t tempo_atual = to_ms_since_boot(get_absolute_time());
        if (estado_botao_anterior && !estado_botao_atual) {
            if ((tempo_atual - tempo_ultimo_pressionamento) > delay_debounce_ms) {
                tela_atual = (tela_atual + 1) % 4;
                tempo_ultimo_pressionamento = tempo_atual;
                ssd1306_fill(&display, false); // Limpa o display
            }
        }
        estado_botao_anterior = estado_botao_atual;

        // Verifica o estado de alerta
        if (fila_estado_alerta != NULL) {
            xQueuePeek(fila_estado_alerta, &estado_alerta_atual, 0);
        }

        // Exibe dados no display conforme a tela selecionada
        if (xQueuePeek(fila_dados_sensores, &dados_sensores, pdMS_TO_TICKS(50)) == pdPASS) {
            ssd1306_fill(&display, false);
            if (tela_atual == 0) {
                // Tela 1: Informações básicas
                snprintf(buffer, sizeof(buffer), "QntChuva:%.2fmm", dados_sensores.volume_chuva_mmh);
                ssd1306_draw_string(&display, buffer, 0, 0, false);
                snprintf(buffer, sizeof(buffer), "Chuva: %.1f%%", dados_sensores.volume_chuva_percent);
                ssd1306_draw_string(&display, buffer, 0, 13, false);
                snprintf(buffer, sizeof(buffer), "Nivel: %.1f%%", dados_sensores.nivel_agua_percent);
                ssd1306_draw_string(&display, buffer, 0, 26, false);
                snprintf(buffer, sizeof(buffer), "Status: %s", estado_alerta_atual ? "ALERTA!" : "Normal");
                ssd1306_draw_string(&display, buffer, 0, 39, false);
                const char* cor_display;
                if (dados_sensores.nivel_agua_percent > 95.0f) cor_display = "Cor: V. Pisc.";
                else if (dados_sensores.nivel_agua_percent < 70.0f && dados_sensores.volume_chuva_percent > 80.0f) cor_display = "Cor: Amarelo";
                else if (dados_sensores.nivel_agua_percent >= 70.0f && dados_sensores.nivel_agua_percent < 95.0f && dados_sensores.volume_chuva_percent > 80.0f) cor_display = "Cor: Vermelho";
                else if (dados_sensores.nivel_agua_percent < 70.0f && dados_sensores.volume_chuva_percent <= 80.0f) cor_display = "Cor: Verde";
                else cor_display = "Cor: Apagado";
                ssd1306_draw_string(&display, cor_display, 0, 52, false);
            } else if (tela_atual == 1) {
                // Tela 2: Barras de chuva e nível
                ssd1306_draw_string(&display, "Barra Chuva:", 0, 0, false);
                uint8_t bar_y_chuva = 10, bar_width = SSD1306_WIDTH - 20, bar_height = 8;
                ssd1306_rect(&display, bar_y_chuva, 0, bar_width, bar_height, true, false);
                uint8_t chuva_fill = (uint8_t)(dados_sensores.volume_chuva_percent * (bar_width - 2) / 100.0f);
                if (chuva_fill > 0) ssd1306_rect(&display, bar_y_chuva + 1, 1, chuva_fill, bar_height - 2, true, true);
                ssd1306_draw_string(&display, "Barra Nivel:", 0, 25, false);
                uint8_t bar_y_nivel = 35;
                ssd1306_rect(&display, bar_y_nivel, 0, bar_width, bar_height, true, false);
                uint8_t nivel_fill = (uint8_t)(dados_sensores.nivel_agua_percent * (bar_width - 2) / 100.0f);
                if (nivel_fill > 0) ssd1306_rect(&display, bar_y_nivel + 1, 1, nivel_fill, bar_height - 2, true, true);
                if (xQueueReceive(fila_dados_exibicao, &dados_previsao, 0) == pdPASS) {
                    snprintf(buffer, sizeof(buffer), "Previsao:%.1f%%", dados_previsao.nivel_agua_previsto);
                } else {
                    snprintf(buffer, sizeof(buffer), "Previsao: N/A");
                }
                ssd1306_draw_string(&display, buffer, 0, 50, false);
            } else if (tela_atual == 2) {
                // Tela 3: Gráfico de chuva
                const uint8_t grafico_x = 15, grafico_y = 54, altura_grafico = 45, largura_grafico = 100;
                const char* titulo = "Chuva %";
                uint8_t titulo_width = strlen(titulo) * 5;
                uint8_t titulo_x_pos = (SSD1306_WIDTH - titulo_width) / 2;
                ssd1306_draw_string(&display, titulo, titulo_x_pos, 5, true);
                ssd1306_line(&display, grafico_x, grafico_y, grafico_x + largura_grafico, grafico_y, true);
                ssd1306_line(&display, grafico_x, grafico_y, grafico_x, grafico_y - altura_grafico, true);
                int n = (contagem_grafico < TAMANHO_GRAFICO) ? contagem_grafico : TAMANHO_GRAFICO;
                for (int i = 0; i < n - 1; i++) {
                    int idx_atual = (indice_grafico - n + i + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    int idx_proximo = (indice_grafico - n + i + 1 + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    float chuva_atual = dados_grafico_chuva[idx_atual], chuva_proxima = dados_grafico_chuva[idx_proximo];
                    uint8_t y_atual = grafico_y - (uint8_t)(chuva_atual * altura_grafico / 100.0f);
                    uint8_t y_proximo = grafico_y - (uint8_t)(chuva_proxima * altura_grafico / 100.0f);
                    uint8_t x_atual = grafico_x + (i * largura_grafico / (TAMANHO_GRAFICO - 1));
                    uint8_t x_proximo = grafico_x + ((i + 1) * largura_grafico / (TAMANHO_GRAFICO - 1));
                    ssd1306_line(&display, x_atual, y_atual, x_proximo, y_proximo, true);
                }
                for (int i = 0; i <= 5; i++) {
                    uint8_t y_mark = grafico_y - (i * altura_grafico / 5);
                    ssd1306_line(&display, grafico_x - 3, y_mark, grafico_x, y_mark, true);
                    if (i % 2 == 0) { snprintf(buffer, sizeof(buffer), "%d", i * 20); ssd1306_draw_string(&display, buffer, 0, y_mark - 3, true); }
                }
                for (int i = 0; i <= 4; i++) {
                    uint8_t x_mark = grafico_x + (i * largura_grafico / 4);
                    ssd1306_line(&display, x_mark, grafico_y, x_mark, grafico_y + 2, true);
                    snprintf(buffer, sizeof(buffer), "%d", i * 5); ssd1306_draw_string(&display, buffer, x_mark - 8, grafico_y + 2, true);
                }
            } else if (tela_atual == 3) {
                // Tela 4: Gráfico de nível
                const uint8_t grafico_x = 15, grafico_y = 54, altura_grafico = 45, largura_grafico = 100;
                const char* titulo = "Nivel %";
                uint8_t titulo_width = strlen(titulo) * 5;
                uint8_t titulo_x_pos = (SSD1306_WIDTH - titulo_width) / 2;
                ssd1306_draw_string(&display, titulo, titulo_x_pos, 5, true);
                ssd1306_line(&display, grafico_x, grafico_y, grafico_x + largura_grafico, grafico_y, true);
                ssd1306_line(&display, grafico_x, grafico_y, grafico_x, grafico_y - altura_grafico, true);
                int n = (contagem_grafico < TAMANHO_GRAFICO) ? contagem_grafico : TAMANHO_GRAFICO;
                for (int i = 0; i < n - 1; i++) {
                    int idx_atual = (indice_grafico - n + i + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    int idx_proximo = (indice_grafico - n + i + 1 + TAMANHO_GRAFICO) % TAMANHO_GRAFICO;
                    float nivel_atual = dados_grafico_nivel[idx_atual], nivel_proximo = dados_grafico_nivel[idx_proximo];
                    uint8_t y_atual = grafico_y - (uint8_t)(nivel_atual * altura_grafico / 100.0f);
                    uint8_t y_proximo = grafico_y - (uint8_t)(nivel_proximo * altura_grafico / 100.0f);
                    uint8_t x_atual = grafico_x + (i * largura_grafico / (TAMANHO_GRAFICO - 1));
                    uint8_t x_proximo = grafico_x + ((i + 1) * largura_grafico / (TAMANHO_GRAFICO - 1));
                    ssd1306_line(&display, x_atual, y_atual, x_proximo, y_proximo, true);
                }
                for (int i = 0; i <= 5; i++) {
                    uint8_t y_mark = grafico_y - (i * altura_grafico / 5);
                    ssd1306_line(&display, grafico_x - 3, y_mark, grafico_x, y_mark, true);
                    if (i % 2 == 0) { snprintf(buffer, sizeof(buffer), "%d", i * 20); ssd1306_draw_string(&display, buffer, 0, y_mark - 3, true); }
                }
                for (int i = 0; i <= 4; i++) {
                    uint8_t x_mark = grafico_x + (i * largura_grafico / 4);
                    ssd1306_line(&display, x_mark, grafico_y, x_mark, grafico_y + 2, true);
                    snprintf(buffer, sizeof(buffer), "%d", i * 5); ssd1306_draw_string(&display, buffer, x_mark - 8, grafico_y + 2, true);
                }
            }
            ssd1306_send_data(&display); // Atualiza o display
        }
    }
}

// Tarefa que controla a matriz de LEDs
void tarefa_matriz_led(void *pvParameters) {
    dados_sensores_t dados;
    bool estado_alerta_recebido = false;
    const uint32_t cor_vermelho_matriz = COR_VERMELHO;
    const uint32_t cor_azul_matriz = COR_AZUL;
    const uint32_t cor_amarelo_matriz = COR_AMARELO;
    uint8_t estado_exibicao = 0;
    uint32_t ultimo_tempo_alternancia = 0;
    static bool primeira_entrada_chuva_alta_apos_sem_chuva = true;

    while (true) {
        // Verifica o estado de alerta e os dados dos sensores
        if (fila_estado_alerta != NULL) {
            xQueuePeek(fila_estado_alerta, &estado_alerta_recebido, 0);
        }
        if (xQueuePeek(fila_dados_sensores, &dados, pdMS_TO_TICKS(100)) == pdPASS) {
            bool chuva_alta = (dados.volume_chuva_percent > 80.0f);
            uint32_t tempo_atual = to_ms_since_boot(get_absolute_time());
            if (estado_alerta_recebido) {
                if (chuva_alta) {
                    // Alterna exibições a cada 4 segundos
                    if (primeira_entrada_chuva_alta_apos_sem_chuva) {
                        estado_exibicao = 0;
                        ultimo_tempo_alternancia = tempo_atual;
                        primeira_entrada_chuva_alta_apos_sem_chuva = false;
                    } else if ((tempo_atual - ultimo_tempo_alternancia) >= 4000) {
                        estado_exibicao = (estado_exibicao + 1) % 3;
                        ultimo_tempo_alternancia = tempo_atual;
                    }
                    if (estado_exibicao == 0) matriz_draw_rain_animation(cor_azul_matriz);
                    else if (estado_exibicao == 1) matriz_draw_pattern(PAD_EXC, cor_amarelo_matriz);
                    else matriz_draw_pattern(PAD_X, cor_vermelho_matriz);
                } else {
                    matriz_draw_pattern(PAD_X, cor_vermelho_matriz);
                    primeira_entrada_chuva_alta_apos_sem_chuva = true;
                    estado_exibicao = 0;
                }
            } else {
                matriz_clear(); // Limpa a matriz se não houver alerta
                primeira_entrada_chuva_alta_apos_sem_chuva = true;
                estado_exibicao = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Tarefa que controla o buzzer com base nas condições
void tarefa_buzzer(void *pvParameters) {
    dados_sensores_t dados_atuais;

    while (true) {
        if (xQueuePeek(fila_dados_sensores, &dados_atuais, pdMS_TO_TICKS(50)) == pdPASS) {
            float nivel = dados_atuais.nivel_agua_percent;
            float chuva = dados_atuais.volume_chuva_percent;

            if (nivel > 70.0f && chuva > 80.0f) {
                // Alerta prioritário: nível alto e chuva intensa
                ligar_buzzer(1000);
                vTaskDelay(pdMS_TO_TICKS(1000));
                desligar_buzzer();
                vTaskDelay(pdMS_TO_TICKS(500));
            } else if (chuva > 80.0f) {
                // Alerta de chuva intensa: dois beeps curtos
                ligar_buzzer(1000);
                vTaskDelay(pdMS_TO_TICKS(150));
                desligar_buzzer();
                vTaskDelay(pdMS_TO_TICKS(150));
                ligar_buzzer(1000);
                vTaskDelay(pdMS_TO_TICKS(150));
                desligar_buzzer();
                vTaskDelay(pdMS_TO_TICKS(150));
            } else if (nivel > 70.0f) {
                // Alerta de nível alto: beep intermitente
                ligar_buzzer(1000);
                vTaskDelay(pdMS_TO_TICKS(200));
                desligar_buzzer();
                vTaskDelay(pdMS_TO_TICKS(200));
            } else {
                desligar_buzzer();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else {
            desligar_buzzer();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// --- FUNÇÃO PRINCIPAL ---
int main() {
    stdio_init_all();
    sleep_ms(2000); // Aguarda inicialização do sistema

    // Configura comunicação I2C
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    // Inicializa o display OLED
    ssd1306_init(&display, SSD1306_WIDTH, SSD1306_HEIGHT, false, SSD1306_I2C_ADDR, I2C_PORT);
    ssd1306_config(&display);
    ssd1306_fill(&display, false);
    ssd1306_draw_string(&display, "Iniciando...", 0, 28, false);
    ssd1306_send_data(&display);

    inicializar_matriz_led(); // Inicializa a matriz de LEDs

    // Cria as filas de comunicação
    fila_dados_sensores = xQueueCreate(10, sizeof(dados_sensores_t));
    fila_dados_exibicao = xQueueCreate(5, sizeof(dados_previsao_t));
    fila_estado_alerta = xQueueCreate(1, sizeof(bool));
    if (fila_dados_sensores == NULL || fila_dados_exibicao == NULL || fila_estado_alerta == NULL) {
        while (1); // Trava se as filas não forem criadas
    }

    // Cria as tarefas do FreeRTOS
    xTaskCreate(tarefa_medicao, "Leitura", 512, NULL, 2, NULL);
    xTaskCreate(tarefa_previsao, "Previsao", configMINIMAL_STACK_SIZE + 256, NULL, 1, NULL);
    xTaskCreate(tarefa_exibicao, "Exibicao", configMINIMAL_STACK_SIZE + 512, NULL, 1, NULL);
    xTaskCreate(tarefa_matriz_led, "MatrizLED", configMINIMAL_STACK_SIZE + 768, NULL, 1, NULL);
    xTaskCreate(tarefa_buzzer, "Buzzer", configMINIMAL_STACK_SIZE + 256, NULL, 1, NULL);

    vTaskStartScheduler(); // Inicia o escalonador do FreeRTOS

    while (true) {
        sleep_ms(1000); // Loop infinito (nunca alcançado)
    }

    return 0;
}
#include <stdio.h>
#include <string.h> // Necessário para snprintf e strcpy/strlen
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h" // Adicionado para I2C
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "lib/Display_Bibliotecas/ssd1306.h" // Adicionado para o display SSD1306

// --- DEFINIÇÕES DO DISPLAY OLED ---
#define I2C_PORT i2c1
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 15
#define SSD1306_I2C_ADDR   0x3C // Endereço I2C comum para SSD1306
#define SSD1306_WIDTH      128
#define SSD1306_HEIGHT     64
#define BUTTON_A_PIN 5
//Definição dos pinos do joystick
#define ADC_JOYSTICK_X_PIN 27 //GPIO 27 → ADC1 (nível de água)
#define ADC_JOYSTICK_Y_PIN 26 //GPIO 26 → ADC0 (volume de chuva)

//Estrutura para armazenar os dados de sensores
typedef struct {
    uint16_t nivel_agua_raw;
    uint16_t volume_chuva_raw;
    float nivel_agua_percent;
    float volume_chuva_percent;
    float volume_chuva_mm_h;
    char classificacao_chuva_str[20]; // Para armazenar a string da classificação da chuva
    bool alerta_risco_enchente;      // true se houver risco, false caso contrário
} sensor_data_t;

//Fila global
static QueueHandle_t xQueueSensorData = NULL;

//Instância global do display SSD1306
static ssd1306_t display;

//Função interna para converter % de chuva para mm/h
float percent_to_mm_h(float p) {
    if (p <= 0.0f) return 0.0f;
    else if (p < 30.0f) return (p / 30.0f) * 5.0f;
    else if (p < 60.0f) return 5.0f + ((p - 30.0f) / 30.0f) * 10.0f;
    else if (p < 80.0f) return 15.0f + ((p - 60.0f) / 20.0f) * 15.0f;
    else if (p < 95.0f) return 30.0f + ((p - 80.0f) / 15.0f) * 5.0f;
    else return 35.0f;
}

//Função interna para classificar a chuva (renomeada para evitar conflitos)
const char* classificar_chuva_func(float p_percent) {
    if (p_percent < 0.1f) return "sem chuva";
    else if (p_percent < 30.0f) return "chuva leve";
    else if (p_percent < 60.0f) return "moderada";
    else if (p_percent < 80.0f) return "forte";
    else return "extrema";
}

//Tarefa que lê o joystick e envia dados para a fila
void vJoystickTask(void *pvParameters) {
    //Inicializa ADC
    adc_init();
    adc_gpio_init(ADC_JOYSTICK_X_PIN);
    adc_gpio_init(ADC_JOYSTICK_Y_PIN);

    sensor_data_t dados;

    while (true) {
        //Lê ADC1 → nível de água
        adc_select_input(1); // ADC1 é GPIO27
        dados.nivel_agua_raw = adc_read();
        dados.nivel_agua_percent = (dados.nivel_agua_raw / 4095.0f) * 100.0f;

        //Lê ADC0 → volume de chuva
        adc_select_input(0); // ADC0 é GPIO26
        dados.volume_chuva_raw = adc_read();
        dados.volume_chuva_percent = (dados.volume_chuva_raw / 4095.0f) * 100.0f;

        //Converte volume de chuva em % para mm/h
        dados.volume_chuva_mm_h = percent_to_mm_h(dados.volume_chuva_percent);

        //Classifica a chuva e armazena na estrutura
        strncpy(dados.classificacao_chuva_str, classificar_chuva_func(dados.volume_chuva_percent), sizeof(dados.classificacao_chuva_str) - 1);
        dados.classificacao_chuva_str[sizeof(dados.classificacao_chuva_str) - 1] = '\0'; // Garante terminação nula

        //Verifica se há risco de enchente
        if (dados.nivel_agua_percent >= 70.0f || dados.volume_chuva_percent >= 80.0f) {
            dados.alerta_risco_enchente = true;
        } else {
            dados.alerta_risco_enchente = false;
        }

        //Envia dados para a fila
        if (xQueueSend(xQueueSensorData, &dados, pdMS_TO_TICKS(10)) != pdPASS) { // Pequeno timeout para envio
            printf("Aviso: fila cheia, dado descartado\n\n"); // Mantido para debug
        }

        //Aguarda 1000 ms (frequência de ~1 Hz)
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//Tarefa que consome os dados da fila e exibe no display OLED
void vDisplayTask(void *pvParameters) {
    sensor_data_t recebido;
    char buffer[32];
    bool second_screen_active = false;  // Começa com a primeira tela
    
    // Inicializa o Botão A
    gpio_init(BUTTON_A_PIN);
    gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_A_PIN);
    
    // Variáveis para debounce do botão
    bool previous_button_state = true;  // Pull-up ativo, então estado padrão é HIGH
    uint32_t last_press_time = 0;
    const uint32_t debounce_delay = 300;  // 300ms de debounce
    
    while (true) {
        // Lógica de detecção e debounce do botão
        bool current_button_state = gpio_get(BUTTON_A_PIN);
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Detecta pressionamento do botão (borda de descida) com debounce
        if (previous_button_state && !current_button_state && 
            (current_time - last_press_time > debounce_delay)) {
            
            second_screen_active = !second_screen_active;  // Alterna entre telas
            last_press_time = current_time;
        }
        previous_button_state = current_button_state;
        
        // Espera até receber um dado da fila
        if (xQueueReceive(xQueueSensorData, &recebido, pdMS_TO_TICKS(50)) == pdPASS) {
            // Limpa o buffer do display
            ssd1306_fill(&display, false);
            
            if (second_screen_active) {
                // SEGUNDA TELA - Barras de progresso
                
                // Linha 1: Barra Chuva:
                ssd1306_draw_string(&display, "Barra Chuva:", 0, 0, false);
                
                // Desenha barra de porcentagem de chuva (10px abaixo do texto)
                uint8_t bar_y = 10;
                uint8_t bar_width = 100;
                uint8_t bar_height = 8;
                
                // Desenha contorno da barra
                ssd1306_rect(&display, bar_y, 0, bar_width, bar_height, true, false);
                
                // Calcula largura de preenchimento e desenha parte preenchida
                uint8_t rain_fill = (uint8_t)(recebido.volume_chuva_percent * (bar_width - 2) / 100.0f);
                if (rain_fill > 0) {
                    ssd1306_rect(&display, bar_y + 1, 1, rain_fill, bar_height - 2, true, true);
                }
                
                // Linha 2: Barra Nível:
                ssd1306_draw_string(&display, "Barra Nivel:", 0, 25, false);
                
                // Desenha barra de porcentagem do nível de água
                uint8_t nivel_y = 35;
                
                // Desenha contorno da barra
                ssd1306_rect(&display, nivel_y, 0, bar_width, bar_height, true, false);
                
                // Calcula largura de preenchimento e desenha parte preenchida
                uint8_t nivel_fill = (uint8_t)(recebido.nivel_agua_percent * (bar_width - 2) / 100.0f);
                if (nivel_fill > 0) {
                    ssd1306_rect(&display, nivel_y + 1, 1, nivel_fill, bar_height - 2, true, true);
                }
                
                // Linha 3: Previsão (espaço reservado para implementação futura)
                ssd1306_draw_string(&display, "Previsao:", 0, 50, false);
                
            } else {
                // PRIMEIRA TELA - Display original
                
                // Linha 1: Qnt Chuva: [valor] mm/h
                snprintf(buffer, sizeof(buffer), "QntChuva:%.2fmm", recebido.volume_chuva_mm_h);
                ssd1306_draw_string(&display, buffer, 0, 0, false);

                // Linha 2: Chuva: [valor] %
                snprintf(buffer, sizeof(buffer), "Chuva: %.1f %%", recebido.volume_chuva_percent);
                ssd1306_draw_string(&display, buffer, 0, 13, false);

                // Linha 3: Nivel Agua: [valor] %
                snprintf(buffer, sizeof(buffer), "Nivel: %.1f %%", recebido.nivel_agua_percent);
                ssd1306_draw_string(&display, buffer, 0, 26, false);

                // Linha 4: Status (Normal ou ALERTA)
                if (recebido.alerta_risco_enchente) {
                    snprintf(buffer, sizeof(buffer), "Status: ALERTA!");
                } else {
                    snprintf(buffer, sizeof(buffer), "Status: Normal");
                }
                ssd1306_draw_string(&display, buffer, 0, 39, false);

                // Linha 5: Cor
                const char* cor_texto;
                if (recebido.alerta_risco_enchente) {
                    cor_texto = "Cor: Vermelho";
                } else {
                    cor_texto = "Cor: Verde";
                }
                ssd1306_draw_string(&display, cor_texto, 0, 52, false);
            }
        
            // Envia o buffer atualizado para o display físico
            ssd1306_send_data(&display);
        }
    }
}


//Função principal
int main() {
    stdio_init_all();
    sleep_ms(2000); //Aguarda a inicialização do terminal e estabilização
    printf("\n=== Sistema de Alerta de Enchente - Inicializando ===\n\n");

    //Inicialização do I2C para o display OLED
    i2c_init(I2C_PORT, 100 * 1000); // I2C a 100KHz
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN); // Pull-ups são importantes para I2C
    gpio_pull_up(I2C_SCL_PIN);
    printf("Interface I2C inicializada nos pinos SDA=%d, SCL=%d.\n", I2C_SDA_PIN, I2C_SCL_PIN);

    //Inicialização do display SSD1306
    ssd1306_init(&display, SSD1306_WIDTH, SSD1306_HEIGHT, false, SSD1306_I2C_ADDR, I2C_PORT);
    ssd1306_config(&display);
    ssd1306_fill(&display, false); // Limpa o display
    ssd1306_draw_string(&display, "Iniciando...", 0, 0, false);
    ssd1306_send_data(&display);
    printf("Display OLED SSD1306 inicializado.\n");


    //Cria fila com capacidade para 10 elementos do tipo sensor_data_t
    xQueueSensorData = xQueueCreate(10, sizeof(sensor_data_t));
    if (xQueueSensorData == NULL) {
        printf("Erro: falha na criação da fila!\n");
        while(1); // Trava o sistema
    }
    printf("Fila de dados criada.\n");

    //Cria as tarefas
    BaseType_t joystick_task_status = xTaskCreate(vJoystickTask, "JoystickTask", 512, NULL, 2, NULL);
    BaseType_t display_task_status = xTaskCreate(vDisplayTask, "DisplayTask", 1024, NULL, 1, NULL); // Stack maior para display

    if (joystick_task_status != pdPASS || display_task_status != pdPASS) {
        printf("Erro: falha na criação de uma ou mais tarefas!\n");
         while(1); // Trava o sistema
    }
    printf("Tarefas JoystickTask e DisplayTask criadas.\n");
    
    //Inicia o agendador do FreeRTOS
    printf("Iniciando scheduler do FreeRTOS...\n");
    vTaskStartScheduler();

    //Se o sistema chegar aqui, o scheduler falhou
    while (true) {
        printf("Erro: Scheduler não pôde iniciar!\n");
        sleep_ms(1000);
    }

    return 0; // Teoricamente, nunca alcançado
}
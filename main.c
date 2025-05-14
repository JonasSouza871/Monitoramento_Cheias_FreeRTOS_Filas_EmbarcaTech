#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

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
} sensor_data_t;

//Fila global
static QueueHandle_t xQueueSensorData = NULL;

//Tarefa que lê o joystick e envia dados para a fila
void vJoystickTask(void *pvParameters) {
    //Inicializa ADC
    adc_init();
    adc_gpio_init(ADC_JOYSTICK_X_PIN);
    adc_gpio_init(ADC_JOYSTICK_Y_PIN);

    //Função interna para converter % de chuva para mm/h
    float percent_to_mm_h(float p) {
        if (p <= 0.0f) return 0.0f;
        else if (p < 30.0f) return (p / 30.0f) * 5.0f;
        else if (p < 60.0f) return 5.0f + ((p - 30.0f) / 30.0f) * 10.0f;
        else if (p < 80.0f) return 15.0f + ((p - 60.0f) / 20.0f) * 15.0f;
        else if (p < 95.0f) return 30.0f + ((p - 80.0f) / 15.0f) * 5.0f;
        else return 35.0f;
    }

    //Função interna para classificar a chuva
    const char* classificar_chuva(float p) {
        if (p < 0.1f) return "sem chuva";
        else if (p < 30.0f) return "chuva leve";
        else if (p < 60.0f) return "moderada";
        else if (p < 80.0f) return "forte";
        else return "extrema";
    }

    sensor_data_t dados;

    while (true) {
        //Lê ADC1 → nível de água
        adc_select_input(1);
        dados.nivel_agua_raw = adc_read();
        dados.nivel_agua_percent = (dados.nivel_agua_raw / 4095.0f) * 100.0f;

        //Lê ADC0 → volume de chuva
        adc_select_input(0);
        dados.volume_chuva_raw = adc_read();
        dados.volume_chuva_percent = (dados.volume_chuva_raw / 4095.0f) * 100.0f;

        //Converte volume de chuva em % para mm/h
        dados.volume_chuva_mm_h = percent_to_mm_h(dados.volume_chuva_percent);

        //Exibe os dados no terminal serial
        printf("=== MONITORAMENTO DE ENCHENTE ===\n");
        printf("Nível água: %.1f%% | ", dados.nivel_agua_percent);
        printf("Chuva: %.1f%% (%.2f mm/h – %s)\n",
               dados.volume_chuva_percent,
               dados.volume_chuva_mm_h,
               classificar_chuva(dados.volume_chuva_percent));

        //Verifica se há risco de enchente
        if (dados.nivel_agua_percent >= 70.0f || dados.volume_chuva_percent >= 80.0f) {
            printf("!!! ALERTA: RISCO DE ENCHENTE !!!\n\n");
        } else {
            printf("Status: Normal\n\n");
        }

        //Envia dados para a fila
        if (xQueueSend(xQueueSensorData, &dados, 0) != pdPASS) {
            printf("Aviso: fila cheia, dado descartado\n\n");
        }

        //Aguarda 200 ms (frequência de ~5 Hz)
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//Tarefa que apenas consome os dados da fila
void vProcessDataTask(void *pvParameters) {
    sensor_data_t recebido;

    while (true) {
        //Espera até receber um dado da fila
        if (xQueueReceive(xQueueSensorData, &recebido, portMAX_DELAY) == pdPASS) {
            //Neste exemplo, os dados são apenas descartados
            //Aqui poderia ser feita gravação em SD, envio por rede, etc.
        }
    }
}

//Função principal
int main() {
    stdio_init_all();
    sleep_ms(1000); //Aguarda a inicialização do terminal
    printf("\n=== Sistema de Alerta de Enchente - Inicializando ===\n\n");

    //Cria fila com capacidade para 10 elementos do tipo sensor_data_t
    xQueueSensorData = xQueueCreate(10, sizeof(sensor_data_t));
    if (xQueueSensorData == NULL) {
        printf("Erro: falha na criação da fila!\n");
        return -1;
    }

    //Cria as tarefas
    xTaskCreate(vJoystickTask, "JoystickTask", 512, NULL, 2, NULL);
    xTaskCreate(vProcessDataTask, "ProcessData", 512, NULL, 1, NULL);

    //Inicia o agendador do FreeRTOS
    vTaskStartScheduler();

    //Se o sistema chegar aqui, o scheduler falhou
    while (true) {
        printf("Erro: Scheduler não pôde iniciar!\n");
        sleep_ms(1000);
    }

    return 0;
}
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lib/DS18b20/ds18b20.h"
#include "lib/Display_Bibliotecas/ssd1306.h"
#include "lib/Matriz_Bibliotecas/matriz_led.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"

// ----------------- CONFIGURAÇÃO WI-FI -----------------
#define WIFI_SSID     "Jonas Souza"
#define WIFI_PASSWORD "12345678"

// ----------------- PINOS -----------------
#define PIN_DS18B20   20    //Sensor de temperatura DS18B20
#define PIN_VRY       26    //Joystick vertical (ADC0)
#define PIN_BTN_A     5     //Botão A
#define PIN_RGB_R     11    //LED RGB (vermelho)
#define PIN_RGB_G     12    //LED RGB (verde)
#define PIN_RGB_B     13    //LED RGB (azul)
#define PIN_BUZZER    10    //Buzzer
#define I2C_SDA       14    //I2C SDA (OLED)
#define I2C_SCL       15    //I2C SCL (OLED)

// ----------------- CONFIGURAÇÃO OLED -----------------
#define OLED_I2C_PORT i2c1  //Instância I2C
#define OLED_ADDR     0x3C  //Endereço I2C do OLED

// ----------------- VALORES DE RPM -----------------
#define RPM_MIN 300.0f      //RPM mínimo simulado
#define RPM_MAX 2000.0f     //RPM máximo simulado

// ----------------- VARIÁVEIS GLOBAIS -----------------
static ssd1306_t oled;              //Estrutura do display OLED
static float temperatura_atual;     //Temperatura lida pelo sensor
static int setpoint = 20;           //Temperatura desejada (°C)
static volatile uint16_t duty_cycle_pwm = 0; //Duty cycle do PWM
static float rpm_simulado = RPM_MIN; //RPM simulado
static bool selecionando = true;    //Modo de seleção do setpoint
static bool exibir_tela_principal = true; //Alterna tela principal/RPM
static uint slice_r, slice_g, slice_b; //PWM slices para RGB
static uint chan_r, chan_g, chan_b;    //PWM canais para RGB

// ----------------- PROTÓTIPOS DAS FUNÇÕES -----------------
void Task_Sensor(void *pv);       //Lê temperatura
void Task_Input(void *pv);        //Ajusta setpoint com joystick
void Task_Control(void *pv);      //Controle PI e PWM do LED RGB
void Task_Buzzer(void *pv);       //Controla o buzzer
void Task_Display(void *pv);      //Gerencia OLED e matriz de LEDs
void Task_Webserver(void *pv);    //Servidor web
static err_t webserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err); //Aceita conexões TCP (usado por Task_Webserver)
static err_t webserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err); //Processa requisições HTTP (usado por Task_Webserver)
static err_t webserver_sent(void *arg, struct tcp_pcb *tpcb, uint16_t len); //Fecha conexão após envio (usado por Task_Webserver)

// ============================================================================
// Tasks
// ============================================================================

//Lê a temperatura do sensor DS18B20 a cada 1 segundo
void Task_Sensor(void *pv) {
    ds18b20_init(PIN_DS18B20); //Inicializa o sensor
    while (true) {
        temperatura_atual = ds18b20_get_temperature(); //Atualiza temperatura
        vTaskDelay(pdMS_TO_TICKS(1000)); //Espera 1 segundo
    }
}

//Ajusta o setpoint com o joystick e confirma com o botão A
void Task_Input(void *pv) {
    adc_init(); //Inicializa ADC
    adc_gpio_init(PIN_VRY); //Configura pino do joystick
    adc_select_input(0); //Seleciona canal ADC0
    gpio_init(PIN_BTN_A); //Inicializa botão
    gpio_set_dir(PIN_BTN_A, GPIO_IN); //Define como entrada
    gpio_pull_up(PIN_BTN_A); //Ativa pull-up interno

    bool ultimo_estado_btn = false; //Estado anterior do botão
    int ultima_direcao = 0; //Última direção do joystick

    while (true) {
        uint16_t valor_raw = adc_read(); //Lê valor do joystick
        bool estado_btn_atual = (gpio_get(PIN_BTN_A) == 0); //Botão pressionado?
        int direcao = (valor_raw > 3000 ? 1 : (valor_raw < 1000 ? -1 : 0)); //Detecta movimento

        if (selecionando) {
            //Ajusta setpoint entre 10 e 30°C
            if (direcao == 1 && ultima_direcao == 0 && setpoint < 30) setpoint++;
            if (direcao == -1 && ultima_direcao == 0 && setpoint > 10) setpoint--;
            if (estado_btn_atual && !ultimo_estado_btn) selecionando = false; //Confirma
        } else {
            if (estado_btn_atual && !ultimo_estado_btn) selecionando = true; //Volta ao modo de seleção
        }

        ultimo_estado_btn = estado_btn_atual;
        ultima_direcao = direcao;
        vTaskDelay(pdMS_TO_TICKS(100)); //Espera 100ms
    }
}

//Implementa controle PI e ajusta PWM do LED RGB
void Task_Control(void *pv) {
    const float kp = 120.0f; //Ganho proporcional
    const float ki = 120.0f / 15.0f; //Ganho integral
    const float h = 1.0f; //Período de amostragem (1s)
    static float integral = 0.0f; //Acumulador do erro integral

    while (true) {
        //Calcula erro
        float erro = temperatura_atual - (float)setpoint;
        float P = kp * erro; //Termo proporcional
        integral += ki * erro * h; //Termo integral
        integral = fmaxf(fminf(integral, 4096.0f), -4096.0f); //Limita integral

        //Calcula sinal de controle e converte para duty cycle
        float U = P + integral;
        int32_t duty = (int32_t)((U + 4096.0f) * (65535.0f / 8192.0f));
        duty = duty < 0 ? 0 : (duty > 65535 ? 65535 : duty); //Limita entre 0 e 65535
        duty_cycle_pwm = duty; //Atualiza variável global

        //Simula RPM com base no duty cycle
        rpm_simulado = RPM_MIN + (RPM_MAX - RPM_MIN) * (duty / 65535.0f);

        //Define cor do LED RGB com base no erro absoluto
        float erro_abs = fabsf(erro);
        uint16_t r = 0, g = 0, b = 0;
        if (selecionando) {
            r = 0; g = 0; b = 0; //Desliga RGB durante seleção
        } else if (erro_abs > 9.6f) {
            r = duty; //Vermelho para emergência
        } else if (erro_abs >= 3.6f) {
            r = duty; g = duty; //Amarelo para preocupação
        } else {
            g = duty; //Verde para tranquilo
        }

        //Aplica valores PWM ao LED RGB
        pwm_set_chan_level(slice_r, chan_r, r);
        pwm_set_chan_level(slice_g, chan_g, g);
        pwm_set_chan_level(slice_b, chan_b, b);

        vTaskDelay(pdMS_TO_TICKS(1000)); //Espera 1 segundo
    }
}

//Controla o buzzer com base no erro absoluto
void Task_Buzzer(void *pv) {
    gpio_set_function(PIN_BUZZER, GPIO_FUNC_PWM); //Configura pino como PWM
    uint slice = pwm_gpio_to_slice_num(PIN_BUZZER);
    uint chan = pwm_gpio_to_channel(PIN_BUZZER);
    pwm_set_enabled(slice, true); //Habilita PWM

    while (true) {
        float erro = fabsf(temperatura_atual - (float)setpoint);

        if (selecionando) {
            //Desativa buzzer durante seleção
            pwm_set_enabled(slice, false);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (erro > 9.6f) {
            //Erro grave: beep forte e rápido (1000 Hz, 100ms ligado, 100ms desligado)
            pwm_set_clkdiv(slice, 125.0f / 1000.0f); //Frequência de 1000 Hz
            pwm_set_wrap(slice, 1000); //Resolução para o tom
            pwm_set_chan_level(slice, chan, 500); //Duty cycle de 50%
            pwm_set_enabled(slice, true);
            vTaskDelay(pdMS_TO_TICKS(100)); //Beep por 100ms
            pwm_set_enabled(slice, false);
            vTaskDelay(pdMS_TO_TICKS(100)); //Pausa de 100ms
        } else if (erro >= 3.6f) {
            //Erro intermediário: beep de preocupação (500 Hz, 200ms ligado, 600ms desligado)
            pwm_set_clkdiv(slice, 125.0f / 500.0f); //Frequência de 500 Hz
            pwm_set_wrap(slice, 1000);
            pwm_set_chan_level(slice, chan, 500);
            pwm_set_enabled(slice, true);
            vTaskDelay(pdMS_TO_TICKS(200)); //Beep por 200ms
            pwm_set_enabled(slice, false);
            vTaskDelay(pdMS_TO_TICKS(600)); //Pausa de 600ms
        } else {
            //Erro tranquilo: beep suave (200 Hz, 300ms ligado, 1000ms desligado)
            pwm_set_clkdiv(slice, 125.0f / 200.0f); //Frequência de 200 Hz
            pwm_set_wrap(slice, 1000);
            pwm_set_chan_level(slice, chan, 500);
            pwm_set_enabled(slice, true);
            vTaskDelay(pdMS_TO_TICKS(300)); //Beep por 300ms
            pwm_set_enabled(slice, false);
            vTaskDelay(pdMS_TO_TICKS(1000)); //Pausa de 1000ms
        }
    }
}

//Gerencia exibição no OLED e matriz de LEDs
//Gerencia exibição no OLED e matriz de LEDs
void Task_Display(void *pv) {
    char buf[32]; //Buffer para strings
    uint32_t ultimo_alternar = to_ms_since_boot(get_absolute_time()); //Alternância de telas

    while (true) {
        uint32_t agora = to_ms_since_boot(get_absolute_time());
        //Alterna telas a cada 5 segundos
        if (!selecionando && agora - ultimo_alternar > 5000) {
            exibir_tela_principal = !exibir_tela_principal;
            ultimo_alternar = agora;
        }

        ssd1306_fill(&oled, false); //Limpa OLED

        if (selecionando) {
            //Tela de ajuste do setpoint
            ssd1306_draw_string(&oled, "Ajuste Setpoint:", 0, 0, false);
            snprintf(buf, sizeof(buf), "   %2d C", setpoint);
            ssd1306_draw_string(&oled, buf, 0, 16, false);
            ssd1306_draw_string(&oled, "[A] Confirma", 0, 32, false);
        } else if (exibir_tela_principal) {
            //Tela principal de monitoramento
            snprintf(buf, sizeof(buf), "Temp: %4.1f C", temperatura_atual);
            ssd1306_draw_string(&oled, buf, 0, 0, false);
            snprintf(buf, sizeof(buf), "Set:  %3d C", setpoint);
            ssd1306_draw_string(&oled, buf, 0, 16, false);
            snprintf(buf, sizeof(buf), "Erro: %4.1f C", setpoint - temperatura_atual);
            ssd1306_draw_string(&oled, buf, 0, 32, false);
            snprintf(buf, sizeof(buf), "PWM:  %5u", duty_cycle_pwm);
            ssd1306_draw_string(&oled, buf, 0, 48, false);

            //Exibe erro na matriz de LEDs com arredondamento
            float erro = fabsf(setpoint - temperatura_atual);
            int parte_inteira = (int)floorf(erro);
            float parte_decimal = erro - parte_inteira;
            int digito = (parte_decimal >= 0.6f) ? parte_inteira + 1 : parte_inteira;

            uint32_t cor;
            switch (digito) {
                case 0: cor = COR_BRANCO;  break;
                case 1: cor = COR_PRATA;   break;
                case 2: cor = COR_CINZA;   break;
                case 3: cor = COR_VIOLETA; break;
                case 4: cor = COR_AZUL;    break;
                case 5: cor = COR_MARROM;  break;
                case 6: cor = COR_VERDE;   break;
                case 7: cor = COR_OURO;    break;
                case 8: cor = COR_LARANJA; break;
                case 9: cor = COR_AMARELO; break;
                default: cor = COR_OFF;    break;
            }
            if (erro > 9.6f) {
                //Erro alto: padrão X vermelho
                matriz_draw_pattern(PAD_X, COR_VERMELHO);
            } else {
                //Exibe dígito do erro
                matriz_draw_number(digito, cor);
            }
        } else {
            //Tela de exibição do RPM
            snprintf(buf, sizeof(buf), "RPM: %4.0f", rpm_simulado);
            ssd1306_draw_string(&oled, buf, 0, 0, false);

            //Barra de progresso do RPM
            float range = RPM_MAX - RPM_MIN;
            float pos = (rpm_simulado - RPM_MIN) / range;
            int len = (int)(pos * oled.width);
            for (int i = 0; i < len; i++) {
                ssd1306_line(&oled, i, 20, i, 25, true);
            }

            //Exibe valores mínimo e máximo de RPM
            snprintf(buf, sizeof(buf), "Min:%4.0f Max:%4.0f", RPM_MIN, RPM_MAX);
            ssd1306_draw_string(&oled, buf, 0, 32, false);

            //Indica ação
            const char *acao = (temperatura_atual > setpoint) ? "ESFRIAR!!" : "ESQUENTAR!!";
            ssd1306_draw_string(&oled, acao, 0, 50, false);
        }

        ssd1306_send_data(&oled); //Atualiza display
        vTaskDelay(pdMS_TO_TICKS(100)); //Espera 100ms
    }
}

// ============================================================================
// Funções do Webserver (usadas exclusivamente por Task_Webserver)
// ============================================================================

//Fecha conexão após envio
static err_t webserver_sent(void *arg, struct tcp_pcb *tpcb, uint16_t len) {
    tcp_close(tpcb);
    return ERR_OK;
}

//Processa requisições HTTP
static err_t webserver_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    pbuf_free(p); //Descarta requisição

    //Calcula brilho percentual do LED RGB
    int brilho = (duty_cycle_pwm * 100) / 65535;

    //Monta corpo da página HTML
    static char body[640];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta http-equiv=\"refresh\" content=\"2\">"
        "<title>Monitor Pico W</title>"
        "<style>body{font-family:sans-serif;padding:16px}"
        "h1{font-size:28px}</style></head><body>"
        "<h1>Dados do Sistema (att. 2 s)</h1>"
        "<p>Temperatura atual: %.2f ℃</p>"
        "<p>Setpoint: %d ℃</p>"
        "<p>RPM simulado: %.0f (Min 300 Max 2000)</p>"
        "<p>Brilho LED RGB: %d %%</p>"
        "</body></html>",
        temperatura_atual, setpoint, rpm_simulado, brilho);

    //Monta cabeçalho HTTP
    char header[128];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", body_len);

    //Envia resposta
    tcp_write(tpcb, header, header_len, TCP_WRITE_FLAG_COPY);
    tcp_write(tpcb, body, body_len, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);

    //Registra callback para fechar após envio
    tcp_sent(tpcb, webserver_sent);
    return ERR_OK;
}

//Aceita nova conexão TCP
static err_t webserver_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, webserver_recv);
    return ERR_OK;
}

//Inicia servidor web na porta 80
void Task_Webserver(void *pv) {
    if (cyw43_arch_init()) {
        printf("Falha ao iniciar Wi-Fi\n");
        vTaskDelete(NULL);
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    cyw43_arch_enable_sta_mode();

    printf("Conectando a %s…\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
            CYW43_AUTH_WPA2_AES_PSK, 20000)) {
        printf("Falha na conexão\n");
        vTaskDelete(NULL);
    }
    printf("Conectado!\n");
    if (netif_default)
        printf("IP: %s\n", ipaddr_ntoa(&netif_default->ip_addr));

    struct tcp_pcb *srv = tcp_new();
    if (!srv || tcp_bind(srv, IP_ADDR_ANY, 80) != ERR_OK) {
        printf("Erro bind porta 80\n");
        vTaskDelete(NULL);
    }
    srv = tcp_listen(srv);
    tcp_accept(srv, webserver_accept);
    printf("HTTP on 80\n");

    while (true) {
        cyw43_arch_poll();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============================================================================
// main
// ============================================================================

//Inicializa o sistema e cria as tasks
int main() {
    stdio_init_all(); //Inicializa comunicação serial
    inicializar_matriz_led(); //Configura a matriz de LEDs

    //Inicializa I2C para o OLED
    i2c_init(OLED_I2C_PORT, 400000); //Frequência de 400kHz
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init(&oled, 128, 64, false, OLED_ADDR, OLED_I2C_PORT); //Configura OLED
    ssd1306_config(&oled);

    //Inicializa PWM para LED RGB
    gpio_set_function(PIN_RGB_R, GPIO_FUNC_PWM);
    gpio_set_function(PIN_RGB_G, GPIO_FUNC_PWM);
    gpio_set_function(PIN_RGB_B, GPIO_FUNC_PWM);
    slice_r = pwm_gpio_to_slice_num(PIN_RGB_R); chan_r = pwm_gpio_to_channel(PIN_RGB_R);
    slice_g = pwm_gpio_to_slice_num(PIN_RGB_G); chan_g = pwm_gpio_to_channel(PIN_RGB_G);
    slice_b = pwm_gpio_to_slice_num(PIN_RGB_B); chan_b = pwm_gpio_to_channel(PIN_RGB_B);
    pwm_set_wrap(slice_r, 65535); pwm_set_wrap(slice_g, 65535); pwm_set_wrap(slice_b, 65535);
    pwm_set_enabled(slice_r, true); pwm_set_enabled(slice_g, true); pwm_set_enabled(slice_b, true);

    //Cria tasks do FreeRTOS
    xTaskCreate(Task_Sensor, "Sensor", 256, NULL, 3, NULL); //Prioridade alta para sensor
    xTaskCreate(Task_Input, "Input", 512, NULL, 2, NULL);
    xTaskCreate(Task_Control, "Control", 512, NULL, 2, NULL);
    xTaskCreate(Task_Display, "Display", 512, NULL, 1, NULL);
    xTaskCreate(Task_Buzzer, "Buzzer", 256, NULL, 1, NULL);
    xTaskCreate(Task_Webserver, "WebSrv", 1024, NULL, 1, NULL);

    vTaskStartScheduler(); //Inicia o escalonador do FreeRTOS
    while (true) tight_loop_contents();
    return 0;
}
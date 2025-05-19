# 🚀 Tempestade Radar – Sistema de Monitoramento e Alerta de Enchentes  
> *Um sistema IoT embarcado para monitoramento em tempo real de níveis de água e chuva, com previsão e alertas visuais/sonoros.*

## 📝 Descrição Breve  
O **Tempestade Radar** é um sistema embarcado baseado no Raspberry Pi Pico que monitora níveis de água e volume de chuva em tempo real usando sensores analógicos conectados via ADC. Ele integra um display OLED SSD1306 para exibição de dados e gráficos, uma matriz de LEDs WS2812 para alertas visuais, LEDs indicadores (vermelho e verde), um buzzer com controle PWM para notificações sonoras e um botão para navegação entre telas. Ideal para prevenção de enchentes, o sistema calcula percentuais, prevê níveis de água com base em tendências históricas e emite alertas configuráveis. Utiliza FreeRTOS para multitarefa, o Pico SDK para interação com hardware e bibliotecas personalizadas (`ssd1306` e `matriz_led`) para o display e matriz de LEDs, compilado via CMake.

## ✨ Funcionalidades Principais  
* 📈 Leitura de sensores analógicos para nível de água e volume de chuva  
* 🔄 Previsão de nível de água com base em histórico e impacto da chuva  
* 🌐 Exibição de dados, barras de progresso e gráficos no display OLED  
* 🔔 Alarme sonoro via buzzer com padrões distintos por condição  
* 🖥️ Animações na matriz de LEDs e controle de LEDs indicadores para alertas  
* 🎛️ Navegação entre quatro telas (dados, barras, gráficos) via botão físico  

## ⚙️ Pré-requisitos / Hardware Necessário  
### Hardware  
| Componente            | Quant. | Observações                          |
|-----------------------|--------|--------------------------------------|
| Raspberry Pi Pico     | 1      | Microcontrolador principal           |
| Display OLED SSD1306  | 1      | 128x64 pixels, I2C, endereço 0x3C    |
| Matriz de LEDs WS2812 | 1      | Controlada via PIO (pino definido em `matriz_led.h`) |
| LED Vermelho          | 1      | Conectado ao GPIO 13                 |
| LED Verde             | 1      | Conectado ao GPIO 11                 |
| Buzzer                | 1      | PWM controlado, conectado ao GPIO 10 |
| Botão                 | 1      | Conectado ao GPIO 5, com pull-up     |
| Sensor Analógico (X)  | 1      | Nível de água, ADC1 (GPIO 27)        |
| Sensor Analógico (Y)  | 1      | Volume de chuva, ADC0 (GPIO 26)      |

### Software / Ferramentas  
- **SDK**: Raspberry Pi Pico SDK  
- **Toolchain**: GCC para ARM (via CMake)  
- **Bibliotecas**:  
  - `ssd1306` (controle do display OLED)  
  - `matriz_led` (controle da matriz WS2812)  
  - FreeRTOS (gerenciamento de tarefas)  
- **Sistema Operacional Testado**: Linux (Ubuntu 20.04/22.04), Windows 10/11 (com WSL)  
- **Ferramentas Adicionais**: Git, CMake (versão 3.15+), Make  

## 🔌 Conexões / Configuração Inicial  
### Pinagem resumida  
- **I2C SDA**: GPIO 14 (display OLED)  
- **I2C SCL**: GPIO 15 (display OLED)  
- **Botão**: GPIO 5 (entrada com pull-up)  
- **ADC X (nível de água)**: GPIO 27 (ADC1)  
- **ADC Y (volume de chuva)**: GPIO 26 (ADC0)  
- **LED Vermelho**: GPIO 13 (saída)  
- **LED Verde**: GPIO 11 (saída)  
- **Buzzer**: GPIO 10 (saída PWM)  
- **Matriz WS2812**: Pino definido em `matriz_led.h` (verificar biblioteca)  

> **Nota**: Conecte um GND comum entre todos os componentes. A tensão recomendada é 3.3V, compatível com o Raspberry Pi Pico.

### Configuração de Software (primeira vez)  
```bash
git clone https://github.com/seu-usuario/TempestadeRadar.git
cd TempestadeRadar
git submodule update --init --recursive
```

## ▶️ Como Compilar e Executar  
```bash
mkdir build && cd build
export PICO_SDK_PATH=~/pico-sdk  # Ajuste o caminho para o Pico SDK
cmake ..
make -j4
```
- Para subir o firmware à placa:  
  ```bash
  make flash
  ```
- **Logs**: Acesse via UART (GPIO 0 e 1) com um terminal serial (ex.: `minicom` ou `picocom`, 115200 baud).  
- **Dashboard**: Não há interface gráfica externa; os dados são exibidos diretamente no OLED.

### 📁 Estrutura do Projeto  
```
TempestadeRadar/
├── .vscode/              # Configurações do VS Code
├── build/                # Diretório de compilação (gerado)
├── lib/                  # Bibliotecas personalizadas
│   ├── Display_Bibliotecas/
│   │   ├── font.h        # Fontes para o display OLED
│   │   ├── ssd1306.c     # Driver do SSD1306
│   │   ├── ssd1306.h     # Header do SSD1306
│   ├── Matriz_Bibliotecas/
│   │   ├── generated/    # Padrões gerados para a matriz
│   │   ├── matriz_led.c  # Driver da matriz WS2812
│   │   ├── matriz_led.h  # Header da matriz WS2812
│   ├── ws2812.pio        # Programa PIO para WS2812
├── FreeRTOSConfig.h      # Configurações do FreeRTOS
├── .gitignore            # Arquivos ignorados pelo Git
├── CMakeLists.txt        # Configuração do CMake
├── diagram.json          # Diagrama do projeto (opcional)
├── main.c                # Código principal
├── pico_sdk_import.cmake # Importação do Pico SDK
├── README.md             # Este arquivo
└── wokwi.toml            # Configuração para simulação no Wokwi
```

## 🐛 Debugging / Solução de Problemas  
- **Wi-Fi**: Não aplicável (projeto offline).  
- **Travamentos**: Verifique o tamanho das pilhas das tarefas em `main.c` (ex.: 512 para `TaskMedicao`); aumente se necessário em `FreeRTOSConfig.h`.  
- **Sensores**: Certifique-se de que os ADCs variam entre 0 e 4095; ruído pode indicar conexões soltas.  
- **Display OLED**: Se não exibir, confirme o endereço I2C (0x3C) e pull-ups em SDA/SCL.  
- **Buzzer**: Sem som? Teste o PWM com `buzzer_on(1000)`; ajuste `pwm_set_clkdiv` se distorcido.  
- **Matriz de LEDs**: Se não acender, verifique o pino em `matriz_led.h` e a inicialização em `inicializar_matriz_led()`.  

## 👤 Autor / Contato  
- **Nome**: Jonas Souza  
- **E-mail**: Jonassouza871@hotmail.com 

---

Seja bem-vindo(a) ao **Tempestade Radar**! Este projeto foi criado para ajudar na prevenção de enchentes, e sua participação pode torná-lo ainda melhor. Clone o repositório, experimente as funcionalidades e sinta-se à vontade para enviar sugestões ou pull requests. Juntos, podemos fazer a diferença! 🚀

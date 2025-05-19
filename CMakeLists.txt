
#Versão mínima do CMake requerida
cmake_minimum_required(VERSION 3.13)

#Configurações de padrões de linguagem
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#Define o tipo de placa como Raspberry Pi Pico W
set(PICO_BOARD pico CACHE STRING "Board type")

#Inclui o SDK do Pico
include(pico_sdk_import.cmake)

#Configura o FreeRTOS
set(FREERTOS_KERNEL_PATH "C:/Users/Jonas/FreeRTOS-Kernel")
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

#Define o nome do projeto
project(RTOS_filas C CXX ASM)

#Inicializa o SDK do Pico
pico_sdk_init()

#Diretórios de inclusão para headers do projeto
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/lib/Display_Bibliotecas
    ${CMAKE_SOURCE_DIR}/lib/Matriz_Bibliotecas
)

#Cria o executável com os arquivos fonte
add_executable(RTOS_filas
    main.c
    lib/Display_Bibliotecas/ssd1306.c
    lib/Matriz_Bibliotecas/matriz_led.c
)

#Vincula as bibliotecas necessárias ao executável
target_link_libraries(RTOS_filas
    pico_stdlib              #Biblioteca padrão do Pico
    hardware_i2c             #Driver I2C do Pico SDK
    hardware_pwm             #Driver PWM do Pico SDK
    hardware_pio             #Driver PIO do Pico SDK
    hardware_adc             #Driver ADC do Pico SDK
    FreeRTOS-Kernel          #Kernel do FreeRTOS
    FreeRTOS-Kernel-Heap4    #Gerenciador de memória do FreeRTOS
)

#Habilita saída padrão via USB e UART
pico_enable_stdio_usb(RTOS_filas 1)
pico_enable_stdio_uart(RTOS_filas 1)

#Gera arquivos adicionais (binário, UF2, etc.)
pico_add_extra_outputs(RTOS_filas)

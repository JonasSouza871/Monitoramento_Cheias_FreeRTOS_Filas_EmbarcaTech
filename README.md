# Sistema de Monitoramento de Enchentes

## Descrição

O Sistema de Monitoramento de Enchentes é um projeto embarcado desenvolvido para o Raspberry Pi Pico, utilizando o sistema operacional de tempo real FreeRTOS. Este sistema é projetado para monitorar o nível de água e o volume de chuva em tempo real, utilizando um joystick como entrada simulada para os sensores. Os dados são exibidos em um display OLED, e alertas são fornecidos através de LEDs.

## Funcionalidades

- **Monitoramento em Tempo Real**: Leitura contínua do nível de água e volume de chuva.
- **Exibição de Dados**: Visualização dos dados em um display OLED.
- **Alertas Visuais**: Utilização de LEDs para indicar alertas de enchente.
- **Previsão de Enchentes**: Implementação de regressão linear para prever possíveis enchentes.
- **Alternância de Telas**: Navegação entre diferentes telas de exibição através de um botão.

## Componentes Utilizados

- **Raspberry Pi Pico**: Microcontrolador principal.
- **Joystick**: Utilizado como entrada simulada para os sensores de nível de água e volume de chuva.
- **Display OLED**: Para exibição dos dados monitorados.
- **LEDs**: Para alertas visuais.
- **Botão**: Para alternância entre diferentes telas de exibição.

## Configuração do Ambiente

1. **Hardware**:
   - Conecte o joystick, display OLED, LEDs e botão ao Raspberry Pi Pico conforme o esquema de conexão.
   - Certifique-se de que todos os componentes estão corretamente alimentados e conectados.

2. **Software**:
   - Instale o SDK do Raspberry Pi Pico e o FreeRTOS.
   - Clone este repositório para o seu ambiente de desenvolvimento.
   - Configure o projeto no seu ambiente de desenvolvimento preferido (por exemplo, VSCode com PlatformIO).

## Como Utilizar

1. **Compilação e Upload**:
   - Compile o código-fonte e faça o upload para o Raspberry Pi Pico.

2. **Operação**:
   - Ao ligar o sistema, os dados de nível de água e volume de chuva serão exibidos no display OLED.
   - Utilize o botão para alternar entre diferentes telas de exibição.
   - Observe os LEDs para alertas de enchente.

## Estrutura do Projeto

- `src/`: Contém o código-fonte do projeto.
- `include/`: Contém os arquivos de cabeçalho.
- `docs/`: Contém documentação adicional e esquemas de conexão.

## Contribuição

Contribuições são bem-vindas! Sinta-se à vontade para abrir issues e pull requests.


# bongarbit-sampler

Sampler com disparo por botão em ESP32-S3, tocando um sample via DAC I2S
PCM5102A usando a biblioteca [M16](https://github.com/algomusic/M16).

## Hardware

- ESP32-S3 N16R8 (16MB flash Quad-SPI + 8MB PSRAM Octal-SPI)
- DAC I2S PCM5102A (breakout genérico, ex. "GY-PCM5102")
- Leitor de cartão microSD (módulo SPI, opcional - ver `USE_SD_SAMPLER` abaixo)
- 1 botão momentâneo (trigger de sample)

Plano completo de fiação, com justificativa de cada pino e as ressalvas
específicas do N16R8: [docs/circuit.md](docs/circuit.md).

O que esta branch mudou em relação ao estado inicial do projeto, e a
justificativa de cada alteração:
[docs/mudancas-vs-main.md](docs/mudancas-vs-main.md).

## Firmware

`src/main.cpp` toca o sample através da engine `Samp` da M16. A fonte do
áudio é escolhida em tempo de compilação via `USE_SD_SAMPLER`:

- **`1` (padrão):** no boot, monta o SD e carrega um `.wav` inteiro pra RAM
  (a M16 prioriza a PSRAM de 8MB desse board). O cartão só é acessado nesse
  momento - depois disso a reprodução é só da PSRAM, sem tocar no SD de novo.
  Coloque um único arquivo `.wav` (44.1kHz recomendado, mono ou estéreo, PCM
  8/16/24/32-bit) na raiz do cartão antes de ligar.
- **`0`:** toca um sample embutido direto na flash do firmware
  (`src/kick_normalizado_data.h`, gerado a partir de um `.wav` com
  `tools/wav_to_header.py` - ver seção abaixo), sem precisar de cartão SD.

Em ambos os modos, apertar o botão de trigger (GPIO 4) reinicia a reprodução
do começo.

## Gerando um sample embutido na flash

Pra usar o modo `USE_SD_SAMPLER=0` com outro sample:

```
python3 tools/wav_to_header.py <entrada.wav> src/<nome>_data.h <NOME_ARRAY>
```

Depois ajuste o `#include` e a chamada de `samp.setTable(...)` em
`src/main.cpp` pra apontar pro novo header.

## Build

```
pio run            # compila
pio run -t upload  # grava
pio device monitor # log serial (115200 baud)
```

Requer uma versão do `platform = espressif32` com core Arduino 3.x
(baseado em ESP-IDF 5), pois a M16 usa o driver I2S novo (`i2s_std`).

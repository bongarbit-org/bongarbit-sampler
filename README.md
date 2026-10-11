# bongarbit-sampler

Firmware para ESP32-S3 com DAC I2S PCM5102A, sobre a biblioteca
[M16](https://github.com/algomusic/M16). O repositório tem dois instrumentos,
que compartilham o mesmo motor de áudio (blocos de float, buffer de DMA de
5,8 ms) e os mesmos headers testados no PC:

- **Sampler** (`src/main.cpp`, env padrão `esp32-s3-devkitc-1`): três botões
  disparam samples carregadas do cartão SD para a PSRAM, e um quarto botão,
  segurado, baixa a mixagem inteira uma oitava.
- **Agbau** (`src/agbau_main.cpp`, env `agbau`): a migração do
  [Agbau](https://www.instagram.com/bongarbit/), hoje uma Bela mais um Arduino,
  para um único ESP32-S3. São 16 vozes com envelope ADSR, e o kit fica numa
  partição da flash. **Estágio atual:** quatro botões tocando quatro vozes,
  ouvido na placa. O plano completo e a situação de cada etapa estão em
  [docs/agbau-migration.md](docs/agbau-migration.md).

## Hardware

- ESP32-S3 N16R8 (16 MB de flash Quad-SPI + 8 MB de PSRAM Octal-SPI)
- DAC I2S PCM5102A (breakout genérico, ex. "GY-PCM5102")
- Leitor de microSD (módulo SPI): só o sampler usa
- Botões momentâneos nos GPIO 4, 5, 6 e 7, cada um ligado ao GND

A fiação do sampler, com a justificativa de cada pino e as armadilhas do
N16R8, está em [docs/circuit.md](docs/circuit.md). O mapa de pinos do Agbau
foi lido da placa de circuito do próprio Agbau e está em
[docs/agbau-circuit.md](docs/agbau-circuit.md), com o que ainda precisa ser
conferido no instrumento antes de fiar.

A placa tem dois conectores USB. O áudio só é confiável com ela ligada pelo
**COM** ([docs/bancada.md](docs/bancada.md), seção 11). O log serial do
sampler sai pela USB nativa; o do Agbau sai pelo COM.

## Ambiente

O PlatformIO roda num venv local (ignorado pelo git):

```
python3 -m venv .venv && .venv/bin/pip install platformio
```

A plataforma é o fork **pioarduino**, fixado numa versão exata no
`platformio.ini`, porque a M16 precisa do core Arduino 3.x (ESP-IDF 5) e a
plataforma oficial `espressif32` ainda não o oferece. A M16 também está fixada
num commit exato. O motivo das duas fixações está no [CLAUDE.md](CLAUDE.md).

## Sampler

```
.venv/bin/pio run -t upload       # compila e grava
.venv/bin/pio device monitor      # log serial, 115200 baud (USB nativa)
```

A fonte do áudio é escolhida em tempo de compilação por `USE_SD_SAMPLER`, em
`src/main.cpp`:

- **`1` (padrão):** no boot, monta o SD e carrega as samples inteiras na PSRAM.
  O GPIO 4 toca o primeiro `.wav` que o cartão listar, e os GPIO 5 e 6 tocam
  `/1.wav` e `/2.wav`. O cartão só é lido no boot e pode ser removido depois.
  Use WAV de 16 bits a 44,1 kHz.
- **`0`:** toca um kick embutido no firmware (`src/kick_normalizado_data.h`),
  sem cartão. Nesse modo não há os GPIO 5 e 6.

Nos dois modos, segurar o GPIO 7 baixa tudo uma oitava, e bater de novo num
botão que já está tocando reinicia a sample do começo.

### Gerando um sample embutido na flash

Para usar `USE_SD_SAMPLER=0` com outro sample:

```
python3 tools/wav_to_header.py <entrada.wav> src/<nome>_data.h <NOME_ARRAY>
```

Depois ajuste o `#include` e a chamada de `samp.setTable(...)` em
`src/main.cpp` para o novo header.

## Agbau

O kit (24 WAVs, ~9 MB) é gravado **uma vez** numa partição própria da flash,
separado do firmware. Regravar o firmware, inclusive o do sampler, não apaga o
kit.

1. Gerar a imagem do kit a partir das samples do Agbau (o repositório do
   Agbau ao lado deste). A imagem vai para `.pio/`, que o git ignora:

   ```
   python3 tools/agbau_kit.py ../agbau/Bela/agbau-sampler-poly/samples --partition .pio/agbau_kit/kit.bin
   ```

2. Gravar a imagem na partição `samples`. Leva uns 2 minutos e só precisa ser
   repetido quando o kit mudar:

   ```
   ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --baud 921600 write_flash 0x310000 .pio/agbau_kit/kit.bin
   ```

3. Gravar o firmware e abrir o log, que neste env sai pelo COM:

   ```
   .venv/bin/pio run -e agbau -t upload
   .venv/bin/pio device monitor
   ```

No boot, o firmware confere o CRC do kit e diz quantas takes encontrou. Cada
botão toca uma voz: GPIO 4 → b1 (baixo), GPIO 5 → p1 (percussão, 5 takes),
GPIO 6 → p3 (ganzá) e GPIO 7 → p5 (percussão, 3 takes). Para trocar a voz de
um botão, mude `kButtonVoice` no topo de `src/agbau_main.cpp`.

## Testes

```
.venv/bin/pio test -e native
```

São oito suítes, 128 casos, rodando no PC sem placa: disparo dos botões,
limitador, pitch shifter, envelope, vozes, matriz de teclas e a imagem do kit.
Os mesmos headers entram no firmware e nos testes, então o teste fala do
código que vai para a placa. O que só a placa responde (DAC, PSRAM, buffer de
DMA, o som) fica registrado em [docs/bancada.md](docs/bancada.md).

## Documentação

- [CLAUDE.md](CLAUDE.md): as decisões de projeto e o que não "consertar", em
  inglês.
- [docs/circuit.md](docs/circuit.md): fiação do sampler.
- [docs/circuit-rc.md](docs/circuit-rc.md): a variante de baixa latência
  dessa fiação.
- [docs/agbau-migration.md](docs/agbau-migration.md): o plano do Agbau, etapa
  por etapa, com a situação de cada uma.
- [docs/agbau-circuit.md](docs/agbau-circuit.md): o mapa de pinos do Agbau.
- [docs/bancada.md](docs/bancada.md): tudo o que foi medido e ouvido na placa,
  com data e conector.

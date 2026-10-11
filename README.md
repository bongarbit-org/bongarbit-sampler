# bongarbit-sampler

Firmware para ESP32-S3 com DAC I2S PCM5102A, sobre a biblioteca
[M16](https://github.com/algomusic/M16). O repositório tem **dois
instrumentos**, cada um na sua pasta, que compartilham a mesma placa e o mesmo
motor de áudio (blocos de float, buffer de DMA de 5,8 ms):

- **[Sampler](sampler/)** (env `sampler`, o padrão): três botões disparam
  samples carregadas do cartão SD para a PSRAM, e um quarto botão, segurado,
  baixa a mixagem inteira uma oitava.
- **[Agbau](agbau/)** (env `agbau`): a migração do
  [Agbau](https://www.instagram.com/bongarbit/), hoje uma Bela mais um Arduino,
  para um único ESP32-S3. São 16 vozes com envelope ADSR, e o kit fica numa
  partição da flash. **Estágio atual:** três botões tocando três vozes,
  ouvidos na placa, mais o botão de modo e o pedal, com repeat e looper
  testados no host e ainda não tocados na placa. O plano completo e a situação de cada etapa estão em
  [agbau/docs/migration.md](agbau/docs/migration.md).

Como usar cada um está no README da pasta dele.

## Organização

```
sampler/            só o sampler usa o que está aqui
  src/              firmware: main.cpp, o pitch shifter, o kick embutido
  bench/            sd_test.cpp, diagnóstico do cartão SD
  samples/          os WAVs de onde saem os headers embutidos
  tools/            wav_to_header.py
  docs/             circuit-rc.md, a variante de baixa latência dos botões
agbau/              só o Agbau usa o que está aqui
  src/              firmware: main.cpp, vozes, envelope, matriz, leitor do kit
  bench/            agbau_storage.cpp, o spike de armazenamento
  tools/            agbau_kit.py, que gera a imagem do kit
  docs/             migration.md (o plano) e circuit.md (o mapa de pinos)
  partitions.csv    a tabela de partições com o kit
shared/             o que os dois compilam ou usam igual
  src/              o motor: AudioBlock, ring de DMA, TriggerInput, limitador
  sim/              o modelo dos botões, para os testes e para o trigger_sim
  bench/            i2s_bench.cpp, a medição do transporte I2S
  docs/             circuit.md (a placa base) e bancada.md (o diário da placa)
test/               testes no PC, divididos igual: shared/, sampler/, agbau/
platformio.ini      um projeto só, um env por sketch
```

A regra é que nada dentro de `sampler/` é usado pelo Agbau, e vice-versa: o
que os dois usam mora em `shared/`. No firmware, o build garante isso, porque
cada env só tem no caminho de include `shared/src` e a pasta do próprio
sketch.

Os testes ficam em `test/`, e não dentro de cada instrumento, porque o
PlatformIO só aceita uma pasta de testes por projeto. Dentro dela a divisão é a
mesma.

Continua sendo **um** projeto do PlatformIO, e não dois, de propósito. Os dois
instrumentos rodam na mesma placa, com a mesma plataforma e a mesma M16
fixadas num commit exato, e o motor que compartilham é testado uma vez só.
Separar em dois projetos duplicaria essas fixações e criaria um terceiro só
para o código comum.

## Hardware

- ESP32-S3 N16R8 (16 MB de flash Quad-SPI + 8 MB de PSRAM Octal-SPI)
- DAC I2S PCM5102A (breakout genérico, ex. "GY-PCM5102")
- Leitor de microSD (módulo SPI): só o sampler usa
- Botões momentâneos nos GPIO 4, 5, 6 e 7, cada um ligado ao GND

A fiação da placa base, com a justificativa de cada pino e as armadilhas do
N16R8, está em [shared/docs/circuit.md](shared/docs/circuit.md). O mapa de
pinos do Agbau foi lido da placa de circuito do próprio Agbau e está em
[agbau/docs/circuit.md](agbau/docs/circuit.md), com o que ainda precisa ser
conferido no instrumento antes de fiar.

A placa tem dois conectores USB. O áudio só é confiável com ela ligada pelo
**COM** ([shared/docs/bancada.md](shared/docs/bancada.md), seção 11). O log
serial do sampler sai pela USB nativa; o do Agbau sai pelo COM.

## Ambiente

O PlatformIO roda num venv local (ignorado pelo git). Todos os comandos deste
repositório rodam da raiz dele:

```
python3 -m venv .venv && .venv/bin/pip install platformio
```

A plataforma é o fork **pioarduino**, fixado numa versão exata no
`platformio.ini`, porque a M16 precisa do core Arduino 3.x (ESP-IDF 5) e a
plataforma oficial `espressif32` ainda não o oferece. A M16 também está fixada
num commit exato. O motivo das duas fixações está no [CLAUDE.md](CLAUDE.md).

| Env | Pasta | O que grava |
|---|---|---|
| `sampler` (padrão) | `sampler/src/` | o sampler |
| `sd_test` | `sampler/bench/` | diagnóstico do cartão, com a `SD.h` do core |
| `agbau` | `agbau/src/` | o Agbau |
| `agbau_storage` | `agbau/bench/` | o spike de armazenamento do Agbau |
| `i2s_bench` | `shared/bench/` | a medição do transporte I2S, sem M16 |
| `native` | `test/` | nada: roda os testes no PC |

```
.venv/bin/pio run -e agbau -t upload
```

## Testes

```
.venv/bin/pio test -e native                   # tudo
.venv/bin/pio test -e native -f 'agbau/*'      # só um instrumento
```

São oito suítes, 128 casos, rodando no PC sem placa: disparo dos botões,
limitador, pitch shifter, envelope, vozes, matriz de teclas e a imagem do kit.
Os mesmos headers entram no firmware e nos testes, então o teste fala do
código que vai para a placa. O que só a placa responde (DAC, PSRAM, buffer de
DMA, o som) fica registrado em [shared/docs/bancada.md](shared/docs/bancada.md).

## Documentação

- [CLAUDE.md](CLAUDE.md): as decisões de projeto e o que não "consertar", em
  inglês.
- [shared/docs/circuit.md](shared/docs/circuit.md): a fiação da placa base e
  dos botões do sampler.
- [shared/docs/bancada.md](shared/docs/bancada.md): tudo o que foi medido e
  ouvido na placa, com data e conector.
- [sampler/docs/circuit-rc.md](sampler/docs/circuit-rc.md): a variante de
  baixa latência dos botões do sampler.
- [agbau/docs/migration.md](agbau/docs/migration.md): o plano do Agbau, etapa
  por etapa, com a situação de cada uma.
- [agbau/docs/circuit.md](agbau/docs/circuit.md): o mapa de pinos do Agbau.

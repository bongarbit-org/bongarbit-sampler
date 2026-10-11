# Sampler

Três botões disparam samples carregadas do cartão SD para a PSRAM, e um quarto
botão, segurado, baixa a mixagem inteira uma oitava. O motor de áudio, o
condicionamento dos botões e a fiação da placa são compartilhados com o Agbau
e ficam em [`shared/`](../shared/); o mapa do repositório está no
[README da raiz](../README.md).

Os comandos rodam da raiz do repositório.

```
.venv/bin/pio run -e sampler -t upload   # compila e grava (é o env padrão)
.venv/bin/pio device monitor             # log serial, 115200 baud (USB nativa)
```

A fiação está em [shared/docs/circuit.md](../shared/docs/circuit.md).

## Fonte do áudio

É escolhida em tempo de compilação por `USE_SD_SAMPLER`, em
[src/main.cpp](src/main.cpp):

- **`1` (padrão):** no boot, monta o SD e carrega as samples inteiras na PSRAM.
  O GPIO 4 toca o primeiro `.wav` que o cartão listar, e os GPIO 5 e 6 tocam
  `/1.wav` e `/2.wav`. O cartão só é lido no boot e pode ser removido depois.
  Use WAV de 16 bits a 44,1 kHz.
- **`0`:** toca um kick embutido no firmware
  (`src/kick_normalizado_data.h`), sem cartão. Nesse modo não há os GPIO 5 e 6.

Nos dois modos, segurar o GPIO 7 baixa tudo uma oitava, e bater de novo num
botão que já está tocando reinicia a sample do começo.

## Gerando um sample embutido na flash

Para usar `USE_SD_SAMPLER=0` com outro sample:

```
python3 sampler/tools/wav_to_header.py <entrada.wav> sampler/src/<nome>_data.h <NOME_ARRAY>
```

Depois ajuste o `#include` e a chamada de `samp.setTable(...)` em
[src/main.cpp](src/main.cpp) para o novo header. Os WAVs de origem dos headers
que já existem estão em [samples/](samples/).

## Diagnóstico do cartão

Se o SD não monta, o env `sd_test` ([bench/sd_test.cpp](bench/sd_test.cpp))
grava o exemplo da `SD.h` do próprio core Arduino, sem SdFat nem M16. Serve
para separar "cartão ou fiação com problema" de "problema da biblioteca":

```
.venv/bin/pio run -e sd_test -t upload
```

## Documentação

- [docs/circuit-rc.md](docs/circuit-rc.md): a variante de baixa latência dos
  botões, com o filtro RC e o rig para medir a latência de verdade.
- [shared/docs/bancada.md](../shared/docs/bancada.md): o que foi medido e
  ouvido na placa. As seções 1 a 12 são do sampler e da placa base; as 13 e
  14, do Agbau.

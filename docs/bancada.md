# Bancada — plano para o dia da placa

Roteiro para a primeira montagem física do sampler. Até agora **nada neste
projeto foi verificado em hardware**: latência, profundidade do ring de DMA,
PSRAM, cartão SD e o filtro RC são todos aritmética, simulação ou datasheet.
Este documento existe para trocar isso por medição, na ordem que faz um defeito
apontar para a própria causa.

Complementa, não substitui:

- [circuit.md](circuit.md) — fiação base e justificativa de cada pino
- [circuit-rc.md](circuit-rc.md) — o filtro RC e o rig de latência em detalhe

---

## 1. Lista de peças

### Placas e módulos

| Qtd | Item | Observação |
|---|---|---|
| 1 | ESP32-S3 **N16R8** | 16MB flash + 8MB PSRAM. A variante importa — ver [circuit.md](circuit.md) |
| 1 | DAC PCM5102A (breakout "GY-PCM5102") | confira se o pino `FLT` está exposto |
| 1 | Módulo leitor microSD (SPI) | 3.3V |
| 1 | Cartão microSD | FAT32, qualquer tamanho |
| 4 | Tactile switch de 4 pernas | 6mm, os comuns |

### Componentes passivos

| Qtd | Valor | Para quê | Marcação |
|---|---|---|---|
| 6 | 10 kΩ | 4 pull-ups do RC + 2 do divisor do probe | marrom · preto · laranja · dourado |
| 4 | 220 Ω | resistor série do RC | vermelho · vermelho · marrom · dourado |
| 5 | 100 nF cerâmico | 4 do RC + 1 do probe | serigrafia `104` |

Valores não são críticos: 4,7 k–10 k no pull-up e 100 Ω–1 k no série funcionam
igual. O que importa é a relação entre os dois.

### Ferramentas e acessórios

- Protoboard e jumpers macho-macho
- **Cabo USB-C de dados** — não serve cabo só de carga; é a causa nº 1 de
  "a placa não aparece"
- Fone de ouvido ou caixa amplificada com P2
- Multímetro (para as checagens frias e o divisor do RC)
- Opcional, se tiver: osciloscópio — dispensa o rig de latência

### Arquivos no cartão

Coloque na **raiz** do cartão, antes de ligar:

- `/1.wav`, `/2.wav`, `/3.wav` — os três pads
- **mais um quarto `.wav`** — ver a armadilha abaixo

Formato: 44,1 kHz recomendado, mono ou estéreo, PCM 8/16/24/32-bit.

> **Armadilha do `FILE_INDEX`.** O GPIO4 não toca um arquivo com nome fixo: ele
> toca `FILE_INDEX 0`, que é **o primeiro arquivo que o cartão enumera**. Se o
> cartão só tiver `1.wav`, `2.wav` e `3.wav`, o GPIO4 vai duplicar um dos pads.
> Para ter quatro sons distintos, ponha um quarto arquivo — e a ordem de
> enumeração decide qual deles o GPIO4 pega, então o log de boot é quem vai te
> contar. Use samples **curtos e percussivos**, com ataque no primeiro frame:
> um `.wav` que começa em silêncio faz o probe de latência dar timeout.

---

## 2. O circuito

### Mapa de pinos

| Sinal | GPIO | Vai para |
|---|---|---|
| I2S BCK | 16 | PCM5102A `BCK` |
| I2S WS | 17 | PCM5102A `LCK` |
| I2S DOUT | 18 | PCM5102A `DIN` |
| SD CS | 10 | microSD `CS` |
| SD MOSI | 11 | microSD `MOSI` / DI |
| SD SCLK | 12 | microSD `SCK` |
| SD MISO | 13 | microSD `MISO` / DO |
| Trigger | 4 | botão → GND |
| Pad 1 (`/1.wav`) | 5 | botão → GND |
| Pad 2 (`/2.wav`) | 6 | botão → GND |
| Pad 3 (`/3.wav`) | 7 | botão → GND |
| Probe de latência | 1 | divisor polarizado (fase E) |

**GPIO 33–37 não existem para você** neste board: são as linhas do PSRAM
Octal. Guias genéricos de ESP32-S3 listam esses pinos como livres.

### Pinos de controle do PCM5102A

| Pino | Ligar a | Por quê |
|---|---|---|
| `SCK` | **GND** | habilita o clock interno; sem isso precisaria de oscilador externo |
| `FMT` | GND | formato I2S padrão |
| `XSMT` | **3V3** | se ficar baixo ou flutuando, a saída fica **muda** mesmo com I2S funcionando |
| `DEMP` | GND | desativa de-ênfase |
| `FLT` | **3V3** | filtro de baixa latência: 80 µs em vez de 0,50 ms, e sem *pre-ringing* no ataque |

Muitos breakouts genéricos já amarram esses pinos internamente e nem os expõem.
Confira a serigrafia. Se o `FLT` estiver preso em GND, você perde 420 µs e
segue a vida — não inviabiliza nada.

### Botões: comece SEM o RC

Fase A a D usam o botão nu:

```
GPIO ──● botão ●── GND      (pull-up interno, ativo em baixo)
```

O RC entra só na **fase F**. Isso é deliberado: montar 12 componentes antes do
primeiro boot significa que uma falha pode estar em qualquer lugar. Primeiro o
sistema mais simples que pode funcionar, depois uma variável de cada vez.

> As duas pernas do *mesmo lado* de um tactile switch já vêm curto-circuitadas
> de fábrica. Use uma perna de cada lado — se o botão atravessa o vão central
> da protoboard, uma perna de cada lado do vão. Duas pernas da mesma coluna =
> botão permanentemente fechado.

### Diagrama de blocos

```
                +----------------------+
   Trigger ─────┤ GPIO4                |
   Pad 1 ───────┤ GPIO5                |
   Pad 2 ───────┤ GPIO6                |
   Pad 3 ───────┤ GPIO7                |
                |                      |
                | GPIO16 (BCK)  ───────┼──► PCM5102A BCK
                | GPIO17 (WS)   ───────┼──► PCM5102A LCK
                | GPIO18 (DOUT) ───────┼──► PCM5102A DIN
                |                      |    SCK→GND  FMT→GND
                |                      |    XSMT→3V3 FLT→3V3
                |                      |    DEMP→GND
                | GPIO10 (CS)   ───────┼──► SD CS
                | GPIO11 (MOSI) ───────┼──► SD MOSI
                | GPIO12 (SCK)  ───────┼──► SD SCK
                | GPIO13 (MISO) ◄──────┼─── SD MISO
                |                      |
                | GPIO1 (ADC) ◄────────┼─── divisor do probe (fase E)
                |                      |
                | 3V3 / GND comum ─────┼──► DAC, SD, botões
                +----------------------+
```

**Terra comum de verdade.** O [circuit.md](circuit.md) documenta um caso real
de áudio instável causado por GND flutuante entre placa e DAC. Um único trilho.

---

## 3. Preparar o software

```bash
cd ~/Documents/Codigo/bongarbit/bongarbit-sampler
.venv/bin/pio test -e native      # deve dar 23/23 antes de encostar na placa
.venv/bin/pio run                 # deve compilar
```

Se a suíte ou o build já falharem no seu ambiente, resolva isso **antes** de
ligar a placa — senão você vai depurar dois problemas ao mesmo tempo.

Estado dos interruptores de compilação, como estão hoje em
[../src/main.cpp](../src/main.cpp):

| Macro | Valor | Fase que muda |
|---|---|---|
| `USE_SD_SAMPLER` | 1 | — |
| `DEBUG_BOOT` | 1 | — (deixe ligado) |
| `DEBUG_TRIGGERS` | 0 | fase D |
| `LATENCY_PROBE` | 0 | fase E |
| `DMA_BUFFER_LENGTH` | 64 (5,8 ms) | fase G |

Nada a mudar antes da fase D. O firmware já reporta a PSRAM no boot — ver a
fase B.

---

## 4. Passo a passo

Cada fase tem um critério de sucesso. **Não avance com uma fase vermelha** — a
ordem existe para que a falha aponte a causa.

### Fase A — bancada fria, sem energia

Multímetro em continuidade, placa desligada.

- [ ] Nenhum curto entre 3V3 e GND
- [ ] Cada botão: continuidade só quando apertado (pega a armadilha das pernas)
- [ ] `XSMT` do DAC em 3V3 — se estiver em GND ou solto, a saída fica muda
- [ ] `SCK` do DAC em GND
- [ ] GND comum entre placa, DAC, SD e os quatro botões

### Fase B — primeiro boot, sem esperar som

```bash
.venv/bin/pio run -t upload
.venv/bin/pio device monitor
```

Aperte reset e leia. Esperado, na ordem:

```
PSRAM: 8386295 bytes free of 8388608
SD: init OK
WAV: loaded '<nome>' - 1 ch, 44100 Hz, N frames (~M ms)
Pad 1: loaded '/1.wav' - ...
Pad 2: loaded '/2.wav' - ...
Pad 3: loaded '/3.wav' - ...
DAC: I2S started - BCK=GPIO16 WS=GPIO17 DOUT=GPIO18
Ready. Press the trigger button to play the sample.
```

**Anote qual arquivo o `WAV: loaded` pegou** — é o som do GPIO4.

Critério: chegou em `Ready.` com os quatro carregados.

Isto valida, pela primeira vez: PSRAM inicializa, o cartão monta, o SdFat lê,
os `.wav` cabem na memória. Nenhuma dessas coisas tinha prova.

**Se a primeira linha for isto, pare aqui:**

```
PSRAM: NOT AVAILABLE - samples cannot load. This is a board/build
configuration problem, not wiring: check that platformio.ini still has
board = esp32-s3-devkitc1-n16r8
```

Não adianta mexer em fio nenhum. A `psramAllocSafe()` da M16 não tem fallback
para RAM interna, então sem PSRAM todo `Wav::load()` falha — e o sintoma
seguinte seria um `WAV: failed to load` que parece problema de cartão.

### Fase C — primeiro som

Fone no DAC, volume baixo.

- [ ] Cada um dos quatro botões produz som
- [ ] Cada pad toca o arquivo certo
- [ ] Sem chiado ou estalo contínuo no silêncio

Se não sair som mas o log estiver limpo, o problema é analógico: `XSMT`,
alimentação do DAC, ou o fone. O PCM5102A não tem retorno de status — o
`I2S started` do log **não** prova que sai áudio.

### Fase D — contagem de disparos

Aqui você confirma que o bug de retrigger morreu. Ligue temporariamente:

```c
#define DEBUG_TRIGGERS 1
```

Grave, abra o monitor e bata **uma vez** em cada pad.

- [ ] Exatamente **uma** linha `Pad N: triggered` por batida
- [ ] Segurar o pad apertado por 2 segundos → ainda uma linha só
- [ ] Batida rápida e leve → ainda uma linha

Antes da correção isso imprimia ~112 linhas por batida. Se você vir mais de
uma, o firmware na placa não é o desta branch.

**Volte `DEBUG_TRIGGERS` para 0 antes de seguir.** Serial via USB CDC bloqueia
quando o host não drena o buffer, e isso é um travamento sem limite superior no
caminho do trigger — qualquer medição de latência com ele ligado é ficção.

### Fase E — teste de estresse do ring de DMA

**A fase mais importante do dia.** O ring está em 4 × 64 = 256 frames = 5,8 ms,
contra os 46 ms padrão da M16. Esse valor nunca foi validado.

Toque um **rolo rápido com os quatro pads soando ao mesmo tempo**. Quatro vozes
somadas é o caminho mais pesado do `audioUpdate()` e é onde um estouro aparece
primeiro. Um pad sozinho não prova nada.

- [ ] Sem clique, estalo ou chiado durante o rolo
- [ ] Repita umas dez vezes, variando a intensidade

Se sujar, suba um degrau em [../src/main.cpp](../src/main.cpp) e repita:

| `DMA_BUFFER_LENGTH` | Ring | |
|---|---|---|
| 256 | 23 ms | recuo seguro |
| 128 | 11,6 ms | |
| **64** | **5,8 ms** | onde está |
| 32 | 2,9 ms | |
| 16 | 1,5 ms | |

O `static_assert` no `main.cpp` falha o build se o `#define` for parar depois do
`#include "M16.h"` — sem ele, os 46 ms voltariam sem nenhum outro sintoma.

**Pare aqui e anote o valor que ficou estável.** Todo o resto depende dele.

### Fase F — medir latência de verdade

Monte o divisor polarizado ([circuit-rc.md](circuit-rc.md) tem o detalhe):

```
        3V3
         │
       [10k]
         │
DAC L ──┤├──┬─── GPIO1
       100nF │
           [10k]
             │
            GND
```

A polarização é obrigatória, não estética: a saída do DAC é centrada em 0 V e
oscila para os dois lados, e o ADC só aceita 0–3,3 V.

Ligue `#define LATENCY_PROBE 1`, grave, abra o monitor.

No boot deve sair:

```
PROBE: baseline=~2047 noise=<50 threshold=~90 (GPIO1)
```

- [ ] `noise` abaixo de ~50. Acima de 200 o firmware avisa: divisor mal
      contactado ou terra ruim
- [ ] Cada batida imprime uma linha `PROBE`

```
PROBE /1.wav: total 6180 us  (trigger 210 + audio path 5970)
```

- **trigger** — o que o firmware gastou decidindo (janela de confirmação +
  polling)
- **audio path** — o que veio depois do `start()`: ring de DMA e filtro do DAC.
  **É este número que a escada da fase E move.**

Bata umas 20 vezes e anote o intervalo. **Este é o primeiro número real de
latência deste projeto.** Compare com a previsão de ~6,1 ms; divergência grande
é achado, não erro.

> Se der `no audio within 50000 us`: ou o `.wav` começa em silêncio, ou o
> divisor não está conectado.

### Fase G — descer a escada, agora com número

Com o probe ligado, desça um degrau de `DMA_BUFFER_LENGTH` por vez. A cada
degrau: grave, faça o rolo de quatro vozes, leia o `audio path`.

Pare quando o áudio sujar ou quando o ganho deixar de valer o risco. Abaixo de
~2 ms o curso mecânico do botão e a sua própria variação tocando passam a
dominar — chegar aos 5,8 ms validados já põe o instrumento na faixa
imperceptível para percussão.

### Fase H — montar o RC

Agora sim, um resistor de cada vez. Por botão:

```
                    R1 10k
        3V3 ────────/\/\/\────────┬──────── GPIO
                                  │
                                  │      R2 220
                                  ├──────/\/\/\────── botão ──── GND
                                  │
                             C 100nF
                                  │
                                 GND
```

Monte **um** botão, teste, e só depois replique os outros três.

Checagem com multímetro, antes do firmware:

- [ ] Solto: ~3,3 V no pino
- [ ] Apertado: **~70 mV, não 0 V**. Se der 0 V exato, o R2 não está no
      circuito

Depois, o teste funcional — como o RC torna as proteções de software
redundantes, você prova o hardware **enfraquecendo o software**:

1. Com `TRIG_CONFIRM_US 200` e `TRIG_RELEASE_MS 3`, bata: uma linha por batida
2. Baixe para `TRIG_CONFIRM_US 0` e `TRIG_RELEASE_MS 1`
3. Bata de novo, rápido e leve. **Continuando uma linha por batida, o RC está
   funcionando** — sem ele, esses valores deixariam quique passar
4. **Restaure para 200 e 3**

- [ ] Meça a latência de novo com o RC montado — deve subir ~50 µs

### Fase I — teste de ruído

**Só com `TRIG_CONFIRM_US` restaurado em 200.** A rejeição de glitch vem da
janela de confirmação, não do RC.

Com áudio tocando: passe a mão perto dos fios dos botões, mexa nos jumpers,
ligue algo indutivo por perto (furadeira, motor, fonte chaveada).

- [ ] Nenhuma batida fantasma

Este teste é o que resolve uma questão que a simulação **não** consegue
responder. O RC tem efeito bidirecional sobre ruído: ele previne captação
irradiada de alta impedância, mas **alonga** glitch de contato em ~1081 µs — um
blip de 50 µs vira 1048 µs no pino, o suficiente para atravessar a janela de
200 µs que o rejeitaria sem o filtro. Qual dos dois domina depende do ruído real
da sua montagem, e só a bancada diz.

Se aparecer fantasma **depois** de montar o RC e não antes, é o caso de contato,
e a saída é subir a janela de confirmação para ~1,5 ms (custa 1,5 ms de
latência) ou baixar o R1 para reduzir o alongamento.

---

## 5. Como rodar os testes

### Suíte de unidade — não precisa da placa

```bash
.venv/bin/pio test -e native
```

23 testes, ~2 segundos. O `-e native` não é opcional: um `pio test` sem ele
também mira os ambientes ESP32, o que significa tentar gravar. Continua útil
com a placa em mãos — é o que confirma que a lógica de trigger não regrediu
depois de você mexer nas constantes.

Não há filtro por teste individual; para rodar um só, comente as linhas
`RUN_TEST` no `main()` de [../test/test_trigger/test_trigger.cpp](../test/test_trigger/test_trigger.cpp).

### Varreduras de parâmetro — não precisa da placa

```bash
g++ -O2 -std=c++17 -I sim -I src -o /tmp/trigger_sim sim/trigger_sim.cpp
/tmp/trigger_sim --sweep=all
```

Útil na bancada quando você medir o quique real do seu switch: passe
`--segment-us` e `--bounces` com os valores medidos e veja se as previsões
mudam de lugar.

### Firmware

```bash
.venv/bin/pio run -t upload      # grava
.venv/bin/pio device monitor     # log serial, 115200
.venv/bin/pio run -e sd_test -t upload   # diagnóstico isolado do cartão
```

O `sd_test` usa a `SD.h` do core Arduino em vez de SdFat/M16 — serve para
separar "cartão com problema" de "problema específico da biblioteca".

### Testes rodando na placa

**Ainda não existem.** A infraestrutura está pronta — o `TriggerInput` mora num
header compilado tanto no firmware quanto nos testes, então um ambiente
`pio test -e esp32-s3-devkitc-1` conseguiria exercitar a lógica real junto com
SD, PSRAM e GPIO de verdade. Mas não faz sentido montar isso antes de saber se
os 5,8 ms se sustentam.

---

## 6. Planilha de anotações

Preencha durante o dia. O objetivo do projeto inteiro é ter estes números.

| Medida | Previsto | Medido |
|---|---|---|
| PSRAM livre no boot | ~8,3 MB | |
| Arquivo que o GPIO4 pegou | — | |
| `DMA_BUFFER_LENGTH` estável | 64 (5,8 ms) | |
| `noise` do probe | < 50 | |
| Latência **total** | ~6,1 ms | |
| Latência **audio path** | ~5,9 ms | |
| Latência total com RC | +50 µs | |
| Tensão no pino, apertado (com RC) | ~70 mV | |
| Quique real do switch | 0,1–1 ms | |
| Fantasma no teste de ruído? | não | |

---

## 7. Se der problema

| Sintoma | Causa provável |
|---|---|
| Placa não aparece no `pio device monitor` | cabo USB só de carga; ou falta `-DARDUINO_USB_MODE=1` (já está no `platformio.ini`) |
| Monitor vazio, mas firmware roda | `ARDUINO_USB_CDC_ON_BOOT` — Serial foi para UART0, que não está ligada em nada |
| `SD: init FAILED` | fiação SPI, ou cartão não FAT32. Rode o `sd_test` para isolar |
| `PSRAM: NOT AVAILABLE` | configuração de board, **não** fiação — confira `board =` no `platformio.ini` |
| `WAV: failed to load`, com PSRAM OK | arquivo corrompido, ou `.wav` maior que a PSRAM livre |
| Log limpo, nenhum som | `XSMT` baixo ou flutuando; alimentação do DAC; GND não comum |
| Chiado ou estalo contínuo | ring de DMA raso demais — suba `DMA_BUFFER_LENGTH` |
| Estalo só no rolo de 4 vozes | mesma coisa, no caminho mais pesado |
| Várias linhas `triggered` por batida | firmware gravado não é o desta branch |
| Pad não dispara (com RC) | R2 alto demais, ou pernas do mesmo lado do switch |
| Batida com atraso perceptível (com RC) | R2 alto demais — baixe para 100–220 Ω |
| `PROBE: no audio within 50000 us` | `.wav` começa em silêncio, ou divisor desconectado |
| `PROBE` com `noise` alto | divisor mal contactado, ou terra ruim |
| Fantasma só depois de montar o RC | glitch de contato, alongado pelo filtro — ver fase I |

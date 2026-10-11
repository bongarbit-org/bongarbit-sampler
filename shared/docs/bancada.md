# Bancada — plano para o dia da placa

Roteiro para a primeira montagem física do sampler. Até agora **nada neste
projeto foi verificado em hardware**: latência, profundidade do ring de DMA,
PSRAM, cartão SD e o filtro RC são todos aritmética, simulação ou datasheet.
Este documento existe para trocar isso por medição, na ordem que faz um defeito
apontar para a própria causa.

Complementa, não substitui:

- [circuit.md](circuit.md) — fiação base e justificativa de cada pino
- [sampler/docs/circuit-rc.md](../../sampler/docs/circuit-rc.md) — o filtro RC e
  o rig de latência em detalhe

**Caminhos (11/10/2026).** O repositório foi reorganizado numa pasta por
instrumento, `sampler/` e `agbau/`, mais `shared/` para o que os dois compilam
(o mapa está no [README](../../README.md)). Os caminhos deste documento foram
atualizados para a estrutura nova; os commits anteriores usam a antiga
(`src/`, `sim/`, `tools/`, `docs/`). O env do sampler também mudou de nome, de
`esp32-s3-devkitc-1` para `sampler`. Um `main.cpp` sem pasta, nas seções 1 a
12, é sempre o do sampler: até ali ele era o único.

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
[sampler/src/main.cpp](../../sampler/src/main.cpp):

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
PSRAM: NOT AVAILABLE - samples fall back to internal RAM (N bytes free,
largest block M): small ones load, bigger ones fail. This is a board/build
configuration problem, not wiring: check that platformio.ini still has
board = esp32-s3-devkitc1-n16r8
```

Não adianta mexer em fio nenhum — e pare **mesmo que o resto do boot pareça
normal**. Sem PSRAM, o `Wav::load()` da M16 cai para `malloc()` na RAM interna
(imprime `Wav: Allocated N bytes in RAM`), que tem algumas centenas de KB.
Samples curtos podem caber todos e tocar sem sintoma nenhum; num kit maior, o
que não cabe falha — se for o arquivo do GPIO4 o boot trava em
`WAV: failed to load`, se for de um pad só aquele pad fica mudo. Nos dois
casos parece problema de cartão ou de arquivo.

> **Correção — 11/10/2026.** Este parágrafo dizia que a `psramAllocSafe()` da
> M16 não tem fallback para RAM interna, "então sem PSRAM todo `Wav::load()`
> falha". A premissa vale para a `psramAllocSafe()`; a conclusão não, porque
> o `Wav::load()` (M16 `aef89b2`, `Wav.h`) só tenta a PSRAM primeiro — se ela
> não existe ou recusa a alocação, ele faz `malloc()` na RAM interna. A
> previsão errada era uma falha barulhenta; o comportamento real é uma falha
> parcial e silenciosa, o que torna esta linha do log mais importante, não
> menos. A mensagem do firmware dizia `samples cannot load` e foi corrigida
> junto; agora ela imprime quanta RAM interna sobrou.

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

Se sujar, suba um degrau em [sampler/src/main.cpp](../../sampler/src/main.cpp) e
repita:

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

Monte o divisor polarizado
([sampler/docs/circuit-rc.md](../../sampler/docs/circuit-rc.md) tem o detalhe):

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
`RUN_TEST` no `main()` de
[test/shared/test_trigger/test_trigger.cpp](../../test/shared/test_trigger/test_trigger.cpp).

### Varreduras de parâmetro — não precisa da placa

```bash
g++ -O2 -std=c++17 -I shared/src -o /tmp/trigger_sim shared/sim/trigger_sim.cpp
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
`pio test -e sampler` conseguiria exercitar a lógica real junto com
SD, PSRAM e GPIO de verdade. Mas não faz sentido montar isso antes de saber se
os 5,8 ms se sustentam.

---

## 6. Planilha de anotações

Preencha durante o dia. O objetivo do projeto inteiro é ter estes números.

| Medida | Previsto | Medido |
|---|---|---|
| PSRAM livre no boot | ~8,3 MB | **8.386.096 B** (13/08/2026) |
| Arquivo que o GPIO4 pegou | — | **`/crime-1.wav`** (índice 0 de 21) |
| `DMA_BUFFER_LENGTH` estável | 64 (5,8 ms) | **64 (5,8 ms)** — validado com 4 vozes |
| `noise` do probe | < 50 | |
| Latência **total** | ~6,1 ms | |
| Latência **audio path** | ~5,9 ms | |
| Latência total com RC | +50 µs | |
| Tensão no pino, apertado (com RC) | ~70 mV | |
| Quique real do switch | 0,1–1 ms | |
| Fantasma no teste de ruído? | não | |

---

## 8. Primeira sessão de bancada — 13/08/2026

Montagem base, sem RC e sem o rig de latência. Três botões apenas (GPIO5/6/7);
o GPIO4 ficou sem botão e foi disparado com jumper na mão quando preciso.

### Fases que fecharam

| Fase | Resultado |
|---|---|
| A — bancada fria | passou |
| B — primeiro boot | passou: PSRAM 8.386.096 B, SD OK, os quatro `.wav` carregados |
| C — primeiro som | passou, **depois de dois defeitos de hardware** (abaixo) |
| D — contagem de disparos | passou: 17 batidas → 17 linhas `Pad N: triggered` |
| E em diante | não chegaram a rodar |

O bug de retrigger foi confirmado por contraste na mesma sessão: gravando o
firmware da branch `main` (sem a correção), as mesmas batidas produziram **196
linhas**. É a primeira medição em hardware do efeito metralhadora — até então
só existia no `shared/sim/trigger_sim.cpp`.

### Dois defeitos encontrados, ambos invisíveis no log

**1. `XSMT` flutuando.** Este módulo GY-PCM5102A **não** traz os pinos de
controle resolvidos de fábrica: eles ficam em jumpers de solda de três ilhas
(`H1L`…`H4L`) no verso, **todos abertos**. O `XSMT` tem pull-down interno, então
aberto ele lê 0 V = *soft mute* ativo, e a saída fica muda com I2S, dados,
alimentação e clock todos perfeitos. Medido: 0 V. Corrigido com fio da ilha
`XSMT` ao trilho de 3V3 → 3,29 V.

Os outros três (`FLT`, `DEMP`, `FMT`) flutuando caem em `L` pelo mesmo
pull-down, e `L` é justamente o valor desejado nos três — por isso só o `XSMT`
dá problema.

**2. Contatos intermitentes na protoboard.** Com `XSMT` corrigido, ainda não
saía som: `BCK` e `LCK` liam ~1,6 V **no DAC**, e `DIN` ficava em 0,00 V durante
o sample inteiro. O que parecia um pino morto era **mau contato**, e ele
enganou o diagnóstico por horas.

A revelação veio de dois comportamentos: raspar o jumper do `DIN` fazia o áudio
voltar, e mexer no fio do `XSMT` alternava entre som limpo, som horrível e
silêncio — que é o que se espera de uma entrada digital passeando em torno do
limiar lógico. Depois, sem tocar em nada, tudo voltou sozinho.

**Consequência: toda conclusão baseada numa medida pontual daquele período é
inválida.** Especificamente, ficaram sem resposta:

- Se o GPIO18 tem defeito. A hipótese nasceu de `DIN` em 0 V e "funcionou no
  GPIO8"; as duas observações mediram o contato, não o pino.
- Se o canal RX é o culpado. Tentou-se um A/B/A (`din = -1` contra `din = 21`,
  tudo o mais igual) e ele **não é executável nesta montagem**: depois de cada
  gravação a placa sobe muda, e só passa a tocar após vários resets e mexidas
  nos jumpers. Cada tentativa mede quanto o fio foi sacudido, não a
  configuração. Fica pendente até a montagem estar soldada.

  A linha `seti2sPins(16, 17, 18, -1)` ficou no `main.cpp` **por mérito
  próprio**, não porque tenha sido provada: este projeto não tem entrada I2S
  (o `circuit.md` lista o GPIO21 como não conectado), e o default da M16 aloca
  um canal RX, roda `i2s_channel_init_std_mode()` uma segunda vez com a mesma
  `std_cfg` e habilita um periférico que ninguém lê. Custo de desligar: zero.

### Fase E — a escada do ring de DMA, medida

Primeira medição em hardware do valor que o projeto inteiro existe para
calibrar. Escada percorrida com as quatro vozes soando ao mesmo tempo.

| `DMA_BUFFER_LENGTH` | Ring | Resultado |
|---|---|---|
| 256 | 23 ms | limpo |
| 128 | 11,6 ms | limpo |
| **64** | **5,8 ms** | **limpo com 4 vozes simultâneas** |

**Os 5,8 ms se sustentam.** O valor que estava no `main.cpp` desde o início,
escolhido na mesa e nunca verificado, está validado no caminho mais pesado do
`audioUpdate()`. O orçamento de ~6,1 ms do `circuit-rc.md` fica de pé, e não
foi preciso descer mais — abaixo de ~2 ms o curso mecânico do botão passa a
dominar.

#### Os dois confundidores que fizeram 5,8 ms parecer falhar

Antes de chegar nesse resultado, a escada foi percorrida **para cima**, com a
conclusão oposta — "5,8 ms crepita, inclusive com uma voz só; o estável é 23 ms".
Estava errado, e as duas causas soam exatamente como estouro de DMA:

1. **Samples de 8 bits.** Todos os arquivos do cartão eram 8-bit/11025 Hz. Um
   piso de quantização de ~48 dB crepita nas caudas de decaimento, e a banda
   cortada em 5,5 kHz reforça a impressão de defeito.
2. **Clipping.** O `audioUpdate()` soma as vozes em `int32_t` e clipa uma vez no
   fim. Com samples normalizados a 89% do fundo de escala, **duas** vozes
   simultâneas somam 178% e clipam forte — o que soa como estalo. Este
   confundidor foi *introduzido durante a própria bancada*, ao copiar quatro
   cópias de um kick normalizado para o cartão como "sample limpo".

A escada só passou a medir o ring com arquivos **16-bit/44,1 kHz atenuados a
~22% cada**, onde quatro vozes somam 89% e nenhuma combinação clipa.

**Lição de método:** julgar estouro de DMA de ouvido exige um sample que não
tenha nem piso de ruído próprio nem risco de clipping. Qualquer um dos dois
inverte a conclusão. A fase F (probe no ADC) existe justamente para não
depender disso.

#### Achado de projeto: o firmware não tem headroom por voz

Não é detalhe de bancada. O `audioUpdate()` soma as vozes e clipa, sem
atenuação por voz nem limitador. Um kit de samples normalizado — que é o normal
— **clipa com dois pads simultâneos, por construção**.

**Resolvido em [shared/src/mixer.h](../src/mixer.h)** (branch
`feat/mixer-gain-staging`), com o modelo de uma MPC em vez de divisão por número
de vozes: ganho Q8 por voz, ganho master, e um joelho suave como rede de
segurança. Verificado por 18 testes no host.

O argumento decisivo contra dividir por `PAD_COUNT` apareceu ao escrever esses
testes, e é mais forte do que parecia: para quatro vozes ficarem **inteiramente
abaixo do joelho** — lineares, sem compressão nenhuma — cada voz precisaria de
cerca de **−13,7 dB**, que é *pior* que os −12 dB do ÷4 rejeitado por deixar o
instrumento baixo. Headroom linear para o pior caso é simplesmente caro. O
joelho é o que devolve esse headroom: a unidade, uma voz sozinha fica a 10% do
nível original e quatro vozes ainda não clipam.

Uma correção de percurso que vale registrar: durante a bancada eu afirmei que
"soft clip mais uns 6 dB de folga no material" bastaria. Não basta — com −6 dB
por voz, quatro vozes somam 58448, ainda acima do fundo de escala. O que os 6 dB
garantem é que **uma** voz fica abaixo do joelho, ou seja, bit-exata. O caso de
quatro vozes depende do joelho, sempre.

### Comportamento não explicado

Após cada gravação, a placa sobe **muda**, e passa a funcionar só depois de
vários resets acompanhados de mexidas nos jumpers. Atribuído a contato
intermitente, mas a componente "vários resets" não está explicada por isso
sozinha — vale reavaliar depois da solda, e se persistir, investigar se o
PCM5102A precisa de `XSMT` estável já no power-on.

Causa provável dos contatos: os jumpers `H*L` do verso do DAC exigem solda (ver
defeito 1), o header do módulo tem indício de solda fria, e as pernas dos
jumpers fatigaram com as trocas. **Nada disso se resolve sem ferro de solda**, e
qualquer medição feita antes disso mede o estado dos contatos naquele instante.

**3. `DEBUG_TRIGGERS` custa segundos, não microssegundos.** Com ele em `1` e a
porta serial aberta sem ninguém drenando, o intervalo entre bater no pad e ouvir
o som ficou em **2 a 3 segundos**. O `TRIGDBG()` vem **antes** do
`padSamp[i].start()` no laço, então o disparo só acontece quando a escrita no
USB CDC destrava. Voltando para `0`, a latência caiu para imperceptível.

É a confirmação em hardware do que o `CLAUDE.md` descreve como "unbounded stall
in the trigger path", e o único resultado da sessão que não depende da qualidade
dos contatos. Reforça a ordem do plano: a fase D existe para ser feita **e
desfeita** antes da fase E.

### Técnicas de diagnóstico que renderam

- **Medir os pinos de I2S em DC.** Uma quadrada de 50% de duty lê ~1,6 V num
  multímetro comum. Serve para BCK (2,8 MHz) e WS (44,1 kHz), e distingue
  "periférico rodando" de "pino parado" sem osciloscópio.
- **Medir no módulo, não no header do dev kit.** A serigrafia do DAC é
  inequívoca; contar pinos no header de 22 vias produziu uma medida errada de
  0 V que custou tempo.
- **Usar um sinal conhecido para testar um caminho suspeito.** Encostar o fio
  de `DIN` no GPIO16 fez o DAC converter o clock como lixo e produzir ruído
  audível — o que provou de uma vez que fio, pino do DAC, chip, filtro de
  saída, conector e fone estavam todos bons.
- **Ler o multímetro através da carga.** Com o fone plugado, `LROUT`↔`AGND` em
  resistência deu 40 Ω: a bobina do fone, provando que o jack chega às ilhas.
- **Ligar o log do ESP-IDF.** `Serial.setDebugOutput(true)` mais
  `-DCORE_DEBUG_LEVEL=5` no build trazem os `ESP_LOGE` do driver para o USB CDC.
  Sem isso eles vão para a UART0 (GPIO43/44), que não está ligada em nada, e uma
  falha de `i2s_new_channel()` fica invisível — a M16 não checa o retorno de
  nenhuma das quatro chamadas de inicialização e imprime `M16 is running` de
  qualquer jeito.

### Armadilhas de ambiente

- O usuário precisa estar no grupo `dialout` para acessar `/dev/ttyACM0`. As
  regras de udev que a PlatformIO distribui cobrem `303a:1001`, mas esta placa
  enumera como **`303a:4001`** rodando o firmware de aplicação, então nenhuma
  regra casa e o nó cai no default `root:dialout` 660.
- `pio device monitor` exige TTY (o miniterm chama `termios.tcgetattr`) e falha
  em captura não-interativa. Um leitor pyserial simples resolve — mas precisa
  **reconectar sozinho**, porque o USB nativo re-enumera a cada `RST` e derruba
  o handle aberto.
- O primeiro upload precisou de modo download manual (segurar `BOOT`, tocar
  `RST`, soltar `BOOT`). Os seguintes gravaram direto.

---

## 7. Se der problema

| Sintoma | Causa provável |
|---|---|
| Placa não aparece no `pio device monitor` | cabo USB só de carga; ou falta `-DARDUINO_USB_MODE=1` (já está no `platformio.ini`) |
| Monitor vazio, mas firmware roda | `ARDUINO_USB_CDC_ON_BOOT` — Serial foi para UART0, que não está ligada em nada |
| `SD: init FAILED` | fiação SPI, ou cartão não FAT32. Rode o `sd_test` para isolar |
| `PSRAM: NOT AVAILABLE` | configuração de board, **não** fiação — confira `board =` no `platformio.ini`. Pode aparecer com o sampler tocando: sem PSRAM, samples pequenos carregam na RAM interna |
| `WAV: failed to load`, com PSRAM OK | arquivo corrompido, ou formato que a M16 não lê. Tamanho quase nunca é a causa (esta linha dizia "`.wav` maior que a PSRAM livre" — errado em 11/10/2026: arquivo grande demais é cortado, não recusado; ver a linha de baixo) |
| `WAV: '<arquivo>' TRUNCATED to X of Y frames` | `.wav` maior que 90% da PSRAM ainda livre: a M16 corta o arquivo e o carregamento "dá certo". Os últimos carregados são os cortados. Encurte, reduza a taxa ou tire um sample |
| Log limpo, nenhum som | `XSMT` baixo ou flutuando; alimentação do DAC; GND não comum |
| Chiado ou estalo contínuo | ring de DMA raso demais — suba `DMA_BUFFER_LENGTH` |
| Estalo só no rolo de 4 vozes | mesma coisa, no caminho mais pesado |
| Várias linhas `triggered` por batida | firmware gravado não é o desta branch |
| Pad não dispara (com RC) | R2 alto demais, ou pernas do mesmo lado do switch |
| Batida com atraso perceptível (com RC) | R2 alto demais — baixe para 100–220 Ω |
| `PROBE: no audio within 50000 us` | `.wav` começa em silêncio, ou divisor desconectado |
| `PROBE` com `noise` alto | divisor mal contactado, ou terra ruim |
| Fantasma só depois de montar o RC | glitch de contato, alongado pelo filtro — ver fase I |

---

## 9. Banco de medição de I/O I2S — 14/08/2026

Sessão sem botões e sem cartão: só o DAC, para medir o transporte de áudio e
decidir a arquitetura do motor. Sketch em
[shared/bench/i2s_bench.cpp](../bench/i2s_bench.cpp), ambiente `i2s_bench`.

A pergunta: ficar na task de áudio da M16, ou alimentar o DMA por ISR e
escrever o motor? Até aqui a discussão era leitura de código e aritmética.

### Três modelos de alimentação, nove profundidades de ring

Carga idêntica nos três: quatro vozes lidas da PSRAM pelo mixer real
(`mixer.h`, ganho Q8 e joelho suave). `cpu%` é CPU de verdade, medido por uma
task de prioridade 1 contando num laço contra linha de base ociosa — então
inclui o despacho de interrupção do IDF, que não aparece de dentro do callback.

| Ring | ISR `on_sent` | task, bloco | task, 1 frame (M16 hoje) |
|---|---|---|---|
| 46,44 ms | 2,1% | 2,3% | 21,3% |
| 11,61 ms | 2,2% | 3,0% | 21,9% |
| 5,80 ms | 2,4% | 3,9% | 22,7% |
| 2,90 ms | 2,8% | 5,8% | 24,3% |
| 1,45 ms | 3,6% | 9,5% | 27,4% |
| 0,73 ms | 3,6% | 16,9% | 33,6% |
| 0,36 ms | 5,2% | 17,0% | 33,6% |

**Nenhuma configuração foi rejeitada pelo driver**, até `2 × 8` = 16 frames =
0,36 ms. O piso que a escada do `main.cpp` supunha não existe.

### A escrita por amostra é o gargalo, e o conserto é mais barato do que trocar de arquitetura

A M16 chama `i2s_channel_write()` com 4 bytes, 44.100 vezes por segundo. Medido:
**~1.126 ciclos por frame só na chamada de driver** (22,7% de um core a 5,8 ms,
dos quais 110 ciclos são o preenchimento).

Mas **trocar para escrita por bloco já recupera quase tudo**: 21,3% → 2,3%.
Isso não estava na discussão original, que estava posta como "task+float *ou*
ISR+ponto fixo". Existe um meio.

### Underrun de verdade, e onde cada modelo quebra

A v1 deste banco contou `on_send_q_ovf` como underrun. Errado no modelo de ISR:
esse evento dispara uma vez por descritor completado quando ninguém consome a
fila de envio do driver, então contava 100% dos descritores enquanto o tom
estava audivelmente perfeito. A v2 mede o **intervalo entre refills** e marca os
que passam do ring inteiro — que é o que de fato significa o DAC repetir dado
velho.

- **ISR: zero `STARVED` em toda a escada**, até 0,36 ms. `gap_max` ficou em
  100-101% do período esperado em tudo.
- **Task: `qovf` passa a ter significado** (a fila está em uso) e começa a
  transbordar em **1,45 ms**, piorando abaixo. Piso prático do modelo de task:
  **~2,9 ms**.

### Despacho de interrupção: ~1.380 ciclos

Subtraindo o preenchimento (110 ciclos/frame × 44.100 = 2,02% constante) do CPU
total, nas duas configurações com resolução suficiente:

```
4 x 16 (2756 int/s):  3,6% − 2,02% → 1376 ciclos/interrupção
4 x 8  (5512 int/s):  5,2% − 2,02% → 1384 ciclos/interrupção
```

**~1.380 ciclos, 5,75 µs por interrupção.** É o termo que cresce quando a escada
desce, e tem consequência de projeto direta:

| Config | Ring | Interrupções/s | cpu% |
|---|---|---|---|
| `4 × 8` | 0,73 ms | 5.512 | 5,2% |
| `2 × 16` | **0,73 ms** | 2.756 | **3,6%** |

Mesma latência, metade das interrupções. **Para um ring de profundidade dada,
descritores maiores e menos numerosos são mais baratos** — `2 × 16` domina
`4 × 8`.

### O custo de uma voz real, e o "sub-microssegundo" confirmado

As duas linhas de ISR diferem só no preenchimento, então a diferença é o custo
de ler a PSRAM:

| Preenchimento | Ciclos/frame |
|---|---|
| 4 vozes da PSRAM pelo mixer | **110** |
| Gerador trivial (compare + 2 stores) | 18 |

**~23 ciclos por voz por frame** (96 ns a 240 MHz), incluindo leitura de PSRAM,
ganho Q8, soma e joelho suave. O argumento que sustenta o ring raso em
`main.cpp` — "sobra o cache miss de PSRAM, que é sub-microssegundo" — **está
confirmado**, e agora com número em vez de adjetivo. Leitura sequencial; o cache
dá conta.

Uma previsão escrita **antes** de medir estimava ~100 ciclos/frame para quatro
vozes e 1,8% do período em `4 × 64`. Saiu 110 ciclos/frame e 2,4%.

### Jitter

```
ISR,  4 x 16:  1844 / 1847 / 2112 ciclos   (max 14% acima do min)
task, 1 frame:  906 / 5204 / 70863 ciclos  (78x de spread)
```

### Decisão

**Alvo ≥ 2,9 ms → task com escrita por bloco.** 5,8% de CPU, zero `qovf`, e
permite **float**, porque não é contexto de ISR (a FPU do Xtensa não está
disponível em ISR). Muito menos código que escrever o motor.

**Alvo sub-milissegundo → ISR com `2 × 16` (0,73 ms) a 3,6%.** Custo: ponto fixo
no preenchimento, e o `mixer.h` passaria a Q12 em vez de float.

Recomendação: **começar pela task com bloco.** Entrega quase todo o ganho de
CPU, mantém float, e a ISR fica como upgrade se o pitch shifter e o audio-in
pedirem os 2 ms de volta.

### Ressalvas

- Tudo aqui é **saída**. Com audio-in, soma o ring de entrada e o filtro de
  decimação do codec.
- A resolução do `cpu%` é de 0,1 ponto, então os valores de despacho nas
  configurações de poucas interrupções por segundo têm erro grande. Os
  ~1.380 ciclos vêm das duas linhas de alta taxa, onde a resolução sustenta.
- `disableCore1WDT()` retornou `Failed to remove Core 1 IDLE task from WDT`.
  Não houve abort, provavelmente porque a IDLE do core 1 não estava inscrita
  neste build — mas significa que a proteção que eu achei ter desligado nunca
  estava ligada.
- Latência absoluta continua não medida: falta a fase F. O filtro do DAC
  (~0,50 ms, `FLT` em GND) é constante e cancela nas comparações acima, mas
  entra em qualquer número absoluto.

---

## 10. Correção à seção 9: a M16 já fazia escrita por bloco — 11/10/2026

A seção 9 mediu três modelos de alimentação do I2S e recomendou **task com
escrita por bloco**, pela diferença de ~19% de um núcleo contra a escrita por
frame. A recomendação continua certa. A premissa, não: a linha
`task, 1 frame (M16)` modelava uma M16 que já não existia quando o firmware
foi gravado.

### O que foi encontrado

Ao começar a trocar a alimentação do firmware, lendo a M16 instalada:

- `i2s_write_samples()` é hoje um shim **`[[deprecated]]`** sobre
  `audioBlockWrite()`.
- `audioBlockWrite()` acumula `M16_BLOCK_SIZE` (32) frames num buffer e faz
  **uma** chamada `i2s_channel_write()` por bloco.
- O firmware chamava o shim. O compilador vinha avisando; o aviso estava
  perdido no meio dos avisos internos da própria M16.

Ou seja: o ganho de CPU que a seção 9 recomendou perseguir **já estava no
bolso**, entregue pela biblioteca, sem mudança de código nossa.

A dependência era `https://github.com/algomusic/M16.git`, sem revisão.

### Por que isso não é só uma curiosidade

| Fato | Consequência |
|---|---|
| O checkout da M16 foi destruído e re-clonado em 10/10/2026 | O commit contra o qual a bancada de 13–14/08 rodou é **irrecuperável** (o clone tem uma única entrada de reflog) |
| O transporte mudou entre aquele commit e este | A escada de DMA e o resultado de "4 vozes limpas em 5,8 ms" passam a **provisórios** |
| O mesmo update trouxe um modo dual-core em que o shim retorna `false` e **não escreve nada** | Um default mudando lá em cima emudeceria a placa. O firmware agora declara `setIsDualCore(false)` em vez de herdar |

A M16 está pinada no commit `aef89b2` desde então, pelo mesmo argumento que já
valia para `platform`. **O pin não é tranquilizador — é honesto:** ele fixa uma
versão que a placa nunca rodou nas sessões de 13–14/08. A diferença é que
agora um resultado de hardware fica preso a alguma coisa.

### Isso invalida os números da seção 9?

Não. O banco (`shared/bench/i2s_bench.cpp`) não usa M16 — ele chama o driver do
IDF direto. Os três modelos que ele mede são modelos *nossos*, e as medidas de
CPU, jitter e underrun por profundidade de ring continuam valendo como
caracterização do transporte I2S deste hardware.

O que muda é a leitura de **uma** linha: `task, 1 frame (M16)` deve ser lida
como "escrita por frame no driver", um modelo genérico, e **não** como "o que a
M16 faz". A M16 está no grupo `task, block`.

E muda a conclusão prática: a troca do firmware para blocos deixou de ser
justificada por CPU. Foi feita por outros dois motivos, que seguem de pé:

1. **Localidade de PSRAM** — renderizar voz-por-fora faz cada voz percorrer 32
   frames contíguos do próprio buffer, em vez de intercalar quatro streams.
   O agrupamento da M16 não faz nada por isso.
2. **Janela para DSP** — e este é o que forçou. A M16 guarda a *nossa saída*,
   depois de já termos tido que decidir qual é. Um pitch shifter precisa de uma
   janela de *entrada*. Nenhum agrupamento na biblioteca cria isso.

### Estado do firmware depois disso

Gravado e confirmado por log em 11/10/2026:

```
M16 is running (single-core audio task mode)
ENGINE: 32-frame float blocks (0.73 ms), ring 4 x 64 (5.80 ms)
FX: hold GPIO7 to pitch shift -12.0 semitones (ratio 0.5000); adds 11.6 ms
    when engaged, bit-exact bypass when not
Ready. GPIO4/5/6 trigger samples; hold GPIO7 to pitch shift the mix.
```

Boot limpo, PSRAM 8189 KB, três samples carregados, sem watchdog ao longo da
janela de captura. 71 testes no host passando.

### O que a próxima sessão precisa responder, de ouvido

Nada disso foi julgado por ouvido ainda, e três das quatro perguntas são de
ouvido mesmo:

1. **O motor de blocos soa idêntico ao anterior?** Deve soar. É o que o teste
   `test_float_knee_matches_the_integer_reference` prevê (1 LSB em toda a
   faixa). Se **não** soar idêntico, o teste está medindo a coisa errada e é
   mais importante que a diferença audível.
2. **Os 5,8 ms continuam limpos** com 4 vozes nesta versão da M16? Rajadas
   drenam um ring mais rápido, não mais devagar, então a expectativa é sim.
3. **O joelho suave num rolo de quatro vozes** — pergunta acumulada desde o
   commit do mixer.
4. **O pitch shifter.** O que ouvir, especificamente: um clique ao engatar ou
   soltar (seria a rampa dry/wet falhando), e o *warble* de ~43 Hz, que é
   esperado e não um defeito.

Fase F (latência absoluta) continua pendente e agora tem um componente novo a
separar: os ~11,6 ms do shifter só existem com o efeito engatado.

---

## 11. O motor de blocos e o pitch shifter, de ouvido — 11/10/2026

Duas das quatro perguntas da seção 10 estão respondidas.

### O susto, e o que ele ensinou

Depois de gravar os três commits que reescreveram o motor de áudio, o
instrumento ficou **mudo**. Com o firmware recém-trocado, ele era o suspeito
óbvio.

Não era. `DEBUG_ENGINE` (novo, uma linha por segundo do `loop()`) resolveu numa
única captura:

```
ENG blocks/s 1378  peak 5327  playing[trig 1 p1 0 p2 0]  pins[4:0 5:1 6:1 7:0]  fx 1
```

| Campo | Leitura |
|---|---|
| `blocks/s 1378` | Exatamente 44100/32. **O relógio do I2S** estava ditando o ritmo, não o nosso código — e o driver consumia todo bloco. Uma escrita estourando o timeout de 100 ms daria ~10/s |
| `peak 5327` | Sinal real chegou na última instrução antes do DMA |
| `playing[...]` | As vozes disparavam |
| `fx 1` | O efeito engatava |

Da CPU até o GPIO18, tudo certo. O defeito era **conexão** — a mesma classe de
falha que já custou uma sessão inteira a este projeto quando um GPIO foi
acusado no lugar de um mau contato.

**A lição é de método:** "parou de funcionar depois da sua mudança" e "a sua
mudança quebrou" não são a mesma afirmação, e a diferença entre as duas é uma
medição de dois minutos. Vale gravar porque este projeto já errou exatamente
assim, na direção oposta — culpando o hardware antes (clipping confundido com
underrun) e o código agora.

### O resultado

Com o contato resolvido: **som limpo, e o pitch shifter funcionando.** O motor
de blocos em float e o shifter saem do estado "não ouvido".

O motor soar certo é, em si, a confirmação do que
`test_float_knee_matches_the_integer_reference` previa: trocar ponto fixo por
float em todo o caminho de áudio **não mudou o som**.

### O que ainda falta de ouvido

1. **Rolo de quatro vozes.** Fecha de uma vez a reconfirmação dos 5,8 ms nesta
   versão da M16 *e* o joelho suave, porque é a única condição em que as vozes
   somam acima do joelho. Detalhe prático: o pico medido foi ~5300 de 32768
   (≈ −16 dB), porque os samples no cartão são as cópias atenuadas feitas para
   a escada de DMA. Pode ser preciso subir o `masterGain` para provocar a
   colisão de verdade.
2. **No shifter, duas coisas específicas:** um clique ao engatar ou soltar
   (seria a rampa dry/wet falhando) e o *warble* de ~43 Hz, que é previsto e
   não é defeito.

Fase F (latência absoluta) continua pendente.

### Recorrência: três vezes na mesma sessão

O som caiu e voltou **três** vezes em 11/10/2026, e o padrão é mais útil que
cada ocorrência:

| # | Quando | Diagnóstico |
|---|---|---|
| 1 | Depois de gravar os três commits do motor novo | `DEBUG_ENGINE`: 1378 blocos/s, `peak 5327` → firmware inocente, era conexão |
| 2 | Mexendo na conexão | Som limpo, pitch shifter funcionando |
| 3 | Depois de gravar o build sem `DEBUG_ENGINE` | Boot único e limpo, 16 s sem reinício, sem watchdog → firmware inocente de novo |
| 4 | Sem mudar nada no software | Som voltou |

**As duas vezes que caiu foram imediatamente depois de uma gravação,** e gravar
significa um reset via RTS e encostar na placa. O firmware foi explicitamente
descartado nas duas: na primeira por medição do caminho de áudio, na segunda
porque o único diff era um `Serial.printf` no `loop()` e não houve panic nem
reinício.

Isso localiza a falha numa **junta mecânica**, não no DAC e não no código. Não
é o XSMT mudo (que daria silêncio estável, não intermitente ligado a
manipulação). A hipótese que sobra é um dos fios de dados ou de alimentação
cujo contato cede com movimento.

Enquanto isso não for resolvido fisicamente, **qualquer** julgamento de ouvido
nesta bancada carrega esse ruído: um "não tocou" só vale depois de uma linha de
`DEBUG_ENGINE` confirmando que o firmware está produzindo sinal.

```bash
PLATFORMIO_BUILD_FLAGS='-DDEBUG_ENGINE=1' pio run -e sampler -t upload
```

Os dois diagnósticos (`DEBUG_ENGINE`, `DEBUG_TRIGGERS`) são `#ifndef`-guarded
justamente para isso: ligar num build sem editar — e sem esquecer de reverter —
um arquivo versionado.

### Correção: não é junta mecânica, correlaciona com o conector USB

A conclusão acima ("junta mecânica") está **errada**, ou pelo menos superada.

O que foi observado em seguida: o som pega **com confiabilidade** quando a
placa está ligada pelo conector **COM** (a ponte WCH, USB id `1a86:55d3`), e
**não pega** quando ligada pelo conector **USB** nativo (GPIO 19/20).

Isso explica os dados melhor do que manipulação explicava:

- "Confiável num conector" não é comportamento de contato intermitente.
  Intermitência não escolhe conector.
- As duas quedas vieram logo depois de gravar — e gravar é exatamente quando o
  cabo troca de porto. A correlação que eu li como "mexer na placa" era a
  correlação com qual conector estava em uso.

A placa tem **dois** conectores USB, e o `platformio.ini` afirmava que não —
dizia que só existia o nativo. Essa falsidade já havia custado uma captura
serial vazia (cabo no COM, `Serial` saindo pelo nativo) e agora estava negando
a existência do conector que funciona. Corrigido.

#### Mecanismo: ainda não sabido

Hipóteses, em ordem de quão fácil é separá-las:

| Hipótese | Como testar |
|---|---|
| **Alimentação/terra** — caminhos de 5V e de retorno diferentes por conector, e o regulador do DAC já esteve marginal nesta bancada (`4vin` mediu 2,75 V numa sessão) | Medir `VIN` e `A3V3` do DAC, com a placa ligada, em cada conector. Sag no nativo = é isso |
| **O periférico USB nativo em si** — GPIO 19/20 sendo acionados como D-/D+ quando o host enumera | Ligar o conector **nativo** num carregador comum / power bank, sem host de dados. Se funcionar assim, o problema é enumeração/periférico, não o caminho de energia daquele conector |
| **Algo cabeado perto de 19/20** | Conferir o que está nos headers vizinhos |

O teste do carregador é o que separa energia de periférico sem tocar em código,
e é por onde começar.

#### Consequência imediata para a bancada

Não é "bug conhecido, conviver": é uma variável que invalida comparações. **Todo
resultado de ouvido tem que registrar em qual conector a placa estava.** Os
resultados de 11/10/2026 (motor de blocos e pitch shifter ouvidos) valem — mas
valem *com* essa anotação, e as medições de latência da fase F não podem ser
feitas antes disso estar entendido, porque alimentação marginal e latência não
se separam depois.

### Com os samples reais: joelho suave confirmado — 11/10/2026

As cópias de −12 dB saíram da raiz do cartão (foram para `samples-12db/`, não
apagadas — são o setup da escada de DMA) e o material real do Pedro voltou.

Isso muda a condição de teste de forma importante:

| | Cópias da escada | Samples reais |
|---|---|---|
| Pico | −13 dBFS, quatro arquivos idênticos | −3,5 / −3,6 / −2,8 dBFS, vários a 0,0 |
| Formato | 16 bit / 44,1 kHz | **8 bit / 11,025 kHz** |
| Soma de 3 vozes | ~67% do fundo de escala — **nunca alcança o joelho** | ~267% — **bem acima** |

Ou seja: todos os resultados anteriores de "não clipa" foram obtidos com
material que não tinha como clipar. Esta é a primeira vez que o joelho suave
foi exercitado de verdade.

**Resultado: som e efeito ótimos com três vozes.** O joelho está fazendo o
trabalho — com o clamp duro, três vozes a ~0 dBFS seriam achatamento audível em
cada sobreposição, que é exatamente o clique que originou o `mixer.h`.

Duas observações honestas sobre o que isso ensina:

- **O piso de 8 bits não incomodou.** Ele é real (~48 dB de quantização) e está
  documentado como um dos dois confounds de agosto. Mas com este material ele
  não se manifestou como problema. Isso sugere que, dos dois confounds, o
  **clipping** era o que realmente dominava o "encraquelado" — o piso de 8 bits
  era aritmética correta com peso menor do que eu atribuí.
- **"Quatro vozes" saiu de pauta.** O GPIO7 virou o efeito, então o máximo
  neste firmware é GPIO4+5+6. Três vozes a ~0 dBFS já passam de 267% do fundo
  de escala, então testam o joelho com folga; mas a escada de DMA de agosto
  (medida *com quatro*) não tem como ser reproduzida sem devolver o pad.

#### Estado de ouvido ao fim de 11/10/2026

| Item | Estado |
|---|---|
| Motor de blocos em float | **ouvido, limpo** — confirma a previsão de 1 LSB: trocar ponto fixo por float não mudou o som |
| Pitch shifter | **ouvido, funcionando** com material real |
| Joelho suave em colisão | **ouvido, limpo** com 3 vozes acima do joelho |
| Ring de 5,8 ms | limpo com 3 vozes nesta versão da M16; o teste de 4 vozes não é mais possível |
| Conector USB | **não explicado** — áudio confiável no COM, não no nativo |
| Latência absoluta (fase F) | **não medida**, e bloqueada até o conector estar entendido |

---

## 12. O fade de borda da M16 — 11/10/2026 (não ouvido)

Achado lendo o código da M16, não ouvindo. **Nada desta seção foi verificado
na placa.**

### O que a M16 faz

`Wav::load()` (M16 `aef89b2`, `Wav.h:199`) termina chamando
`applyEdgeFades()` sempre, sem parâmetro para desligar. A função
(`Wav.h:209–249`) multiplica o começo e o fim de cada buffer por uma rampa de
cosseno:

- **10 ms em tempo do arquivo**: 441 frames a 44,1 kHz, 110 a 11,025 kHz (a
  taxa dos samples reais do cartão);
- limitada a 25% do arquivo;
- o primeiro e o último frame viram **exatamente 0**. A rampa está em −32 dB
  com 1 ms, −20 dB com 2 ms e −6 dB com 5 ms.

O comentário da M16 diz para que ela serve: tirar cliques "quando o áudio faz
loop ou nas bordas do arquivo". Este sampler não faz loop, e na borda inicial
de um sample de percussão o "clique" é o ataque.

Nenhum caminho de carregamento do firmware escapa dela. O GPIO4 usa
`loadNumber()` e os pads usam `load()`, e os dois acabam em `load()`
(`loadNumber/First/Next/Prev` passam por `loadByIndex()`, que chama `load()`
em `Wav.h:1280`). `setExternalBuffer()` só troca a alocação. O envelope do
`Samp` fica desligado, porque precisa de uma tabela compartilhada que o
firmware nunca inicializa — então essa rampa é o único fade no caminho.

O caminho de flash (`USE_SD_SAMPLER=0`) não tem rampa nenhuma. Por isso o
CLAUDE.md estava errado ao dizer que um sample é "bit-idêntico" tocando do SD
ou da flash: a conversão é idêntica, o buffer não. Corrigido lá e no
`sampler/tools/wav_to_header.py`.

### Quanto pesa, num modelo

Um modelo em Python da rampa (mesma fórmula; o arredondamento pode diferir em
1 LSB do `cosf` da placa), aplicado ao kick do repositório
(`sampler/samples/kick_normalizado.wav`, o mesmo que o modo flash toca):

| Janela | Pico sem → com rampa | Energia |
|---|---|---|
| Primeiro 1 ms | 4990 → 88 | −38,7 dB |
| Primeiros 2 ms | 6094 → 283 | −28,8 dB |
| Primeiros 5 ms | 26792 → 8958 | −10,9 dB |
| Primeiros 10 ms | 29225 → 19750 | −4,9 dB |
| Últimos 10 ms | 148 → 144 | irrelevante |

O pico global do kick está em **6 ms**, dentro da rampa. Carregado pelo SD, ele
toca com o pico 3,4 dB mais baixo e com o clique de ataque praticamente
apagado. O `kick.wav` original (48 kHz, estéreo, não normalizado) dá as mesmas
proporções, até 0,1 dB. Os
samples reais do cartão (8 bit / 11,025 kHz) não estão no repositório e não
foram medidos.

O fim pesa pouco: um sample que decai até o silêncio chega lá com amostras
quase nulas, e a rampa mexe nelas sem efeito.

### O que o modelo não responde

Se isso é **audível**. A previsão é que sim em ataques secos (kick, slap, aro)
e não em material de ataque lento. Mas um transiente de 2 ms que o resto do
som pode mascarar é exatamente o tipo de coisa que a aritmética superestima.
Isso só se resolve de ouvido.

### A chave: `WAV_RESTORE_EDGES`

Está em `sampler/src/main.cpp`, guardada por `#ifndef`, com **padrão 0** — o
comportamento da M16, inalterado. Com 1, o firmware relê do cartão, depois de
cada `load()`, as bordas que a rampa tocou, usando `Wav::readFrames()`:

- `readFrames()` converte para int16 com **a mesma aritmética** de `load()`
  neste pin. Conferido linha a linha: 8, 16 e 24 bits, 32 bits inteiro e 32
  bits float com clamp. Os frames relidos saem como se a rampa nunca tivesse
  rodado.
- Relê **20 ms** em cada ponta, o dobro da rampa. Reler a mais não faz mal
  (os frames voltam idênticos); reler a menos deixaria o fim da rampa no lugar,
  sem sintoma nenhum.
- Se o arquivo foi **truncado** por falta de PSRAM, só o começo é restaurado.
  O fim é o corte da M16, não o do arquivo, e ali a rampa evita um clique de
  verdade.
- Se a releitura falhar, imprime `edge re-read FAILED` (incondicional) e o
  sample toca como hoje — nunca com lixo, porque `readFrames()` só escreve o
  que conseguiu ler.

Custo:

| | |
|---|---|
| RAM interna | **+8.192 bytes** de `.bss`, medido (27.448 → 35.640): é exatamente o `static uint8_t tempBuffer[8192]` do próprio `readFrames()` (`Wav.h:554`), separado do que o `load()` usa. Só existe com a chave em 1 |
| Flash | +0,8 KB |
| Boot | Duas leituras curtas a mais por arquivo, de 20 ms de áudio cada |
| Reprodução | Nenhum. Tudo acontece no `setup()`, e o SD continua intocado depois do boot |
| Manutenção | Acoplada à M16. Em todo bump, reler `applyEdgeFades()` (o comprimento) e comparar `readAudioData()` com `readFrames()` (a conversão) |

### A alternativa descartada: um carregador próprio

`loadHeaderOnly()` + `psramAllocSafe()` + `readFrames()` num buffer nosso
também evita a rampa, mas perde mais do que ganha:

- **A política de alocação da M16.** O teto de 90% da PSRAM livre com segunda
  tentativa a 75%, o truncamento (um arquivo grande demais passaria a falhar
  em vez de ser cortado) e o fallback para `malloc()` na RAM interna. A
  `psramAllocSafe()` sozinha não tem fallback nenhum.
- **O estado do `Wav`.** `getBuffer()` ficaria `nullptr`, `isLoaded()` falso,
  e `getFrameCount()` igual a `getFileFrameCount()` — qualquer aviso de
  truncamento que compare os dois nunca dispararia.
- **O limite do chunk `data`.** `readFrames()` não para no fim dele: pedir
  frames demais converte como áudio o que vier depois (chunks `LIST` ou `cue`,
  que DAW costuma anexar). Teria que ser limitado à mão.
- **Leitura curta silenciosa.** `readFrames()` devolve só "leu alguma coisa",
  não quantos frames. Uma leitura curta deixa o resto de um buffer recém-alocado
  com lixo, a menos que ele seja zerado antes.
- **`readFramesFast()` não é equivalente.** É o caminho que o
  `wav_to_header.py` dizia espelhar, e ele difere em float de 32 bits: não faz
  clamp em ±1,0 (`Wav.h:714–716`). Em inteiro é igual.

Reler as bordas depois do `load()` não perde nada disso.

### Como ouvir

1. Anotar o conector USB. Pela seção 11, todo resultado de ouvido registra o
   conector.
2. Gravar o build normal e tocar cada pad isolado.
3. Gravar com a chave ligada:
   ```bash
   PLATFORMIO_BUILD_FLAGS='-DWAV_RESTORE_EDGES=1' pio run -e sampler -t upload
   ```
   Conferir no boot uma linha `fades undone` por arquivo. Sem ela, o build não
   é o que se pensa. A M16 continua imprimindo `Applying 10ms fade` antes dela,
   e isso é esperado.
4. Comparar o ataque. O que deve mudar é o "tec" inicial de kick, slap ou aro,
   que fica mais presente. Um clique **novo** no começo de algum sample quer
   dizer que o arquivo começa fora do zero — um defeito do arquivo que a rampa
   estava escondendo, não da chave.
5. Registrar aqui. Se a diferença for audível e para melhor, virar o padrão
   para 1 num commit próprio.

### Estado

| Item | Estado |
|---|---|
| A rampa existe e roda em todo sample do SD | **confirmado no código** (M16 `aef89b2`) |
| Efeito no kick do repositório | **modelado no host**, não medido na placa |
| Releitura bit-idêntica à conversão do `load()` | **lido no código**, não executado |
| `WAV_RESTORE_EDGES=1` | **compila**; nunca rodou na placa |
| `WAV_RESTORE_EDGES=0` (padrão) | build **idêntico ao do `main`** em todas as seções (`.text`, `.rodata`, `.data`, `.bss`) — o padrão não mudou nada |
| Audível | **não ouvido** |

## 13. Spike de armazenamento do Agbau: opção A e escada de vozes — 11/10/2026

Primeira rodada de `agbau/bench/agbau_storage.cpp` (env `agbau_storage`) na
placa. Contexto em [agbau/docs/migration.md](../../agbau/docs/migration.md),
etapa 0. **Placa no conector COM** (ponte WCH `1a86:55d3`), que neste env também
é por onde sai a serial — ver seção 11.

Só a opção **A** (kit numa partição da flash, mapeado) foi medida. A **B**
(SD → PSRAM) não rodou: o cartão montou, mas não tinha `/agbau/`.

### Opção A

| Medida | Resultado |
|---|---|
| Gravar o kit (8,996 MB, `esptool write_flash` a 921600) | 1 min 47 s, hash verificado — é o custo de trocar o kit nesta opção |
| `esp_partition_mmap()` dos 8,996 MB | **funciona**, 53 µs; páginas de MMU livres 378 → 240 |
| Checagem de limites da imagem | 56 µs |
| CRC completo no boot | 1078 ms |
| 16 fluxos crus (sem envelope) lendo da flash | média 3730 ciclos/bloco (**2,1%**), pior 4772 (2,7%) de 174 149 |

A maior dúvida da opção A — se um mapeamento de ~9 MB cabe ao lado dos 8 MB
de PSRAM já mapeados — está respondida: cabe, e sobram 240 páginas de 64 KB.

### Escada de vozes (lendo da flash)

`VoiceBank` real, todas as vozes em loop com o envelope de verdade (o custo de
um ganzá segurado, a voz mais cara do Agbau). Cada degrau: 6 s de seno de
440 Hz com as vozes renderizando mudas, depois 6 s das vozes audíveis.

| Vozes | Render médio | Pior | Maior intervalo entre renders | Blocos/s mín. |
|---|---|---|---|---|
| 1 | 1,5% | 2,5% | 1397 µs | 1378 |
| 4 | 4,7% | 6,5% | 1390 µs | 1378 |
| 8 | 8,9% | 11,9% | 1374 µs | 1378 |
| **16** | **17,3%** | **21,5%** | 1314 µs | 1378 |
| 32 | 34,1% | 40,3% | 1223 µs | 1378 |
| 64 | 68,0% | 79,9% | 1042 µs | 1378 |

O buffer de DMA tem 5804 µs. Nenhum intervalo chegou perto disso em nenhum
degrau: **por aritmética, não houve underrun de 1 a 64 vozes.** O intervalo
de ~1,4 ms é a cadência normal: o `audioBlockWrite()` libera um buffer de
DMA de 64 frames (1,45 ms) por vez, e o motor renderiza dois blocos de 32 em
seguida.

O Agbau usa no máximo 16 vozes, e com elas sobra quatro vezes o orçamento.
Isso também cobre a revalidação do buffer de 5,8 ms contra o pin atual da M16
(CLAUDE.md), numa carga quatro vezes maior que a do rolo de quatro vozes
original — pelos números e de ouvido: **nenhum clique no seno em nenhum
degrau**, de 1 a 64 vozes.

**Custo por voz:** ~1,05% do bloco por voz real, contra ~0,13% por fluxo cru
na medição da opção A. A voz com envelope sai ~8× mais cara que a leitura
pura. Não é problema com 16 vozes, mas indica onde está o custo — o laço de
`Voice::render()`, não a memória — se um dia faltar CPU.

### Situação

| Item | Status |
|---|---|
| mmap, CRC, custo de render, escada | **medido na placa** |
| Escada sem estalos no seno | **ouvido na placa** (COM): nenhum clique, de 1 a 64 vozes |
| Opção B (SD → PSRAM, com as bordas restauradas) | **não medida, e dispensada**: com esses números a escolha foi a A ([agbau/docs/migration.md](../../agbau/docs/migration.md), etapa 0). Para medir mesmo assim, basta copiar `sd_kit/` para `/agbau/` no cartão |
| Comparação amostra a amostra A × B | **não executada** (depende da B) |

## 14. Primeiro som do Agbau: quatro botões — 11/10/2026

Primeira rodada de `agbau/src/main.cpp` (env `agbau`, commit `55a2af7`). Ainda
sem matriz: os quatro botões que já estavam na bancada (GPIO 4–7, ao GND), cada
um tocando uma voz. O objetivo era testar os disparos, não onde cada som fica.
O kit é lido da partição `samples` (opção A), gravada na rodada da seção 13 e
não regravada desde então. **Placa no conector COM**, que também leva a serial
(`DEBUG_TRIGGERS` e `DEBUG_ENGINE` ligados).

| Botão | Voz | Preset |
|---|---|---|
| GPIO4 | b1 | baixo, 1 take |
| GPIO5 | p1 | percussão, 5 takes |
| GPIO6 | p3 | ganzá, 1 take em loop |
| GPIO7 | p5 | percussão, 3 takes |

### Pelo log (977 s, 270 toques)

A linha do `DEBUG_ENGINE` sai uma vez por segundo, e os horários são do PC.
Então o log mostra que cada voz para, mas não mede o tempo exato do release.

- **Motor:** 1378–1380 blocos/s o tempo todo, `dropped 0` na fila, pior
  render 6,4% do bloco. No máximo 3 vozes ao mesmo tempo; pico de 17 942
  (55% da escala), sem clipar.
- **Toques:** GPIO4 69, GPIO5 90, GPIO6 105, GPIO7 6. Todos com a soltura
  correspondente; nenhum preso, nenhuma soltura perdida.
- **b1** ainda aparece até 0,8 s depois de solto, compatível com o release de
  1 s. **p3** já tinha parado na linha seguinte em todas as 27 solturas
  isoladas.
- **p1:** saíram as cinco takes, de 9 a 14 linhas cada. Some de 0,4 a 1,6 s
  depois do toque porque as takes duram 1,3–1,7 s e acabam antes do decay de
  3 s. O envelope congela onde a take acabou, como no Bela.
- **p5:** some em até 3 s (as takes foram cortadas em A+D = 3,01 s). Em 6
  toques saíram só p5.3.2 e p5.3.3. A chance disso por acaso é de ~9%, então não
  indica defeito, mas a amostra é pequena.
- **Disparos duplos:** 12 vezes, o mesmo botão disparou duas vezes dentro de uma
  mesma leitura da serial (~0,2 s). O log não separa um toque rápido de um
  quique do contato ao soltar. O ouvido separa, logo abaixo.

### De ouvido

- **Nenhum flam:** nenhum toque único soou duplo. Os 12 disparos duplos foram
  toques rápidos de verdade.
- **Nenhum estalo ao repetir rápido a mesma percussão.** O porte mantém o
  retrigger do Bela: a take volta ao início e o envelope sobe a partir do nível
  atual. O plano deixava para decidir de ouvido se isso estalava em rulos; com
  estes botões, não estala, e fica como está.
- **Ganzá sem buraco nem estalo na emenda do loop.** É a opção A, sem o fade de
  borda da M16 (seção 12) que abriria um buraco de ~20 ms a cada volta.
- **As takes do p1 soam diferentes entre si**, e a variação agradou.

### Situação

| Item | Status |
|---|---|
| Disparo, soltura, release por preset, sorteio de takes | **ouvido na placa** (COM) e conferido no log |
| Retrigger sem estalo | **ouvido**, repetindo um botão rápido |
| Matriz 4×4 | **não testada**: os botões passam por `KeyMatrix<4,1>`, sem varredura de colunas |
| 16 vozes ao mesmo tempo no firmware do Agbau | **não testado aqui**. A escada da seção 13 roda o mesmo `VoiceBank` até 64 vozes sem clique |

## 15. Agbau: três vozes, botão de modo e pedal — 11/10/2026

A bancada tem quatro botões. O quarto deixou de ser uma voz e virou o botão de
modo, e entrou um quinto botão para o pedal:

| Botão | Função |
|---|---|
| GPIO4 | b1, baixo |
| GPIO5 | p1, percussão, 5 takes |
| GPIO6 | p3, ganzá |
| GPIO7 | botão de modo (o GPIO40 no mapa final), última linha da `KeyMatrix<4,1>` |
| GPIO39 | pedal do looper, já no pino do mapa final. É um `TriggerInput` próprio, fora da matriz |

Nesta rodada, o botão de modo e o pedal **só aparecem no log**: cada toque, o
intervalo desde o toque anterior (só no modo) e o tempo apertado. Eles não
mudam o som. O que vão fazer está em
[agbau/docs/looper.md](../../agbau/docs/looper.md). Firmware ainda sem
commit; placa no COM, com `DEBUG_TRIGGERS` e `DEBUG_ENGINE` ligados.

### Pelo log

- **Motor:** 1378 blocos/s, `dropped 0`. O render usa 1,1% do bloco parado e
  no pior caso 6,4% com as três vozes juntas, o mesmo da seção 14.
- **Vozes:** cada toque tem a sua soltura, nos três botões. As três tocaram
  juntas (`playing[b1 p1.3.4 p3.1]`, pico de 17 421, 53% da escala). O p1
  sorteou takes diferentes, o b1 continua uma linha a mais depois de solto
  (release de 1 s) e o p3 some na linha seguinte.
- **Pedal:** um disparo por toque, sem disparo duplo, soltura com 204 ms.
- **Botão de modo:** um disparo por toque. O primeiro toque depois do boot sai
  como `first tap`. Os toques ficaram apertados de 98 a 159 ms, bem abaixo de
  1 s, o tempo em que segurar o botão sai do repeat.

### Tempo batido à mão

Sete intervalos seguidos, batidos num tempo constante: 676, 695, 680, 671,
688, 706 e 670 ms. A média é 683,7 ms (87,8 bpm), com desvio padrão de 13,3 ms
(1,9%). O menor e o maior intervalo correspondem a 89,6 e 85,0 bpm.

O `looper.md` decide que vale o **último intervalo, sem média**, como no
`.ino`. Com estes toques, o tempo do repeat cairia em qualquer ponto entre
85,0 e 89,6 bpm, conforme qual tenha sido o último toque.

O `.ino` daria **660 ms (90,9 bpm) para os sete**, todos iguais. O bug 1 do
`looper.md` (`(1000 / bpm) * 60` em inteiros) só produz tempos em degraus de
60 ms, e de 84 a 90 bpm todos caem em 660 ms. No original, o tempo saía
estável e 3,5% rápido. Com o bug corrigido, ele sai certo e varia ±2% de um
tap para o outro. É uma diferença de jogabilidade que só vai poder ser julgada
de ouvido, quando o repeat existir.

Antes destes toques, dois intervalos longos: 29 499 ms e 135 468 ms. Pela
regra do `looper.md`, o primeiro mudaria o tempo para um tempo a cada 29,5 s,
como o `.ino` também faria. O segundo passa de 60 s e só recomeçaria a
contagem.

### Situação

| Item | Status |
|---|---|
| Três vozes, soltura, sorteio de takes | **ouvido na placa** com este firmware e conferido no log |
| Botão de modo e pedal: disparo, soltura, intervalo, tempo apertado | **conferido no log** |
| Repeat e looper | **não existiam nesta rodada**. A spec (`looper.md`) foi aprovada depois dela, e o código veio em seguida |

## 16. Agbau: repeat e looper, de ouvido — 11/10/2026

Primeira rodada com o repeat e o looper de
[agbau/docs/looper.md](../../agbau/docs/looper.md) (commit `9331a10`), na
mesma bancada da seção 15: b1, p1 e p3 nos GPIO 4–6, o botão de modo no GPIO7
e o pedal no GPIO39. Os LEDs não estão ligados.

O roteiro foi o da seção "Na bancada atual" do `looper.md`, e **os cinco
pontos soaram como a spec descreve**:

| Teste | O que se esperava |
|---|---|
| Repeat | bater o tempo no modo e segurar o p1: repete no tempo, trocando de take. O b1 no repeat passa de 4 para 2 por compasso. Segurar o modo 1 s sai do repeat |
| Ganzá no repeat | segurar o p3 no repeat: o loop recomeça a cada batida e para ao soltar |
| Bug 7 | segurar o p3 no modo nota, bater o tempo e soltar: o ganzá para |
| Looper | gravar, fechar, abrir overdub e tocar por cima: o overdub fecha sozinho no começo do loop. Segurar o pedal 1 s apaga, e nada continua tocando |
| Repeat dentro do loop | um loop gravado com repeat se mantém no tempo, volta após volta |

O registro é o relato de quem tocou ("me parece tudo ok"). Nenhum log desta
rodada foi lido, e não houve medição de deriva na placa: a garantia de que o
loop não deriva vem dos testes do host (`test_loop_does_not_drift`,
`test_repeat_grid_does_not_drift`), e o ouvido não contradisse.

Uma coisa que confundiu no caminho, e que não é defeito: na bancada só há o b1,
então a divisão vai de 4 para 2 por compasso e não volta. A divisão sobrevive
à saída e à volta do repeat, como no `.ino`, e só um reset a devolve a 4.

### Situação

| Item | Status |
|---|---|
| Repeat, divisão, sair segurando | **ouvido na placa** |
| Looper: gravar, overdub, apagar | **ouvido na placa** |
| Bug 7 (ganzá atravessando a troca de modo) | **ouvido na placa** |
| Repeat e looper com as 16 teclas | **não testado**: falta a matriz |
| LEDs | **não testados**: nada ligado nos GPIO 15 e 8 |

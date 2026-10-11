# Circuito alternativo — versão de baixa latência

Guia de montagem do experimento de latência mínima. São **duas** alterações de
hardware em relação ao [circuit.md](circuit.md):

1. **Filtro RC nos 4 botões** — tira o quique de contato do software.
2. **Pino `FLT` do PCM5102A em 3V3** — troca o filtro digital do DAC pelo de
   baixa latência. Um jumper.

Leitor microSD, alimentação e terra comum não mudam.

## O orçamento de latência

O firmware desta branch já fez a parte de software. A conta completa,
batida-até-som:

| Termo | antes | agora | como |
|---|---|---|---|
| Debounce do GPIO4 | 25 ms | **0,2 ms** | software — mesmo esquema dos pads |
| Confirmação dos pads | 0,2 ms | 0,2 ms | inalterado (é a defesa contra fantasma) |
| Polling do `loop()` | ~50 µs + risco de travar no Serial | ~20 µs | software — log de disparo desligado por padrão |
| **Ring DMA do I2S** | **46,4 ms** | **5,8 ms** | software — `DMA_BUFFER_LENGTH` |
| Filtro digital do DAC | 0,50 ms | **0,08 ms** | **hardware — `FLT` em 3V3** |
| Filtro RC | — | +0,05 ms | **hardware — custo, não ganho** |
| **Total (pads)** | **~47 ms** | **~6,1 ms** | |
| **Total (GPIO4)** | **~72 ms** | **~6,1 ms** | |

O termo que dominava tudo era o ring de DMA da M16, e ninguém tinha olhado
para ele. Os 0,2 ms de debounce que a gente vinha discutindo eram 0,4% do
problema.

⚠️ **O ring de 5,8 ms ainda não foi validado em hardware.** Se der estouro
(clique, chiado), o caminho é subir — ver "Calibrar o ring" no fim.

## Por que fazer isso

Um botão mecânico não liga limpo: as lâminas quicam, e em ~2 ms o pino vê uma
sequência de abre-fecha em vez de um evento único. Hoje isso é filtrado no
firmware, e funciona. O que o RC acrescenta:

- **O pino passa a ver um evento único.** Simulado: uma prensada com 2 ms de
  quique produz **14 transições** no pino sem o RC e **2** com ele (uma descida,
  uma subida). O software para de precisar adivinhar qual transição é a batida.
- **Jitter.** Disparar na primeira leitura baixa significa disparar em qualquer
  microcontato que o polling pegar primeiro — a incerteza é a estrutura do
  quique mais o período do loop. Com o RC a descida vira monotônica e
  determinística, e a incerteza cai para só o período do loop. Em percussão,
  variação de tempo incomoda mais que latência absoluta: latência você compensa
  tocando adiantado, variação não.
- **Impedância do fio.** O pull-up interno do ESP32 é fraco (~45 kΩ). Num jumper
  de protoboard isso é uma antena de alta impedância, no mesmo circuito que um
  BCK de I2S chaveando a ~2,8 MHz. Trocar por 10 kΩ em paralelo (≈8,2 kΩ
  efetivos) reduz a captação em ~5×.

### O que ele NÃO faz — e onde ele PIORA

Contra glitch de **contato** (fecho momentâneo: sujeira, vibração, filete de
metal), o RC não só deixa de ajudar — ele **atrapalha**. E isso é o oposto da
intuição, então vale o número medido:

| Glitch no botão | LOW que chega ao pino | fator |
|---|---|---|
| 50 µs | **1048 µs** | 21× |
| 200 µs | 1281 µs | 6,4× |
| 1000 µs | 2081 µs | 2,1× |

Todo glitch sai **~1081 µs mais longo**, sempre a mesma parcela. A razão é a
assimetria que faz o circuito ser bom para batida: a descarga é rápida (21 µs)
e a **recarga é lenta** (818 µs). O pino desce quase instantaneamente e depois
leva mais de um milissegundo para voltar a cruzar V_IH.

Consequência prática, e ela importa: **com o RC montado, os 200 µs de
`TRIG_CONFIRM_US` deixam de rejeitar glitch de contato.** Sem o RC, um blip de
50 µs dá 50 µs de nível baixo e a janela o descarta. Com o RC, o mesmo blip dá
1048 µs e passa. Para rejeitar depois do RC seria preciso uma janela maior que
o alongamento — ~1,5 ms, contra os 0,2 ms de hoje.

A contrapartida, e é ela que justifica montar o filtro assim mesmo: ruído
**irradiado**, acoplado capacitivamente, chega por impedância altíssima, e aí o
capacitor o curto-circuita antes de o pino chegar perto do limiar. Nesse caso o
RC previne o glitch inteiramente, em vez de alongá-lo. Esse caso **não está
simulado** — não trate como número.

Ou seja, o efeito do RC sobre batida fantasma é **bidirecional**, e qual dos
dois domina depende de que tipo de ruído a sua montagem realmente sofre. É
justamente isso que o teste de ruído na bancada resolve, e é por isso que ele
não é opcional.

Resumo: o RC resolve quique e jitter (medido). Contra glitch ele ajuda no caso
irradiado (não medido) e atrapalha no caso de contato (medido). A janela de
confirmação no software é a única rejeição medida que temos.

## Alteração 2: `FLT` do PCM5102A em 3V3

O PCM5102A tem dois filtros digitais de interpolação, selecionados pelo pino
`FLT` (pino 11):

| `FLT` | Filtro | Group delay |
|---|---|---|
| GND (padrão, e o que o `circuit.md` mandava) | FIR linear-phase | **22 amostras** = 0,50 ms a 44,1 kHz |
| **3V3** | IIR minimum-phase | **~80 µs** |

São ~420 µs de graça, num orçamento que agora é de ~6 ms — 7% do total, por um
jumper.

E para percussão o filtro de baixa latência é provavelmente **melhor em
qualidade também**, não só mais rápido. Filtro linear-phase produz *pre-ringing*:
oscilação que aparece **antes** do transiente, porque a resposta ao impulso é
simétrica. Num ataque de bumbo isso borra a batida. Minimum-phase concentra a
energia depois do transiente, o que é como a percussão acústica se comporta. A
troca em áudio geral é discutível; em transiente percussivo, não muito.

**Cheque a sua placa antes.** Muitos breakouts genéricos ("GY-PCM5102") já
amarram `FLT` a GND internamente e nem expõem o pino. Se estiver exposto, ligue
a 3V3. Se estiver amarrado, essa melhoria não está disponível sem retrabalho na
placa — e aí você fica com os 0,50 ms, o que não inviabiliza nada.

## Lista de compras

Para os 4 botões:

| Qtd | Componente | Marcação |
|---|---|---|
| 4 | Resistor 10 kΩ (¼ W) | marrom · preto · laranja · dourado |
| 4 | Resistor 220 Ω (¼ W) | vermelho · vermelho · marrom · dourado |
| 4 | Capacitor cerâmico 100 nF | serigrafia `104` |
| 2 | Resistor 10 kΩ | *para o rig de medição* |
| 1 | Capacitor cerâmico 100 nF | *para o rig de medição* |

Valores não são críticos: 4,7 k–10 k no pull-up e 100 Ω–1 k no série funcionam
igual. O que importa é a **relação** entre os dois, explicada abaixo.

Mais um fio para o `FLT` → 3V3.

## O circuito, por botão

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

Repetir os quatro: GPIO4 (trigger), GPIO5, GPIO6, GPIO7 (pads).

## Por que funciona: as constantes de tempo são assimétricas

Esse é o ponto que faz o circuito servir tão bem para um pad de percussão.
Descida e subida passam por caminhos diferentes, com τ muito diferentes:

Valores efetivos, já contando o pull-up interno de ~45 kΩ em paralelo com R1
(≈8,2 kΩ) e o limiar Schmitt do ESP32 (V_IL 0,82 V / V_IH 2,47 V):

| Evento | Caminho da corrente | τ | Efeito prático |
|---|---|---|---|
| **Batida** (descida) | capacitor descarrega por **R2** | **21 µs** | Alvo de 86 mV. Latência medida na simulação: **+50 µs** ponta a ponta, ~2 amostras de áudio |
| **Quique durante a batida** | recarrega por **R1** | 818 µs | Uma abertura de 300 µs mal levanta a tensão, longe do limiar. O pino nem percebe |
| **Soltura** (subida) | carrega por **R1** | 818 µs | ~1,2 ms até cruzar V_IH. Musicalmente irrelevante |
| **Quique na soltura** | qualquer refechamento zera o capacitor | — | A subida só completa quando o quique realmente acaba |

Ou seja: **rápido na batida, filtrado na soltura** — exatamente a forma que a
lógica dos pads quer, e você ganha de graça, pela física.

**Limite do filtro:** ele absorve segmentos de quique de até ~1 ms. A partir de
~3 ms de abertura o capacitor tem tempo de carregar além do limiar e o quique
volta a aparecer (simulado: 14 transições de novo). Switch de qualidade quica em
0,1–1 ms, então isso cobre o caso normal; se o seu switch estiver muito gasto,
o caminho é aumentar C — ao custo proporcional de latência na batida.

Funciona porque as entradas GPIO do ESP32 têm **Schmitt trigger**. Sem
histerese, uma rampa lenta cruzando o limiar oscilaria e produziria várias
transições — é o erro clássico de quem tenta debounce RC em entrada comum.

O R2 tem duas funções: dar o τ rápido da descida e limitar o pico de corrente
de descarga do capacitor (3,3 V / 220 Ω ≈ 15 mA por alguns µs), o que preserva
o contato do botão. Sem ele o capacitor descarrega direto pelo switch.

Consumo em regime, com o botão apertado: 3,3 V / 10,2 kΩ ≈ **0,32 mA por
botão**. Os quatro juntos não chegam a 1,3 mA.

## Montagem na protoboard

1. **Cuidado com o tactile switch de 4 pernas:** as duas pernas do *mesmo lado*
   já vêm curto-circuitadas de fábrica. Use uma perna de cada lado — se o botão
   atravessa o vão central da protoboard, uma perna de cada lado do vão. Duas
   pernas da mesma coluna = botão sempre fechado.
2. Monte **um** botão primeiro, teste, e só depois replique. Errar quatro vezes
   o mesmo circuito é frustrante.
3. **O capacitor perto do pino.** Ele é a referência de ruído; jumper longo
   entre o capacitor e o GPIO desfaz parte do benefício.
4. Terra comum de verdade — o mesmo trilho de GND do DAC e do SD. O
   [circuit.md](circuit.md) descreve um caso real de áudio instável causado por
   GND flutuante.

## Como testar sem osciloscópio

**Com multímetro, antes de ligar o firmware.** Meça a tensão no pino:

- Solto: deve ficar em ~3,3 V.
- Apertado: deve ficar em **~70 mV**, não em 0 V. Isso é o divisor R1/R2
  funcionando. Se der 0 V exato, o R2 não está no circuito. Se der acima de
  ~300 mV, confira o valor do R2 — muito alto e o pino não chega ao nível
  baixo lógico.

**Com o firmware, o teste bom.** Com `DEBUG_TRIGGERS 1`, o firmware imprime `Pad N:
triggered` a cada disparo. Como o RC torna as proteções de software
redundantes, dá para provar que ele funciona **enfraquecendo o software** e
verificando que nada piora:

1. Com o firmware como está (`TRIG_CONFIRM_US 200`, `TRIG_RELEASE_MS 3`), bata
   nos pads e confirme uma linha por batida.
2. Temporariamente, em [../src/main.cpp](../src/main.cpp), baixe para
   `TRIG_CONFIRM_US 0` e `TRIG_RELEASE_MS 1`.
3. Bata de novo, inclusive rápido e de leve. **Se continuar uma linha por
   batida, o RC está fazendo o trabalho.** Sem o RC, esses valores deixariam
   passar quique e ruído.
4. **Restaure para 200 e 3.** Mantenha as duas proteções: numa protoboard não
   há garantia de que os quatro RC ficaram bem contactados, e defesa em
   profundidade custa 0,2 ms.

Os quatro passos acima estão validados contra **o código real do firmware**
(`pio test -e native` e `--sweep=rc`, que hoje instanciam o mesmo
`TriggerInput` que roda na placa): com o RC, mesmo `confirm 0 / release 0` dá
**1 disparo por batida**, enquanto o pino cru nessa configuração dá 7.

Um detalhe do passo 3 que só apareceu quando o simulador passou a usar o
código de verdade: mesmo com `TRIG_CONFIRM_US 0`, o firmware ainda exige o pino
em nível baixo em **duas leituras consecutivas** do `loop()`. Ou seja, ele
mantém de graça um período de polling (~20 µs) de rejeição de glitch. Não conte
com isso como proteção — é margem, não defesa — mas saiba que "confirm 0" não
significa "sem nenhum filtro".

**Teste de ruído — com os valores restaurados.** Com tudo montado, `confirm` de
volta em 200 µs e o áudio tocando, passe a mão perto dos fios dos botões, mexa
nos jumpers, ligue algo indutivo por perto (furadeira, motor, fonte chaveada).
Não pode sair batida nenhuma.

Faça este teste **depois** de restaurar, não durante o passo 3: a rejeição de
glitch vem da janela de confirmação no software, não do RC (ver "O que ele NÃO
faz"). Com `confirm 0` você estaria testando o pad justamente sem a proteção
que cobre esse cenário.

## Medir de verdade — o rig de latência

Todo o orçamento acima é **aritmética**: τ do RC, group delay do datasheet,
profundidade de ring. Nunca ninguém viu o número. Este rig mede a cadeia
inteira, de dedo a som, com dois resistores e um capacitor.

A parte que importa é que ele observa a **saída analógica do DAC**. Um marcador
em GPIO não serve: o dreno do DMA e o filtro do DAC acontecem *depois* de a CPU
escrever, e são justamente os dois termos que estamos tentando calibrar.

### O circuito

```
        3V3
         │
       [10k]
         │
DAC L ──┤├──┬─── GPIO1  (ADC1_CH0)
       100nF │
           [10k]
             │
            GND
```

Os dois resistores criam um ponto médio em **1,65 V**, e o capacitor acopla o
áudio em cima dele. Isso é obrigatório, não estético: a saída do DAC é
centrada em 0 V e oscila para os dois lados; sem o deslocamento, o semiciclo
negativo entraria no ADC, que só aceita 0–3,3 V. Com a polarização, o silêncio
fica no meio da escala e o sinal balança em volta sem nunca ir a negativo.

Qualquer um dos canais serve — samples mono saem nos dois. Some 2× 10 kΩ e
1× 100 nF à lista de compras.

O GPIO1 é ADC1_CH0 e está livre neste projeto (os ocupados são 4–7 botões,
10–13 SD, 16–18 I2S).

### Usar

Em [../src/main.cpp](../src/main.cpp), ligue:

```c
#define LATENCY_PROBE 1
```

No boot, ele amostra a saída em silêncio para aprender onde fica a linha de
base e quanto ela oscila, e deriva o limiar de detecção **do rig real** em vez
de um chute:

```
PROBE: baseline=2047 noise=12 threshold=88 (GPIO1)
```

Se o ruído vier acima de ~200 ele avisa — geralmente é o divisor mal contactado
ou terra ruim.

Cada batida imprime:

```
PROBE /1.wav: total 6180 us  (trigger 210 + audio path 5970)
```

- **trigger** — o que o firmware gastou decidindo (a janela `TRIG_CONFIRM_US`
  mais o período de polling).
- **audio path** — o que veio depois do `start()`: ring de DMA e filtro do DAC.
  **É este número que a escada abaixo move.**

Duas limitações honestas: o `analogRead()` do Arduino leva ~50–100 µs, então a
resolução é dessa ordem — o que é 1–2% de uma medida de ~6 ms, aceitável. E o
sample precisa começar com transiente; se o `.wav` abrir com silêncio, o probe
mede o silêncio junto e acusa timeout.

Deixe `LATENCY_PROBE 0` fora da bancada: ele põe um `analogRead()` bloqueante
dentro do `loop()`.

## Calibrar o ring de DMA — a parte que precisa da placa

Esta é a única alteração do pacote que **não** pôde ser verificada sem
hardware, e é a de maior impacto. O `src/engine_config.h` (que entra pelo
`audio_block.h`, antes do `#include "M16.h"`) define:

```c
#define DMA_BUFFERS       4
#define DMA_BUFFER_LENGTH 64      // 256 frames = 5,8 ms
```

O ring precisa absorver a maior pausa que o produtor de áudio possa levar. Neste
firmware não existe escrita em flash, nem WiFi, nem SD depois do boot — sobra o
cache miss de PSRAM dentro do `audioUpdate()`, que é sub-microssegundo. Por isso
dá para ser bem mais raso que os 46 ms da M16, que foram dimensionados para
sobreviver a um apagamento de setor de flash.

**Teste de estresse:** toque um rolo rápido com **os quatro pads soando ao mesmo
tempo**. É o caminho mais pesado do `audioUpdate()` — quatro vozes somadas — e é
onde um estouro aparece primeiro. Um pad sozinho não prova nada.

| Config | Ring | |
|---|---|---|
| 4 × 256 | 23 ms | recuo seguro |
| 4 × 128 | 11,6 ms | |
| **4 × 64** | **5,8 ms** | onde está hoje |
| 4 × 32 | 2,9 ms | |
| 4 × 16 | 1,5 ms | ISR a cada 363 µs |
| 2 × 16 | 0,73 ms | piso — abaixo de 2 descritores não há double buffering |

Um `static_assert` no `main.cpp` falha o build se o `#define` for parar depois
do `#include` — sem ele, os 46 ms voltariam sem nenhum outro sintoma.

Vale saber por que dá para ser tão raso aqui: o ring existe para cobrir a maior
pausa que o produtor de áudio possa levar, e neste firmware **as pausas grandes
não existem** — nada escreve em flash em runtime, não há WiFi, o SD não é tocado
depois do boot. Sobra o cache miss de PSRAM dentro do `audioUpdate()`, que custa
1–2 µs; com ring de 1,5 ms seriam necessárias centenas de faltas consecutivas de
pior caso para furar. A task de áudio, além disso, já roda em prioridade máxima
fixada no core 0 (a M16 faz isso), com o `loop()` no core 1 — não há folga a
extrair de escalonamento.

## O procedimento, em ordem

Sábado, com a placa:

1. **Validar os 5,8 ms.** Grave como está e faça o teste de estresse acima.
   Se estourar, suba um degrau e repita até ficar limpo. Este passo vem antes de
   tudo: se 5,8 ms não se sustentar, o resto da conversa muda de assunto.
2. **Montar o rig** e ligar `LATENCY_PROBE 1`. Anote o número real. É a primeira
   vez que este projeto vai ter um.
3. **Descer a escada com o número na mão** — um degrau, mede, ouve, decide.
   Pare quando o áudio sujar ou quando o ganho deixar de valer o risco.

Uma nota de proporção: abaixo de ~2 ms você entra na faixa em que o curso
mecânico do botão e a variação do seu próprio tempo tocando passam a dominar.
Chegar aos 5,8 ms validados já põe o instrumento na faixa imperceptível para
percussão; o resto é rendimento decrescente.

## Ajustes finos de firmware, depois da bancada

- `TRIG_RELEASE_MS` pode cair de 3 para 1 ms — o RC já garante a soltura limpa,
  e isso aumenta a taxa máxima de rolo.
- **`TRIG_CONFIRM_US` não deve cair.** Ele custa 0,2 ms e é a única defesa
  medida contra batida fantasma; o RC não cobre esse caso. Baixe só durante o
  passo 3 do teste, e restaure.
- `DEBUG_TRIGGERS` já vem em `0`. Ligar coloca um `Serial.printf` dentro do laço
  de polling, e USB CDC bloqueia quando o host não drena o buffer — ou seja, um
  travamento sem limite superior no caminho do trigger. Use para diagnosticar
  entrada, nunca com o áudio sendo avaliado.

Antes de mexer nos números, vale rodar o simulador para ver o que cada valor
faz — ele roda no PC, sem placa:

```bash
g++ -O2 -std=c++17 -o /tmp/trigger_sim sim/trigger_sim.cpp
/tmp/trigger_sim --sweep=all
```

## Se der problema

| Sintoma | Causa provável |
|---|---|
| Pad não dispara nunca | R2 alto demais, ou pernas do mesmo lado do switch |
| Pino não sobe a 3,3 V ao soltar | R1 sem contato, ou capacitor em curto |
| Continua disparando várias vezes | Capacitor não está no circuito (confira a leitura de ~70 mV apertado) |
| Batida sai com atraso perceptível | R2 alto demais — τ da descida cresceu. Baixe para 100–220 Ω |
| Batida fantasma persiste | Fio dos botões passando junto aos fios de I2S; afaste e encurte |

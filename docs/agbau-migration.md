# Como se aproximar do Agbau: Bela e Arduino saem, um ESP32-S3 fica

O [Agbau](https://www.instagram.com/bongarbit/) é outro instrumento do
Bongarbit: 16 botões (matriz 4×4) disparam até 16 vozes polifônicas com
envelope ADSR, hoje divididas entre um Arduino Pro Micro (lê os botões, roda
tap-tempo e um looper com overdub, fala MIDI USB) e uma Bela (recebe esse MIDI
e toca os samples). A decisão tomada em conversa foi: sem Arduino, sem MIDI —
um único ESP32-S3 lê os botões e toca o áudio.

Este documento ordena o trabalho de chegar lá a partir do que já existe nesta
branch, e nomeia o que ainda não tem resposta. É um documento vivo: a primeira
versão foi conferida contra a fonte do Agbau (`agbau/Bela/agbau-sampler-poly/`,
`agbau/Arduino/agbau/agbau.ino`, as 24 WAVs e o `Keypad.cpp` vendorizado) em
11/10/2026, e cinco premissas dela estavam erradas. As seções abaixo já estão
corrigidas; o que mudou e por quê está registrado em cada uma, porque o erro
original é plausível e voltaria se apagado.

## Por que a base daqui serve

Nada no Agbau processa áudio de entrada; é disparo e soma de sample com
envelope de amplitude, sem filtro, reverb, pitch ou FFT (`agbau/Bela/
agbau-sampler-poly/render.cpp:215-226`, `Sampler.cpp:108-126`). É uma carga de
DSP muito menor que o pitch shifter que já roda nesta branch. A parte de
maior risco técnico — um motor em bloco, float, sem alocação, com as samples
residentes antes do primeiro som, aguentando tempo real no S3 — já está
provada aqui. O que falta é engenharia de funcionalidade, não pesquisa de
arquitetura.

## Ordem recomendada

| # | Etapa | Por que nessa posição |
|---|---|---|
| 0 | Onde as samples moram: ferramenta de kit + spike | O kit não cabe na PSRAM como está. Decide a placa (com ou sem SD) e quanto material precisa ser cortado. Trabalho de papel e bancada, barato |
| 1 | Envelope ADSR por voz | Sem ele nenhuma voz do Agbau se comporta como deveria |
| 2 | 16 vozes + takes aleatórios + loop (ganzás) + tabela do kit | Depende só do 1. A voz enxerga `const int16_t*` + frames, então não depende da escolha do 0 |
| 3 | Varredura da matriz 4×4 | Generaliza o `TriggerInput`; independe de 1 e 2 |
| 4 | Mapa de pinos | Antes de fiar qualquer coisa; o 0 decide se o SD ocupa 4 pinos |
| 5 | Firmware integrado: primeiro som | Junta 0–4. É também a revalidação pendente do ring de 5,8 ms, numa carga maior que a de 4 vozes |
| 6 | Repeat (tap-tempo) e looper | O maior e mais arriscado; spec antes de código, e só depois de 5 tocar |
| 7 | Placa física | Fora do firmware; pode andar em paralelo depois do 4 |

A primeira versão punha o orçamento de memória (então etapa 4) e o mapa de
pinos (então 5) *depois* do código que dependia deles — e o próprio texto
dizia "decidir isso antes da etapa 2". Eles vieram para a frente.

### 0. Onde as samples moram

As 24 samples são **float32 mono 44,1 kHz** (formato WAV 3), 17,99 MB em disco.
Em int16 caem para **8,995 MB**, e o N16R8 tem **8,389 MB** de PSRAM
(8 MiB), antes de qualquer outra alocação. Os dois loops de ganzá
(`p3.1.wav`, `p7.1.wav`, ~20 s cada) somam 3,57 MB — 40% do kit.

Há um corte que não muda o som em nada: nas percussões que não são ganzá, o
decay linear de 3 s chega a zero em A+D = 3,01 s e o sustain é 0, então todo
frame depois disso é multiplicado por 0 (`p2.2.1`, `p5.3.*`, `p6.3.2-3`).
Somando a isso o corte de caudas abaixo de −60 dBFS (`p4.3.1` e `p8.3.1` têm
mais da metade do arquivo em silêncio), o kit cai para **8,279 MB**. Cabe — por
~110 KB, contra uma PSRAM que nunca está inteira livre. Não é margem.
`tools/agbau_kit.py` reproduz esses números.

Duas saídas, e a escolha fica para depois de medir as duas na placa
(`src/test_agbau_storage.cpp`, env `agbau_storage`):

- **(A) Partição de dados na flash, mapeada em memória.** A flash do N16R8 tem
  16 MB; uma tabela de partições própria (`partitions_agbau.csv`: app de
  3 MB + `samples` de 12 MB) leva o kit inteiro sem cortar nada, e
  `esp_partition_mmap()` devolve um ponteiro para ele. Não é território novo:
  com `USE_SD_SAMPLER=0` o firmware atual já toca um kick direto da flash. Sai
  o módulo SD (4 GPIOs e um ponto de falha a menos) e o boot fica instantâneo.
  O custo: trocar o kit pede um PC e regravar a partição — o Bela também pedia
  `scp`.
- **(B) SD → PSRAM, como o sampler atual.** Mantém a troca de kit pelo cartão.
  Exige cortar os loops de ganzá para ter margem, e o ponto de corte é uma
  decisão musical (fechar no compasso), não técnica. Reduzir a taxa desses
  loops para 22 kHz também cabe, mas tira o agudo de um ganzá, que é quase só
  agudo.

Seja qual for a escolha, a conversão float32 → int16 acontece uma vez, no PC,
em `tools/agbau_kit.py`, com o mesmo arredondamento de `audioToI16()`. O M16
também converte float32 ao carregar, mas truncando com ×32767; converter antes
dá um ponto único de conversão e faz o boot ler metade dos bytes.

### 1. Envelope ADSR

Hoje `src/audio_block.h` tem o joelho suave mas nenhum envelope de amplitude.
O `ADSR.cpp/h` do Agbau (`agbau/Bela/agbau-sampler-poly/ADSR.h`) é o clássico
attack/decay/sustain/release por amostra, sobre um gerador de rampa **linear**
(`Ramp.cpp`).

**Não é um processador `process(AudioBlock&)` da cadeia** — a primeira versão
deste documento dizia que era, e não pode ser: são 16 envelopes independentes,
cada um aplicado ao fluxo mono de *uma* voz antes da soma. Um bloco já somado
não tem mais o que envelopar. O envelope (`src/envelope.h`) gera um ganho por
frame que a voz aplica ao próprio sinal, um bloco por vez.

Pelo mesmo motivo, "bypass bit-exact quando não há nota ativa" não é o teste
certo. Os contratos que valem são:

- `test_block_size_does_not_change_the_output`, obrigatório para qualquer
  processador desta branch: o envelope tem estado entre blocos, então tem que
  dar o mesmo resultado em pedaços irregulares e numa chamada só;
- no sustain 1,0 o ganho é exatamente `1.0f`, de modo que um baixo segurado
  sai bit-exato em relação à sample;
- concordância frame a frame com uma cópia do `ADSR`/`Ramp` do Bela
  (`sim/bela_adsr_reference.h`), nos três presets — o mesmo papel que
  `mixer.h` cumpre para o joelho suave: é o som que já foi aprovado tocando.

**Não é um ADSR genérico — são dois padrões de uso opostos com a mesma
classe**, selecionados pela flag `releaseOnNoteOff` (`render.cpp:84-104`):

| Grupo | ADSR (A, D, S, R) | `releaseOnNoteOff` | Comportamento real |
|---|---|---|---|
| Baixos (1–8) | `0.01, 0.0, 1.0, 1.0` | `true` (padrão) | Sustenta no nível 1 enquanto a nota estiver ligada; note-off manda pro release de 1s. Instrumento convencional note-on/note-off. |
| Percussão (1–8, exceto ganzás) | `0.01, 3.0, 0.0, 3.0` | **`false`** | O decay de 3s *é* o som inteiro (sustain=0, já chega em silêncio no fim do decay). Note-off é **ignorado** — `noteOff()` só chama `release()` se a flag for true. Um toque rápido ainda deixa o hit decair sozinho até o fim. |
| Ganzás (percussão 3 e 7) | `0.01, 0.0, 1.0, 0.1` + loop | `true` (sobrescrito) | Ataque instantâneo, sustain em 1 enquanto o buffer faz loop (nunca chama `release()` por conta própria); só sai do loop quando chega o note-off, com release de 100ms. Funciona como instrumento sustentado, não one-shot. |

O seletor (obedece ou ignora note-off) é política da **voz**, não do envelope
— no Bela também é: quem consulta a flag é `noteOff()` em `render.cpp`, não o
`ADSR`. Sem ele a percussão do Agbau se comporta como baixo, ou vice-versa.

Dois detalhes do Bela que o porte muda de propósito, documentados no header:
o fim de cada segmento crava o valor alvo (no Bela o sustain fica em
1 ± erro de arredondamento acumulado em 441 incrementos), e um segmento de
tempo zero salta direto para o alvo (no Bela, `rampTo(x, 0)` não se move).
Nenhum dos três presets cai no segundo caso. E um que o porte mantém:
`trigger()` rampa do nível **atual** até 1, não de zero — o `rampTo(0.0,
0.0001)` em `ADSR.cpp:42` é sobrescrito na linha 46 antes de ter efeito.

### 2. Dezesseis vozes

A estrutura voice-outer de `audioUpdate()` já soma vozes concorrentes; o
trabalho é ter 16 instâncias de voz (`src/voice.h`: takes + ponteiro de leitura
+ envelope) em vez de 4, cada uma com:
- lista de takes candidatos e escolha aleatória a cada disparo (como
  `p1.3.1.wav` … `p1.3.5.wav`);
- flag de loop-enquanto-pressionado para as duas vozes de ganzá;
- flag de "release no note-off" independente por voz;
- ganho 0,5, como `Sampler.cpp:113` — o Bela somava as 16 vozes a meio ganho e
  não tinha limitador.

**O disparo é um pedido, não uma escrita.** No sampler atual, `loop()` chama
`samp.start()` direto e a corrida com a task de áudio custa no máximo um
bloco, porque `start()` não troca de buffer. No Agbau um disparo troca de take
— ponteiro *e* comprimento —, e uma leitura rasgada entre os dois lê além do
fim de um buffer menor. Então `noteOn()`/`noteOff()` só registram o pedido, e a
task de áudio o aplica na borda do bloco: o mesmo padrão de
`PitchShift::setEngaged()`, pelo mesmo motivo.

O porte é **fiel primeiro**: retrigger volta a leitura a 0 com um take novo e
o envelope subindo do nível em que estava (`Sampler.cpp:37-39`). Isso pode
estalar num rulo, porque a posição salta com o ganho alto. O Agbau já é tocado
ao vivo assim; a decisão de corrigir fica para depois de ouvir na placa.
Uma diferença que não muda o som: a voz desliga quando o envelope chega ao
fim. O Bela deixava um ganzá solto em loop a ganho zero para sempre.

O mapa botão → voz vem do `.ino`: `Keypad` calcula `kcode = linha·4 + coluna`
e `notaMidi[kcode] = 60 + 4·coluna + linha`; no Bela, notas 60–67 são
`b1`–`b8` e 68–75 são `p1`–`p8`. Ou seja: colunas 0–1 são os baixos,
colunas 2–3 a percussão, e os ganzás (p3, p7) caem na coluna 2 linha 2 e
coluna 3 linha 2. `src/agbau_kit.h` carrega essa tabela, testada contra a
transcrição do `.ino`.

### 3. Varredura de matriz 4×4

O `TriggerInput` já faz a parte que importa: ler o pino bruto, confirmar por
tempo, rearmar por nível contínuo, sem debounce de biblioteca. Uma matriz
adiciona só o laço de estrobar coluna a coluna e ler linhas — os diodos já
existem na placa do Agbau para evitar fantasmas (`Circuito/Agbaixo_BOM.csv`:
16 diodos). Cada célula continua precisando de uma instância de
`TriggerInput`: dezesseis, não uma.

**A direção da varredura não é livre — os diodos a fixam.** O `Keypad`
vendorizado (`Arduino/libraries/Keypad/src/Keypad.cpp:83-100`) puxa uma
**coluna** para LOW (Pro Micro 2, 3, 4, 5), lê as quatro **linhas** com pull-up
(9, 8, 7, 6) e devolve a coluna a alta impedância (`INPUT`, não `HIGH` — duas
colunas acionadas ao mesmo tempo por teclas pressionadas não podem brigar).
Invertida, a varredura encontra os diodos ao contrário e não lê nada. Os nomes
no `.ino` confundem: `pinosColunas` tem tamanho `LINHAS` e vice-versa, mas é
`pinosLinhas` que vai no argumento `row` do construtor.

Duas consequências de ler por matriz em vez de por pino:
- **O filtro RC de [circuit-rc.md](circuit-rc.md) não se aplica.** Um capacitor
  por linha seria compartilhado por quatro teclas e atrasaria cada coluna da
  varredura. A janela de confirmação do `TriggerInput` passa a ser a única
  defesa contra disparo fantasma — já era a única que cobria glitch de
  contato.
- **Cada célula é amostrada uma vez por varredura**, não a cada passada de
  `loop()`. O período de varredura precisa ficar bem abaixo dos 200 µs de
  `TRIG_CONFIRM_US` (ao menos quatro amostras por janela), ou a confirmação
  vira função da varredura e não do botão.

### 4. Mapa de pinos

O firmware atual ocupa: SD em HSPI (10/11/12/13), I2S (16/17/18), disparo
(4/5/6), FX (7), probe ADC (1). Os cinco últimos são do sampler e ficam livres
para o Agbau. Excluindo PSRAM octal (33–37), flash (26–32), strapping
(0/3/45/46), USB nativo (19/20) e UART0 (43/44), sobram **18 GPIOs**:
1, 2, 4–9, 14, 15, 21, 38–42, 47, 48. O Agbau precisa de 12 (matriz 8 + modo
+ pedal + 2 LEDs) — com folga, não "por pouco" como dizia a primeira versão.
Na opção A da etapa 0, o SD sai e libera mais 4.

Um cuidado: o DevKitC-1 liga o LED RGB endereçável no GPIO48 ou no GPIO38,
conforme a revisão da placa. Evitar os dois até conferir na placa real.

O mapa explícito, com a correspondência pino a pino do Pro Micro, fica em
[agbau-circuit.md](agbau-circuit.md).

### 5. Firmware integrado

Um sketch novo, `src/agbau_main.cpp`, num env `agbau` que compartilha os
headers de `src/` — o Agbau é outro conjunto de vozes e outro conjunto de
botões sobre o mesmo motor. O override do ring de DMA e seus `static_assert`
saem de `main.cpp` para um header incluído antes de `M16.h` pelos dois
sketches, para que exista uma definição só.

O teste de bancada mais pesado — as 16 teclas de uma vez — é também a
revalidação que o ring de 5,8 ms deve desde a troca de pin do M16 (ver
CLAUDE.md), numa carga quatro vezes maior que a do teste original.

O Bela não tinha limitador; aqui a soma passa pelo joelho suave. Com muita
polifonia os picos saem mais macios do que saíam no Bela. É uma diferença
intencional, e audível só onde o Bela estaria clipando.

### 6. Repeat (tap-tempo) e looper

O `.ino` do Agbau tem ~465 linhas incluindo tap-tempo e um looper de MIDI com
overdub (3 camadas, até 64 mensagens por camada, LED de status). O README do
Agbau documenta bugs no overdub (contagem de botões inconsistente entre
camadas, note-off disparando cedo demais), e a leitura do código achou mais
quatro:

- `float interval = (1000 / bpm) * 60;` (`agbau.ino:159`) divide inteiros: a
  120 bpm dá 480 ms em vez de 500, 4% rápido.
- Dois toques com mais de 60 s entre eles fazem `bpmBotao` (`agbau.ino:144`)
  truncar para 0, e a linha 159 divide por zero. No AVR isso dá um valor
  qualquer; no Xtensa do ESP32, é um panic.
- `startOverdub()` deixa `currentLayer` chegar a 3 (`agbau.ino:394-395`) e
  escreve `NloopMsgs[3]` num array de 3 posições.
- Durante o overdub, `playLoop()` reproduz pelo mesmo `noteOn()` que grava
  quando `isRecording` está ligado (`agbau.ino:220`, `424`): a reprodução se
  regrava na camada nova. É a causa provável da contagem inconsistente.

Portar isso é reescrever uma máquina de estado de sequenciamento, não só
ligar um botão a uma voz — e reescrever **a partir de uma especificação**
(`docs/agbau-looper.md`, aprovada antes de qualquer código), não do `.ino`. Sem
MIDI, o tempo pode ser contado em frames de áudio em vez de `millis()`: os
eventos do loop são disparados pela task de áudio na borda de bloco, sem
deriva contra o som. Vale fazer **depois** que o resto já produz som.

### 7. Placa física

`Circuito/Agbaixo_PCB.kicad_pcb` é um carrier para o Arduino Pro Micro: dois
conectores de 12 pinos no footprint do Pro Micro, confirmado pelo BOM. Sem o
Arduino essa placa não serve como está. Duas saídas: fiação solta de um dev
board ESP32-S3 nesses mesmos headers (suficiente para bancada/prototipagem),
ou uma revisão de PCB nova desenhada para o módulo do S3. Não bloqueia nada
do firmware — só precisa existir antes de o instrumento sair da bancada.

## Fora de escopo aqui

`audioUpdate()` sem alocação, samples residentes antes do primeiro som, nada
lido de armazenamento lento durante a execução — nenhuma dessas decisões muda.
Na opção A da etapa 0 a residência é na flash mapeada em vez da PSRAM, e
continua valendo pelo mesmo motivo: nada escreve na flash em tempo de
execução, então o cache nunca é desligado sob a task de áudio. O Agbau é outro
conjunto de vozes e outro conjunto de botões sobre o mesmo motor, não um motor
novo.

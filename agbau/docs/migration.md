# Como se aproximar do Agbau: Bela e Arduino saem, um ESP32-S3 fica

O [Agbau](https://www.instagram.com/bongarbit/) é outro instrumento do
Bongarbit: 16 botões (matriz 4×4) disparam até 16 vozes polifônicas com
envelope ADSR. Hoje elas estão divididas entre um Arduino Pro Micro e uma
Bela. O Arduino lê os botões, roda o tap-tempo e um looper com overdub e fala
MIDI USB. A Bela recebe esse MIDI e toca os samples. A decisão tomada em
conversa foi: sem Arduino, sem MIDI. Um único ESP32-S3 lê os botões e toca o
áudio.

Este documento registra o que já foi feito de cada etapa, onde está e por que
foi feito assim, e diz o que falta. É um documento vivo. A primeira versão era
só o plano, conferido contra a fonte do Agbau (`agbau/Bela/agbau-sampler-poly/`,
`agbau/Arduino/agbau/agbau.ino`, as 24 WAVs e o `Keypad.cpp` vendorizado) em
11/10/2026, e cinco premissas dela estavam erradas. Onde o erro original é
plausível e poderia voltar, a seção diz qual era.

O que foi ouvido na placa está em
[shared/docs/bancada.md](../../shared/docs/bancada.md), seções 13 a 15. Neste
documento, "seção N" sem outro nome é uma seção da bancada.

## Por que a base daqui serve

Nada no Agbau processa áudio de entrada. Ele dispara samples, aplica um
envelope de amplitude e soma, sem filtro, reverb, pitch ou FFT (`agbau/Bela/
agbau-sampler-poly/render.cpp:215-226`, `Sampler.cpp:108-126`). É uma carga de
DSP muito menor que a do pitch shifter do sampler. A parte de maior risco
técnico já estava provada no sampler antes de o porte começar: um motor em
bloco, em float, sem alocação, com as samples residentes antes do primeiro
som, aguentando tempo real no S3. O porte foi engenharia de funcionalidade,
não pesquisa de arquitetura.

## Situação em 11/10/2026

| # | Etapa | Situação | Onde |
|---|---|---|---|
| 0 | Onde as samples moram | **Feita.** Flash mapeada (opção A), medida e ouvida (seção 13) | `agbau/tools/agbau_kit.py`, `agbau/src/kit_image.h` |
| 1 | Envelope ADSR | **Feita.** Testada no host contra a cópia do Bela, ouvida nos três presets (seção 14) | `agbau/src/envelope.h` |
| 2 | Vozes e kit | **Feita.** Testada no host contra o `Sampler` do Bela e o mapa de notas do `.ino`, ouvida (seção 14) | `agbau/src/voice.h`, `agbau/src/agbau_kit.h` |
| 3 | Matriz 4×4 | Testada no host. Na placa, só como `KeyMatrix<4,1>` (botões diretos, sem varredura de colunas) | `agbau/src/key_matrix.h` |
| 4 | Mapa de pinos | No papel, lido da placa de circuito. Três pontos a conferir no instrumento | [circuit.md](circuit.md) |
| 5 | Firmware integrado | **Em andamento.** Três vozes ouvidas, botão de modo e pedal conferidos no log (seção 15). Falta a matriz | `agbau/src/main.cpp` |
| 6 | Repeat e looper | Spec aprovada, implementada e testada no host. **Ainda não tocada na placa** | [looper.md](looper.md), `tap_tempo.h`, `looper.h`, `performer.h` |
| 7 | Placa física | Não começada | — |

A ordem das etapas mudou uma vez. A primeira versão do plano punha o
orçamento de memória e o mapa de pinos *depois* do código que dependia deles,
e por isso eles vieram para a frente (etapas 0 e 4).

## O que falta, na ordem em que faz sentido

1. **Tocar o repeat e o looper na placa.** O código existe e passa nos testes
   do host. Ninguém ouviu ainda. A bancada atual (três vozes, modo e pedal)
   cobre todo o comportamento; o roteiro está em [looper.md](looper.md),
   seção "Na bancada atual".
2. **Conferir o instrumento antes de fiar.** Três coisas que só o instrumento
   confirma (detalhes na etapa 4):
   - onde está o botão de modo: o `.ino` usa o pino D15, mas a placa liga o
     conector livre (J6) no A0;
   - se os headers J24–J39 estão vazios. Eles ligam cada coluna direto ao GND,
     então qualquer coisa plugada ali prende uma coluna e a matriz inteira lê
     errado;
   - qual GPIO o LED RGB do DevKitC-1 ocupa (38 ou 48, conforme a revisão da
     placa).
3. **Matriz 4×4 na placa.** Fiar as colunas nos GPIO 4–7 e as linhas nos
   GPIO 1, 2, 42 e 41. No firmware, trocar a `KeyMatrix<4,1>` de botões diretos
   pela `KeyMatrix<4,4>` com IO de verdade: uma coluna em LOW por vez, as
   outras em alta impedância. O botão de modo sai da matriz e vai para o
   GPIO40, como um `TriggerInput` próprio, igual ao pedal. Na bancada,
   conferir três coisas: que não há tecla fantasma, que o settle de 10 µs
   basta na fiação real, e que as 16 teclas tocam as 16 vozes certas.
4. **Carga máxima pelo caminho completo.** O motor já aguentou 64 vozes na
   escada da seção 13, mas falta tocar muitas teclas juntas passando pela
   matriz e pelo looper. Se a Bela estiver disponível, fazer um A/B de ouvido
   com ela.
5. **LEDs.** O firmware já escreve nos pinos finais (GPIO15 modo, GPIO8
   looper). Falta ligá-los e ver.
6. **Placa física** (etapa 7).

## As etapas

### 0. Onde as samples moram: a flash mapeada

**Feito:** o kit inteiro mora numa partição de dados da flash,
`samples`, de 12,9 MB (`agbau/partitions.csv`, ao lado de um app de 3 MB). O
firmware a mapeia na memória com `esp_partition_mmap()` e lê as samples ali
mesmo, sem copiar. `agbau/tools/agbau_kit.py` gera a imagem, que é gravada uma
vez com esptool e não é tocada pelo upload do firmware. `agbau/src/kit_image.h`
interpreta a imagem sem confiar em nada dela, porque seus ponteiros são lidos
pela task de áudio: cada offset é conferido em 64 bits, e uma gravação parcial
é pega pelo CRC no boot. A escolha foi feita depois do spike
`agbau/bench/agbau_storage.cpp` (env `agbau_storage`, seção 13): o mapeamento
de ~9 MB cabe ao lado da PSRAM, 16 vozes usam 17% do bloco, e a escada de 1 a
64 vozes não estalou.

**Por que a PSRAM não serviu.** As 24 samples são float32 mono 44,1 kHz (WAV
formato 3), 17,99 MB em disco. Em int16 caem para 8,995 MB, e o N16R8 tem
8,389 MB de PSRAM (8 MiB), antes de qualquer outra alocação. Os dois loops de
ganzá (`p3.1.wav`, `p7.1.wav`, ~20 s cada) são 40% do kit. Com cortes sem
perda, o kit cairia para 8,286 MB e caberia por ~103 KB, o que não é margem:
- nas percussões, o decay de 3 s chega a zero em A+D = 3,01 s, e no porte todo
  frame depois disso é multiplicado por 0 exato;
- caudas abaixo de −60 dBFS podem sair, com 10 ms de fade em cada corte.

`agbau_kit.py --trim-envelope --trim-tail-db -60` reproduz esses números. A
flash leva o kit inteiro, sem corte.

**A alternativa descartada**, SD → PSRAM como no sampler (opção B), não
chegou a ser medida: o cartão não tinha a pasta `/agbau/`. Ela exigiria cortar
os loops de ganzá, e o ponto do corte é uma decisão musical. Também traria de
volta dois comportamentos do `Wav::load()` da M16 que a flash evita: o fade de
10 ms nas pontas de toda sample, que num ganzá em loop vira um buraco de ~20 ms
a cada volta; e o truncamento silencioso quando a PSRAM livre não basta. O
spike contorna os dois (`SPIKE_RESTORE_EDGES` e um bloco único com
`setExternalBuffer()`), e é ali que está o código, se a opção B voltar. O
corte de cauda tem ainda um custo que não aparece no nível do sinal: muda o
frame em que a take acaba, que é onde o envelope congela (etapa 2). Em
`p4.3.1`, o próximo toque passaria a atacar a partir de 0,83 em vez de 0,57.

A conversão float32 → int16 acontece uma vez, no PC, em `agbau_kit.py`, com o
mesmo arredondamento de `audioToI16()`. A M16 também converte ao carregar, mas
truncando com ×32767.

### 1. Envelope ADSR

**Feito:** `agbau/src/envelope.h`, um ADSR linear com um ganho por frame. É um
porte do `ADSR`/`Ramp` do Bela, conferido frame a frame contra uma cópia literal
deles (`test/agbau/bela_reference.h`, `test_envelope`) nos três presets. Ouvido
na placa nos três (seção 14).

**Não é um processador `process(AudioBlock&)` da cadeia.** A primeira versão
deste documento dizia que era, e não pode ser. São 16 envelopes
independentes, cada um aplicado ao sinal mono de *uma* voz antes da soma, e um
bloco já somado não tem mais o que envelopar. Pelo mesmo motivo, o "bypass
bit-exato" dos processadores da cadeia não é o contrato certo. Os que valem,
e que os testes fixam:
- o resultado não depende do tamanho do bloco
  (`test_block_size_does_not_change_the_output`);
- no sustain 1,0, o ganho é exatamente `1.0f`, então um baixo segurado sai
  bit-exato em relação à sample;
- concordância com a cópia do Bela, que tem o mesmo papel que `mixer.h` tem
  para o joelho suave: é o som que já foi aprovado tocando.

**É uma classe com dois usos opostos**, selecionados pela flag
`releaseOnNoteOff` (`render.cpp:84-104`):

| Grupo | ADSR (A, D, S, R) | `releaseOnNoteOff` | Comportamento |
|---|---|---|---|
| Baixos (b1–b8) | `0.01, 0.0, 1.0, 1.0` | `true` | Sustenta enquanto a nota estiver ligada; o note-off leva ao release de 1 s |
| Percussão (p1–p8, menos os ganzás) | `0.01, 3.0, 0.0, 3.0` | **`false`** | O decay de 3 s *é* o som inteiro. O note-off é **ignorado**: um toque rápido decai sozinho até o fim |
| Ganzás (p3, p7) | `0.01, 0.0, 1.0, 0.1` + loop | `true` | Faz loop enquanto a tecla está apertada; o note-off leva ao release de 100 ms |

Quem consulta a flag é a voz, não o envelope. No Bela também é assim: quem a
lê é o `noteOff()` do `render.cpp`, não o `ADSR`.

**Onde o porte difere do Bela, de propósito**, como documentado no header:
- cada segmento termina exatamente no alvo. No Bela, o erro acumulado deixa
  o sustain "1,0" em 1,0000049, o release "a 0" em +0,0005 e o decay da
  percussão em −0,00108 (−59 dB);
- um segmento de tempo zero salta direto para o alvo. No Bela,
  `rampTo(x, 0)` não se move. Nenhum dos três presets cai nesse caso.

E um ponto em que o porte é fiel: `trigger()` rampa do nível **atual** até 1,
não de zero. O `rampTo(0.0, 0.0001)` em `ADSR.cpp:42` é sobrescrito na linha
46 antes de ter efeito.

### 2. Dezesseis vozes e o kit

**Feito:** `agbau/src/voice.h` (`Voice`, takes + posição de leitura +
envelope, porte do `Sampler` do Bela; e `VoiceBank<16>`) e
`agbau/src/agbau_kit.h` (as 16 vozes, os três presets e o mapa tecla → voz).
Testado contra a cópia do `Sampler` e contra uma transcrição do `.ino`
(`test_voice`). Ouvido na placa: takes sorteadas, release de cada preset,
retrigger sem estalo (seção 14).

Cada voz tem a sua lista de takes, sorteada a cada disparo (`p1.3.1` …
`p1.3.5`), o loop dos ganzás, a sua política de note-off e ganho 0,5, como
`Sampler.cpp:113`. O Bela somava as 16 vozes a meio ganho e sem limitador.
Aqui a soma passa pelo joelho suave de `audio_block.h`, então com muita
polifonia os picos saem mais macios que no Bela. É uma diferença intencional,
e só é audível onde o Bela estaria clipando.

**Só a task de áudio toca numa voz.** No sampler, `loop()` chama
`samp.start()` direto, e a corrida com a task de áudio custa no máximo um
bloco, porque `start()` não troca de buffer. No Agbau, um disparo troca de take
(ponteiro *e* comprimento), e uma leitura rasgada entre os dois lê além do fim
de um buffer menor. Por isso `loop()` nunca escreve numa voz. No firmware
atual, ele manda bordas de tecla ao `Performer` (etapa 6), que roda na task de
áudio e liga e desliga as vozes ali, no topo do bloco. O `VoiceBank` ainda tem
a sua própria fila (`NoteQueue`), usada pelo spike.

**O porte é fiel ao Bela**, inclusive nas pegadinhas:
- o retrigger volta a leitura a 0 com uma take nova, e o envelope sobe do
  nível em que estava (`Sampler.cpp:37-39`);
- quando uma take one-shot acaba antes do envelope, o Bela chama `release()`
  e **congela** o envelope ali, porque `Sampler::process()` retorna antes de
  avançá-lo (`Sampler.cpp:109-111`). As percussões curtas acabam antes dos 3 s
  do decay, então o próximo toque ataca a partir de 0,44–0,57 (p1, p4, p8), e
  não de zero. O porte reproduz isso por construção: voz que não toca não é
  renderizada, e o envelope dela não anda.

O plano temia que isso estalasse num rulo, porque a posição de leitura salta
com o ganho alto. Foi ouvido na placa e **não estala** (seção 14), e fica como
está.

**Uma diferença que não muda o som:** a voz para de ser renderizada quando o
envelope chega ao fim. O Bela deixava um ganzá solto em loop a ganho zero para
sempre. Isso só funciona porque o release do porte termina em 0,0 exato. Com o
erro acumulado do Bela, ele terminaria em 1e-9, `silent()` nunca dispararia, e
o ganzá ficaria em loop do mesmo jeito (`test_every_release_ends_on_exact_zero`).

**O mapa tecla → voz vem do `.ino`.** O `Keypad` calcula
`kcode = linha·4 + coluna`, e o `.ino` manda a nota
`notaMidi[kcode] = 60 + 4·coluna + linha`. No Bela, as notas 60–67 são
`b1`–`b8` e as 68–75 são `p1`–`p8`. Ou seja, as colunas 0–1 são os baixos, as
colunas 2–3 a percussão, e os ganzás (p3, p7) ficam na linha 2 das colunas 2 e
3. `agbauVoiceForKey()` carrega essa tabela.

### 3. Varredura da matriz 4×4

**Feito:** `agbau/src/key_matrix.h`, testada no host contra uma matriz simulada
(`test_key_matrix`): com diodos, sem diodos e com linhas lentas. Na placa, por
enquanto, só como `KeyMatrix<4,1>`: quatro botões diretos ao GND são uma
matriz de uma coluna, e essa coluna é o terra. Assim os botões da bancada já
passam pelo mesmo código que a matriz vai usar.

Cada tecla é um `TriggerInput`, a mesma máquina de estado de todos os botões
do sampler. **Não há debounce novo**: a matriz só muda como o nível bruto de
cada tecla é lido.

**A direção da varredura não é livre: os diodos a fixam.** O `Keypad`
vendorizado (`Keypad.cpp:83-100`) puxa uma **coluna** para LOW, lê as quatro
**linhas** com pull-up e devolve a coluna a alta impedância (`INPUT`, não
`HIGH`, para que duas colunas acionadas por teclas apertadas não briguem). O
PCB confirma: cada tecla fica entre a sua linha e o anodo do seu diodo, e o
catodo vai para a coluna. Varrida ao contrário, a matriz não lê nada
(`test_scanning_the_wrong_way_reads_nothing`). Os nomes no `.ino` confundem:
`pinosColunas` tem tamanho `LINHAS` e vice-versa, mas é `pinosLinhas` que vai
no argumento `row` do construtor.

Duas consequências de ler por matriz em vez de por pino:
- **O filtro RC de
  [sampler/docs/circuit-rc.md](../../sampler/docs/circuit-rc.md) não se
  aplica.** Um capacitor por linha seria compartilhado por quatro teclas e
  atrasaria cada coluna. A janela de confirmação do `TriggerInput` é a única
  defesa contra disparo fantasma.
- **Cada tecla é amostrada uma vez por varredura**, e o período da varredura
  passa a ser o período de amostragem de cada tecla. Ele precisa caber pelo
  menos quatro vezes nos 200 µs de `TRIG_CONFIRM_US`, e um `static_assert`
  garante isso. O settle de 10 µs depois de cada coluna é o que cobre a
  subida lenta de uma linha pelo pull-up. Curto demais, uma tecla segurada
  aparece na coluna seguinte (`test_short_settle_ghosts_into_the_next_column`).
  Os 10 µs só foram validados no modelo. A fiação real é o passo 3 de "O que
  falta".

### 4. Mapa de pinos

**Feito, no papel:** [circuit.md](circuit.md), lido das nets do PCB do Agbau
(`Agbaixo_PCB.kicad_pcb`), não do esquemático, e conferido contra o `.ino`:

| Função | GPIO |
|---|---|
| Colunas 0–3 | 4, 5, 6, 7 |
| Linhas 0–3 | 1, 2, 42, 41 |
| Botão de modo | 40 |
| Pedal do looper | 39 |
| LEDs (modo, looper) | 15, 8 |
| I2S do DAC | 16, 17, 18 (como no sampler) |

Sobram GPIOs com folga. O Agbau usa 12 para os botões e LEDs, e não usa SD,
porque o kit está na flash. Ficam fora de propósito: a PSRAM octal (33–37), a
flash (26–32), os pinos de strapping (0/3/45/46), o USB nativo (19/20), a
UART0 (43/44), e o 38 e o 48, porque um dos dois leva o LED RGB do DevKitC-1,
conforme a revisão da placa.

O PCB mostrou três coisas que o `.ino` não explica, e que precisam ser
conferidas no instrumento antes de fiar (passo 2 de "O que falta"):
- o botão de modo do `.ino` (pino 15) não está ligado na placa, e o único
  conector de botão livre (J6) vai no A0;
- dezesseis headers (J24–J39) ligam cada coluna ao GND. Se algo estiver
  plugado neles, a matriz lê errado;
- J4 é uma saída MIDI DIN que o firmware nunca usou, e que deixa de existir.

A bancada de hoje usa dois pinos fora desse mapa: as três vozes e o botão de
modo nos GPIO 4–7, que no mapa final são as colunas. O pedal já está no pino
final (39), e os LEDs também (15 e 8).

### 5. Firmware integrado

**Feito até aqui:** `agbau/src/main.cpp`, env `agbau`. Ele compartilha
`shared/src/` com o sampler: o Agbau é outro conjunto de vozes e de botões
sobre o mesmo motor. O ring de DMA de 5,8 ms está em
`shared/src/engine_config.h` e entra pelo `audio_block.h`, então vale para
todo motor em bloco com uma definição só, e o build falha se o `M16.h` vier
antes dele. O ring foi revalidado com o pin atual da M16 na escada de 1 a 64
vozes, sem clique (seção 13).

Na placa, o firmware cresceu em passos pequenos:
1. **Quatro botões, quatro vozes** (seção 14), para ouvir os disparos antes de
   decidir qualquer coisa sobre a posição das teclas.
2. **Três vozes, botão de modo e pedal** (seção 15). Como a bancada tem quatro
   botões, os GPIO 4–6 tocam b1, p1 e p3 (um de cada comportamento), e o GPIO7
   virou o botão de modo, ainda na mesma `KeyMatrix<4,1>`. Um quinto botão, no
   GPIO39, é o pedal, já no pino do mapa final, com um `TriggerInput` próprio.
   Os dois passaram primeiro só pelo log. Os tempos batidos à mão nessa rodada
   entraram nas decisões do tap-tempo.
3. **Repeat e looper** (etapa 6): o botão de modo e o pedal agora comandam os
   dois. Compilado e testado no host, **ainda não tocado na placa**.

Falta a matriz (passo 3 de "O que falta").

### 6. Repeat (tap-tempo) e looper

**Feito:** a spec [looper.md](looper.md), aprovada antes de qualquer código, e
a implementação:
- `agbau/src/tap_tempo.h`: o tap-tempo e o repeat;
- `agbau/src/looper.h`: o looper, com a base e até três overdubs;
- `agbau/src/performer.h`: o que liga as teclas aos dois e às vozes.

Tudo testado no host (`test_performer`). **Ainda não tocado na placa.**

A spec reescreve o comportamento do `.ino` a partir de uma regra: **o design
e a jogabilidade são os do Agbau como foi escrito**, e sai só o que é bug.
Por isso, coisas que parecem candidatas a "melhoria" ficaram:
- o tap não tem faixa e não faz média;
- o repeat soa na hora, mas um novo aperto dentro de uma subdivisão espera a
  grade que já corria;
- o loop tem comprimento livre;
- o overdub fecha quando o loop volta ao começo.

A leitura do `.ino` achou **dez bugs**, todos corrigidos e cada um com um teste
que leva o nome dele. A lista completa está no [looper.md](looper.md). Entre
eles:
- o tempo em degraus de 60 ms;
- a divisão por zero após uma pausa longa;
- a quarta camada escrevendo fora do array;
- o overdub regravando a própria reprodução;
- três jeitos de deixar um ganzá tocando para sempre.

Os dois bugs que o README do Agbau cita são o 5 (contagem de botões
inconsistente) e o `TODO` da linha 179 (note-off logo antes do note-on
seguinte). O segundo, a spec mantém: na prática é o recomeço do ganzá a cada
batida do repeat.

**O que muda em relação ao Arduino é o relógio.** Sem MIDI, o tempo é contado
em frames de áudio pela task de áudio, e não em `millis()`. O `Performer` roda
no topo de cada bloco: recebe as bordas de tecla que o `loop()` mandou e
decide o que soa. As batidas do repeat e os eventos do loop caem na borda do
bloco (no máximo 0,73 ms de atraso, sem acumular). Assim nada deriva contra o
som: em uma hora de repeat ou em cem voltas de loop, cada evento cai no frame
exato (`test_repeat_grid_does_not_drift`, `test_loop_does_not_drift`).

### 7. Placa física

**Não começada.** `Circuito/Agbaixo_PCB.kicad_pcb` é um carrier para o Arduino
Pro Micro: dois conectores de 12 pinos no footprint do Pro Micro, confirmado
pelo BOM. Sem o Arduino, essa placa não serve como está. No curto prazo, a
saída é fiar um DevKitC-1 do ESP32-S3 nos headers J1/J2 dela, mais o
PCM5102A, seguindo [circuit.md](circuit.md). No longo prazo, uma revisão da
placa no KiCad desenhada para o módulo do S3. Não bloqueia nada do firmware:
só precisa existir antes de o instrumento sair da bancada.

## Fora de escopo aqui

`audioUpdate()` sem alocação, samples residentes antes do primeiro som, nada
lido de armazenamento lento durante a execução: nenhuma dessas decisões muda.
No Agbau a residência é na flash mapeada em vez da PSRAM, e continua valendo
pelo mesmo motivo: nada escreve na flash em tempo de execução, então o cache
nunca é desligado sob a task de áudio. O Agbau é outro conjunto de vozes e de
botões sobre o mesmo motor, não um motor novo.

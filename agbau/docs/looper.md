# Repeat e looper: especificação

**Aprovada em 11/10/2026.** Etapa 6 de [migration.md](migration.md).
Implementada em `agbau/src/tap_tempo.h` (tap-tempo e repeat),
`agbau/src/looper.h` e `agbau/src/performer.h` (o que liga as teclas aos dois
e às vozes), testada no host (`test/agbau/test_performer`) e ouvida na bancada
de três vozes, modo e pedal ([shared/docs/bancada.md](../../shared/docs/bancada.md),
seção 16). Mudar o comportamento é mudar este documento primeiro.

A implementação precisou fixar quatro detalhes que o texto aprovado deixava
em aberto. Eles estão marcados com *(implementação)* onde aparecem: o
primeiro toque depois do boot, onde ficam os note-offs de fechamento, o
comprimento máximo de um loop e a posição em que uma batida do repeat é
gravada.

**A regra que decide as dúvidas** (11/10/2026): o design e a jogabilidade
são os do Agbau como foi escrito. O que o `.ino` faz de propósito, ou que
quem toca o Agbau já tem nas mãos, fica. Sai só o que é bug: o que trava o
instrumento, escreve fora da memória ou deixa uma voz presa.

A fonte é o `agbau.ino` (`agbau/Arduino/agbau/agbau.ino`, no repositório do
Agbau), lido em 11/10/2026. Esta especificação descreve o comportamento
*pretendido*, não o código. Onde o `.ino` faz algo que parece acidental, a
especificação diz qual dos dois é, e por quê. Os bugs conhecidos estão na
última seção, cada um com a regra que o corrige.

## O que o `.ino` faz hoje

Para ter a referência por perto, sem precisar abrir o `.ino`:

- **Botão de modo** (pino 15 no `.ino`). Cada toque mede o intervalo desde o
  toque anterior, transforma em bpm e liga o modo repeat. Segurar por mais de
  1 s volta ao modo nota.
- **No repeat, o baixo não soa.** O último baixo tocado (1–8) escolhe a
  divisão do compasso: 2, 3, 4, 6, 8, 12, 16 ou 32 por compasso de 4 tempos.
  O padrão é o baixo 3, ou seja, 4 por compasso (um por tempo).
- **No repeat, a percussão não soa ao ser apertada.** Enquanto houver tecla de
  percussão apertada, a última apertada se repete no intervalo da divisão. Ao
  soltar todas, sai um note-off.
- **LED de modo:** pisca no tempo enquanto o repeat está ligado.
- **Pedal do looper:** o primeiro toque grava, o segundo fecha o loop e começa
  a tocar, o terceiro abre um overdub, o quarto fecha o overdub, e assim por
  diante. Segurar por mais de 1 s enquanto toca apaga tudo.
- **O que o looper grava:** tudo o que passa por `noteOn()`/`noteOff()`,
  inclusive o que o repeat gera. São no máximo 64 mensagens por camada.
- **LED do looper:** apagado quando vazio, aceso gravando, piscando duas vezes
  por volta quando toca.

## Princípios

1. **O tempo é contado em frames de áudio, pela task de áudio.** Não há
   `millis()` nem `micros()` no repeat nem no looper. Um contador de frames
   avança `AUDIO_BLOCK_FRAMES` a cada bloco. O tempo do repeat e o loop são
   posições nesse contador, de modo que não derivam contra o som: o relógio
   *é* o som.
2. **Os eventos caem na borda do bloco.** Um evento agendado para o frame `t`
   é aplicado no início do primeiro bloco que começa em `t` ou depois. O erro
   é de no máximo 32 frames (0,73 ms) e não acumula, porque o próximo evento
   é calculado a partir da origem, não do evento anterior. A `Voice` não tem
   como começar no meio de um bloco, e 0,73 ms está abaixo do jitter do
   `loop()` do `.ino`.
3. **`loop()` só reporta botões.** Ele passa a postar *bordas de tecla*
   (tecla, apertou/soltou), e não eventos de voz. Quem decide o que soa é um
   objeto que roda na task de áudio, no topo de cada bloco: o `Performer`.
   Ele consome as bordas, avança o repeat e o looper e
   entrega note-on/note-off ao `VoiceBank` na mesma task. A fila continua
   sendo o único caminho de `loop()` para o áudio. O que muda é o que ela
   carrega.
4. **Uma tecla é medida no bloco em que é aplicada.** O instante de um toque
   (para o tap-tempo e para a gravação) é o frame do bloco que o consome. O
   atraso entre `loop()` e esse bloco é de no máximo um bloco, e vale igual
   para todos os toques. Um tap de 500 ms erra no máximo 0,15%.
5. **Nada aloca nem bloqueia.** As camadas são arrays fixos, dimensionados em
   tempo de compilação.
6. **Não presumir o tamanho do bloco.** O `Performer` recebe o número de
   frames do bloco. O teste equivalente ao `test_block_size_does_not_change_the_output`
   exige que, em blocos irregulares, cada evento caia no mesmo frame-alvo
   (seção "Testes").

## Tap-tempo

**Período em frames inteiros.** O tempo guardado é o período de um tempo
(semínima) em frames: `P = intervalo entre dois toques`, medido em frames. A
120 bpm, `P = 22050` exatos. Não há divisão inteira de milissegundos (bug 1)
nem bpm intermediário.

**Sem faixa: todo intervalo vira o tempo**, como no `.ino`. Um toque solto
depois de uma pausa longa deixa o tempo lento. É o que acontece hoje, e quem
toca já conta com isso. Há uma única exceção, onde o `.ino` quebra: um
intervalo de 60 s ou mais (no `.ino`, `bpm = 0` e divisão por zero, bug 2). Esse
toque não muda o tempo. Ele conta como "primeiro toque", e o próximo mede a
partir dele.

O lado curto também não tem limite. Um toque duplo rápido dá um tempo muito
rápido, como no `.ino`. O que impede o quique de contar como toque é o
`TriggerInput`, que já faz isso para todas as teclas. Um repeat mais rápido
que um bloco toca no máximo uma vez por bloco, assim como o `.ino` tocava no
máximo uma vez por volta do `loop()`.

**Tempo inicial:** 120 bpm (`P = 22050`), o valor do `.ino`.

**O primeiro toque depois do boot** não muda o tempo, só começa a contagem
*(implementação)*. O `.ino` mede esse toque a partir do boot
(`tempoBotaoAnterior = 0`), o que dá um tempo que ninguém bateu.

**Média:** nenhuma. Vale o último intervalo, como no `.ino`.

**Cada toque realinha a grade.** A origem do tempo passa a ser o frame do
toque, como o `previousMillis = millis()` do `.ino`. Bater o tempo é também
dizer onde cai o "um".

**Segurar ≥ 1 s sai do repeat.** O toque que começa o aperto já foi um tap (e
ligou o repeat). Se o botão continuar apertado por 1 s, o repeat desliga. É o
comportamento do `.ino`. A duração é medida com o mesmo `TriggerInput`: do
disparo até o frame atual, enquanto `held()`.

## Repeat

**Divisão.** No repeat, apertar um baixo `i` (0–7) não soa: só escolhe a
divisão `D = {2, 3, 4, 6, 8, 12, 16, 32}[i]` por compasso de 4 tempos. A
divisão sobrevive à saída e à volta do repeat, como no `.ino`. O padrão é
`D = 4` (baixo 3).

**Grade.** Os toques do repeat caem em `origem + k · 4P / D`, calculados em
64 bits a partir da origem. O erro fica abaixo de 1 frame e não acumula, mesmo
quando `4P / D` não é inteiro (por exemplo, `D = 3`).

**O que se repete.** Repete-se a tecla de percussão apertada mais
recentemente *entre as que continuam apertadas*. O `.ino` repete a última
apertada mesmo depois de solta, enquanto houver *qualquer* outra apertada.
Isso parece acidental (bug 6).

**Primeiro toque: imediato**, como no `.ino`, com a mesma exceção que ele
tem. A grade do repeat tem uma origem: a última batida do repeat ou o último
tap, o que for mais recente (`previousMillis`, `agbau.ino:149` e `:184`).
- Se o aperto vem **mais de um período** depois dessa origem, a voz soa na
  hora, e a grade recomeça no aperto. É o caso comum: começar um repeat
  depois de uma pausa.
- Se vem **antes de completar um período**, a voz espera a próxima batida da
  grade que já estava correndo. Soltar e apertar de novo rápido, ou trocar de
  percussão no meio de um repeat, mantém o groove em vez de reiniciá-lo. O
  mesmo vale logo depois de um tap: o primeiro toque cai uma batida depois do
  tap.

Ninguém escreveu isso de propósito: é o efeito de o `previousMillis` só andar
nas batidas e no tap. Mas é como o Agbau responde na mão, e por isso fica.
Quantizar sempre à grade do tap foi descartado: atrasaria o começo de um
repeat em até uma subdivisão (500 ms com divisão 4 a 120 bpm).

**A cada toque da grade:** note-on da voz que se repete. A `Voice` já faz
retrigger a partir do nível atual do envelope. Se a voz mudou desde o último
toque, a anterior recebe note-off antes. Para a percussão o note-off não muda
nada. Para o ganzá, ele faz o loop recomeçar a cada toque, como no `.ino`.

**Fim.** Quando nenhuma percussão do repeat está apertada, a última voz
repetida recebe note-off (o ganzá para em 0,1 s).

**Teclas que atravessam a troca de modo.** Uma tecla é solta *no modo em que
foi apertada*:
- tecla apertada no modo nota e solta no repeat: recebe o note-off normal.
  No `.ino`, um ganzá segurado durante a troca fica em loop para sempre
  (bug 7);
- tecla apertada no repeat e ainda segura quando o repeat desliga: o repeat
  para e manda note-off da voz repetida. Soltar a tecla depois não faz nada.

As teclas do repeat ficam numa máscara de bits, uma por tecla, e não num
contador. Um contador que só sobe e desce fica negativo ou preso quando uma
tecla atravessa a troca de modo (bug 5).

**LED de modo:** aceso na primeira metade de cada tempo, apagado na segunda,
na grade do tap. Apagado no modo nota.

## Looper

**O que se grava:** os eventos de voz (voz, ligou/desligou) que o `Performer`
gera a partir das teclas *ao vivo*, inclusive os do repeat, como no `.ino`.
**Não se grava a reprodução do próprio loop** (bug 4). O loop e o ao vivo
passam pelo mesmo `VoiceBank`, mas só o ao vivo vai para a camada.

**Estados:**

| Estado | Pedal (toque) | Pedal segurado ≥ 1 s | LED do looper |
|---|---|---|---|
| Vazio | começa a gravar a camada 0 | — | apagado |
| Gravando | fecha o loop: o comprimento é o tempo gravado; passa a tocar | apaga (volta a vazio) | aceso |
| Tocando | abre overdub numa camada nova, se houver | apaga | pisca, travado no início do loop |
| Overdub | fecha o overdub; continua tocando | apaga | aceso |

Assim como no tap, o toque que começa um aperto longo já fez a sua transição.
O apagar vem 1 s depois e desfaz tudo.

**Camadas:** 4 no total, a base mais três overdubs. O `.ino` deixa abrir três
overdubs, o README do Agbau diz "até 3 camadas de overdub", e foi o que quem
tocou teve nas mãos. O array do `.ino` só tem 3 posições, por isso o terceiro
overdub escreve fora dele (bug 3). Aqui o array tem 4. Com as camadas cheias,
o pedal em "tocando" não faz nada.

**Capacidade:** 1024 eventos por camada, contra os 64 do `.ino`. Num repeat a
32 por compasso, 64 eventos enchem em menos de um compasso. No pior caso
(divisão 32 a 160 bpm, ~21 eventos/s), 1024 duram ~48 s. Cada evento ocupa
4 bytes (posição no loop em frames, voz, ligou/desligou), o que dá 16 KB para
as quatro camadas, em RAM interna. Camada cheia: a gravação fecha
sozinha, como no `.ino`, e conta uma perda visível no `DEBUG_ENGINE`
(`full`). As últimas 32 posições de cada camada ficam reservadas para os
note-offs de fechamento (abaixo), de modo que uma camada cheia ainda fecha
sem deixar voz presa.

**Comprimento máximo** *(implementação)*: ~380 s. A posição ocupa 24 bits
do evento, e uma primeira gravação que chegue lá fecha sozinha, contando como
camada cheia.

**Onde uma batida do repeat é gravada** *(implementação)*: na posição da
grade a que ela pertence, e não no começo do bloco em que foi aplicada. Se
fosse no bloco, o loop carregaria o tamanho do bloco em cada volta
(`test_block_size_does_not_change_the_schedule` pega isso). Pelo mesmo
motivo, as batidas do repeat de cada bloco são tratadas antes das teclas
desse bloco, e cada camada continua em ordem de posição.

**Comprimento:** livre, em frames, do frame em que a gravação começou ao frame
em que fechou. Não é quantizado ao tempo, como no `.ino`. Seria coerente
quantizar só se o repeat também fosse travado à grade do tap, e ele não é
(acima).

**Overdub:** grava na camada nova com posições módulo o comprimento do loop.
Fecha no pedal, com a camada cheia, ou **na próxima passagem pelo início do
loop**, como no `.ino` (`agbau.ino:326`). Aberto aos 75% do loop, o overdub
grava os 25% finais e fecha sozinho. Para gravar sobre o loop inteiro, abre-se
no começo.

O `.ino` testa isso com `currentLoopTime % loopDuration <= 0.01`, ou seja,
`== 0` em milissegundos inteiros. Na maior parte das vezes isso acerta, porque
o `loop()` do Pro Micro roda mais rápido que 1 ms. Mas o `millis()` do AVR pula
um valor a cada ~42 ms, e uma volta lenta do `loop()` também pula. Quando o
milissegundo exato é pulado, o overdub segue por mais uma volta inteira. Aqui
o teste é "a posição passou pelo início do loop neste bloco", que não depende
de acertar um frame exato. Isso é a intenção do `.ino`, sem a falha
ocasional. A leitura é do código. Não houve medição no Pro Micro.

Na prática, o bug 4 fazia muitos overdubs fecharem antes, por encher a
camada: a reprodução regravada ocupa as 64 posições, e uma base com dezenas de
eventos enche a camada nova em menos de uma volta. Com o bug 4 corrigido e
1024 posições, o overdub vai até o início do loop. O comportamento pretendido
volta, mas é diferente do que se ouvia.

**Notas abertas ao fechar uma camada.** Uma tecla ainda apertada quando a
gravação ou o overdub fecha tem o note-on gravado e não o note-off. Tocado em
loop, um ganzá ficaria ligado para sempre. Ao fechar, cada voz ligada nessa
camada sem note-off recebe um note-off gravado na posição de fechamento (bug 8).
*(implementação)* Na primeira gravação, essa posição é o último frame da
volta. Um note-off no frame 0 cairia junto com um note-on no começo do loop
e o desligaria na mesma hora. No overdub, é onde ele fechou. Uma tecla
apertada no mesmo bloco em que a primeira gravação fecha vai para o último
frame da volta, e não para o primeiro. Ela acabou de soar ao vivo, e assim
não soa duas vezes nesse bloco.

**Apagar** manda note-off para toda voz que o loop ligou e ainda não desligou,
antes de esvaziar as camadas. No `.ino`, apagar durante um ganzá do loop o
deixa tocando (bug 9).

**Reprodução:** a cada bloco, toca os eventos de todas as camadas cuja posição
cai em `[pos, pos + frames)`, inclusive quando esse intervalo atravessa o fim
do loop. Dentro de um bloco, a ordem é a da posição, e na mesma posição, a das
camadas.

**Ao vivo por cima do loop.** O loop e as teclas dividem as mesmas 16 vozes,
como no Bela. Um note-off do loop corta um ganzá que esteja sendo segurado ao
vivo. É aceito, e não é corrigido aqui.

**LED do looper, tocando:** aceso no primeiro e no terceiro quarto de cada
volta, contados a partir do início do loop. São as duas piscadas por volta do
`.ino`, mas sem a deriva do `previousblinkt`, que corre solto.

## Bugs do `.ino` e a regra que corrige cada um

| # | Bug | Onde | Regra |
|---|---|---|---|
| 1 | `(1000 / bpm) * 60` em inteiros: 480 ms em vez de 500 a 120 bpm, 4% rápido | `agbau.ino:159` | Período guardado em frames, medido direto, sem bpm |
| 2 | Toques com mais de 60 s entre si dão `bpm = 0` e divisão por zero (panic no ESP32) | `:144`, `:159` | Faixa válida de intervalo; fora dela o tempo não muda |
| 3 | `currentLayer` chega a 3 e escreve `NloopMsgs[3]` num array de 3 | `:390-395` | Overdub só abre se houver camada livre |
| 4 | O overdub regrava a reprodução, porque `playLoop()` passa pelo `noteOn()` que grava | `:219`, `:424` | Só o ao vivo é gravado |
| 5 | `botoesApertados` fica negativo ou preso quando uma tecla atravessa a troca de modo | `:252`, `:266` | Máscara de teclas do repeat, e não contador |
| 6 | Repete a última percussão apertada mesmo depois de solta | `:255-256` | Repete a mais recente entre as ainda apertadas |
| 7 | Ganzá segurado durante a troca para repeat nunca recebe note-off | `:264-268` | A tecla é solta no modo em que foi apertada |
| 8 | Nota segurada quando a gravação fecha fica ligada para sempre no loop | `:374` | Note-off gravado na posição de fechamento |
| 9 | Apagar o loop não desliga o que o loop ligou | `:331-355` | Note-off de toda voz aberta pelo loop antes de esvaziar |
| 10 | LEDs piscam por `millis()` relativo ao último piscar e derivam do tempo e do loop | `:162`, `:455` | LEDs derivados da mesma grade em frames |

Os bugs 1 a 4 já estavam em [migration.md](migration.md). Os bugs 5 a 10
saíram da leitura do `.ino` feita para esta especificação. O README do Agbau
cita "contagem de botões inconsistente" e "note-off disparando cedo demais".
O primeiro é o bug 5, provavelmente agravado pelo 4. O segundo é o `TODO` da
linha 179: o note-off sai logo antes do note-on seguinte. Na Bela isso não
muda nada para a percussão, e para o ganzá é o recomeço do loop a cada toque,
que esta especificação mantém.

## Testes (host, antes da placa)

Em `test/agbau/test_performer`, 24 casos, cada um com o nome do que quebra.
Cada bug abaixo foi reintroduzido no código uma vez para conferir que o
teste dele falha.

- `test_tempo_at_120_is_22050_frames`: dois toques a 500 ms dão `P = 22050`
  exatos, e os 683 ms da bancada dão 683 ms, não 660 (bug 1).
- `test_a_long_gap_starts_over_instead_of_dividing_by_zero` (bug 2).
- `test_a_fourth_overdub_is_refused` (bug 3).
- `test_overdub_does_not_record_the_playback` (bug 4).
- `test_a_key_is_released_in_the_mode_it_was_pressed` (bugs 5 e 7).
- `test_repeats_the_most_recent_key_still_held` (bug 6).
- `test_closing_a_layer_closes_its_held_notes`,
  `test_a_full_layer_closes_with_room_for_its_note_offs` (bug 8) e
  `test_erase_releases_every_voice_the_loop_started` (bug 9).
- O que é do Agbau e fica: `test_a_slow_tap_is_still_a_tempo`,
  `test_repeat_hits_at_once_after_a_pause`,
  `test_a_repress_within_a_subdivision_keeps_the_grid`,
  `test_first_hit_after_a_tap_waits_for_the_tap_grid`,
  `test_bass_in_repeat_sets_the_division_and_is_silent`,
  `test_holding_mode_one_second_leaves_repeat`,
  `test_overdub_closes_at_the_loop_start`,
  `test_the_first_frame_plays_as_the_loop_comes_round` e os dois LEDs.
- `test_repeat_grid_does_not_drift`: uma hora a `D = 3` com um tempo ímpar,
  em que `4P/D` não é inteiro. Cada batida cai exatamente em
  `origem + ⌊k·4P/D⌋` e é aplicada menos de um bloco depois.
- `test_loop_does_not_drift`: cem voltas de um loop cujo comprimento não é
  múltiplo do bloco. Cada evento cai no mesmo frame-alvo de cada volta.
- `test_block_size_does_not_change_the_schedule`: a mesma execução (taps,
  troca de divisão, ganzá no modo nota, gravação, overdub com repeat) com
  blocos de 1, 7, 32, 3, 64, 13, 128 e 5 frames e com blocos de 32. A lista de
  eventos de voz e seus frames-alvo é idêntica. A *saída* não fica
  bit-idêntica, porque o evento cai na borda do bloco; o que é fato é o
  agendamento.

## Na bancada atual (3 vozes, modo e pedal)

Hoje a bancada tem quatro botões: GPIO4 b1 (baixo), GPIO5 p1 (percussão),
GPIO6 p3 (ganzá) e GPIO7 o botão de modo, todos por `KeyMatrix<4,1>`, mais o
pedal no GPIO39. Com eles se ouve todo o repeat:
- tap-tempo, e segurar para sair;
- a divisão padrão `D = 4` e, depois de apertar o b1 no repeat, `D = 2`;
- repeat de percussão (p1, com o sorteio de takes a cada toque) e de ganzá
  (p3, com o loop recomeçando a cada toque);
- os bugs 5 e 7 de propósito: segurar o p3, trocar de modo, soltar.

Os LEDs não estão ligados. O firmware já escreve nos pinos finais deles
(GPIO15 modo, GPIO8 looper), e cada mudança de estado do repeat e do looper
sai na serial (`REPEAT ...`, `LOOP ...`). Para o looper, a
bancada tem desde 11/10/2026 um quinto botão no **GPIO39**, ao GND, que é o
pino do pedal no mapa final ([circuit.md](circuit.md)). Assim nada muda de
pino quando a bancada virar o instrumento. Ele é um `TriggerInput` próprio,
fora da `KeyMatrix<4,1>`, porque no instrumento o pedal também fica fora da
matriz.

## Decisões

Todas tomadas em 11/10/2026, pela regra do topo:

1. Faixa do tap: nenhuma, como no `.ino`. Só um intervalo de 60 s ou mais,
   onde o `.ino` divide por zero, deixa de mudar o tempo.
2. Tempo: o último intervalo, sem média, como no `.ino`.
3. Primeiro toque do repeat: imediato, com a exceção do `.ino` (antes de um
   período, espera a grade que já corria).
4. Camadas: a base mais três overdubs.
5. Capacidade: 1024 eventos por camada.
6. Comprimento do loop: livre.
7. Overdub: fecha na próxima passagem pelo início do loop, como no `.ino`.
8. Pedal na bancada: um quinto botão, no GPIO39.

# Agbau no ESP32-S3: mapa de pinos

Este é o mapa para tirar o Arduino Pro Micro da placa do Agbau e pôr um
ESP32-S3 N16R8 no lugar, ligando fios aos dois soquetes de 12 pinos onde o Pro
Micro encaixava (`Circuito/Agbaixo_PCB`: J1 e J2). DAC (PCM5102A) e, se for o
caso, SD seguem exatamente [circuit.md](circuit.md); este documento cobre só o
que é do Agbau: matriz, botão de modo, pedal e LEDs. Contexto em
[agbau-migration.md](agbau-migration.md), etapa 4.

O que está aqui foi lido do **PCB** (`Agbaixo_PCB.kicad_pcb`, nets de cada pad),
não do esquemático — que não tem labels — e conferido contra o `agbau.ino`.
Os dois concordam em quase tudo. As três exceções estão na seção
"O que ainda não tem resposta", e uma delas precisa ser vista no instrumento
antes de fiar.

## O que a placa do Agbau liga

Os soquetes seguem a pinagem do Pro Micro, com o pino 1 no topo (lado do USB).
A orientação foi confirmada pelos próprios sinais: os catodos dos diodos caem
em D2–D5 e os conectores das teclas em D6–D9, exatamente as colunas e linhas
do `.ino`.

| Soquete | Pro Micro | Liga em | Função |
|---|---|---|---|
| J2.1 | TX0 (D1) | R3 (220 Ω) → J4.1 | Saída MIDI DIN (ver abaixo) |
| J2.3 | GND | J4.2 | GND |
| J2.5–J2.8 | D2, D3, D4, D5 | catodos de D1–D4, D5–D8, D9–D12, D13–D16 | **Colunas 0–3** |
| J2.9–J2.12 | D6, D7, D8, D9 | conectores das teclas, 4 por linha | **Linhas 3, 2, 1, 0** |
| J1.2 | GND | J3, J5, J6, J7, J24–J39 (pino 1) | GND |
| J1.4 | VCC (5 V) | R4 (220 Ω) → J4.3 | Saída MIDI DIN |
| J1.5 | A3 | J3.2 | **Pedal do looper** |
| J1.6 | A2 | R2 (220 Ω) → J7.2 | **LED do looper** |
| J1.7 | A1 | R1 (220 Ω) → J5.2 | **LED de modo** |
| J1.8 | A0 | J6.2 | Conector de botão (ver abaixo) |

Os demais pinos dos soquetes (RX, o segundo GND, RAW, RST, D10, D14, D15, D16)
não estão ligados a nada.

Cada tecla é um conector JST (J8–J23) entre a sua linha e o anodo do seu diodo,
e o catodo vai para a coluna: a corrente só anda da linha para a coluna. Daí
vem a direção da varredura — coluna em LOW, linha lida com pull-up — que
[src/key_matrix.h](../src/key_matrix.h) segue e que
`test_scanning_the_wrong_way_reads_nothing` fixa. Antes isso era deduzido do
`Keypad`; agora está confirmado na placa.

## O mapa no ESP32-S3

| Função | Pro Micro (soquete) | GPIO | Modo |
|---|---|---|---|
| Coluna 0 | D2 (J2.5) | **4** | saída open-drain (LOW puxa, HIGH solta) |
| Coluna 1 | D3 (J2.6) | **5** | saída open-drain |
| Coluna 2 | D4 (J2.7) | **6** | saída open-drain |
| Coluna 3 | D5 (J2.8) | **7** | saída open-drain |
| Linha 0 | D9 (J2.12) | **1** | entrada, pull-up interno |
| Linha 1 | D8 (J2.11) | **2** | entrada, pull-up interno |
| Linha 2 | D7 (J2.10) | **42** | entrada, pull-up interno |
| Linha 3 | D6 (J2.9) | **41** | entrada, pull-up interno |
| Botão de modo | **a confirmar**: D15 no `.ino`, A0/J6 na placa | **40** | entrada, pull-up interno |
| Pedal do looper | A3 (J1.5) | **39** | entrada, pull-up interno |
| LED de modo | A1 (J1.7) | **15** | saída |
| LED do looper | A2 (J1.6) | **8** | saída |
| GND | GND (J1.2 e J2.3) | GND | — |

**Não ligar** ao ESP32: VCC (J1.4), RAW (J1.1) e TX0 (J2.1). São a saída MIDI,
que deixa de existir (ver abaixo).

Por que esses GPIOs:

- **Colunas em 4–7**: são os pinos do disparo e dos pads no sampler, que ficam
  livres no Agbau, e são contíguos no header esquerdo do DevKitC-1. Os LEDs
  ficam no mesmo header (15 e 8), nos dois lados de I2S 16/17/18.
- **Linhas, modo e pedal em 1, 2, 42, 41, 40, 39**: seis pinos contíguos do
  header direito, todos entradas. 39–42 são os pinos de JTAG por pads, livres
  enquanto o JTAG passa pelo USB nativo, que é o padrão.
- **Fora da lista de propósito**: 38 e 48, porque um dos dois leva o LED RGB do
  DevKitC-1 conforme a revisão da placa (conferir na placa real antes de usar
  qualquer um); 0, 3, 45 e 46 (strapping); 19/20 (USB); 26–37 (flash e PSRAM
  octal); 43/44 (UART0). Ver [circuit.md](circuit.md).
- **Livres para depois**: 9, 14, 21 e 47 — e 10–13 se a etapa 0 escolher a
  partição na flash e o SD sair.

A ordem dos headers acima é a do DevKitC-1 oficial. Placas "N16R8" genéricas
costumam repetir a mesma ordem, mas o que vale é a serigrafia.

### Colunas em open-drain

O `Keypad` deixava as colunas ociosas flutuando (`INPUT`) e só puxava uma de
cada vez para LOW. No ESP32 o equivalente é configurar as colunas uma vez como
`OUTPUT_OPEN_DRAIN`: `digitalWrite(LOW)` puxa, `digitalWrite(HIGH)` solta.
Trocar `pinMode()` a cada coluna também funcionaria, mas `pinMode()` passa pelo
`gpio_config()` do IDF e custa microssegundos que a varredura não tem — o
período da varredura inteira é o período de amostragem de cada tecla (ver
`key_matrix.h`).

### Tensão

O ESP32-S3 é 3,3 V e **não tolera 5 V** nos GPIOs. Nada na placa do Agbau
precisa de 5 V além da saída MIDI: os pull-ups são internos, as teclas e o
pedal fecham para o GND ou para uma coluna, e os LEDs são acionados pelo
próprio GPIO.

Os LEDs passam a ter 3,3 V em vez de 5 V atrás dos mesmos 220 Ω: ~6 mA num LED
vermelho (Vf ≈ 2 V), o suficiente. Um LED azul ou branco (Vf ≈ 3 V) ficaria
com ~1,4 mA e bem mais fraco; nesse caso, trocar R1/R2 por algo perto de 68 Ω.

## O que ainda não tem resposta

1. **Onde está o botão de modo.** O `.ino` lê o botão de modo no pino 15
   (`pinoBotaoModo = 15`), mas na placa o 15 (J1.9) não vai a lugar nenhum. O
   único conector de botão sem uso no `.ino` é o J6, ligado ao A0 (J1.8). Ou o
   botão está no J6 e o firmware nunca leu o A0, ou o botão foi ligado no 15
   com um fio por fora da placa. **Olhar no instrumento qual dos dois** antes
   de fiar o GPIO40 em um ou no outro.
2. **J24–J39.** São dezesseis headers de 2 pinos, quatro por coluna, ligando
   cada coluna direto ao GND. Nada no `.ino` os explica — talvez reserva para
   piezos de velocity (o `.ino` tem um comentário sobre isso). Vazios, não
   fazem nada. Mas qualquer coisa condutiva plugada num deles prende aquela
   coluna em LOW: as quatro teclas dela passam a aparecer em todas as
   colunas, e a matriz inteira lê errado. **Conferir que estão vazios.**
3. **J4 é uma saída MIDI DIN** (TX0 por 220 Ω para um pino, 5 V por 220 Ω
   para outro, GND no do meio — o circuito clássico de MIDI OUT), mas o
   `.ino` só fala MIDI pela USB. Sem MIDI no ESP32 ela fica desligada. Fica
   registrado porque é a única coisa na placa que usa 5 V.

## Fiação

Com a placa do Agbau sem o Pro Micro, cada linha da tabela do mapa é um fio do
GPIO indicado ao pino do soquete indicado, mais um GND comum. O PCM5102A e o
SD ligam no ESP32 como em [circuit.md](circuit.md), não na placa do Agbau.
Isso é bancada; uma revisão de PCB para o módulo S3 é a etapa 7 de
[agbau-migration.md](agbau-migration.md).

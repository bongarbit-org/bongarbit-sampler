# Circuito — bongarbit-sampler

Plano de fiação para ESP32-S3 N16R8 + DAC PCM5102A (I2S) + leitor de
microSD (SPI), com 4 botões de disparo (trigger) de sample em pinos
contíguos: GPIO4 (botão principal) e GPIO5/6/7 (pads que tocam `1.wav`,
`2.wav` e `3.wav` da raiz do cartão). Todos ligados a GND físico real,
sem nenhum pino "virtual" de GND.

Fontes usadas para montar este plano:
- Pinout do dev kit: https://randomnerdtutorials.com/esp32-s3-devkitc-pinout-guide/
- Discussão sobre fiação/problemas do PCM5102A: https://forum.pjrc.com/index.php?threads/problem-with-pcm5102a.76661/
- Defaults de pinos I2S da biblioteca M16: `algomusic/M16`, `M16.h`
  (`i2sPinsOut[] = {16, 17, 18, 21}` — bck, ws, dout, din — para ESP32/S2/S3)
  e exemplo `examples/WavPlayer/WavPlayer.ino` (pinos de SPI do SD sugeridos).

## ⚠️ Pinos que NÃO podem ser usados no N16R8

O guia da RandomNerdTutorials cobre o ESP32-S3 genericamente, mas a variante
**N16R8** (16MB flash Quad-SPI + **8MB PSRAM Octal-SPI**) tem uma restrição
adicional que o artigo não detalha: módulos com PSRAM Octal usam GPIOs extras
para as linhas de dados/DQS do PSRAM, além dos pinos de flash padrão.

| Pinos | Motivo | Fonte |
|---|---|---|
| GPIO 0, 3, 45, 46 | Strapping / boot mode | RandomNerdTutorials |
| GPIO 26–32 | Flash SPI (todas as variantes S3) | RandomNerdTutorials |
| **GPIO 33–37** | **Linhas extras do PSRAM Octal (só em módulos R8, como o N16R8)** | Datasheet Espressif ESP32-S3-WROOM-1/1U — não usar mesmo que apareçam livres em guias genéricos |
| GPIO 43, 44 | UART0 (console serial / log de boot) | RandomNerdTutorials |
| GPIO 19, 20 | USB D-/D+ nativo (evitar se for usar USB do ESP32-S3) | Datasheet Espressif |

Por isso este projeto **não** usa a sugestão de VSPI (GPIO 35/36/37/39) do
guia da RandomNerdTutorials — esses pinos colidem com o PSRAM Octal do
N16R8. Em vez disso, o SD usa os pinos de HSPI (10–13), que ficam livres em
qualquer variante do ESP32-S3.

## Mapa de pinos

| Sinal | GPIO ESP32-S3 | Vai para | Observação |
|---|---|---|---|
| I2S BCK  | GPIO 16 | PCM5102A `BCK`  | igual ao default M16 |
| I2S WS (LRCK) | GPIO 18 | PCM5102A `LCK`  | **custom** — trocado do default (17) via `seti2sPins()`, para bater com a fiação da placa de ilha |
| I2S DOUT | GPIO 17 | PCM5102A `DIN`  | **custom** — trocado do default (18) via `seti2sPins()`; note também a inversão de nome (DOUT do ESP32 → DIN do DAC) |
| — | GPIO 21 | (não conectado) | Entrada I2S (DIN) padrão da M16, não usada neste projeto |
| SD CS    | GPIO 13 | Leitor microSD `CS`   | HSPI — custom, trocado do default (10) |
| SD SCLK  | GPIO 11 | Leitor microSD `SCK`  | HSPI — custom, trocado do default (12) |
| SD MISO  | GPIO 10 | Leitor microSD `MISO` (DO/CIPO) | HSPI — custom, trocado do default (13) |
| SD MOSI  | GPIO 12 | Leitor microSD `MOSI` (DI/COPI) | HSPI — custom, trocado do default (11) |
| Trigger  | GPIO 4  | Botão → GND | `INPUT_PULLUP`, ativo em nível baixo |
| Pad 1 (`1.wav`) | GPIO 5  | Botão → GND | `INPUT_PULLUP`, ativo em nível baixo |
| Pad 2 (`2.wav`) | GPIO 6  | Botão → GND | `INPUT_PULLUP`, ativo em nível baixo |
| Pad 3 (`3.wav`) | GPIO 7  | Botão → GND | `INPUT_PULLUP`, ativo em nível baixo |
| 3V3      | 3V3 | VCC do leitor microSD, `VCC`/`3.3V` do PCM5102A* | ver nota de alimentação abaixo |
| GND      | GND | GND comum a DAC, SD e aos 4 botões | **crítico, ver nota abaixo** |

\* módulos PCM5102A costumam ter regulador on-board e aceitar 3.3–5V no
pino `VIN`; confirme a serigrafia da sua placa (ver seção do DAC).

## Alimentação e terra comum

O tópico do fórum PJRC (link acima) descreve um caso real de instabilidade
de áudio causado por **GND não conectado entre a placa e o DAC** — o
sintoma foi ruído/comportamento errático, e a causa raiz não era o
código, era a referência de terra flutuante. Aplicando essa lição aqui:

- Uma única referência de GND comum entre ESP32-S3, PCM5102A, leitor de SD
  e os 4 botões. Não deixe o DAC "pendurado" só pelos fios de sinal.
- No fórum, o VIN do DAC foi alimentado com 5V. A maioria dos módulos
  PCM5102A (breakout genérico "GY-PCM5102") tem regulador linear on-board
  e aceita tanto 3.3V quanto 5V em VIN; alimentar com 5V costuma dar mais
  headroom para o estágio analógico de saída. Use o 5V do dev kit
  (pino `5V`) se sua carga total (DAC + SD) não estressar o regulador
  USB; caso contrário, 3V3 do dev kit também funciona com a maioria dos
  módulos.

## Pinos de controle do PCM5102A

O tópico do PJRC confirma a prática padrão para módulos PCM5102A alimentados
por um único DAC (sem distribuir um master clock externo):

| Pino do PCM5102A | Ligar a | Efeito |
|---|---|---|
| `SCK` (system clock) | **GND** | Habilita o modo de clock automático interno do PCM5102A — gera o master clock a partir do próprio BCK, dispensando um oscilador de 11.2896MHz externo. Confirmado no fórum: *"Just connect SCK to GND at each DAC board... I have always used PCM5102A's in that configuration without any problems."* |
| `FMT` (format select) | **GND** | Seleciona formato I2S padrão (necessário para bater com a saída da M16, que usa I2S Philips) |
| `XSMT` (soft mute) | **3V3** | Precisa ficar em nível alto para desmutar a saída. A maioria dos breakouts genéricos já traz um pull-up para isso, mas confira com um multímetro — se ficar flutuante ou em GND, a saída de áudio fica permanentemente mudo mesmo com o I2S funcionando |
| `FLT` (filter select) | GND | Filtro normal (latência normal); é o default na maioria das placas |
| `DEMP` (de-emphasis) | GND | Desabilita de-ênfase de 44.1kHz (não necessário para reprodução de samples) |

Esses quatro últimos pinos (`FMT`, `XSMT`, `FLT`, `DEMP`) não aparecem no
tópico do fórum (o autor usava um breakout que já os resolve on-board),
mas são padrão do datasheet do PCM5102A — confira a serigrafia da sua
placa, pois em muitos breakouts genéricos eles já vêm amarrados
internamente aos valores acima e nem são expostos.

## Leitor de microSD (SPI)

Usa o barramento HSPI dedicado (não compartilhado com flash/PSRAM):

| Pino do módulo SD | GPIO |
|---|---|
| CS  | 13 |
| SCK | 11 |
| MOSI (DI) | 12 |
| MISO (DO) | 10 |
| VCC | 3V3 (a maioria dos módulos leitores de SD tem regulador para 3.3V; **não** ligar direto em 5V a menos que o módulo diga suportar) |
| GND | GND comum |

## Botões (trigger + 3 pads)

4 tactile switches de 4 pernas, todos com o mesmo padrão de fiação: uma
perna no GPIO (pull-up interno), a outra num pino de **GND físico real**
do dev kit — sem nenhum truque de "GND por software" neste layout.

Nesses botões, as duas pernas do *mesmo lado* já vêm curto-circuitadas
internamente entre si; apertar o botão liga um lado ao outro. Por isso é
preciso usar uma perna de cada lado — por exemplo, se o botão atravessa o
vão central do protoboard, uma perna de um lado e uma do outro, nunca duas
pernas da mesma coluna/lado.

```
GPIO4 (INPUT_PULLUP) ──● trigger ●── GND
GPIO5 (INPUT_PULLUP) ──● pad 1   ●── GND    (1.wav)
GPIO6 (INPUT_PULLUP) ──● pad 2   ●── GND    (2.wav)
GPIO7 (INPUT_PULLUP) ──● pad 3   ●── GND    (3.wav)
```

- **Trigger (GPIO4):** debounce "espera estabilizar antes de disparar"
  (25ms) — dispara só depois da leitura ficar estável.
- **Pads (GPIO5/6/7):** debounce invertido, via biblioteca Bounce2 (10ms).
  O firmware dispara o som na primeira leitura bruta do pino, **antes** de
  esperar o debounce estabilizar — zero atraso entre o toque físico e o
  som. Só depois disso o pino fica bloqueado (ignorado) até o Bounce2
  confirmar que o botão realmente voltou pro estado solto, o que evita
  disparo repetido ("efeito metralhadora") enquanto o botão é segurado,
  sem atrasar a resposta inicial.

Cada pad toca o arquivo correspondente (`/1.wav`, `/2.wav`, `/3.wav`) na
raiz do cartão SD, carregado inteiro na PSRAM no boot.

## Diagrama de blocos

```
                +----------------------+
                |     ESP32-S3 N16R8   |
                |                      |
   Trigger ────┤ GPIO4                |
   Pad 1 ───────┤ GPIO5                |
   Pad 2 ───────┤ GPIO6                |
   Pad 3 ───────┤ GPIO7                |
                |                      |
                | GPIO16 (BCK)  ───────┼───► PCM5102A BCK
                | GPIO18 (WS)   ───────┼───► PCM5102A LCK
                | GPIO17 (DOUT) ───────┼───► PCM5102A DIN
                |                      |     PCM5102A SCK  → GND
                |                      |     PCM5102A FMT  → GND
                |                      |     PCM5102A XSMT → 3V3
                |                      |     PCM5102A FLT/DEMP → GND
                |                      |
                | GPIO13 (CS)   ───────┼───► SD CS
                | GPIO11 (SCK)  ───────┼───► SD SCK
                | GPIO12 (MOSI) ───────┼───► SD MOSI
                | GPIO10 (MISO) ◄──────┼──── SD MISO
                |                      |
                |  3V3 / GND (comum) ──┼───► DAC, SD, botão
                +----------------------+
```

# Circuito alternativo — botões com debounce RC

Guia de montagem para a versão dos 4 botões com filtro RC no hardware, em vez
de deixar o quique de contato inteiro por conta do software.

O resto do circuito — DAC PCM5102A, leitor microSD, alimentação, terra comum —
**não muda em nada**. Só a fiação dos botões. Para tudo o mais, siga
[circuit.md](circuit.md).

## Por que fazer isso

Um botão mecânico não liga limpo: as lâminas quicam, e em ~2 ms o pino vê uma
sequência de abre-fecha em vez de um evento único. Hoje isso é filtrado no
firmware, e funciona. O que o software **não** consegue fazer:

- **Jitter.** Disparar na primeira leitura baixa significa disparar em qualquer
  microcontato que o polling pegar primeiro — a incerteza é a estrutura do
  quique mais o período do loop, tranquilamente centenas de µs. Com o RC a
  descida vira monotônica e determinística, e o jitter cai para só o período do
  loop, ~50 µs. Em percussão, variação de tempo incomoda mais que latência
  absoluta: latência você compensa tocando adiantado, variação não.
- **Ruído captado no fio.** O pull-up interno do ESP32 é fraco (~45 kΩ). Num
  jumper de protoboard isso é uma antena de alta impedância, no mesmo circuito
  que um BCK de I2S chaveando a ~2,8 MHz. Um pico que leve o pino a baixo por
  alguns microssegundos vira uma batida fantasma.

O ganho maior é esse segundo: **um bumbo tocando sozinho do nada é um defeito
muito pior que 1 ms de atraso.**

## Lista de compras

Para os 4 botões:

| Qtd | Componente | Marcação |
|---|---|---|
| 4 | Resistor 10 kΩ (¼ W) | marrom · preto · laranja · dourado |
| 4 | Resistor 220 Ω (¼ W) | vermelho · vermelho · marrom · dourado |
| 4 | Capacitor cerâmico 100 nF | serigrafia `104` |

Valores não são críticos: 4,7 k–10 k no pull-up e 100 Ω–1 k no série funcionam
igual. O que importa é a **relação** entre os dois, explicada abaixo.

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

| Evento | Caminho da corrente | τ | Efeito prático |
|---|---|---|---|
| **Batida** (descida) | capacitor descarrega por **R2** | **22 µs** | Praticamente instantâneo — ~1 amostra de áudio. A latência da batida não é afetada |
| **Quique durante a batida** | recarrega por **R1** | 1 ms | Uma abertura de 300 µs mal levanta 25% da tensão, longe do limiar. O pino nem percebe |
| **Soltura** (subida) | carrega por **R1** | 1 ms | ~1,4 ms até cruzar o limiar. Musicalmente irrelevante |
| **Quique na soltura** | qualquer refechamento zera o capacitor | — | A subida só completa quando o quique realmente acaba |

Ou seja: **rápido na batida, filtrado na soltura** — exatamente a forma que a
lógica dos pads quer, e você ganha de graça, pela física.

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

**Com o firmware, o teste bom.** O `DEBUG_SAMPLER` já imprime `Pad N:
triggered` a cada disparo. Como o RC torna as proteções de software
redundantes, dá para provar que ele funciona **enfraquecendo o software** e
verificando que nada piora:

1. Com o firmware como está (`PAD_CONFIRM_US 200`, `PAD_RELEASE_MS 3`), bata
   nos pads e confirme uma linha por batida.
2. Temporariamente, em [../src/main.cpp](../src/main.cpp), baixe para
   `PAD_CONFIRM_US 0` e `PAD_RELEASE_MS 1`.
3. Bata de novo, inclusive rápido e de leve. **Se continuar uma linha por
   batida, o RC está fazendo o trabalho.** Sem o RC, esses valores deixariam
   passar quique e ruído.
4. **Restaure para 200 e 3.** Mantenha as duas proteções: numa protoboard não
   há garantia de que os quatro RC ficaram bem contactados, e defesa em
   profundidade custa 0,2 ms.

**Teste de ruído.** Com tudo montado e o áudio tocando, passe a mão perto dos
fios dos botões, mexa nos jumpers, ligue algo indutivo por perto (furadeira,
motor, fonte chaveada). Não pode sair batida nenhuma. Esse é o cenário que o
RC existe para cobrir e que nenhum ajuste de software resolve.

## Depois que estiver funcionando

Com bordas limpas garantidas, dá para simplificar o firmware — mas só depois de
confirmar na bancada, não antes:

- O **GPIO4** ainda usa `DEBOUNCE_MS 25`, o esquema antigo de "esperar
  estabilizar". São 25 ms de latência que o RC torna desnecessários. Migrar o
  GPIO4 para o mesmo esquema dos pads é o maior ganho de latência que sobrou no
  projeto.
- `PAD_CONFIRM_US` e `PAD_RELEASE_MS` podem cair, se o teste acima confirmar.

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

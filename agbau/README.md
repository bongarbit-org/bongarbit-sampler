# Agbau

A migração do [Agbau](https://www.instagram.com/bongarbit/), hoje uma Bela mais
um Arduino, para um único ESP32-S3: 16 vozes com envelope ADSR, e o kit numa
partição da flash. **Estágio atual:** três botões tocando três vozes,
ouvidos na placa, mais o botão de modo e o pedal, com repeat e looper testados
no host e ouvidos na placa. O plano completo e a situação de cada etapa estão em
[docs/migration.md](docs/migration.md).

O motor de áudio, o condicionamento dos botões e a fiação da placa base são
compartilhados com o sampler e ficam em [`shared/`](../shared/); o mapa do
repositório está no [README da raiz](../README.md).

## Gravando

O kit (24 WAVs, ~9 MB) é gravado **uma vez** numa partição própria da flash,
separado do firmware. Regravar o firmware, inclusive o do sampler, não apaga o
kit. Os comandos rodam da raiz do repositório.

1. Gerar a imagem do kit a partir das samples do Agbau (o repositório do
   Agbau, ao lado deste, não esta pasta). A imagem vai para `.pio/`, que o git
   ignora:

   ```
   python3 agbau/tools/agbau_kit.py ../agbau/Bela/agbau-sampler-poly/samples --partition .pio/agbau_kit/kit.bin
   ```

2. Gravar a imagem na partição `samples`. Leva uns 2 minutos e só precisa ser
   repetido quando o kit mudar. O endereço é a linha `samples` de
   [partitions.csv](partitions.csv):

   ```
   ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --baud 921600 write_flash 0x310000 .pio/agbau_kit/kit.bin
   ```

3. Gravar o firmware e abrir o log, que neste env sai pelo COM:

   ```
   .venv/bin/pio run -e agbau -t upload
   .venv/bin/pio device monitor
   ```

No boot, o firmware confere o CRC do kit e diz quantas takes encontrou. Três
botões tocam uma voz cada: GPIO 4 → b1 (baixo), GPIO 5 → p1 (percussão,
5 takes) e GPIO 6 → p3 (ganzá). Para trocar a voz de um botão, mude
`kButtonVoice` no topo de [src/main.cpp](src/main.cpp). 

O GPIO 7 é o **botão de modo**, como no Agbau:
- cada toque bate o tempo (o intervalo desde o toque anterior) e liga o
  repeat;
- segurar por 1 s volta ao modo nota;
- no repeat, segurar uma percussão a repete no tempo, e apertar o baixo
  escolhe a divisão sem soar. Na bancada só há o b1: são 4 batidas por
  compasso até o b1 ser apertado no repeat, e 2 depois.

O GPIO 39 é o **pedal do looper**:
- um toque grava;
- o segundo fecha o loop e começa a tocar;
- o terceiro abre um overdub, que fecha no próximo pedal ou quando o loop
  volta ao começo;
- são até três overdubs;
- segurar por 1 s apaga tudo.

O comportamento completo está em [docs/looper.md](docs/looper.md). Os LEDs
saem nos GPIO 15 (modo) e 8 (looper), e cada mudança de estado aparece no log.

Enquanto esses LEDs não estão ligados, o **LED RGB da própria placa** mostra
os mesmos dois estados: **azul** é o LED de modo (pisca no tempo do repeat),
**vermelho** é o do looper (aceso gravando, piscando duas vezes por volta
tocando), e **magenta** são os dois acesos ao mesmo tempo. Ele fica no GPIO 48
ou no 38, conforme a revisão do DevKitC-1. O firmware usa o 48; se o LED não
acender, troque `STATUS_RGB_PIN` para 38 no topo de [src/main.cpp](src/main.cpp).

## O spike de armazenamento

O env `agbau_storage` ([bench/agbau_storage.cpp](bench/agbau_storage.cpp)) é a
medição que escolheu onde o kit mora: mapeado da flash (opção A, a escolhida)
ou carregado do SD para a PSRAM (opção B, que não chegou a rodar: o cartão não
tinha a pasta `/agbau/`). Depois roda uma escada de 1 a 64 vozes para ouvir.
Como preparar a imagem e o cartão está no topo do sketch; os resultados, em
[shared/docs/bancada.md](../shared/docs/bancada.md), seção 13.

## Documentação

- [docs/migration.md](docs/migration.md): o plano, etapa por etapa, com a
  situação de cada uma.
- [docs/circuit.md](docs/circuit.md): o mapa de pinos, lido da placa de
  circuito do Agbau. O DAC segue a placa base, em
  [shared/docs/circuit.md](../shared/docs/circuit.md).
- [shared/docs/bancada.md](../shared/docs/bancada.md): o que foi medido e
  ouvido na placa. As seções 13 e 14 são do Agbau.

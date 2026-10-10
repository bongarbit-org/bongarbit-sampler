/*
 * Standalone I2S tone test - no M16, no SdFat, no PSRAM, no buttons.
 *
 * Exists to answer one question that the sampler firmware cannot: is the
 * PCM5102A wiring capable of producing sound at all? It talks to the ESP-IDF
 * i2s_std driver directly and, unlike M16, checks and prints the return code
 * of every call - a failed i2s_new_channel() in M16 is silent, and its
 * "M16 is running" line prints either way.
 *
 * Output is a continuous ~441 Hz square wave, so nothing has to be pressed and
 * there is no sample content to blame.
 *
 *   tone audible -> wiring and DAC are fine, the fault is in the firmware
 *   silence      -> the fault is the module, its XSMT pin, or the wiring
 *
 * Same pins as the sampler: BCK=16, WS=17, DOUT=18 (M16 defaults).
 */
#include <Arduino.h>
#include <driver/i2s_std.h>

static i2s_chan_handle_t txHandle = NULL;
static bool i2sReady = false;

#define TONE_SAMPLE_RATE 44100
#define TONE_PERIOD      100 // frames per square wave cycle -> 441 Hz
#define TONE_AMPLITUDE   12000

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== I2S tone test (no M16) ===");
  Serial.println("BCK=GPIO16  WS=GPIO17  DOUT=GPIO18");

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  esp_err_t err = i2s_new_channel(&chanCfg, &txHandle, NULL);
  Serial.printf("i2s_new_channel        : %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return;

  i2s_std_config_t stdCfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = GPIO_NUM_16,
          .ws   = GPIO_NUM_17,
          .dout = GPIO_NUM_18,
          .din  = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv   = false,
          },
      },
  };
  err = i2s_channel_init_std_mode(txHandle, &stdCfg);
  Serial.printf("i2s_channel_init_std   : %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return;

  err = i2s_channel_enable(txHandle);
  Serial.printf("i2s_channel_enable     : %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return;

  i2sReady = true;
  Serial.println("Writing a continuous 441 Hz square wave. You should hear a steady tone.");
}

void loop() {
  if (!i2sReady) {
    delay(1000);
    return;
  }

  static int16_t frames[512]; // 256 stereo frames
  static int     phase       = 0;
  static uint32_t totalBytes = 0;
  static uint32_t lastReport = 0;

  for (int i = 0; i < 512; i += 2) {
    int16_t v = (phase < TONE_PERIOD / 2) ? TONE_AMPLITUDE : -TONE_AMPLITUDE;
    frames[i]     = v; // left
    frames[i + 1] = v; // right
    phase = (phase + 1) % TONE_PERIOD;
  }

  size_t written = 0;
  esp_err_t err = i2s_channel_write(txHandle, frames, sizeof(frames), &written, portMAX_DELAY);
  totalBytes += written;

  // Heartbeat: proves the write is not blocked forever, which would look
  // identical to silence from the outside.
  if (millis() - lastReport > 2000) {
    lastReport = millis();
    Serial.printf("writing... %s, %u bytes total\n", esp_err_to_name(err), (unsigned)totalBytes);
  }
}

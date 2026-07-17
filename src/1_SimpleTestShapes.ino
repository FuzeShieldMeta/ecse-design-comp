/*
 * Plasma effect adapted from the ESP32 HUB75 DMA library's PatternPlasma
 * example and the Pixelmatix Aurora / LedEffects Plasma projects.
 */

#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>
#include <FastLED.h>

// Two 64x32 modules stacked vertically and exposed as one 64x64 display.
#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define PANEL_ROWS  2
#define PANEL_COLS  1
#define PANEL_CHAIN (PANEL_ROWS * PANEL_COLS)

// Panel 1 starts at the top-right and its OUT connector feeds panel 2.
#define PANEL_CHAIN_TYPE CHAIN_TOP_RIGHT_DOWN

// Existing ESP32 DevKit to HUB75 wiring.
#define HUB75_R1   25
#define HUB75_G1   26
#define HUB75_B1   27
#define HUB75_R2   14
#define HUB75_G2   13
#define HUB75_B2   33
#define HUB75_A    23
#define HUB75_B    19
#define HUB75_C    18
#define HUB75_D    17
#define HUB75_E    -1
#define HUB75_LAT  32
#define HUB75_OE   21
#define HUB75_CLK  22

MatrixPanel_I2S_DMA *dma_display = nullptr;
VirtualMatrixPanel_T<PANEL_CHAIN_TYPE> *display = nullptr;

uint16_t time_counter = 0;
uint16_t cycles = 0;
uint16_t fps = 0;
unsigned long fps_timer = 0;

CRGB currentColor;
CRGBPalette16 palettes[] = {
  HeatColors_p,
  LavaColors_p,
  RainbowColors_p,
  RainbowStripeColors_p,
  CloudColors_p
};
CRGBPalette16 currentPalette = palettes[0];

void setup() {
  Serial.begin(115200);
  Serial.println(F("Starting 64x64 chained-panel plasma demo"));

  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);

  mxconfig.gpio.r1 = HUB75_R1;
  mxconfig.gpio.g1 = HUB75_G1;
  mxconfig.gpio.b1 = HUB75_B1;
  mxconfig.gpio.r2 = HUB75_R2;
  mxconfig.gpio.g2 = HUB75_G2;
  mxconfig.gpio.b2 = HUB75_B2;
  mxconfig.gpio.a = HUB75_A;
  mxconfig.gpio.b = HUB75_B;
  mxconfig.gpio.c = HUB75_C;
  mxconfig.gpio.d = HUB75_D;
  mxconfig.gpio.e = HUB75_E;
  mxconfig.gpio.lat = HUB75_LAT;
  mxconfig.gpio.oe = HUB75_OE;
  mxconfig.gpio.clk = HUB75_CLK;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->setBrightness8(90);

  if (!dma_display->begin()) {
    Serial.println(F("I2S DMA memory allocation failed"));
    return;
  }

  display = new VirtualMatrixPanel_T<PANEL_CHAIN_TYPE>(
    PANEL_ROWS, PANEL_COLS, PANEL_RES_X, PANEL_RES_Y
  );
  display->setDisplay(*dma_display);
  display->clearScreen();

  currentPalette = RainbowColors_p;
  fps_timer = millis();
}

void loop() {
  if (display == nullptr) {
    delay(1000);
    return;
  }

  for (int16_t x = 0; x < display->width(); ++x) {
    for (int16_t y = 0; y < display->height(); ++y) {
      int16_t v = 128;
      const uint8_t wibble = sin8(time_counter);

      v += sin16(x * wibble * 3 + time_counter);
      v += cos16(y * (128 - wibble) + time_counter);
      v += sin16(y * x * cos8(-time_counter) / 8);

      currentColor = ColorFromPalette(currentPalette, v >> 8);
      display->drawPixelRGB888(
        x, y, currentColor.r, currentColor.g, currentColor.b
      );
    }
  }

  ++time_counter;
  ++cycles;
  ++fps;

  if (cycles >= 1024) {
    time_counter = 0;
    cycles = 0;
    currentPalette = palettes[random(
      sizeof(palettes) / sizeof(palettes[0])
    )];
  }

  if (fps_timer + 5000 < millis()) {
    Serial.printf_P(PSTR("Effect fps: %u\n"), fps / 5);
    fps_timer = millis();
    fps = 0;
  }
}

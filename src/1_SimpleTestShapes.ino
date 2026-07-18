#include <Arduino.h>
#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>
#include <driver/i2s.h>

// BOP Rhythm: two 64x32 HUB75 panels presented as one 64x64 display.
#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define PANEL_ROWS  2
#define PANEL_COLS  1
#define PANEL_CHAIN (PANEL_ROWS * PANEL_COLS)
#define PANEL_CHAIN_TYPE CHAIN_TOP_RIGHT_DOWN

// Keep this known-good ESP32 WROOM-32 to HUB75 mapping unchanged.
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

// GPIO34-39 are input-only and have no internal pull-ups. Fit external 10k
// pull-ups and wire every switch active-low. GPIO16 uses its internal pull-up.
#define PIN_TWIST_LEFT   34
#define PIN_TWIST_RIGHT  35
#define PIN_PUSH         36
#define PIN_PULL_REST    39
#define PIN_PULL_FULL    16

// PCM5102A connections. The HUB75 driver uses I2S1 on the original ESP32,
// leaving I2S0 for audio. These pins do not alter the display mapping above.
#define PCM5102_BCK       4
#define PCM5102_LCK       5
#define PCM5102_DIN      15
#define AUDIO_I2S_PORT I2S_NUM_0
#define AUDIO_SAMPLE_RATE 44100

// Boot demonstration used while testing the assembled hardware. Set to 0 to
// restore normal manual song selection immediately after startup.
#define STARTUP_SELF_TEST 1
#define SELF_TEST_CAROUSEL_CYCLES 5
#define SELF_TEST_CAROUSEL_INTERVAL_MS 3000UL
#define SELF_TEST_FINAL_COVER_HOLD_MS 1000UL
#define DEFAULT_RENDER_INTERVAL_US 20000UL

MatrixPanel_I2S_DMA *dmaDisplay = nullptr;
VirtualMatrixPanel_T<PANEL_CHAIN_TYPE> *display = nullptr;

enum class Lane : uint8_t { Twist, Push, Pull };
enum class Gesture : uint8_t { None, TwistLeft, TwistRight, PullHalf, PullFull };
enum class Judgment : uint8_t { None, Perfect, Good, Miss };
enum class Screen : uint8_t { Select, Playing, Paused, Results, Failed };
enum class GimmickType : uint8_t { WindGust };
enum class PullState : uint8_t { Rest, Half, Full, Fault };

struct SongDef {
  const char *title;
  const char *artist;
  uint16_t bpm;
  uint8_t bars;
  uint8_t difficulty;
  uint32_t color;
};

struct NoteDef {
  uint32_t hitMs;
  Lane lane;
  bool variant;  // left/right for twist, half/full for pull
  bool bonus;
};

struct GimmickDef {
  uint32_t startMs;
  uint32_t durationMs;
  GimmickType type;
  float amount;
};

constexpr SongDef SONGS[] = {
  {"TEST GRID", "BOP LAB", 96,  8, 0, 0x00eaff},
  {"SYNC STEP", "BOP LAB", 120, 10, 1, 0xff28ba},
  {"PULL RUSH", "BOP LAB", 144, 12, 2, 0xffb000},
};
// Original looping synthesizer patterns. -1 is a rest; values are MIDI notes.
constexpr int8_t MELODIES[][16] = {
  {72, -1, 72, 76, 67, -1, 67, 79, 72, -1, 76, 79, 67, 72, 79, -1},
  {72, 76, 79, 76, 67, 72, 76, 79, 72, 79, 81, 79, 76, 72, 67, -1},
  {72, 79, 76, 84, 79, 76, 72, 67, 72, 76, 79, 84, 81, 79, 76, 72},
};
constexpr int8_t BASSES[][16] = {
  {48, -1, -1, -1, 48, -1, -1, -1, 53, -1, -1, -1, 55, -1, -1, -1},
  {48, -1, 48, -1, 53, -1, 53, -1, 55, -1, 55, -1, 53, -1, 50, -1},
  {48, -1, 48, 48, 53, -1, 53, 53, 55, -1, 55, 55, 58, 55, 53, 50},
};
constexpr size_t SONG_COUNT = sizeof(SONGS) / sizeof(SONGS[0]);
constexpr size_t MAX_NOTES = 220;
constexpr size_t GIMMICK_COUNT = 2;
constexpr int NOTE_HIT_Y = 55;
constexpr int NOTE_TOP_Y = 6;
constexpr int GIMMICK_SAFE_ZONE_Y = 26;  // Bottom 60% begins here.
constexpr int32_t NOTE_TRAVEL_MS = 1800;

NoteDef chart[MAX_NOTES]{};
bool resolved[MAX_NOTES]{};
GimmickDef gimmicks[GIMMICK_COUNT]{};
size_t noteCount = 0;
uint32_t songDurationMs = 0;
volatile uint8_t selectedSong = 0;
int8_t carouselSlide = 0;
uint32_t carouselChangedAt = 0;

struct DebouncedInput {
  int pin;
  bool internalPullup;
  bool stable = false;
  bool raw = false;
  bool pressedEdge = false;
  bool releasedEdge = false;
  uint32_t changedAt = 0;

  DebouncedInput(int inputPin, bool useInternalPullup)
      : pin(inputPin), internalPullup(useInternalPullup) {}

  void begin() {
    pinMode(pin, internalPullup ? INPUT_PULLUP : INPUT);
    stable = raw = !digitalRead(pin);
  }

  void update(uint32_t now) {
    pressedEdge = releasedEdge = false;
    const bool sample = !digitalRead(pin);
    if (sample != raw) {
      raw = sample;
      changedAt = now;
    }
    if (sample != stable && now - changedAt >= 12) {
      stable = sample;
      pressedEdge = stable;
      releasedEdge = !stable;
    }
  }
};

DebouncedInput twistLeft{PIN_TWIST_LEFT, false};
DebouncedInput twistRight{PIN_TWIST_RIGHT, false};
DebouncedInput pushInput{PIN_PUSH, false};
DebouncedInput pullRest{PIN_PULL_REST, false};
DebouncedInput pullFull{PIN_PULL_FULL, true};

volatile Screen screen = Screen::Select;
PullState previousPullState = PullState::Rest;
volatile uint32_t runStartedAt = 0;
volatile uint32_t runStartedAtUs = 0;
uint32_t pausedAt = 0;
uint32_t pausedAtUs = 0;
uint32_t pauseChordAt = 0;
uint32_t endShownAt = 0;
uint32_t score = 0;
uint16_t combo = 0;
uint16_t maxCombo = 0;
uint16_t perfects = 0;
uint16_t goods = 0;
uint16_t misses = 0;
int16_t health = 100;
uint8_t difficulty = 1;
Judgment lastJudgment = Judgment::None;
Lane lastJudgmentLane = Lane::Push;
uint32_t judgmentShownAt = 0;
bool audioReady = false;
uint32_t renderIntervalUs = DEFAULT_RENDER_INTERVAL_US;
bool startupTestActive = STARTUP_SELF_TEST;
bool startupAutoPlay = false;
uint8_t startupCarouselCycles = 0;
uint32_t startupTestChangedAt = 0;

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return dmaDisplay->color565(r, g, b);
}

uint32_t songTime(uint32_t now) {
  return now - runStartedAt;
}

uint16_t perfectWindow() {
  return difficulty == 0 ? 95 : difficulty == 1 ? 70 : 50;
}

uint16_t goodWindow() {
  return difficulty == 0 ? 175 : difficulty == 1 ? 130 : 95;
}

PullState readPullState() {
  if (pullRest.stable && pullFull.stable) return PullState::Fault;
  if (pullFull.stable) return PullState::Full;
  if (pullRest.stable) return PullState::Rest;
  return PullState::Half;
}

bool gimmickActive(GimmickType type, uint32_t t, float *amount = nullptr) {
  for (const GimmickDef &g : gimmicks) {
    if (g.type == type && t >= g.startMs && t < g.startMs + g.durationMs) {
      if (amount != nullptr) *amount = g.amount;
      return true;
    }
  }
  return false;
}

void buildChart() {
  const SongDef &song = SONGS[selectedSong];
  const uint32_t stepMs = 60000UL / song.bpm / 4;
  const uint16_t totalSteps = song.bars * 16;
  noteCount = 0;

  for (uint16_t step = 0; step < totalSteps && noteCount < MAX_NOTES; step += 2) {
    const Lane lane = static_cast<Lane>((step / 2 + step / 16 + selectedSong) % 3);
    chart[noteCount++] = {
      2000UL + step * stepMs,
      lane,
      static_cast<bool>(((step / 2) + selectedSong) & 1),
      static_cast<bool>(step >= totalSteps * 3 / 4)
    };

    if (step > 0 && step % 24 == 0 && noteCount < MAX_NOTES) {
      const Lane second = static_cast<Lane>((static_cast<uint8_t>(lane) + 1) % 3);
      chart[noteCount++] = {2000UL + step * stepMs, second,
                            static_cast<bool>((step / 8) & 1), true};
    }
  }

  memset(resolved, 0, sizeof(resolved));
  songDurationMs = 2000UL + totalSteps * stepMs;
  gimmicks[0] = {songDurationMs / 3,     2800, GimmickType::WindGust, 1.0f};
  gimmicks[1] = {songDurationMs * 2 / 3, 2800, GimmickType::WindGust, 1.0f};
}

float midiFrequency(int8_t note) {
  return note < 0 ? 0.0f : 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

bool initAudio() {
  i2s_config_t config{};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = AUDIO_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 6;
  config.dma_buf_len = 128;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;
  config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  config.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

  i2s_pin_config_t pins{};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PCM5102_BCK;
  pins.ws_io_num = PCM5102_LCK;
  pins.data_out_num = PCM5102_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(AUDIO_I2S_PORT, &config, 0, nullptr) != ESP_OK) {
    Serial.println(F("PCM5102A I2S driver installation failed"));
    return false;
  }
  if (i2s_set_pin(AUDIO_I2S_PORT, &pins) != ESP_OK) {
    Serial.println(F("PCM5102A I2S pin setup failed"));
    i2s_driver_uninstall(AUDIO_I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(AUDIO_I2S_PORT);
  return true;
}

void audioTask(void *) {
  constexpr size_t FRAME_COUNT = 128;
  int16_t samples[FRAME_COUNT * 2];
  float melodyPhase = 0.0f;
  float bassPhase = 0.0f;
  float cuePhase = 0.0f;
  float kickPhase = 0.0f;

  for (;;) {
    const bool active = screen == Screen::Playing;
    const uint32_t t = active ? songTime(millis()) : 0;
    const SongDef &song = SONGS[selectedSong];
    const uint32_t stepMs = 60000UL / song.bpm / 4;
    const uint32_t beatMs = stepMs * 4;
    const uint32_t musicalTime = t >= 2000 ? t - 2000 : 0;
    const uint32_t stepNumber = musicalTime / stepMs;
    const uint8_t patternStep = stepNumber % 16;
    const float melodyHz = active && t >= 2000
                                ? midiFrequency(MELODIES[selectedSong][patternStep])
                                : 0.0f;
    const float bassHz = active && t >= 2000
                              ? midiFrequency(BASSES[selectedSong][patternStep])
                              : 0.0f;
    const uint32_t stepAge = musicalTime % stepMs;
    const uint32_t beatAge = musicalTime % beatMs;

    float cueHz = 0.0f;
    uint32_t cueAge = UINT32_MAX;
    if (active) {
      for (size_t n = 0; n < noteCount; ++n) {
        if (t >= chart[n].hitMs && t - chart[n].hitMs < 90) {
          cueHz = chart[n].lane == Lane::Twist ? 880.0f :
                  chart[n].lane == Lane::Push ? 1046.5f : 1318.5f;
          cueAge = t - chart[n].hitMs;
          break;
        }
      }
    }

    for (size_t i = 0; i < FRAME_COUNT; ++i) {
      int32_t mix = 0;
      const float sampleMs = i * 1000.0f / AUDIO_SAMPLE_RATE;

      if (melodyHz > 0.0f && stepAge + sampleMs < stepMs * 0.78f) {
        melodyPhase += melodyHz / AUDIO_SAMPLE_RATE;
        if (melodyPhase >= 1.0f) melodyPhase -= 1.0f;
        const float age = (stepAge + sampleMs) / stepMs;
        const float envelope = age < 0.06f ? age / 0.06f :
                               age > 0.62f ? (0.78f - age) / 0.16f : 1.0f;
        mix += static_cast<int32_t>(sinf(melodyPhase * TWO_PI) * 3900 *
                                    max(0.0f, envelope));
      }
      if (bassHz > 0.0f && stepAge + sampleMs < stepMs * 0.88f) {
        bassPhase += bassHz / AUDIO_SAMPLE_RATE;
        if (bassPhase >= 1.0f) bassPhase -= 1.0f;
        mix += static_cast<int32_t>(sinf(bassPhase * TWO_PI) * 2300);
      }
      if (active && t >= 2000 && beatAge + sampleMs < 55.0f) {
        const float kickAge = beatAge + sampleMs;
        const float kickHz = 130.0f - kickAge * 1.5f;
        kickPhase += max(45.0f, kickHz) / AUDIO_SAMPLE_RATE;
        if (kickPhase >= 1.0f) kickPhase -= 1.0f;
        mix += static_cast<int32_t>(sinf(kickPhase * TWO_PI) *
                                    (2600.0f * (1.0f - kickAge / 55.0f)));
      }
      if (cueAge != UINT32_MAX && cueAge + sampleMs < 90.0f) {
        cuePhase += cueHz / AUDIO_SAMPLE_RATE;
        if (cuePhase >= 1.0f) cuePhase -= 1.0f;
        const float envelope = 1.0f - (cueAge + sampleMs) / 90.0f;
        mix += static_cast<int32_t>(sinf(cuePhase * TWO_PI) * 1200 * envelope);
      }

      mix = constrain(mix, -12000, 12000);
      samples[i * 2] = samples[i * 2 + 1] = static_cast<int16_t>(mix);
    }

    size_t written = 0;
    i2s_write(AUDIO_I2S_PORT, samples, sizeof(samples), &written, portMAX_DELAY);
  }
}

void startRun(uint32_t now) {
  buildChart();
  score = 0;
  combo = maxCombo = perfects = goods = misses = 0;
  health = 100;
  difficulty = SONGS[selectedSong].difficulty;
  lastJudgment = Judgment::None;
  previousPullState = readPullState();
  runStartedAt = now;
  runStartedAtUs = micros();
  screen = Screen::Playing;
}

void judge(Judgment result, Lane lane, bool bonus = false) {
  lastJudgment = result;
  lastJudgmentLane = lane;
  judgmentShownAt = millis();
  if (result == Judgment::Miss) {
    ++misses;
    combo = 0;
    health -= 12;
  } else {
    ++combo;
    maxCombo = max(maxCombo, combo);
    if (result == Judgment::Perfect) {
      ++perfects;
      health += 3;
    } else {
      ++goods;
      health += 1;
    }
    health = min<int16_t>(health, 100);
    const uint32_t base = result == Judgment::Perfect ? 1000 : 500;
    score += base * (1 + min<uint16_t>(combo / 10, 4)) * (bonus ? 2 : 1);
  }

  if (health <= 0) {
    health = 0;
    screen = Screen::Failed;
    endShownAt = millis();
  }
}

void updateStartupSelfTest(uint32_t now) {
  if (!startupTestActive || screen != Screen::Select) return;

  if (startupCarouselCycles < SELF_TEST_CAROUSEL_CYCLES) {
    if (now - startupTestChangedAt < SELF_TEST_CAROUSEL_INTERVAL_MS) return;
    selectedSong = (selectedSong + 1) % SONG_COUNT;
    carouselSlide = 1;
    carouselChangedAt = now;
    startupTestChangedAt = now;
    ++startupCarouselCycles;
    return;
  }

  // Let the fifth slide settle so its final cover is visible before entering.
  if (now - startupTestChangedAt >= SELF_TEST_FINAL_COVER_HOLD_MS) {
    startupTestActive = false;
    startupAutoPlay = true;
    startRun(now);
  }
}

void updateStartupAutoPlay(uint32_t t) {
  if (!startupAutoPlay) return;
  if (screen != Screen::Playing) {
    startupAutoPlay = false;
    return;
  }

  // Resolve notes at their target timestamps so the unattended display test
  // exercises a complete song instead of failing after the first few notes.
  for (size_t i = 0; i < noteCount; ++i) {
    if (!resolved[i] && t >= chart[i].hitMs) {
      resolved[i] = true;
      judge(Judgment::Perfect, chart[i].lane, chart[i].bonus);
    }
  }
}

Gesture requiredGesture(const NoteDef &note, Lane physical) {
  if (physical == Lane::Twist) {
    return note.variant ? Gesture::TwistRight : Gesture::TwistLeft;
  }
  if (physical == Lane::Pull) {
    return note.variant ? Gesture::PullFull : Gesture::PullHalf;
  }
  return Gesture::None;
}

void handleAction(Lane physicalLane, Gesture gesture, uint32_t t) {
  int best = -1;
  uint32_t bestDelta = UINT32_MAX;

  for (size_t i = 0; i < noteCount; ++i) {
    if (resolved[i] || chart[i].lane != physicalLane) continue;
    const uint32_t delta = abs(static_cast<int32_t>(t - chart[i].hitMs));
    if (delta <= goodWindow() && delta < bestDelta) {
      best = static_cast<int>(i);
      bestDelta = delta;
    }
  }

  if (best < 0) {
    judge(Judgment::Miss, physicalLane);
    return;
  }

  resolved[best] = true;
  if (requiredGesture(chart[best], physicalLane) != gesture) {
    judge(Judgment::Miss, chart[best].lane);
    return;
  }
  judge(bestDelta <= perfectWindow() ? Judgment::Perfect : Judgment::Good,
        chart[best].lane, chart[best].bonus);
}

void updateInputs(uint32_t now) {
  twistLeft.update(now);
  twistRight.update(now);
  pushInput.update(now);
  pullRest.update(now);
  pullFull.update(now);

  const PullState pullState = readPullState();
  const bool halfPullEdge = previousPullState == PullState::Rest &&
                            pullState == PullState::Half;
  const bool fullPullEdge = pullState == PullState::Full &&
                            previousPullState != PullState::Full;
  previousPullState = pullState;
  const bool anyAction = twistLeft.pressedEdge || twistRight.pressedEdge ||
                         pushInput.pressedEdge || halfPullEdge || fullPullEdge;

  if (screen == Screen::Select) {
    // Physical selection is deliberately locked during the deterministic
    // startup carousel test. Controls become active as soon as it finishes.
    if (startupTestActive) return;
    if (twistLeft.pressedEdge) {
      selectedSong = (selectedSong + SONG_COUNT - 1) % SONG_COUNT;
      carouselSlide = -1;
      carouselChangedAt = now;
    }
    if (twistRight.pressedEdge) {
      selectedSong = (selectedSong + 1) % SONG_COUNT;
      carouselSlide = 1;
      carouselChangedAt = now;
    }
    if (pushInput.pressedEdge) startRun(now);
    return;
  }

  if (screen == Screen::Results || screen == Screen::Failed) {
    if (anyAction) {
      screen = Screen::Select;
      carouselChangedAt = now;
    }
    return;
  }

  if (screen == Screen::Paused) {
    if (pushInput.pressedEdge) {
      runStartedAt += now - pausedAt;
      runStartedAtUs += micros() - pausedAtUs;
      screen = Screen::Playing;
    }
    return;
  }
  if (screen != Screen::Playing) return;

  if (pushInput.stable && pullFull.stable) {
    if (pauseChordAt == 0) pauseChordAt = now;
    if (now - pauseChordAt >= 1000) {
      pausedAt = now;
      pausedAtUs = micros();
      screen = Screen::Paused;
      pauseChordAt = 0;
      return;
    }
  } else {
    pauseChordAt = 0;
  }

  if (pullState == PullState::Fault) return;
  const uint32_t t = songTime(now);
  if (twistLeft.pressedEdge) handleAction(Lane::Twist, Gesture::TwistLeft, t);
  if (twistRight.pressedEdge) handleAction(Lane::Twist, Gesture::TwistRight, t);
  if (pushInput.pressedEdge) handleAction(Lane::Push, Gesture::None, t);
  if (halfPullEdge) handleAction(Lane::Pull, Gesture::PullHalf, t);
  if (fullPullEdge) handleAction(Lane::Pull, Gesture::PullFull, t);
}

void expireMisses(uint32_t t) {
  for (size_t i = 0; i < noteCount && screen == Screen::Playing; ++i) {
    if (!resolved[i] && t > chart[i].hitMs + goodWindow()) {
      resolved[i] = true;
      judge(Judgment::Miss, chart[i].lane);
    }
  }
  if (screen == Screen::Playing && t > songDurationMs + 800) {
    screen = Screen::Results;
    endShownAt = millis();
  }
}

void centeredText(const char *text, int y, uint16_t color) {
  display->setTextSize(1);
  display->setTextWrap(false);
  display->setTextColor(color);
  display->setCursor(max(0, (64 - static_cast<int>(strlen(text)) * 6) / 2), y);
  display->print(text);
}

void drawCoverArt(uint8_t index, int x, int y, int size, bool focused) {
  const uint32_t raw = SONGS[index].color;
  const uint8_t r = raw >> 16;
  const uint8_t g = raw >> 8;
  const uint8_t b = raw;
  const uint16_t accent = rgb(r, g, b);
  display->fillRect(x, y, size, size, rgb(r / 14, g / 14, b / 14));
  display->drawRect(x, y, size, size, focused ? rgb(255, 255, 255) : accent);

  if (index == 0) {
    for (int yy = 4; yy < size; yy += 5)
      display->drawFastHLine(x + 1, y + yy, size - 2, rgb(0, 55, 75));
    for (int xx = 3; xx < size; xx += 6)
      display->drawLine(x + size / 2, y + size / 2, x + xx, y + size - 2, accent);
    display->fillCircle(x + size / 2, y + size / 3, max(2, size / 7), rgb(255, 60, 190));
  } else if (index == 1) {
    for (int yy = 2; yy < size - 2; yy += 5)
      for (int xx = 2; xx < size - 2; xx += 5)
        if (((xx + yy) / 5) & 1)
          display->fillRect(x + xx, y + yy, 3, 3, rgb(70, 5, 75));
    display->fillTriangle(x + size / 2, y + 3, x + size - 4, y + size / 2,
                          x + size / 2, y + size - 4, accent);
    display->fillTriangle(x + size / 2, y + 3, x + 4, y + size / 2,
                          x + size / 2, y + size - 4, rgb(60, 0, 100));
  } else {
    display->fillCircle(x + size / 2, y + size / 2, max(3, size / 3), accent);
    for (int yy = y + size / 2; yy < y + size - 3; yy += 4)
      display->drawFastHLine(x + 3, yy, size - 6, rgb(40, 0, 55));
    display->drawLine(x + 2, y + size - 3, x + size / 2, y + 3, rgb(255, 40, 80));
    display->drawLine(x + size - 3, y + size - 3, x + size / 2, y + 3, rgb(255, 40, 80));
  }
}

void drawSelect(uint32_t now) {
  // Start from true black so every pixel not explicitly used is fully off.
  display->fillScreen(0);
  centeredText("SONG SELECT", 0, rgb(0, 255, 255));

  const uint32_t elapsed = now - carouselChangedAt;
  const float progress = elapsed >= 240 ? 1.0f : elapsed / 240.0f;
  const float eased = 1.0f - (1.0f - progress) * (1.0f - progress);
  const int offset = static_cast<int>(carouselSlide * (1.0f - eased) * 34.0f);
  if (progress >= 1.0f) carouselSlide = 0;
  for (int rel = -1; rel <= 1; ++rel) {
    const int index = (selectedSong + rel + SONG_COUNT) % SONG_COUNT;
    constexpr int focusedSize = 28;
    constexpr int sideSize = 22;  // Approximately 20% smaller.
    const int centerX = 32 + rel * 34 + offset;
    const int distanceFromFocus = min(34, abs(centerX - 32));
    const int size = focusedSize -
        distanceFromFocus * (focusedSize - sideSize) / 34;
    const int coverY = 8 + (focusedSize - size) / 2;
    drawCoverArt(index, centerX - size / 2, coverY, size,
                 distanceFromFocus <= 2);
  }

  display->fillTriangle(1, 22, 5, 18, 5, 26, rgb(255, 255, 255));
  display->fillTriangle(62, 22, 58, 18, 58, 26, rgb(255, 255, 255));
  centeredText(SONGS[selectedSong].title, 38, rgb(255, 255, 255));
  char detail[16];
  snprintf(detail, sizeof(detail), "%u BPM  D%u", SONGS[selectedSong].bpm,
           SONGS[selectedSong].difficulty + 1);
  centeredText(detail, 48, rgb(255, 190, 0));
  centeredText("TWIST<> PUSH", 57, rgb(120, 255, 180));
}

int laneX(Lane lane) {
  return lane == Lane::Twist ? 11 : lane == Lane::Push ? 31 : 51;
}

uint16_t noteColor(Lane lane, float brightness) {
  brightness = constrain(brightness, 0.0f, 1.0f);
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  if (lane == Lane::Twist) {
    r = 255; g = 0; b = 0;
  } else if (lane == Lane::Push) {
    r = 0; g = 255; b = 0;
  } else {
    r = 0; g = 80; b = 255;
  }
  // Keep all colour channels stable between frames. Spatial row blending
  // provides the subpixel effect without temporal colour noise.
  return rgb(static_cast<uint8_t>(r * brightness),
             static_cast<uint8_t>(g * brightness),
             static_cast<uint8_t>(b * brightness));
}

void drawInterpolatedRow(int x, int y, int width, Lane lane, float brightness) {
  if (brightness <= 0.01f) return;
  display->drawFastHLine(x, y, width, noteColor(lane, brightness));
}

void drawNote(const NoteDef &note, int x, float y) {
  const int baseY = floorf(y);
  const float fraction = y - baseY;
  // Linear pixel coverage: at a half-pixel position the leading and trailing
  // edge rows each receive 50% brightness while the note body stays constant.
  const float lowerBlend = fraction;
  const float upperBlend = 1.0f - fraction;

  // The two-pixel bar is blended across adjacent rows according to its
  // fractional vertical position, producing smoother apparent movement.
  if (note.lane != Lane::Twist) {
    drawInterpolatedRow(x - 5, baseY - 1, 10, note.lane, upperBlend);
    drawInterpolatedRow(x - 5, baseY, 10, note.lane, 1.0f);
    drawInterpolatedRow(x - 5, baseY + 1, 10, note.lane, lowerBlend);
    return;
  }

  // Twist bars use a 2-pixel-high half on the requested turn side and a
  // 1-pixel-high half on the other side. variant=true means twist right.
  const int tallX = note.variant ? x : x - 5;
  const int shortX = note.variant ? x - 5 : x;
  drawInterpolatedRow(tallX, baseY - 1, 5, note.lane, upperBlend);
  drawInterpolatedRow(tallX, baseY, 5, note.lane, 1.0f);
  drawInterpolatedRow(tallX, baseY + 1, 5, note.lane, lowerBlend);
  drawInterpolatedRow(shortX, baseY, 5, note.lane, upperBlend);
  drawInterpolatedRow(shortX, baseY + 1, 5, note.lane, lowerBlend);
}

void drawGimmickEffect(uint32_t now, uint32_t t) {
  if (gimmickActive(GimmickType::WindGust, t)) {
    // Moving, slightly angled streaks are the only wind telegraph.
    for (int y = 8; y < GIMMICK_SAFE_ZONE_Y; y += 5) {
      const int x = (now / 5 + y * 7) % 76 - 10;
      // Avoid negative/off-panel coordinates in the virtual mapper.
      if (x >= 0 && x + 9 < 64)
        display->drawLine(x, y + 1, x + 9, y, rgb(35, 130, 180));
    }
  }
}

void drawPlaying(uint32_t now) {
  const uint32_t t = songTime(now);
  const uint32_t preciseElapsedUs = screen == Screen::Paused
                                        ? pausedAtUs - runStartedAtUs
                                        : micros() - runStartedAtUs;
  const float preciseTimeMs = preciseElapsedUs * 0.001f;
  display->fillScreen(0);
  // Only the internal separators are needed; coloured outer borders can look
  // like stray pixels beside the left-most and right-most lanes.
  for (int x : {21, 41})
    display->drawFastVLine(x, 7, 50, rgb(45, 45, 45));
  display->fillRect(2, 0, health * 60 / 100, 3,
                    health > 30 ? rgb(0, 255, 100) : rgb(255, 30, 20));

  const bool wind = gimmickActive(GimmickType::WindGust, t);
  drawGimmickEffect(now, t);
  int drawnX[24]{};
  int drawnY[24]{};
  uint8_t drawnCount = 0;
  constexpr int collisionOffsets[] = {0, -10, 10, -20, 20, -30, 30};

  for (size_t i = 0; i < noteCount; ++i) {
    if (resolved[i]) continue;
    const float until = chart[i].hitMs - preciseTimeMs;
    if (until < -200 || until > NOTE_TRAVEL_MS) continue;
    const float y = NOTE_HIT_Y -
        until * static_cast<float>(NOTE_HIT_Y - NOTE_TOP_Y) / NOTE_TRAVEL_MS;
    if (y < NOTE_TOP_Y || y > 61) continue;
    const int collisionY = lroundf(y);

    int x = laneX(chart[i].lane);
    if (wind && y < GIMMICK_SAFE_ZONE_Y) {
      const int direction = ((i + selectedSong) & 1) ? 1 : -1;
      // A gust can visibly carry a note about one full lane sideways. The
      // note's color still communicates which physical action it requires.
      const int strength = 4 + (GIMMICK_SAFE_ZONE_Y - y) / 2;
      x = constrain(x + direction * strength, 6, 58);
    }

    // Find a nearby non-overlapping horizontal slot. With 10x2 bars and a
    // dim antialiasing edge, centres need 10 horizontal or 3 vertical pixels.
    const int desiredX = x;
    bool placed = false;
    for (int offset : collisionOffsets) {
      const int candidateX = desiredX + offset;
      if (candidateX < 6 || candidateX > 58) continue;
      bool collision = false;
      for (uint8_t n = 0; n < drawnCount; ++n) {
        if (abs(candidateX - drawnX[n]) < 10 &&
            abs(collisionY - drawnY[n]) < 3) {
          collision = true;
          break;
        }
      }
      if (!collision) {
        x = candidateX;
        placed = true;
        break;
      }
    }
    if (!placed) continue;

    drawNote(chart[i], x, y);
    if (drawnCount < sizeof(drawnX) / sizeof(drawnX[0])) {
      drawnX[drawnCount] = x;
      drawnY[drawnCount] = collisionY;
      ++drawnCount;
    }
  }

  // Draw last so the lane separators cannot tint any timing-bar pixels.
  display->drawFastHLine(2, NOTE_HIT_Y, 60, rgb(255, 255, 255));

  display->setTextSize(1);
  display->setTextColor(rgb(255, 255, 255));
  display->setCursor(3, 57);
  display->printf("%u", combo);

  if (readPullState() == PullState::Fault)
    centeredText("PULL FAULT", 17, rgb(255, 20, 20));
}

void drawEnd(bool failed) {
  display->fillScreen(0);
  centeredText(failed ? "GAME OVER" : "RESULT", 5,
               failed ? rgb(255, 30, 30) : rgb(0, 255, 255));
  display->setTextSize(1);
  display->setTextColor(rgb(255, 255, 255));
  display->setCursor(4, 18);
  display->printf("SCORE %lu", static_cast<unsigned long>(score));
  display->setCursor(4, 28);
  display->printf("P%u G%u M%u", perfects, goods, misses);
  display->setCursor(4, 38);
  display->printf("MAX COMBO %u", maxCombo);
  const uint16_t hits = perfects + goods;
  const uint8_t accuracy = noteCount ? hits * 100 / noteCount : 0;
  const char grade = accuracy >= 95 ? 'S' : accuracy >= 85 ? 'A' :
                     accuracy >= 70 ? 'B' : accuracy >= 55 ? 'C' : 'D';
  display->setTextSize(2);
  display->setCursor(26, 48);
  display->setTextColor(rgb(255, 190, 0));
  display->print(grade);
}

void render(uint32_t now) {
  if (screen == Screen::Select) {
    drawSelect(now);
  } else if (screen == Screen::Playing) {
    drawPlaying(now);
  } else if (screen == Screen::Paused) {
    drawPlaying(pausedAt);
    display->fillRect(8, 22, 48, 20, rgb(5, 0, 18));
    centeredText("PAUSED", 25, rgb(255, 60, 200));
    centeredText("PUSH", 34, rgb(255, 255, 255));
  } else {
    drawEnd(screen == Screen::Failed);
  }
}

void setup() {
  Serial.begin(115200);
  twistLeft.begin();
  twistRight.begin();
  pushInput.begin();
  pullRest.begin();
  pullFull.begin();
  previousPullState = readPullState();

  HUB75_I2S_CFG config(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  config.gpio.r1 = HUB75_R1;
  config.gpio.g1 = HUB75_G1;
  config.gpio.b1 = HUB75_B1;
  config.gpio.r2 = HUB75_R2;
  config.gpio.g2 = HUB75_G2;
  config.gpio.b2 = HUB75_B2;
  config.gpio.a = HUB75_A;
  config.gpio.b = HUB75_B;
  config.gpio.c = HUB75_C;
  config.gpio.d = HUB75_D;
  config.gpio.e = HUB75_E;
  config.gpio.lat = HUB75_LAT;
  config.gpio.oe = HUB75_OE;
  config.gpio.clk = HUB75_CLK;
  // Render into a hidden framebuffer, then swap it into view. Without this,
  // the panel scans partially cleared/partially drawn frames as they are built.
  config.double_buff = true;
  // Keep the HUB75 clock at 8 MHz for signal integrity across both ribbon
  // cables. Five-bit colour reduces BCM scan time enough to maintain refresh.
  config.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  config.min_refresh_rate = 120;
  config.setPixelColorDepthBits(5);

  dmaDisplay = new MatrixPanel_I2S_DMA(config);
  if (!dmaDisplay->begin()) {
    Serial.println(F("HUB75 DMA allocation failed"));
    return;
  }
  // Brightness calls made before begin() are ignored by this library.
  dmaDisplay->setBrightness8(50);

  // A back buffer must remain visible for at least one complete HUB75 scan.
  // Add 1.5 ms for scheduler jitter and cap presentation at 90 FPS even when
  // the reported panel refresh is unusually high.
  if (dmaDisplay->calculated_refresh_rate > 0) {
    const uint32_t panelFrameUs =
        1000000UL / dmaDisplay->calculated_refresh_rate;
    renderIntervalUs = max<uint32_t>(11111UL, panelFrameUs + 1500UL);
  }

  display = new VirtualMatrixPanel_T<PANEL_CHAIN_TYPE>(
      PANEL_ROWS, PANEL_COLS, PANEL_RES_X, PANEL_RES_Y);
  display->setDisplay(*dmaDisplay);
  display->clearScreen();
  audioReady = initAudio();
  if (audioReady) {
    xTaskCreatePinnedToCore(audioTask, "bop-audio", 4096, nullptr, 2, nullptr, 0);
  }
  carouselChangedAt = millis();
  startupTestChangedAt = carouselChangedAt;
  Serial.printf("BOP Rhythm ready; PCM5102A audio %s\n",
                audioReady ? "enabled" : "disabled");
  Serial.printf("HUB75 refresh %u Hz; presenting every %lu us\n",
                dmaDisplay->calculated_refresh_rate,
                static_cast<unsigned long>(renderIntervalUs));
  Serial.println(F("GPIO34/35/36/39 require external pull-ups"));
}

void loop() {
  if (display == nullptr) {
    delay(1000);
    return;
  }

  const uint32_t now = millis();
  updateInputs(now);
  updateStartupSelfTest(now);
  if ((screen == Screen::Results || screen == Screen::Failed) &&
      now - endShownAt >= 4500) {
    screen = Screen::Select;
    carouselChangedAt = now;
  }
  if (screen == Screen::Playing) {
    const uint32_t t = songTime(now);
    updateStartupAutoPlay(t);
    expireMisses(t);
  }

  static uint32_t lastFrameUs = 0;
  const uint32_t frameNowUs = micros();
  if (frameNowUs - lastFrameUs >= renderIntervalUs) {
    // Advance by a fixed interval for consistent subpixel brightness steps.
    // Resynchronise after a large stall rather than trying to catch up frames.
    if (frameNowUs - lastFrameUs > renderIntervalUs * 4) {
      lastFrameUs = frameNowUs;
    } else {
      lastFrameUs += renderIntervalUs;
    }
    render(now);
    dmaDisplay->flipDMABuffer();
  }
  delay(1);
}

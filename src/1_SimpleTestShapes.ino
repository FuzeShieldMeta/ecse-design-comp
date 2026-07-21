#include <Arduino.h>
#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>
#include <Fonts/Picopixel.h>
#include <driver/i2s.h>
#include <new>

#include "SongImport.h"

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
#define SELF_TEST_CAROUSEL_CYCLES 3
#define SELF_TEST_CAROUSEL_INTERVAL_MS 2000UL
#define SELF_TEST_FINAL_COVER_HOLD_MS 1000UL
#define SELF_TEST_DIFFICULTY_INTERVAL_MS 2000UL
#define SELF_TEST_NEXT_COVER_HOLD_MS 1000UL
#define DEFAULT_RENDER_INTERVAL_US 20000UL
#define ENABLE_NOTE_SUBPIXEL_BLEND 0

MatrixPanel_I2S_DMA *dmaDisplay = nullptr;
VirtualMatrixPanel_T<PANEL_CHAIN_TYPE> *display = nullptr;

enum class Lane : uint8_t { Twist, Push, Pull };
enum class Gesture : uint8_t { None, TwistLeft, TwistRight, PullHalf, PullFull };
enum class Judgment : uint8_t { None, Perfect, Good, Miss };
enum class Screen : uint8_t {
  Select, DifficultySelect, Playing, Paused, Results, Failed
};
enum class DisplayProfile : uint8_t { None, Select, Game };
enum class GimmickType : uint8_t {
  WindGust, ScreenFlash, LanePulse, CommanderApproach
};
enum class PullState : uint8_t { Rest, Half, Full, Fault };
enum class StartupTestPhase : uint8_t {
  Carousel, FinalCover, DifficultyWait
};

struct NoteDef {
  uint32_t hitMs;
  Lane lane;
  bool variant;  // left/right for twist, half/full for pull
  bool bonus;
  uint16_t holdMs;
  bool endVariant;  // Pull state after an optional in-hold transition.
  uint16_t pullTransitionMs;
  uint8_t startColumn;
  uint8_t endColumn;
  uint32_t shiftStartMs;
  uint32_t shiftEndMs;
};

struct GimmickDef {
  uint16_t id;
  uint32_t startMs;
  uint32_t durationMs;
  GimmickType type;
  int8_t target;
  uint32_t color;
  float brightness;
  float rateHz;
  float speed;
  int8_t direction;
  uint8_t density;
  BopImport::GimmickPattern pattern;
};

constexpr size_t MAX_NOTES = 220;
constexpr size_t GIMMICK_COUNT = BopImport::MAX_GIMMICKS;
constexpr int NOTE_HIT_Y = 58;
constexpr int NOTE_TOP_Y = 0;
constexpr int COMMANDER_GAME_Y = -20;
constexpr int COMMANDER_STAGE_Y = 0;
constexpr uint16_t COMMANDER_APPROACH_MS = 520;
constexpr uint16_t GAME_INTRO_HOLD_MS = 900;
constexpr uint16_t GAME_INTRO_RETREAT_MS = 1300;
constexpr uint16_t GAME_INTRO_READY_MS = 400;
constexpr uint32_t GAME_INTRO_MS = GAME_INTRO_HOLD_MS +
                                   GAME_INTRO_RETREAT_MS +
                                   GAME_INTRO_READY_MS;
constexpr uint16_t EARTH_TARGET_MS = 650;
constexpr uint16_t FAILURE_CINEMATIC_MS = 3200;
constexpr uint16_t EARTH_COUNTERATTACK_MS = 800;
constexpr uint16_t VICTORY_CINEMATIC_MS = 3000;
constexpr uint16_t TURRET_FIRE_MS = 170;
constexpr int GIMMICK_SAFE_ZONE_Y = 26;  // Bottom 60% begins here.
constexpr int32_t NOTE_TRAVEL_MS = 1800;
constexpr uint16_t POST_HIT_DISPLAY_MS = 160;
constexpr uint16_t PERFECT_BASE_POINTS = 1000;
constexpr uint16_t GOOD_BASE_POINTS = 650;
constexpr uint16_t HOLD_POINTS_PER_SECOND = 900;
constexpr int NOTE_WIDTH = 11;
constexpr int TWIST_REQUIRED_WIDTH = 8;
constexpr int TWIST_OTHER_WIDTH = 2;
constexpr int LANE_LEFT_X[3] = {0, 22, 42};
constexpr int LANE_DRAW_WIDTH[3] = {21, 19, 22};
constexpr int HEALTH_BAR_WIDTH = 64;
constexpr uint16_t HEALTH_REGEN_FLASH_MS = 350;
constexpr uint8_t SELECT_COLOR_DEPTH_BITS = 8;
constexpr uint8_t GAME_COLOR_DEPTH_BITS = 5;
constexpr uint32_t SELECT_RENDER_INTERVAL_US = 33333UL;  // 30 FPS
constexpr int32_t SAFE_ZONE_LEAD_MS =
    NOTE_TRAVEL_MS * (NOTE_HIT_Y - GIMMICK_SAFE_ZONE_Y) /
    (NOTE_HIT_Y - NOTE_TOP_Y);

NoteDef chart[MAX_NOTES]{};
bool resolved[MAX_NOTES]{};
bool holding[MAX_NOTES]{};
Judgment holdStartJudgment[MAX_NOTES]{};
uint32_t noteVisibleUntilMs[MAX_NOTES]{};
uint32_t holdLastScoreAt[MAX_NOTES]{};
uint32_t holdScoreAccumulator[MAX_NOTES]{};
GimmickDef gimmicks[GIMMICK_COUNT]{};
size_t noteCount = 0;
uint32_t songDurationMs = 0;
volatile uint8_t selectedSong = 0;
volatile uint8_t selectedDifficulty = 1;
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
DisplayProfile activeDisplayProfile = DisplayProfile::None;
uint8_t activeDmaColorDepth = 0;
bool rebuildDisplay(DisplayProfile profile);
PullState previousPullState = PullState::Rest;
volatile uint32_t runStartedAt = 0;
volatile uint32_t runStartedAtUs = 0;
uint32_t introStartedAt = 0;
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
float healthRecoveryBank = 0.0f;
float displayedHealth = 100.0f;
uint32_t healthDamageAt = 0;
uint32_t healthAnimationAt = 0;
uint32_t healthRegenAt = 0;
uint8_t healthRegenFromWidth = 0;
uint8_t healthRegenToWidth = 0;
Judgment lastJudgment = Judgment::None;
Lane lastJudgmentLane = Lane::Push;
uint32_t judgmentShownAt = 0;
Judgment laneJudgments[3] = {
  Judgment::None, Judgment::None, Judgment::None
};
uint32_t laneJudgmentShownAt[3]{};
bool audioReady = false;
uint32_t renderIntervalUs = DEFAULT_RENDER_INTERVAL_US;
bool startupTestActive = STARTUP_SELF_TEST;
bool startupAutoPlay = false;
bool startupDemoRestartPending = false;
bool startupDemoDifficultyPending = false;
bool startupRevengeSequenceActive = false;
uint8_t startupRevengeRunsCompleted = 0;
uint8_t startupCarouselCycles = 0;
uint32_t startupTestChangedAt = 0;
StartupTestPhase startupTestPhase = StartupTestPhase::Carousel;
volatile uint32_t audioRunGeneration = 0;
BopImport::Chart importedChart{};
uint8_t songCovers[BopImport::MAX_SONGS][BopImport::COVER_RGB888_BYTES]{};
bool songCoverLoaded[BopImport::MAX_SONGS]{};

size_t songCount() {
  return BopImport::songCount();
}

const BopImport::Song *importedSong(size_t index) {
  return BopImport::song(index);
}

const char *songTitle(size_t index) {
  const BopImport::Song *external = importedSong(index);
  return external != nullptr ? external->title : "NO SONG";
}

const char *songArtist(size_t index) {
  const BopImport::Song *external = importedSong(index);
  return external != nullptr ? external->artist : "";
}

uint16_t songBpm(size_t index) {
  const BopImport::Song *external = importedSong(index);
  return external != nullptr ? external->bpm : 120;
}

uint32_t songCoverColor(size_t index) {
  const BopImport::Song *external = importedSong(index);
  return external != nullptr ? external->color : 0;
}

void loadSongCovers() {
  for (size_t index = 0; index < songCount(); ++index) {
    songCoverLoaded[index] = BopImport::loadCover(
        index, songCovers[index], BopImport::COVER_RGB888_BYTES);
    if (!songCoverLoaded[index])
      Serial.printf("Cover unavailable or invalid for song %u\n",
                    static_cast<unsigned>(index));
  }
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return dmaDisplay->color565(r, g, b);
}

uint32_t songTime(uint32_t now) {
  const int32_t elapsed = static_cast<int32_t>(now - runStartedAt);
  // A run can begin after the caller captured its frame timestamp because the
  // display-profile rebuild takes time. Never let that older timestamp wrap
  // into a multi-billion-millisecond song time and instantly finish the chart.
  return elapsed < 0 ? 0 : static_cast<uint32_t>(elapsed);
}

bool songClockStarted(uint32_t now) {
  return static_cast<int32_t>(now - runStartedAt) >= 0;
}

uint16_t perfectWindow() {
  return 70;
}

uint16_t goodWindow() {
  return 130;
}

PullState readPullState() {
  if (pullRest.stable && pullFull.stable) return PullState::Fault;
  if (pullFull.stable) return PullState::Full;
  if (pullRest.stable) return PullState::Rest;
  return PullState::Half;
}

bool buildImportedChart() {
  if (!BopImport::loadChart(selectedSong, selectedDifficulty, importedChart)) {
    Serial.printf("Chart import failed for song %u mapping %u\n",
                  selectedSong, selectedDifficulty);
    return false;
  }

  noteCount = min(importedChart.noteCount, MAX_NOTES);
  for (size_t i = 0; i < noteCount; ++i) {
    const BopImport::ChartNote &source = importedChart.notes[i];
    chart[i] = {
      source.hitMs,
      static_cast<Lane>(source.lane),
      source.variant,
      source.bonus,
      source.holdMs,
      source.endVariant,
      source.transitionMs,
      source.lane,
      source.lane,
      0,
      0
    };
  }
  for (size_t i = 0; i < GIMMICK_COUNT; ++i) gimmicks[i].durationMs = 0;
  const size_t importedGimmicks = min(importedChart.gimmickCount,
                                      GIMMICK_COUNT);
  for (size_t i = 0; i < importedGimmicks; ++i) {
    const BopImport::ChartGimmick &source = importedChart.gimmicks[i];
    const GimmickType type = source.type == BopImport::GimmickType::Wind
        ? GimmickType::WindGust
        : source.type == BopImport::GimmickType::ScreenFlash
            ? GimmickType::ScreenFlash
            : source.type == BopImport::GimmickType::Commander
                ? GimmickType::CommanderApproach : GimmickType::LanePulse;
    gimmicks[i] = {source.id, source.startMs, source.durationMs, type,
                   source.target, source.color, source.brightness,
                   source.rateHz, source.speed, source.direction,
                   source.density, source.pattern};
  }
  for (size_t effectIndex = 0;
       effectIndex < importedChart.gimmickNoteEffectCount; ++effectIndex) {
    const BopImport::ChartGimmickNoteEffect &effect =
        importedChart.gimmickNoteEffects[effectIndex];
    const BopImport::ChartGimmick *sourceGimmick = nullptr;
    for (size_t i = 0; i < importedGimmicks; ++i) {
      if (importedChart.gimmicks[i].id == effect.gimmickId) {
        sourceGimmick = &importedChart.gimmicks[i];
        break;
      }
    }
    if (sourceGimmick == nullptr) continue;
    for (size_t noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
      if (importedChart.notes[noteIndex].id != effect.noteId) continue;
      chart[noteIndex].endColumn = effect.targetColumn;
      chart[noteIndex].shiftStartMs = sourceGimmick->startMs +
                                      effect.startOffsetMs;
      chart[noteIndex].shiftEndMs = sourceGimmick->startMs +
                                    effect.endOffsetMs;
      break;
    }
  }
  songDurationMs = importedChart.durationMs;
  return true;
}

bool buildChart() {
  const bool loaded = buildImportedChart();
  if (!loaded) {
    noteCount = 0;
    songDurationMs = 3000;
    for (GimmickDef &gimmick : gimmicks) gimmick.durationMs = 0;
  }
  memset(resolved, 0, sizeof(resolved));
  memset(holding, 0, sizeof(holding));
  memset(noteVisibleUntilMs, 0, sizeof(noteVisibleUntilMs));
  memset(holdLastScoreAt, 0, sizeof(holdLastScoreAt));
  memset(holdScoreAccumulator, 0, sizeof(holdScoreAccumulator));
  for (Judgment &result : holdStartJudgment) result = Judgment::None;
  return loaded;
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
  constexpr float IMPORTED_AUDIO_GAIN = 0.35f;
  int16_t samples[FRAME_COUNT * 2];
  BopImport::WavReader wav;
  uint32_t loadedGeneration = UINT32_MAX;
  bool wavOpenAttempted = false;

  for (;;) {
    const uint32_t now = millis();
    const bool active = screen == Screen::Playing && songClockStarted(now);
    const uint32_t t = active ? songTime(now) : 0;
    const uint8_t songIndex = selectedSong;
    const BopImport::Song *song = importedSong(songIndex);
    const uint32_t generation = audioRunGeneration;
    if (generation != loadedGeneration) {
      wav.close();
      wavOpenAttempted = false;
      loadedGeneration = generation;
    }

    memset(samples, 0, sizeof(samples));
    if (active && song != nullptr && t >= song->audioStartMs &&
        !wavOpenAttempted) {
      wavOpenAttempted = true;
      if (!wav.open(song->audioPath))
        Serial.printf("WAV unavailable or unsupported: %s\n", song->audioPath);
    }
    if (active && song != nullptr && t >= song->audioStartMs &&
        wav.isOpen()) {
      size_t produced = 0;
      while (produced < FRAME_COUNT) {
        size_t frames = wav.readStereo(samples + produced * 2,
                                       FRAME_COUNT - produced);
        if (frames == 0) {
          if (!song->audioLoop || !wav.rewind()) break;
          frames = wav.readStereo(samples + produced * 2,
                                  FRAME_COUNT - produced);
          if (frames == 0) break;
        }
        produced += frames;
        if (produced == FRAME_COUNT || !song->audioLoop || !wav.rewind()) break;
      }
      for (size_t i = 0; i < produced * 2; ++i)
        samples[i] = static_cast<int16_t>(samples[i] * IMPORTED_AUDIO_GAIN);
    }

    size_t written = 0;
    i2s_write(AUDIO_I2S_PORT, samples, sizeof(samples), &written, portMAX_DELAY);
  }
}

bool startRun(uint32_t now) {
  if (songCount() == 0 || selectedSong >= songCount()) return false;
  Serial.printf("Starting song %u mapping %u\n", selectedSong,
                selectedDifficulty);
  if (!buildChart()) {
    Serial.println(F("Selected chart mapping failed to load; run not started"));
    return false;
  }
  // Stop and release the high-colour selection buffers before timing or audio
  // begins, then allocate the faster gameplay profile from a clean heap.
  if (!rebuildDisplay(DisplayProfile::Game)) {
    Serial.println(F("Gameplay display transition failed; run not started"));
    return false;
  }
  now = millis();
  score = 0;
  combo = maxCombo = perfects = goods = misses = 0;
  health = 100;
  healthRecoveryBank = 0.0f;
  displayedHealth = 100.0f;
  healthDamageAt = 0;
  healthAnimationAt = now;
  healthRegenAt = 0;
  healthRegenFromWidth = healthRegenToWidth = HEALTH_BAR_WIDTH;
  lastJudgment = Judgment::None;
  for (uint8_t lane = 0; lane < 3; ++lane) {
    laneJudgments[lane] = Judgment::None;
    laneJudgmentShownAt[lane] = 0;
  }
  previousPullState = readPullState();
  introStartedAt = now;
  runStartedAt = now + GAME_INTRO_MS;
  runStartedAtUs = micros() + GAME_INTRO_MS * 1000UL;
  ++audioRunGeneration;
  screen = Screen::Playing;
  return true;
}

uint8_t scoreMultiplier() {
  return 1 + min<uint16_t>(combo / 10, 4);
}

void awardRawScore(uint32_t rawPoints, bool bonus) {
  score += rawPoints * scoreMultiplier() * (bonus ? 2 : 1);
}

void scoreHoldFrame(size_t index, uint32_t t) {
  const NoteDef &note = chart[index];
  const uint32_t holdEnd = note.hitMs + note.holdMs;
  const uint32_t scoreTo = min(t, holdEnd);
  if (holdLastScoreAt[index] == 0)
    holdLastScoreAt[index] = max(t, note.hitMs);
  if (scoreTo <= holdLastScoreAt[index]) return;

  const uint32_t elapsed = scoreTo - holdLastScoreAt[index];
  holdLastScoreAt[index] = scoreTo;
  const uint8_t accuracyPercent =
      holdStartJudgment[index] == Judgment::Perfect ? 100 : 65;
  // Accumulate thousandths of time-scaled raw points so scoring remains
  // independent of the actual frame interval.
  holdScoreAccumulator[index] +=
      elapsed * HOLD_POINTS_PER_SECOND * accuracyPercent;
  const uint32_t rawPoints = holdScoreAccumulator[index] / 100000UL;
  holdScoreAccumulator[index] %= 100000UL;
  if (rawPoints > 0) awardRawScore(rawPoints, note.bonus);
}

void judge(Judgment result, Lane lane, bool bonus = false) {
  lastJudgment = result;
  lastJudgmentLane = lane;
  judgmentShownAt = millis();
  const uint8_t laneIndex = static_cast<uint8_t>(lane);
  laneJudgments[laneIndex] = result;
  laneJudgmentShownAt[laneIndex] = judgmentShownAt;
  if (result == Judgment::Miss) {
    ++misses;
    combo = 0;
    health -= 12;
    healthDamageAt = judgmentShownAt;
    healthAnimationAt = judgmentShownAt;
  } else {
    const int16_t healthBefore = health;
    ++combo;
    maxCombo = max(maxCombo, combo);
    if (result == Judgment::Perfect) {
      ++perfects;
      healthRecoveryBank += 0.50f;
    } else {
      ++goods;
      healthRecoveryBank += 0.25f;
    }
    if (health >= 100) {
      // Do not bank regeneration while already full for use after a later miss.
      healthRecoveryBank = 0.0f;
    } else {
      while (healthRecoveryBank >= 1.0f && health < 100) {
        ++health;
        healthRecoveryBank -= 1.0f;
      }
      if (health >= 100) healthRecoveryBank = 0.0f;
    }
    health = min<int16_t>(health, 100);
    if (health > healthBefore) {
      const uint8_t previousWidth = constrain(
          healthBefore * HEALTH_BAR_WIDTH / 100, 0, HEALTH_BAR_WIDTH);
      const uint8_t regeneratedWidth = constrain(
          health * HEALTH_BAR_WIDTH / 100, 0, HEALTH_BAR_WIDTH);
      if (regeneratedWidth > previousWidth) {
        healthRegenFromWidth = previousWidth;
        healthRegenToWidth = regeneratedWidth;
        healthRegenAt = judgmentShownAt;
      }
    }
    displayedHealth = max(displayedHealth, static_cast<float>(health));
    const uint32_t base = result == Judgment::Perfect
                              ? PERFECT_BASE_POINTS : GOOD_BASE_POINTS;
    awardRawScore(base, bonus);
  }

  if (health <= 0) {
    health = 0;
    screen = Screen::Failed;
    endShownAt = millis();
  }
}

void updateStartupSelfTest(uint32_t now) {
  if (!startupTestActive) return;
  if (songCount() == 0) {
    startupTestActive = false;
    return;
  }

  if (startupTestPhase == StartupTestPhase::Carousel) {
    // Preview linearly through the manifest to Revenge without wrapping.
    const uint8_t startupStepLimit = min<size_t>(
        SELF_TEST_CAROUSEL_CYCLES, songCount() - 1);
    if (startupCarouselCycles < startupStepLimit) {
      if (now - startupTestChangedAt < SELF_TEST_CAROUSEL_INTERVAL_MS) return;
      ++selectedSong;
      carouselSlide = 1;
      carouselChangedAt = now;
      startupTestChangedAt = now;
      ++startupCarouselCycles;
      if (startupCarouselCycles == startupStepLimit)
        startupTestPhase = StartupTestPhase::FinalCover;
      return;
    }
    startupTestPhase = StartupTestPhase::FinalCover;
    startupTestChangedAt = now;
    return;
  }

  if (startupTestPhase == StartupTestPhase::FinalCover) {
    if (now - startupTestChangedAt < SELF_TEST_FINAL_COVER_HOLD_MS) return;
    // Preserve the final carousel selection (Revenge in the current manifest)
    // and overlay its difficulty picker without clearing either select buffer.
    carouselSlide = 0;
    selectedDifficulty = 0;
    screen = Screen::DifficultySelect;
    carouselChangedAt = now;
    startupTestPhase = StartupTestPhase::DifficultyWait;
    startupTestChangedAt = now;
    Serial.printf("Startup demo difficulty popup: song %u\n", selectedSong);
    return;
  }

  if (startupTestPhase == StartupTestPhase::DifficultyWait &&
      screen == Screen::DifficultySelect &&
      now - startupTestChangedAt >= SELF_TEST_DIFFICULTY_INTERVAL_MS) {
    // Start with EASY. The results transition will return to this same song
    // for NORM and HARD before the circular song carousel resumes.
    if (startRun(now)) {
      startupTestActive = false;
      startupAutoPlay = true;
      startupRevengeSequenceActive = true;
      startupRevengeRunsCompleted = 0;
    } else {
      // Leave the popup/test state intact and retry after a short visible hold.
      startupTestChangedAt = now;
    }
  }
}

void updateStartupAutoPlay(uint32_t t) {
  if (!startupAutoPlay) return;
  if (screen != Screen::Playing) {
    startupAutoPlay = false;
    return;
  }

  // Resolve notes at their target timestamps so the unattended display test
  // exercises Perfect, Good, and Miss feedback as well as sustained notes.
  for (size_t i = 0; i < noteCount; ++i) {
    if (resolved[i]) continue;

    // Hold notes are completed perfectly so their full tails remain visible.
    if (chart[i].holdMs > 0 && t < chart[i].hitMs + chart[i].holdMs) {
      if (t < chart[i].hitMs) continue;
      if (!holding[i]) {
        holdLastScoreAt[i] = chart[i].hitMs;
        holdScoreAccumulator[i] = 0;
      }
      holding[i] = true;
      holdStartJudgment[i] = Judgment::Perfect;
      scoreHoldFrame(i, t);
      continue;
    }
    if (chart[i].holdMs > 0) {
      if (holdStartJudgment[i] == Judgment::None) {
        holdStartJudgment[i] = Judgment::Perfect;
        holdLastScoreAt[i] = chart[i].hitMs;
      }
      scoreHoldFrame(i, t);
      holding[i] = false;
      resolved[i] = true;
      noteVisibleUntilMs[i] = chart[i].hitMs + chart[i].holdMs +
                              POST_HIT_DISPLAY_MS;
      judge(Judgment::Perfect, chart[i].lane, chart[i].bonus);
      continue;
    }

    const uint8_t example = i % 12;
    // Example 2 intentionally passes the full window and is expired as a miss
    // by expireMisses(). The wider spacing lets the full demo song complete.
    if (example == 2) continue;
    const bool goodExample = example == 1 || example == 5 || example == 9;
    const uint32_t demonstrationTime = chart[i].hitMs +
        (goodExample ? perfectWindow() + 10 : 0);
    if (t < demonstrationTime) continue;

    holding[i] = false;
    resolved[i] = true;
    noteVisibleUntilMs[i] = chart[i].hitMs + POST_HIT_DISPLAY_MS;
    judge(goodExample ? Judgment::Good : Judgment::Perfect,
          chart[i].lane, chart[i].bonus);
  }
}

void updateRepeatingDemo(uint32_t now) {
  if (!startupAutoPlay) return;

  if (screen == Screen::Results || screen == Screen::Failed) {
    const uint32_t resultHold = 4000UL;
    if (now - endShownAt < resultHold) return;
    if (songCount() == 0) {
      startupAutoPlay = false;
      return;
    }

    // Run Revenge consecutively on EASY, NORM, and HARD. Only after HARD has
    // finished does the ordinary circular song sequence advance to song zero.
    if (startupRevengeSequenceActive && startupRevengeRunsCompleted < 2) {
      ++startupRevengeRunsCompleted;
      selectedDifficulty = startupRevengeRunsCompleted;
      carouselSlide = 0;
      Serial.printf("Startup demo repeats Revenge on mapping %u\n",
                    selectedDifficulty);
    } else {
      startupRevengeSequenceActive = false;
      selectedSong = (selectedSong + 1) % songCount();
      carouselSlide = 1;
    }
    carouselChangedAt = now;
    startupTestChangedAt = now;
    startupDemoRestartPending = true;
    startupDemoDifficultyPending = false;
    screen = Screen::Select;
    return;
  }

  if (screen == Screen::Select && startupDemoRestartPending &&
      now - startupTestChangedAt >= SELF_TEST_NEXT_COVER_HOLD_MS) {
    startupDemoRestartPending = false;
    startupDemoDifficultyPending = true;
    startupTestChangedAt = now;
    carouselChangedAt = now;
    screen = Screen::DifficultySelect;
    return;
  }

  if (screen == Screen::DifficultySelect && startupDemoDifficultyPending &&
      now - startupTestChangedAt >= SELF_TEST_DIFFICULTY_INTERVAL_MS) {
    if (startRun(now)) {
      startupDemoDifficultyPending = false;
    } else {
      startupTestChangedAt = now;
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

  if (requiredGesture(chart[best], physicalLane) != gesture) {
    resolved[best] = true;
    judge(Judgment::Miss, chart[best].lane);
    return;
  }
  const Judgment timing = bestDelta <= perfectWindow()
                              ? Judgment::Perfect : Judgment::Good;
  if (chart[best].holdMs > 0) {
    holding[best] = true;
    holdStartJudgment[best] = timing;
    holdLastScoreAt[best] = max(t, chart[best].hitMs);
    holdScoreAccumulator[best] = 0;
  } else {
    resolved[best] = true;
    noteVisibleUntilMs[best] = chart[best].hitMs + POST_HIT_DISPLAY_MS;
    judge(timing, chart[best].lane, chart[best].bonus);
  }
}

bool requiredHoldActive(const NoteDef &note, uint32_t t) {
  if (note.lane == Lane::Twist)
    return note.variant ? twistRight.stable : twistLeft.stable;
  if (note.lane == Lane::Pull) {
    const PullState state = readPullState();
    if (note.pullTransitionMs > 0) {
      const uint32_t transitionAt = note.hitMs + note.pullTransitionMs;
      const uint32_t transitionDelta = abs(static_cast<int32_t>(t - transitionAt));
      // Either deliberate pull position is accepted around the state-change
      // marker, giving the player the same timing tolerance as a normal hit.
      if (transitionDelta <= goodWindow())
        return state == PullState::Half || state == PullState::Full;
      const bool requiresFull = t < transitionAt
                                    ? note.variant : note.endVariant;
      return requiresFull ? state == PullState::Full
                          : state == PullState::Half;
    }
    return note.variant ? state == PullState::Full
                        : state == PullState::Half;
  }
  return pushInput.stable;
}

void updateHolds(uint32_t t) {
  if (startupAutoPlay) return;
  for (size_t i = 0; i < noteCount && screen == Screen::Playing; ++i) {
    if (!holding[i] || resolved[i]) continue;
    if (t >= chart[i].hitMs + chart[i].holdMs) {
      scoreHoldFrame(i, t);
      holding[i] = false;
      resolved[i] = true;
      noteVisibleUntilMs[i] = chart[i].hitMs + chart[i].holdMs +
                              POST_HIT_DISPLAY_MS;
      judge(holdStartJudgment[i], chart[i].lane, chart[i].bonus);
    } else if (!requiredHoldActive(chart[i], t)) {
      holding[i] = false;
      resolved[i] = true;
      judge(Judgment::Miss, chart[i].lane);
    } else {
      scoreHoldFrame(i, t);
    }
  }
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
    // carousel test and its repeating autoplay song transitions.
    if (startupTestActive || startupAutoPlay || songCount() == 0) return;
    if (twistLeft.pressedEdge) {
      selectedSong = (selectedSong + songCount() - 1) % songCount();
      carouselSlide = -1;
      carouselChangedAt = now;
    }
    if (twistRight.pressedEdge) {
      selectedSong = (selectedSong + 1) % songCount();
      carouselSlide = 1;
      carouselChangedAt = now;
    }
    if (pushInput.pressedEdge) {
      screen = Screen::DifficultySelect;
      carouselChangedAt = now;
    }
    return;
  }

  if (screen == Screen::DifficultySelect) {
    // The startup/repeating demo drives this popup deterministically.
    if (startupTestActive || startupAutoPlay) return;
    if (twistLeft.pressedEdge && selectedDifficulty > 0) {
      --selectedDifficulty;
      carouselChangedAt = now;
    }
    if (twistRight.pressedEdge && selectedDifficulty < 2) {
      ++selectedDifficulty;
      carouselChangedAt = now;
    }
    if (fullPullEdge) {
      screen = Screen::Select;
      carouselChangedAt = now;
    } else if (pushInput.pressedEdge) {
      startRun(now);
    }
    return;
  }

  if (screen == Screen::Results || screen == Screen::Failed) {
    const uint32_t protectedHold = screen == Screen::Failed
                                       ? FAILURE_CINEMATIC_MS
                                       : VICTORY_CINEMATIC_MS;
    if (now - endShownAt < protectedHold) return;
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

  // The opening encounter is non-interactive. Ignoring controls here prevents
  // an early press from becoming a miss before the soundtrack has begun.
  if (!songClockStarted(now)) return;

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
    if (!resolved[i] && !holding[i] && t > chart[i].hitMs + goodWindow()) {
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

void centeredSmallText(const char *text, int top, uint16_t color) {
  display->setFont(&Picopixel);
  display->setTextSize(1);
  display->setTextWrap(false);
  display->setTextColor(color);
  int16_t x1, y1;
  uint16_t width, height;
  display->getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  display->setCursor((64 - static_cast<int>(width)) / 2 - x1, top - y1);
  display->print(text);
  display->setFont(nullptr);
}

void smallTextCenteredAt(const char *text, int centerX, int top,
                         uint16_t color) {
  display->setFont(&Picopixel);
  display->setTextSize(1);
  display->setTextWrap(false);
  display->setTextColor(color);
  int16_t x1, y1;
  uint16_t width, height;
  display->getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  display->setCursor(centerX - static_cast<int>(width) / 2 - x1, top - y1);
  display->print(text);
  display->setFont(nullptr);
}

// The DMA colour depth is fixed when its framebuffers are allocated. Cover
// art nevertheless retains RGB888 source values, so distribute each channel's
// two discarded low bits spatially across a stable 4x4 Bayer pattern. At
// normal viewing distance adjacent LEDs blend into intermediate shades. This
// is deliberately used only by the song-selection covers; gameplay keeps its
// existing RGB565 palette and timing cost.
uint8_t ditherCoverChannel(uint8_t value, int sourceX, int sourceY) {
  if (activeDmaColorDepth >= 8) return value;
  static constexpr uint8_t bayer4x4[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5},
  };
  uint8_t sixBit = value >> 2;
  const uint8_t remainder = value & 0x03;
  const uint8_t threshold = bayer4x4[sourceY & 3][sourceX & 3];
  if (sixBit < 63 && remainder * 4 > threshold) ++sixBit;
  // Expand back to an eight-bit argument without inventing another level;
  // drawPixelRGB888() will submit this value to the six-bit DMA planes.
  return static_cast<uint8_t>((sixBit << 2) | (sixBit >> 4));
}

void drawCoverArt(uint8_t index, int x, int y, int size, bool focused) {
  const uint32_t raw = songCoverColor(index);
  const uint8_t r = raw >> 16;
  const uint8_t g = raw >> 8;
  const uint8_t b = raw;
  const uint16_t accent = rgb(r, g, b);
  if (index < BopImport::MAX_SONGS && songCoverLoaded[index]) {
    for (int destinationY = 0; destinationY < size; ++destinationY) {
      const int screenY = y + destinationY;
      if (screenY < 0 || screenY >= 64) continue;
      const int sourceY = destinationY * BopImport::COVER_HEIGHT / size;
      for (int destinationX = 0; destinationX < size; ++destinationX) {
        const int screenX = x + destinationX;
        if (screenX < 0 || screenX >= 64) continue;
        const int sourceX = destinationX * BopImport::COVER_WIDTH / size;
        const size_t sourceOffset =
            (sourceY * BopImport::COVER_WIDTH + sourceX) * 3;
        display->drawPixelRGB888(
            screenX, screenY,
            ditherCoverChannel(songCovers[index][sourceOffset],
                               sourceX, sourceY),
            ditherCoverChannel(songCovers[index][sourceOffset + 1],
                               sourceX, sourceY),
            ditherCoverChannel(songCovers[index][sourceOffset + 2],
                               sourceX, sourceY));
      }
    }
  } else {
    display->fillRect(x, y, size, size, rgb(r / 14, g / 14, b / 14));
    display->drawLine(x + 2, y + 2, x + size - 3, y + size - 3, accent);
    display->drawLine(x + size - 3, y + 2, x + 2, y + size - 3, accent);
  }
  const uint16_t border = focused ? rgb(255, 255, 255) : accent;
  const int left = max(0, x);
  const int right = min(63, x + size - 1);
  if (y >= 0 && y < 64 && right >= left)
    display->drawFastHLine(left, y, right - left + 1, border);
  if (y + size - 1 >= 0 && y + size - 1 < 64 && right >= left)
    display->drawFastHLine(left, y + size - 1, right - left + 1, border);
  const int top = max(0, y);
  const int bottom = min(63, y + size - 1);
  if (x >= 0 && x < 64 && bottom >= top)
    display->drawFastVLine(x, top, bottom - top + 1, border);
  if (x + size - 1 >= 0 && x + size - 1 < 64 && bottom >= top)
    display->drawFastVLine(x + size - 1, top, bottom - top + 1, border);
}

void drawSelect(uint32_t now) {
  // Start from true black so every pixel not explicitly used is fully off.
  display->fillScreen(0);
  centeredSmallText("SONG SELECT", 0, rgb(0, 255, 255));
  if (songCount() == 0) {
    centeredSmallText("NO SONG FILES", 24, rgb(255, 80, 80));
    centeredSmallText("RUN UPLOADFS", 36, rgb(255, 255, 255));
    return;
  }

  const uint32_t elapsed = now - carouselChangedAt;
  const float progress = elapsed >= 240 ? 1.0f : elapsed / 240.0f;
  const float eased = 1.0f - (1.0f - progress) * (1.0f - progress);
  const int offset = static_cast<int>(carouselSlide * (1.0f - eased) * 34.0f);
  if (progress >= 1.0f) carouselSlide = 0;
  const int totalSongs = static_cast<int>(songCount());
  for (int rel = -1; rel <= 1; ++rel) {
    const int index = (static_cast<int>(selectedSong) + rel + totalSongs) %
                      totalSongs;
    constexpr int focusedSize = 36;
    constexpr int sideSize = 28;  // Approximately 20% smaller.
    const int centerX = 32 + rel * 34 + offset;
    const int distanceFromFocus = min(34, abs(centerX - 32));
    const int size = focusedSize -
        distanceFromFocus * (focusedSize - sideSize) / 34;
    const int coverY = 6 + (focusedSize - size) / 2;
    drawCoverArt(index, centerX - size / 2, coverY, size,
                 distanceFromFocus <= 2);
  }

  display->fillTriangle(1, 22, 5, 18, 5, 26, rgb(255, 255, 255));
  display->fillTriangle(62, 22, 58, 18, 58, 26, rgb(255, 255, 255));
  const uint16_t arrowBorder = ((now / 250) & 1)
      ? rgb(0, 210, 255) : rgb(0, 45, 150);
  display->drawTriangle(1, 22, 5, 18, 5, 26, arrowBorder);
  display->drawTriangle(62, 22, 58, 18, 58, 26, arrowBorder);
  centeredSmallText(songTitle(selectedSong), 43, rgb(255, 255, 255));
  char detail[16];
  snprintf(detail, sizeof(detail), "%u BPM", songBpm(selectedSong));
  centeredSmallText(detail, 50, rgb(255, 190, 0));
  centeredSmallText("PUSH:SELECT", 57, rgb(120, 255, 180));
}

uint16_t difficultyColor(uint8_t difficulty, float brightness = 1.0f) {
  static const uint8_t colors[3][3] = {
    {40, 255, 120},   // EASY
    {255, 190, 0},    // NORM
    {255, 45, 145},   // HARD
  };
  brightness = constrain(brightness, 0.0f, 1.0f);
  return rgb(static_cast<uint8_t>(colors[difficulty][0] * brightness),
             static_cast<uint8_t>(colors[difficulty][1] * brightness),
             static_cast<uint8_t>(colors[difficulty][2] * brightness));
}

void drawDifficultySelect(uint32_t now) {
  // This is an overlay on the existing song-select buffers. Do not clear or
  // redraw the full matrix: the cover carousel and its title remain in place.
  display->fillRoundRect(5, 5, 54, 33, 3, rgb(3, 3, 20));
  display->drawRoundRect(5, 5, 54, 33, 3, rgb(0, 150, 220));
  centeredSmallText("DIFFICULTY", 7, rgb(100, 220, 255));

  // The carousel buffers may contain opposite phases of the flashing song
  // arrows. Paint a stable version into both buffers while this overlay is
  // active so buffer swaps cannot turn that phase difference into 30 Hz flash.
  const uint16_t stableArrowBorder = rgb(0, 150, 220);
  display->fillTriangle(1, 22, 5, 18, 5, 26, rgb(255, 255, 255));
  display->fillTriangle(62, 22, 58, 18, 58, 26, rgb(255, 255, 255));
  display->drawTriangle(1, 22, 5, 18, 5, 26, stableArrowBorder);
  display->drawTriangle(62, 22, 58, 18, 58, 26, stableArrowBorder);

  static const char *const names[3] = {"EASY", "NORM", "HARD"};
  constexpr int centers[3] = {15, 32, 49};
  for (uint8_t difficulty = 0; difficulty < 3; ++difficulty) {
    const bool focused = difficulty == selectedDifficulty;
    const int size = focused ? 12 : 10;
    const int x = centers[difficulty] - size / 2;
    const int y = focused ? 14 : 15;
    const uint16_t accent = difficultyColor(difficulty);
    display->fillRect(x, y, size, size,
                      difficultyColor(difficulty, focused ? 0.22f : 0.08f));
    display->drawRect(x, y, size, size,
                      focused ? rgb(255, 255, 255)
                              : difficultyColor(difficulty, 0.55f));

    // One, two, or three beats make the density of each option readable even
    // without relying on its colour.
    const int bars = difficulty + 1;
    const int barWidth = 2;
    const int gap = 2;
    const int patternWidth = bars * barWidth + (bars - 1) * gap;
    const int patternX = centers[difficulty] - patternWidth / 2;
    for (int bar = 0; bar < bars; ++bar) {
      const int height = 3 + bar;
      display->fillRect(patternX + bar * (barWidth + gap),
                        y + size - height - 2, barWidth, height, accent);
    }
    smallTextCenteredAt(names[difficulty], centers[difficulty], 29, accent);
  }

  if (((now - carouselChangedAt) / 300) & 1) {
    const int center = centers[selectedDifficulty];
    display->drawPixel(center - 1, 12, rgb(255, 255, 255));
    display->drawPixel(center, 12, rgb(255, 255, 255));
    display->drawPixel(center + 1, 12, rgb(255, 255, 255));
  }
}

int laneX(Lane lane) {
  return lane == Lane::Twist ? 11 : lane == Lane::Push ? 31 : 51;
}

void drawInvaderShip(int yOffset, uint16_t bodyColor, uint16_t eyeColor,
                     bool alternatePose, const uint8_t turretFire[3],
                     int launchY) {
  // A wide, chunky command invader based on the supplied 64x64 pixel mockup.
  // The one-pixel stair steps are intentional: this is sprite art drawn with
  // primitives, so it stays crisp on the physical LED matrix.
  display->drawLine(18, yOffset + 3, 22, yOffset + 7, bodyColor);
  display->drawLine(45, yOffset + 3, 41, yOffset + 7, bodyColor);
  display->fillRect(22, yOffset + 3, 20, 2, bodyColor);
  display->fillRect(17, yOffset + 5, 30, 2, bodyColor);
  display->fillRect(12, yOffset + 7, 40, 4, bodyColor);
  display->fillRect(7, yOffset + 9, 50, 3, bodyColor);
  display->fillRect(3, yOffset + 11, 58, 2, bodyColor);

  // Contrasting armour bands survive the limited gameplay colour depth and
  // make the commander read as a detailed arcade sprite instead of one blob.
  const uint16_t armourHighlight = rgb(190, 255, 35);
  const uint16_t armourShadow = rgb(0, 105, 115);
  const uint16_t cockpit = rgb(180, 35, 255);
  display->drawFastHLine(24, yOffset + 4, 16, armourHighlight);
  display->drawFastHLine(13, yOffset + 7, 8, armourShadow);
  display->drawFastHLine(43, yOffset + 7, 8, armourShadow);
  display->drawFastHLine(8, yOffset + 12, 11, armourShadow);
  display->drawFastHLine(45, yOffset + 12, 11, armourShadow);
  display->fillRect(28, yOffset + 6, 8, 2, cockpit);
  display->drawPixel(31, yOffset + 5, rgb(255, 255, 255));
  display->drawPixel(32, yOffset + 5, rgb(255, 255, 255));

  // Cockpit eyes and a central grille give the enemy a readable expression.
  display->fillRect(17, yOffset + 8, 6, 3, eyeColor);
  display->fillRect(41, yOffset + 8, 6, 3, eyeColor);
  display->drawFastHLine(29, yOffset + 9, 6, eyeColor);

  // Animated wing tips create the classic two-frame Space Invaders shuffle.
  if (alternatePose) {
    display->drawLine(3, yOffset + 10, 0, yOffset + 7, bodyColor);
    display->drawLine(60, yOffset + 10, 63, yOffset + 7, bodyColor);
    display->drawFastHLine(7, yOffset + 14, 7, bodyColor);
    display->drawFastHLine(50, yOffset + 14, 7, bodyColor);
  } else {
    display->drawLine(3, yOffset + 12, 0, yOffset + 15, bodyColor);
    display->drawLine(60, yOffset + 12, 63, yOffset + 15, bodyColor);
    display->drawFastHLine(4, yOffset + 14, 8, bodyColor);
    display->drawFastHLine(52, yOffset + 14, 8, bodyColor);
  }

  const uint16_t turretColors[3] = {
    rgb(255, 45, 35), rgb(255, 205, 0), rgb(0, 145, 255)
  };
  const uint16_t turretHighlights[3] = {
    rgb(255, 135, 40), rgb(255, 255, 80), rgb(0, 255, 255)
  };
  for (uint8_t lane = 0; lane < 3; ++lane) {
    const int x = laneX(static_cast<Lane>(lane));
    const bool firing = turretFire != nullptr && turretFire[lane] > 0;
    const int recoil = firing && turretFire[lane] > 110 ? -1 : 0;
    display->fillRect(x - 2, yOffset + 12 + recoil, 5, 3,
                      turretColors[lane]);
    display->drawFastHLine(x - 1, yOffset + 12 + recoil, 3,
                           turretHighlights[lane]);
    display->fillRect(x, yOffset + 15 + recoil, 1, 3,
                      turretHighlights[lane]);
    if (!firing) continue;

    const uint16_t flash = turretFire[lane] > 90
                               ? rgb(255, 255, 255)
                               : rgb(0, 190, 255);
    display->drawPixel(x, yOffset + 18, flash);
    if (turretFire[lane] > 60) {
      display->drawPixel(x - 1, yOffset + 19, flash);
      display->drawPixel(x + 1, yOffset + 19, flash);
    }
    for (int beamY = max(0, yOffset + 20); beamY < launchY; ++beamY)
      display->drawPixel(x, beamY, rgb(0, 75, 170));
  }
}

void drawCinematicEarth(int centerY, int radius, uint8_t rotation) {
  // High-contrast RGB565-safe colours keep the planet readable on the panel:
  // a cyan atmosphere, saturated blue ocean, green land, and white ice/clouds.
  const uint16_t atmosphere = rgb(0, 220, 255);
  const uint16_t ocean = rgb(0, 55, 210);
  const uint16_t oceanLight = rgb(0, 115, 255);
  const uint16_t land = rgb(35, 220, 70);
  const uint16_t landLight = rgb(150, 255, 50);
  const uint16_t cloud = rgb(235, 255, 255);
  const int centerX = 31;
  display->fillCircle(centerX, centerY, radius, atmosphere);
  display->fillCircle(centerX, centerY, max(1, radius - 2), ocean);
  display->drawCircle(centerX - 2, centerY - 2, max(1, radius - 4),
                      oceanLight);

  const int diameter = radius * 2 - 5;
  const int scroll = diameter > 0 ? rotation % diameter : 0;
  auto drawLandPatch = [&](int baseX, int baseY, int width, int height,
                           uint16_t color) {
    const int innerRadius = max(1, radius - 3);
    for (int py = 0; py < height; ++py) {
      for (int px = 0; px < width; ++px) {
        const int localX = (baseX + px + scroll) % diameter - diameter / 2;
        const int localY = baseY + py;
        if (localX * localX + localY * localY <=
            innerRadius * innerRadius)
          display->drawPixel(centerX + localX, centerY + localY, color);
      }
    }
  };
  drawLandPatch(2, -radius / 2, 6, 4, land);
  drawLandPatch(5, -radius / 2 + 3, 5, 5, landLight);
  drawLandPatch(diameter / 2 + 2, 1, 7, 4, land);
  drawLandPatch(diameter / 2, 4, 5, 3, landLight);
  display->drawFastHLine(centerX - radius / 2, centerY - radius + 4,
                         radius, cloud);
  display->drawFastHLine(centerX - radius + 5, centerY - 1,
                         max(2, radius / 2), cloud);
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

void drawPullRow(int leftX, int y, bool fullPull, float coverage) {
  if (coverage <= 0.01f) return;
  if (fullPull) {
    display->drawFastHLine(leftX, y, NOTE_WIDTH,
                           noteColor(Lane::Pull, coverage));
    return;
  }

  // Interpolate symmetrically from a dark violet centre to the normal bright
  // pull blue at both edges. Every horizontal pixel is one gradient step.
  constexpr float CENTER_RED = 22.0f;
  constexpr float CENTER_GREEN = 0.0f;
  constexpr float CENTER_BLUE = 24.0f;
  constexpr float EDGE_RED = 0.0f;
  constexpr float EDGE_GREEN = 80.0f;
  constexpr float EDGE_BLUE = 255.0f;
  constexpr float PURPLE_CORE_RADIUS = 0.30f;
  constexpr int centerPixel = NOTE_WIDTH / 2;
  for (int pixel = 0; pixel < NOTE_WIDTH; ++pixel) {
    const float distance = abs(pixel - centerPixel) /
                           static_cast<float>(centerPixel);
    // Keep a wider violet centre, then ease into blue without a hard edge.
    float blend = constrain((distance - PURPLE_CORE_RADIUS) /
                                (1.0f - PURPLE_CORE_RADIUS),
                            0.0f, 1.0f);
    blend = blend * blend * (3.0f - 2.0f * blend);
    const float red = CENTER_RED + (EDGE_RED - CENTER_RED) * blend;
    const float green = CENTER_GREEN +
                        (EDGE_GREEN - CENTER_GREEN) * blend;
    const float blue = CENTER_BLUE + (EDGE_BLUE - CENTER_BLUE) * blend;
    display->drawPixel(
        leftX + pixel, y,
        rgb(static_cast<uint8_t>(red * coverage),
            static_cast<uint8_t>(green * coverage),
            static_cast<uint8_t>(blue * coverage)));
  }
}

void drawPullTrail(int centerX, int top, int bottom, bool fullPull) {
  if (bottom < top) return;
  for (int row = top; row <= bottom; ++row)
    drawPullRow(centerX - NOTE_WIDTH / 2, row, fullPull, 1.0f);
}

void drawTwistRow(int x, int y, int width, bool requiredSide,
                  float coverage) {
  if (coverage <= 0.01f) return;
  const float brightness = coverage * (requiredSide ? 1.0f : 0.25f);
  const uint16_t color = requiredSide
      ? rgb(0, static_cast<uint8_t>(255 * brightness), 0)
      : rgb(static_cast<uint8_t>(255 * brightness), 0, 0);
  display->drawFastHLine(x, y, width, color);
}

uint16_t twistSplitColor(bool requiredSide, float brightness) {
  brightness *= requiredSide ? 1.0f : 0.25f;
  return requiredSide
      ? rgb(0, static_cast<uint8_t>(255 * brightness), 0)
      : rgb(static_cast<uint8_t>(255 * brightness), 0, 0);
}

void twistSegmentGeometry(int centerX, bool turnRight,
                          int &requiredX, int &otherX) {
  const int leftEdge = centerX - NOTE_WIDTH / 2;
  if (turnRight) {
    otherX = leftEdge;
    requiredX = leftEdge + TWIST_OTHER_WIDTH + 1;
  } else {
    requiredX = leftEdge;
    otherX = leftEdge + TWIST_REQUIRED_WIDTH + 1;
  }
}

void drawNote(const NoteDef &note, int x, float y, int noteTopY) {
#if ENABLE_NOTE_SUBPIXEL_BLEND
  const int baseY = floorf(y);
  const float fraction = y - baseY;
  // Frame-aware directional interpolation. Look slightly ahead by a fraction
  // of the distance travelled per presented frame, brighten the forward row,
  // and dim the trailing row. Geometry and the fully lit body stay unchanged.
  const float pixelsPerFrame =
      (NOTE_HIT_Y - noteTopY) * renderIntervalUs /
      (NOTE_TRAVEL_MS * 1000000.0f);
  const float forwardPosition = constrain(
      fraction + pixelsPerFrame * 0.35f, 0.0f, 1.0f);
  const float lowerBlend = sqrtf(forwardPosition);
  const float upperBlend = powf(1.0f - forwardPosition, 1.35f);
  const float forwardGlow = lowerBlend * min(0.06f, pixelsPerFrame * 0.12f);
  const float backwardGlow = upperBlend * min(0.015f, pixelsPerFrame * 0.025f);
#else
  // Temporary diagnostic mode: snap notes to whole rows with no fractional
  // brightness transfer, leading glow, or trailing glow.
  const int baseY = lroundf(y);
  const float lowerBlend = 0.0f;
  const float upperBlend = 1.0f;
  const float forwardGlow = 0.0f;
  const float backwardGlow = 0.0f;
#endif

  // The two-pixel bar is blended across adjacent rows according to its
  // fractional vertical position, producing smoother apparent movement.
  if (note.lane == Lane::Push) {
    drawInterpolatedRow(x - 5, baseY - 2, NOTE_WIDTH, note.lane, backwardGlow);
    drawInterpolatedRow(x - 5, baseY - 1, NOTE_WIDTH, note.lane, upperBlend);
    drawInterpolatedRow(x - 5, baseY, NOTE_WIDTH, note.lane, 1.0f);
    drawInterpolatedRow(x - 5, baseY + 1, NOTE_WIDTH, note.lane, lowerBlend);
    drawInterpolatedRow(x - 5, baseY + 2, NOTE_WIDTH, note.lane, forwardGlow);
    return;
  }
  if (note.lane == Lane::Pull) {
    for (int rowOffset = -2; rowOffset <= 2; ++rowOffset) {
      const float coverage = rowOffset == -2 ? backwardGlow :
                             rowOffset == -1 ? upperBlend :
                             rowOffset == 0 ? 1.0f :
                             rowOffset == 1 ? lowerBlend : forwardGlow;
      drawPullRow(x - NOTE_WIDTH / 2, baseY + rowOffset,
                  note.variant, coverage);
    }
    return;
  }

  // The requested turn occupies eight of the note's eleven pixels. The other
  // direction remains a two-pixel dim-red cue, separated by one dark pixel.
  int requiredX = 0;
  int otherX = 0;
  twistSegmentGeometry(x, note.variant, requiredX, otherX);
  for (int rowOffset = -2; rowOffset <= 2; ++rowOffset) {
    const float coverage = rowOffset == -2 ? backwardGlow :
                           rowOffset == -1 ? upperBlend :
                           rowOffset == 0 ? 1.0f :
                           rowOffset == 1 ? lowerBlend : forwardGlow;
    drawTwistRow(requiredX, baseY + rowOffset,
                 TWIST_REQUIRED_WIDTH, true, coverage);
    drawTwistRow(otherX, baseY + rowOffset,
                 TWIST_OTHER_WIDTH, false, coverage);
  }
}

void drawGimmickEffect(uint32_t now, uint32_t t) {
  for (const GimmickDef &gimmick : gimmicks) {
    if (gimmick.durationMs == 0 || t < gimmick.startMs ||
        t >= gimmick.startMs + gimmick.durationMs) continue;
    // Commander movement is applied when the foreground sprite is drawn.
    if (gimmick.type == GimmickType::CommanderApproach) continue;
    const uint32_t elapsedMs = t - gimmick.startMs;
    const float progress = static_cast<float>(elapsedMs) / gimmick.durationMs;
    const float envelope = gimmick.rateHz > 0.0f
        ? 0.5f - 0.5f * cosf(2.0f * PI * elapsedMs * 0.001f * gimmick.rateHz)
        : 1.0f - fabsf(progress * 2.0f - 1.0f);
    const float strength = gimmick.type == GimmickType::WindGust
                               ? gimmick.brightness
                               : gimmick.brightness * envelope;
    const uint8_t red = static_cast<uint8_t>(
        ((gimmick.color >> 16) & 0xFF) * strength);
    const uint8_t green = static_cast<uint8_t>(
        ((gimmick.color >> 8) & 0xFF) * strength);
    const uint8_t blue = static_cast<uint8_t>(
        (gimmick.color & 0xFF) * strength);
    const uint16_t effectColor = rgb(red, green, blue);
    if (gimmick.type == GimmickType::WindGust) {
      for (uint8_t streak = 0; streak < gimmick.density; ++streak) {
        const int y = 8 + streak * (GIMMICK_SAFE_ZONE_Y - 9) /
                            gimmick.density;
        const int travel = static_cast<int>(now * gimmick.speed / 5.0f +
                                             y * 7) % 76;
        const int x = gimmick.direction < 0 ? 64 - travel : travel - 10;
        if (x >= 0 && x + 9 < 64)
          display->drawLine(x, y + 1, x + 9, y, effectColor);
      }
      continue;
    }

    const int firstLane = gimmick.target < 0 ? 0 : gimmick.target;
    const int lastLane = gimmick.target < 0 ? 2 : gimmick.target;
    for (int lane = firstLane; lane <= lastLane; ++lane) {
      const int x = gimmick.type == GimmickType::ScreenFlash &&
                            gimmick.target < 0
                        ? 0 : LANE_LEFT_X[lane];
      const int width = gimmick.type == GimmickType::ScreenFlash &&
                                gimmick.target < 0
                            ? 64 : LANE_DRAW_WIDTH[lane];
      if (gimmick.pattern == BopImport::GimmickPattern::Solid) {
        display->fillRect(x, 1, width, 63, effectColor);
      } else if (gimmick.pattern == BopImport::GimmickPattern::Stripes) {
        for (int y = 2; y < 64; y += 3)
          display->drawFastHLine(x, y, width, effectColor);
      } else {
        for (int y = 2; y < 64; y += 4)
          for (int pixelX = x + ((y / 4) & 1) * 2;
               pixelX < x + width; pixelX += 4)
            display->fillRect(pixelX, y, min(2, x + width - pixelX), 2,
                              effectColor);
      }
      if (gimmick.type == GimmickType::ScreenFlash && gimmick.target < 0)
        break;
    }
  }
}

void drawGameIntro(uint32_t now) {
  const uint32_t age = now - introStartedAt;
  display->fillScreen(0);

  // Sparse fixed stars establish scale without producing noisy low-bit-depth
  // gradients on the HUB75 panels.
  const uint16_t star = rgb(100, 120, 180);
  for (uint8_t i = 0; i < 12; ++i)
    display->drawPixel((i * 17 + 5) % 64, (i * 11 + 3) % 54, star);

  float retreat = 0.0f;
  if (age > GAME_INTRO_HOLD_MS) {
    retreat = constrain(
        (age - GAME_INTRO_HOLD_MS) /
            static_cast<float>(GAME_INTRO_RETREAT_MS),
        0.0f, 1.0f);
    retreat = retreat * retreat * (3.0f - 2.0f * retreat);
  }
  const int commanderY = lroundf(COMMANDER_STAGE_Y +
      (COMMANDER_GAME_Y - COMMANDER_STAGE_Y) * retreat);
  const int earthY = lroundf(51 + 31 * retreat);
  const int earthRadius = lroundf(17 - 4 * retreat);
  const uint8_t noFire[3]{};
  const int launchY = constrain(commanderY + 20, NOTE_TOP_Y, 20);
  drawInvaderShip(commanderY, rgb(60, 255, 35), rgb(4, 0, 18),
                  ((age / 220) & 1) != 0, noFire, launchY);
  drawCinematicEarth(earthY, earthRadius, age / 70);

  // The final beat of the cinematic presents an empty, ready playfield. Both
  // actors have cleared the matrix before the song and first notes can begin.
  if (age >= GAME_INTRO_HOLD_MS + GAME_INTRO_RETREAT_MS) {
    const uint16_t laneWall = rgb(0, 42, 125);
    for (int x : {0, 21, 41, 63})
      display->drawFastVLine(x, NOTE_TOP_Y,
                             NOTE_HIT_Y - NOTE_TOP_Y + 1, laneWall);
    for (uint8_t lane = 0; lane < 3; ++lane)
      display->drawFastHLine(LANE_LEFT_X[lane], NOTE_HIT_Y,
                             LANE_DRAW_WIDTH[lane], rgb(255, 255, 255));
  }
}

void drawPlaying(uint32_t now) {
  if (!songClockStarted(now)) {
    drawGameIntro(now);
    return;
  }
  const uint32_t t = songTime(now);
  const uint32_t preciseElapsedUs = screen == Screen::Paused
                                        ? pausedAtUs - runStartedAtUs
                                        : micros() - runStartedAtUs;
  const float preciseTimeMs = preciseElapsedUs * 0.001f;
  display->fillScreen(0);
  drawGimmickEffect(now, t);

  uint8_t turretFire[3]{};
  for (size_t i = 0; i < noteCount; ++i) {
    const int32_t launchAt = static_cast<int32_t>(chart[i].hitMs) -
                             NOTE_TRAVEL_MS;
    const int32_t launchAge = static_cast<int32_t>(preciseTimeMs) - launchAt;
    if (launchAge < 0 || launchAge >= TURRET_FIRE_MS) continue;
    const uint8_t lane = min<uint8_t>(chart[i].startColumn, 2);
    turretFire[lane] = max<uint8_t>(
        turretFire[lane], TURRET_FIRE_MS - launchAge);
  }

  const bool invaderStep = ((t / 260) & 1) != 0;
  int commanderY = COMMANDER_GAME_Y;
  uint16_t commanderColor = rgb(60, 255, 35);
  for (const GimmickDef &gimmick : gimmicks) {
    if (gimmick.type != GimmickType::CommanderApproach ||
        gimmick.durationMs == 0 || t < gimmick.startMs ||
        t >= gimmick.startMs + gimmick.durationMs) continue;
    const uint32_t elapsed = t - gimmick.startMs;
    const uint32_t edgeDuration = min<uint32_t>(
        COMMANDER_APPROACH_MS, max<uint32_t>(1, gimmick.durationMs / 2));
    float stageAmount = 1.0f;
    if (elapsed < edgeDuration) {
      stageAmount = elapsed / static_cast<float>(edgeDuration);
    } else if (gimmick.durationMs - elapsed < edgeDuration) {
      stageAmount = (gimmick.durationMs - elapsed) /
                    static_cast<float>(edgeDuration);
    }
    stageAmount = stageAmount * stageAmount * (3.0f - 2.0f * stageAmount);
    commanderY = lroundf(COMMANDER_GAME_Y +
        (COMMANDER_STAGE_Y - COMMANDER_GAME_Y) * stageAmount);
    commanderColor = rgb(
        static_cast<uint8_t>(((gimmick.color >> 16) & 0xFF) *
                             gimmick.brightness),
        static_cast<uint8_t>(((gimmick.color >> 8) & 0xFF) *
                             gimmick.brightness),
        static_cast<uint8_t>((gimmick.color & 0xFF) * gimmick.brightness));
  }
  const int noteTopY = constrain(commanderY + 20, NOTE_TOP_Y, 20);
  drawInvaderShip(commanderY, commanderColor, rgb(4, 0, 18),
                  invaderStep, turretFire, noteTopY);

  // Neon-blue lane walls begin under the cannons, visually connecting each
  // projectile source to its matching player control.
  const uint16_t laneWall = rgb(0, 42, 125);
  for (int x : {0, 21, 41, 63})
    display->drawFastVLine(x, noteTopY, NOTE_HIT_Y - noteTopY + 1,
                           laneWall);

  // Draw the lane-specific timing segments before notes so approaching notes
  // remain visible as they cross the line. These colours deliberately avoid
  // the red, green, and blue note palette: white=idle, yellow=perfect,
  // magenta=good, and orange=miss.
  for (uint8_t laneIndex = 0; laneIndex < 3; ++laneIndex) {
    uint16_t barColor = rgb(255, 255, 255);
    const Judgment result = laneJudgments[laneIndex];
    if (result != Judgment::None &&
        now - laneJudgmentShownAt[laneIndex] < 300) {
      barColor = result == Judgment::Perfect ? rgb(255, 255, 0) :
                 result == Judgment::Good ? rgb(255, 0, 255) :
                                            rgb(255, 80, 0);
    }
    display->drawFastHLine(LANE_LEFT_X[laneIndex], NOTE_HIT_Y,
                           LANE_DRAW_WIDTH[laneIndex], barColor);
  }

  for (size_t i = 0; i < noteCount; ++i) {
    const bool lingering = resolved[i] && noteVisibleUntilMs[i] > 0 &&
                           preciseTimeMs <= noteVisibleUntilMs[i];
    if (resolved[i] && !lingering) continue;
    const float until = chart[i].hitMs - preciseTimeMs;
    const float visibleAfterHit = chart[i].holdMs + POST_HIT_DISPLAY_MS;
    if (until < -visibleAfterHit || until > NOTE_TRAVEL_MS) continue;
    const float y = NOTE_HIT_Y -
        until * static_cast<float>(NOTE_HIT_Y - noteTopY) / NOTE_TRAVEL_MS;
    float tailY = y;
    if (chart[i].holdMs > 0) {
      const float tailUntil = chart[i].hitMs + chart[i].holdMs - preciseTimeMs;
      tailY = NOTE_HIT_Y -
          tailUntil * static_cast<float>(NOTE_HIT_Y - noteTopY) /
          NOTE_TRAVEL_MS;
      if (y < noteTopY || tailY > 63) continue;
    } else if (y < noteTopY || y > 63) {
      continue;
    }
    const int startX = laneX(static_cast<Lane>(chart[i].startColumn));
    const int targetX = laneX(static_cast<Lane>(chart[i].endColumn));
    int x = startX;
    if (chart[i].startColumn != chart[i].endColumn &&
        preciseTimeMs >= chart[i].shiftStartMs) {
      const float transition = chart[i].shiftEndMs > chart[i].shiftStartMs
          ? constrain((preciseTimeMs - chart[i].shiftStartMs) /
                          (chart[i].shiftEndMs - chart[i].shiftStartMs),
                      0.0f, 1.0f)
          : 1.0f;
      const float eased = transition * transition *
                          (3.0f - 2.0f * transition);
      x = lroundf(startX + (targetX - startX) * eased);
    }

    if (chart[i].holdMs > 0) {
      const int railTop = max(noteTopY, static_cast<int>(lroundf(tailY)));
      const int railBottom = min(63, static_cast<int>(lroundf(y)));
      if (railBottom >= railTop) {
        const int trailHeight = railBottom - railTop + 1;
        if (chart[i].lane == Lane::Twist) {
          int requiredX = 0;
          int otherX = 0;
          twistSegmentGeometry(x, chart[i].variant, requiredX, otherX);
          display->fillRect(requiredX, railTop, TWIST_REQUIRED_WIDTH,
                            trailHeight, twistSplitColor(true, 1.0f));
          display->fillRect(otherX, railTop, TWIST_OTHER_WIDTH,
                            trailHeight, twistSplitColor(false, 1.0f));
        } else if (chart[i].lane == Lane::Pull) {
          if (chart[i].pullTransitionMs > 0) {
            const float transitionUntil =
                chart[i].hitMs + chart[i].pullTransitionMs - preciseTimeMs;
            const float transitionY = NOTE_HIT_Y -
                transitionUntil * static_cast<float>(NOTE_HIT_Y - noteTopY) /
                    NOTE_TRAVEL_MS;
            const int transitionRow = static_cast<int>(lroundf(transitionY));
            drawPullTrail(x, railTop, min(railBottom, transitionRow),
                          chart[i].endVariant);
            drawPullTrail(x, max(railTop, transitionRow + 1), railBottom,
                          chart[i].variant);
          } else {
            drawPullTrail(x, railTop, railBottom, chart[i].variant);
          }
        } else {
          const uint16_t trailColor = noteColor(chart[i].lane, 1.0f);
          display->fillRect(x - 5, railTop, NOTE_WIDTH,
                            trailHeight, trailColor);
        }
      }
    }
    if (y <= 63) drawNote(chart[i], x, y, noteTopY);
  }

  // Keep health on the top row as foreground UI. Notes, commander movement,
  // and gimmick effects cannot cover or replace the meter.
  display->drawFastHLine(0, 0, 64, 0);
  const uint32_t healthFrameMs = now - healthAnimationAt;
  healthAnimationAt = now;
  if (displayedHealth < health) {
    displayedHealth = health;
  } else if (displayedHealth > health && now - healthDamageAt >= 450) {
    displayedHealth = max(static_cast<float>(health),
                          displayedHealth - healthFrameMs * 0.018f);
  }
  const int healthyWidth = constrain(
      health * HEALTH_BAR_WIDTH / 100, 0, HEALTH_BAR_WIDTH);
  const int displayedWidth = constrain(
      static_cast<int>(lroundf(displayedHealth * HEALTH_BAR_WIDTH / 100.0f)),
      0, HEALTH_BAR_WIDTH);
  display->drawFastHLine(0, 0, healthyWidth,
                         health > 30 ? rgb(0, 255, 100)
                                     : rgb(255, 30, 20));
  if (healthRegenToWidth > healthRegenFromWidth &&
      now - healthRegenAt < HEALTH_REGEN_FLASH_MS) {
    display->drawFastHLine(healthRegenFromWidth, 0,
                           healthRegenToWidth - healthRegenFromWidth,
                           rgb(0, 255, 0));
  }
  if (displayedWidth > healthyWidth) {
    const uint32_t damageAge = now - healthDamageAt;
    const bool flashVisible = damageAge >= 450 || ((damageAge / 75) & 1) == 0;
    if (flashVisible) {
      display->drawFastHLine(healthyWidth, 0,
                             displayedWidth - healthyWidth,
                             damageAge < 450 ? rgb(255, 0, 0)
                                             : rgb(130, 0, 0));
    }
  }

  display->setFont(&Picopixel);
  display->setTextSize(1);
  display->setTextColor(rgb(255, 255, 255));
  display->setCursor(0, 63);
  display->printf("%lu", static_cast<unsigned long>(score));
  display->setFont(nullptr);

  if (readPullState() == PullState::Fault)
    centeredText("PULL FAULT", 17, rgb(255, 20, 20));
}

void drawEarthExplosion(uint32_t age) {
  constexpr int centerX = 31;
  constexpr int centerY = 49;
  const uint16_t deepRed = rgb(145, 0, 20);
  const uint16_t hotRed = rgb(255, 35, 0);
  const uint16_t orange = rgb(255, 120, 0);
  const uint16_t yellow = rgb(255, 245, 20);
  const uint16_t whiteHot = rgb(255, 255, 255);
  const int radius = min<uint32_t>(36, 3 + age / 43);
  const int pulse = ((age / 70) & 1) ? 1 : 0;

  // Multiple offset fireballs create an irregular blast substantially larger
  // than the planet it replaces—the intended arcade-comedy exaggeration.
  display->fillCircle(centerX, centerY, radius, deepRed);
  constexpr int8_t lobeX[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
  constexpr int8_t lobeY[8] = {0, -3, -4, -3, 0, 3, 4, 3};
  const int lobeDistance = max(2, radius * 3 / 4);
  const int lobeRadius = max(2, radius / 3 + pulse);
  for (uint8_t lobe = 0; lobe < 8; ++lobe) {
    display->fillCircle(centerX + lobeX[lobe] * lobeDistance / 4,
                        centerY + lobeY[lobe] * lobeDistance / 4,
                        lobeRadius, lobe & 1 ? hotRed : orange);
  }
  display->fillCircle(centerX, centerY, max(2, radius * 2 / 3), orange);
  display->fillCircle(centerX - pulse, centerY + pulse,
                      max(1, radius / 2), yellow);
  display->fillCircle(centerX + pulse, centerY - pulse,
                      max(1, radius / 4), whiteHot);

  // Recognisable ocean, atmosphere, and land fragments keep the joke legible:
  // this is Earth coming apart, not merely a generic fireball.
  constexpr int8_t velocityX[12] = {
    -4, -3, -2, -1, 1, 2, 3, 4, -3, 3, -2, 2
  };
  constexpr int8_t velocityY[12] = {
    -3, -1, 2, 4, 4, 2, -1, -3, -4, -4, 3, 3
  };
  const uint16_t fragmentColors[4] = {
    rgb(0, 180, 255), rgb(0, 55, 210),
    rgb(35, 220, 70), rgb(150, 255, 50)
  };
  const int travel = 4 + age / 45;
  for (uint8_t piece = 0; piece < 12; ++piece) {
    const int x = centerX + velocityX[piece] * travel / 3;
    const int y = centerY + velocityY[piece] * travel / 3;
    if (x >= 0 && x < 63 && y >= 0 && y < 63)
      display->fillRect(x, y, 2, 2, fragmentColors[piece % 4]);
  }

  for (uint8_t spark = 0; spark < 10; ++spark) {
    const int x = (centerX + spark * 19 + age / 17) % 64;
    const int y = (centerY + spark * 13 + age / 29) % 64;
    display->drawPixel(x, y, spark & 1 ? yellow : whiteHot);
  }
}

void drawEarthTargetBeams(int commanderY, uint32_t age) {
  float progress = constrain(age / static_cast<float>(EARTH_TARGET_MS),
                             0.0f, 1.0f);
  progress = progress * progress * (3.0f - 2.0f * progress);
  constexpr int impactX[3] = {23, 31, 39};
  const uint16_t outerColors[3] = {
    rgb(255, 30, 25), rgb(255, 190, 0), rgb(0, 135, 255)
  };
  const uint16_t coreColors[3] = {
    rgb(255, 150, 80), rgb(255, 255, 120), rgb(100, 255, 255)
  };
  for (uint8_t lane = 0; lane < 3; ++lane) {
    const int startX = laneX(static_cast<Lane>(lane));
    const int startY = max(0, commanderY + 20);
    const int endX = lroundf(startX + (impactX[lane] - startX) * progress);
    const int endY = lroundf(startY + (42 - startY) * progress);
    // Five-pixel coloured envelope plus a white-hot core is deliberately
    // excessive, while the converging geometry still reads as turret fire.
    for (int thickness = -2; thickness <= 2; ++thickness)
      display->drawLine(startX + thickness, startY,
                        endX + thickness / 2, endY, outerColors[lane]);
    display->drawLine(startX, startY, endX, endY, coreColors[lane]);
    display->fillCircle(endX, endY, progress > 0.8f ? 3 : 1,
                        coreColors[lane]);
    if (progress > 0.88f)
      display->drawCircle(impactX[lane], 42, 4 + ((age / 55) & 1),
                          rgb(255, 255, 255));
  }
}

void drawEarthVictoryCannon(uint32_t attackAge) {
  // Earth returns with a turret intentionally far too large for the planet.
  drawCinematicEarth(52, 12, attackAge / 60);
  const uint16_t darkMetal = rgb(20, 35, 105);
  const uint16_t metal = rgb(80, 120, 210);
  const uint16_t highlight = rgb(170, 235, 255);
  const uint16_t chargeColor = ((attackAge / 70) & 1)
                                   ? rgb(255, 255, 255)
                                   : rgb(0, 255, 255);
  display->fillRect(22, 39, 19, 6, darkMetal);
  display->fillRect(24, 37, 15, 5, metal);
  display->drawFastHLine(25, 37, 13, highlight);
  display->fillRect(27, 29, 9, 9, darkMetal);
  display->fillRect(28, 27, 7, 10, metal);
  display->drawFastVLine(29, 28, 8, highlight);
  display->fillRect(29, 20, 5, 9, darkMetal);
  display->fillRect(30, 19, 3, 9, highlight);

  float fire = constrain(attackAge /
                             static_cast<float>(EARTH_COUNTERATTACK_MS),
                         0.0f, 1.0f);
  fire = fire * fire * (3.0f - 2.0f * fire);
  display->fillCircle(31, 19, 2 + ((attackAge / 80) & 1), chargeColor);
  if (fire <= 0.02f) return;

  const int beamTop = lroundf(19 + (8 - 19) * fire);
  // A nine-pixel plasma envelope with a five-pixel white/cyan core dwarfs the
  // cannon barrel but still follows a sensible straight shot into the cockpit.
  display->fillRect(27, beamTop, 9, 20 - beamTop, rgb(0, 90, 255));
  display->fillRect(29, beamTop, 5, 20 - beamTop, rgb(0, 255, 255));
  display->fillRect(30, beamTop, 3, 20 - beamTop, rgb(255, 255, 255));
  display->fillCircle(31, beamTop, fire > 0.85f ? 5 : 3, chargeColor);
  if (fire > 0.88f)
    display->drawCircle(31, 11, 6 + ((attackAge / 45) & 1),
                        rgb(255, 255, 255));
}

void drawInvaderExplosion(uint32_t age) {
  constexpr int centerX = 31;
  constexpr int centerY = 12;
  const uint16_t alienGreen = rgb(70, 255, 35);
  const uint16_t toxicLime = rgb(210, 255, 20);
  const uint16_t plasmaCyan = rgb(0, 230, 255);
  const uint16_t plasmaPurple = rgb(210, 30, 255);
  const uint16_t whiteHot = rgb(255, 255, 255);
  const int radius = min<uint32_t>(34, 4 + age / 38);
  const int pulse = ((age / 55) & 1) ? 2 : 0;

  // Expanding toxic plasma rings and asymmetric lobes make the commander's
  // destruction intentionally much larger than its original sprite.
  display->fillCircle(centerX, centerY, radius, plasmaPurple);
  constexpr int8_t lobeX[10] = {-5, -4, -2, 1, 4, 5, 3, 0, -3, -5};
  constexpr int8_t lobeY[10] = {-1, -4, -5, -5, -3, 1, 4, 5, 4, 2};
  const int reach = max(3, radius * 4 / 5);
  for (uint8_t lobe = 0; lobe < 10; ++lobe) {
    const uint16_t color = lobe % 3 == 0 ? alienGreen :
                           lobe % 3 == 1 ? plasmaCyan : toxicLime;
    display->fillCircle(centerX + lobeX[lobe] * reach / 5,
                        centerY + lobeY[lobe] * reach / 5,
                        max(2, radius / 3 + (lobe & 1)), color);
  }
  display->drawCircle(centerX, centerY, max(2, radius - pulse), whiteHot);
  display->fillCircle(centerX, centerY, max(2, radius / 2), toxicLime);
  display->fillCircle(centerX, centerY, max(1, radius / 4), whiteHot);

  // Armour and all three coloured turrets remain identifiable as oversized
  // chunks sailing away from the blast.
  const uint16_t chunkColors[7] = {
    alienGreen, rgb(0, 105, 115), plasmaPurple,
    rgb(255, 45, 35), rgb(255, 205, 0), rgb(0, 145, 255), whiteHot
  };
  constexpr int8_t velocityX[14] = {
    -5, -4, -3, -2, -1, 1, 2, 3, 4, 5, -4, 4, -2, 2
  };
  constexpr int8_t velocityY[14] = {
    -2, 1, 3, 5, -4, -4, 5, 3, 1, -2, -5, -5, 4, 4
  };
  const int travel = 3 + age / 42;
  for (uint8_t piece = 0; piece < 14; ++piece) {
    const int x = centerX + velocityX[piece] * travel / 3;
    const int y = centerY + velocityY[piece] * travel / 3;
    if (x >= 0 && x < 62 && y >= 0 && y < 62)
      display->fillRect(x, y, piece % 3 == 0 ? 3 : 2, 2,
                        chunkColors[piece % 7]);
  }

  for (uint8_t spark = 0; spark < 18; ++spark) {
    const int x = (centerX + spark * 23 + age / 13) % 64;
    const int y = (centerY + spark * 17 + age / 19) % 64;
    display->drawPixel(x, y, spark & 1 ? plasmaCyan : toxicLime);
  }
}

void drawEnd(bool failed, uint32_t now) {
  display->fillScreen(0);
  const uint32_t age = now - endShownAt;
  const uint32_t approachAge = min<uint32_t>(age, COMMANDER_APPROACH_MS);
  float entrance = approachAge / static_cast<float>(COMMANDER_APPROACH_MS);
  entrance = entrance * entrance * (3.0f - 2.0f * entrance);
  const int entranceY = lroundf(COMMANDER_GAME_Y +
      (COMMANDER_STAGE_Y - COMMANDER_GAME_Y) * entrance);
  const uint32_t stagedAge = age > COMMANDER_APPROACH_MS
                                 ? age - COMMANDER_APPROACH_MS : 0;
  const uint8_t noFire[3]{};
  if (failed && age < FAILURE_CINEMATIC_MS) {
    // The victorious commander targets a returning Earth, then the planet is
    // replaced by a deliberately ridiculous screen-filling explosion.
    const int descent = min<uint32_t>(stagedAge / 650, 3);
    uint8_t volley[3]{};
    if (age >= COMMANDER_APPROACH_MS) {
      volley[0] = volley[1] = volley[2] = 150;
    }
    const uint16_t enemyColor = ((stagedAge / 100) & 1)
                                    ? rgb(255, 55, 20)
                                    : rgb(80, 255, 25);
    drawInvaderShip(entranceY + descent, enemyColor, rgb(0, 0, 0),
                    ((stagedAge / 120) & 1) != 0, volley, 20);
    if (stagedAge < EARTH_TARGET_MS) {
      drawCinematicEarth(51, 12, stagedAge / 65);
      drawEarthTargetBeams(entranceY + descent, stagedAge);
      centeredSmallText("EARTH: UH OH", 27, rgb(255, 255, 255));
    } else {
      drawEarthExplosion(stagedAge - EARTH_TARGET_MS);
    }
    return;
  }

  if (failed) {
    drawInvaderShip(-8, rgb(80, 255, 25), rgb(0, 0, 0),
                    ((age / 180) & 1) != 0, noFire, 20);
  } else if (age < VICTORY_CINEMATIC_MS) {
    const uint32_t explosionAt = COMMANDER_APPROACH_MS +
                                 EARTH_COUNTERATTACK_MS;
    const uint32_t attackAge = age > COMMANDER_APPROACH_MS
                                   ? age - COMMANDER_APPROACH_MS : 0;
    if (age >= explosionAt) {
      drawEarthVictoryCannon(EARTH_COUNTERATTACK_MS);
      drawInvaderExplosion(age - explosionAt);
      return;
    }
    // Earth raises its oversized defence cannon while the commander moves on
    // stage; the invader strobes only when the beam reaches its cockpit.
    const bool takingHit = attackAge > EARTH_COUNTERATTACK_MS * 3 / 4;
    const uint16_t hitColor = takingHit && ((age / 55) & 1)
                                  ? rgb(255, 255, 255) : rgb(70, 255, 35);
    drawInvaderShip(entranceY, hitColor, rgb(255, 30, 30),
                    true, noFire, 20);
    drawEarthVictoryCannon(attackAge);
    return;
  }

  centeredSmallText(failed ? "INVADER WINS" : "ALIEN DEFEATED", 22,
                    failed ? rgb(255, 50, 25) : rgb(0, 255, 255));
  char scoreText[24];
  snprintf(scoreText, sizeof(scoreText), "SCORE %lu",
           static_cast<unsigned long>(score));
  centeredSmallText(scoreText, 29, rgb(255, 255, 255));
  char judgmentText[24];
  snprintf(judgmentText, sizeof(judgmentText), "P%u G%u M%u",
           perfects, goods, misses);
  centeredSmallText(judgmentText, 37, rgb(255, 255, 255));
  char comboText[24];
  snprintf(comboText, sizeof(comboText), "MAX COMBO %u", maxCombo);
  centeredSmallText(comboText, 45, rgb(255, 255, 255));
  const uint16_t hits = perfects + goods;
  const uint8_t accuracy = noteCount ? hits * 100 / noteCount : 0;
  const char grade = accuracy >= 95 ? 'S' : accuracy >= 85 ? 'A' :
                     accuracy >= 70 ? 'B' : accuracy >= 55 ? 'C' : 'D';
  display->setTextSize(2);
  display->setCursor(26, 51);
  display->setTextColor(rgb(255, 190, 0));
  display->print(grade);
}

void render(uint32_t now) {
  if (screen == Screen::Select) {
    drawSelect(now);
  } else if (screen == Screen::DifficultySelect) {
    drawDifficultySelect(now);
  } else if (screen == Screen::Playing) {
    drawPlaying(now);
  } else if (screen == Screen::Paused) {
    drawPlaying(pausedAt);
    display->fillRect(8, 22, 48, 20, rgb(5, 0, 18));
    centeredText("PAUSED", 25, rgb(255, 60, 200));
    centeredText("PUSH", 34, rgb(255, 255, 255));
  } else {
    drawEnd(screen == Screen::Failed, now);
  }
}

void releaseDisplayBuffers() {
  if (dmaDisplay != nullptr) dmaDisplay->stopDMAoutput();
  delete display;
  display = nullptr;
  delete dmaDisplay;
  dmaDisplay = nullptr;
  activeDmaColorDepth = 0;
  activeDisplayProfile = DisplayProfile::None;
}

bool allocateDisplayBuffers(uint8_t colorDepth) {
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
  config.double_buff = true;
  config.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  config.min_refresh_rate = 120;
  config.setPixelColorDepthBits(colorDepth);
  config.latch_blanking = 2;
  config.clkphase = false;

  MatrixPanel_I2S_DMA *newDma =
      new (std::nothrow) MatrixPanel_I2S_DMA(config);
  if (newDma == nullptr || !newDma->begin()) {
    delete newDma;
    return false;
  }
  auto *newVirtual = new (std::nothrow)
      VirtualMatrixPanel_T<PANEL_CHAIN_TYPE>(
          PANEL_ROWS, PANEL_COLS, PANEL_RES_X, PANEL_RES_Y);
  if (newVirtual == nullptr) {
    newDma->stopDMAoutput();
    delete newDma;
    return false;
  }
  newVirtual->setDisplay(*newDma);
  newDma->setBrightness8(90);
  newVirtual->clearScreen();

  dmaDisplay = newDma;
  display = newVirtual;
  activeDmaColorDepth = colorDepth;
  if (dmaDisplay->calculated_refresh_rate > 0) {
    const uint32_t panelFrameUs =
        1000000UL / dmaDisplay->calculated_refresh_rate;
    renderIntervalUs = max<uint32_t>(11111UL, panelFrameUs + 1500UL);
  } else {
    renderIntervalUs = DEFAULT_RENDER_INTERVAL_US;
  }
  return true;
}

bool rebuildDisplay(DisplayProfile profile) {
  if (profile == activeDisplayProfile && display != nullptr &&
      dmaDisplay != nullptr) return true;

  const uint8_t requestedDepth = profile == DisplayProfile::Select
                                     ? SELECT_COLOR_DEPTH_BITS
                                     : GAME_COLOR_DEPTH_BITS;
  releaseDisplayBuffers();
  // Give the I2S DMA driver a scheduling point after releasing its descriptors
  // before allocating a differently-sized set of buffers.
  delay(10);
  uint8_t allocatedDepth = requestedDepth;
  if (!allocateDisplayBuffers(requestedDepth)) {
    // The first select-to-game transition can briefly leave released DMA heap
    // blocks unavailable. Retry the smaller gameplay allocation once after the
    // allocator has had another scheduling interval; otherwise EASY can be
    // skipped even though its chart loaded correctly.
    if (profile == DisplayProfile::Game) {
      Serial.printf("Gameplay DMA retry; heap=%u largest=%u\n",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      delay(25);
      if (allocateDisplayBuffers(requestedDepth)) {
        activeDmaColorDepth = requestedDepth;
        activeDisplayProfile = profile;
        Serial.printf("HUB75 game buffers recovered: %u-bit colour, %u Hz\n",
                      activeDmaColorDepth,
                      dmaDisplay->calculated_refresh_rate);
        return true;
      }
    }
    // Selection can still operate if eight-bit double buffering does not fit
    // on a particular ESP32 revision. Do not repeatedly retry every frame.
    if (profile != DisplayProfile::Select ||
        !allocateDisplayBuffers(6)) {
      Serial.println(F("HUB75 DMA buffer rebuild failed"));
      return false;
    }
    allocatedDepth = 6;
    Serial.println(F("HUB75 selection buffer fell back to 6-bit colour"));
  }
  activeDmaColorDepth = allocatedDepth;
  activeDisplayProfile = profile;
  if (profile == DisplayProfile::Select) {
    // Covers and text do not require the gameplay presentation rate. Keep the
    // panel scanning continuously, but swap completed selection buffers only
    // at 30 FPS to reduce visible redraw/swap activity.
    renderIntervalUs = max(renderIntervalUs, SELECT_RENDER_INTERVAL_US);
  }
  Serial.printf("HUB75 %s buffers: %u-bit colour, %u Hz, %lu us/frame\n",
                profile == DisplayProfile::Select ? "selection" : "game",
                activeDmaColorDepth,
                dmaDisplay->calculated_refresh_rate,
                static_cast<unsigned long>(renderIntervalUs));
  return true;
}

void setup() {
  Serial.begin(115200);
  const bool songFilesystemReady = BopImport::begin();
  Serial.printf("Song filesystem %s; %u imported chart(s)\n",
                songFilesystemReady ? "mounted" : "unavailable",
                static_cast<unsigned>(BopImport::songCount()));
  loadSongCovers();
  if (songCount() == 0) startupTestActive = false;
  if (startupTestActive) selectedSong = 0;
  twistLeft.begin();
  twistRight.begin();
  pushInput.begin();
  pullRest.begin();
  pullFull.begin();
  previousPullState = readPullState();

  if (!rebuildDisplay(DisplayProfile::Select)) return;
  audioReady = initAudio();
  if (audioReady) {
    xTaskCreatePinnedToCore(audioTask, "bop-audio", 4096, nullptr, 2, nullptr, 0);
  }
  carouselChangedAt = millis();
  startupTestChangedAt = carouselChangedAt;
  Serial.printf("BOP Rhythm ready; PCM5102A audio %s\n",
                audioReady ? "enabled" : "disabled");
  Serial.println(F("GPIO34/35/36/39 require external pull-ups"));
}

void loop() {
  if (display == nullptr) {
    delay(1000);
    return;
  }

  uint32_t now = millis();
  updateInputs(now);
  updateStartupSelfTest(now);
  // Either input path above may synchronously start a run and rebuild the DMA
  // buffers. Refresh the frame clock before evaluating the newly started song.
  now = millis();
  if ((screen == Screen::Results || screen == Screen::Failed) &&
      now - endShownAt >= 4500) {
    screen = Screen::Select;
    carouselChangedAt = now;
  }
  if (screen == Screen::Playing && songClockStarted(now)) {
    const uint32_t t = songTime(now);
    updateStartupAutoPlay(t);
    updateHolds(t);
    expireMisses(t);
  }
  updateRepeatingDemo(now);

  const DisplayProfile desiredProfile =
      (screen == Screen::Select || screen == Screen::DifficultySelect)
          ? DisplayProfile::Select : DisplayProfile::Game;
  if (desiredProfile != activeDisplayProfile &&
      !rebuildDisplay(desiredProfile)) {
    delay(100);
    return;
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

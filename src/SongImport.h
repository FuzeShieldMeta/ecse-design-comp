#pragma once

#include <Arduino.h>
#include <FS.h>

namespace BopImport {

constexpr size_t MAX_SONGS = 8;
constexpr size_t MAX_NOTES = 220;
constexpr size_t MAX_GIMMICKS = 24;
constexpr size_t MAX_GIMMICK_NOTE_EFFECTS = 128;
constexpr size_t WAV_BLOCK_FRAMES = 128;
constexpr size_t COVER_WIDTH = 36;
constexpr size_t COVER_HEIGHT = 36;
constexpr size_t COVER_PIXELS = COVER_WIDTH * COVER_HEIGHT;
constexpr size_t COVER_RGB888_BYTES = COVER_PIXELS * 3;

struct Song {
  char title[17];
  char artist[17];
  char chartPath[48];
  char audioPath[48];
  char coverPath[48];
  uint16_t bpm;
  uint32_t color;
  uint32_t durationMs;
  uint32_t audioStartMs;
  bool audioLoop;
};

struct ChartNote {
  uint16_t id;
  uint32_t hitMs;
  uint8_t lane;       // 0=twist, 1=push, 2=pull
  bool variant;       // twist right / pull full
  bool bonus;
  uint16_t holdMs;
  bool endVariant;
  uint16_t transitionMs;
};

enum class GimmickType : uint8_t {
  Wind,
  ScreenFlash,
  LanePulse,
};

enum class GimmickPattern : uint8_t {
  Solid,
  Stripes,
  Checker,
};

struct ChartGimmick {
  uint16_t id;
  GimmickType type;
  uint32_t startMs;
  uint32_t durationMs;
  int8_t target;  // -1=all, otherwise display lane 0..2.
  uint32_t color;
  float brightness;
  float rateHz;
  float speed;
  int8_t direction;
  uint8_t density;
  GimmickPattern pattern;
};

struct ChartGimmickNoteEffect {
  uint16_t gimmickId;
  uint16_t noteId;
  uint8_t targetColumn;
  uint16_t startOffsetMs;
  uint16_t endOffsetMs;
};

struct Chart {
  ChartNote notes[MAX_NOTES];
  ChartGimmick gimmicks[MAX_GIMMICKS];
  ChartGimmickNoteEffect gimmickNoteEffects[MAX_GIMMICK_NOTE_EFFECTS];
  size_t noteCount;
  size_t gimmickCount;
  size_t gimmickNoteEffectCount;
  uint32_t durationMs;
};

// Mount LittleFS and load chart paths from /songs/index.txt.
bool begin();
size_t songCount();
const Song *song(size_t index);
bool loadChart(size_t index, uint8_t mapping, Chart &out);
// Loads the exact 36x36 raw RGB888 asset referenced by cover= in the chart.
bool loadCover(size_t index, uint8_t *destination, size_t byteCapacity);

class WavReader {
 public:
  bool open(const char *path);
  void close();
  bool isOpen() const;
  bool rewind();
  // Produces 44.1 kHz signed 16-bit stereo frames. PCM source files may use
  // 8/16-bit mono/stereo at 11.025, 22.05, or 44.1 kHz.
  size_t readStereo(int16_t *destination, size_t frameCount);

 private:
  File file_;
  uint32_t dataRemaining_ = 0;
  uint32_t dataStart_ = 0;
  uint32_t dataSize_ = 0;
  uint16_t channels_ = 0;
  uint8_t bitsPerSample_ = 0;
  uint8_t repeatFactor_ = 1;
  uint8_t repeatsRemaining_ = 0;
  int16_t currentLeft_ = 0;
  int16_t currentRight_ = 0;
  uint8_t readBuffer_[512]{};
  size_t readBufferPosition_ = 0;
  size_t readBufferSize_ = 0;

  bool readDataByte(uint8_t &value);
  bool readSourceFrame();
};

}  // namespace BopImport

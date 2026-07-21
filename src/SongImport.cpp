#include "SongImport.h"

#include <LittleFS.h>

namespace BopImport {
namespace {

Song songs[MAX_SONGS]{};
size_t loadedSongCount = 0;
bool filesystemMounted = false;

String valueAfterEquals(const String &line) {
  const int separator = line.indexOf('=');
  if (separator < 0) return String();
  String value = line.substring(separator + 1);
  value.trim();
  return value;
}

uint32_t parseColor(const String &value) {
  const char *text = value.c_str();
  if (value.startsWith("#")) ++text;
  return strtoul(text, nullptr, 16) & 0xFFFFFFUL;
}

size_t splitCsv(const String &value, String *fields, size_t capacity) {
  size_t count = 0;
  int start = 0;
  while (start <= value.length() && count < capacity) {
    int comma = value.indexOf(',', start);
    if (comma < 0) comma = value.length();
    fields[count] = value.substring(start, comma);
    fields[count].trim();
    ++count;
    start = comma + 1;
  }
  return count;
}

bool parseLane(const String &text, uint8_t &lane) {
  if (text.equalsIgnoreCase("twist")) lane = 0;
  else if (text.equalsIgnoreCase("push")) lane = 1;
  else if (text.equalsIgnoreCase("pull")) lane = 2;
  else return false;
  return true;
}

bool parseVariant(uint8_t lane, const String &text, bool &variant) {
  if (lane == 0) {
    if (text.equalsIgnoreCase("left")) variant = false;
    else if (text.equalsIgnoreCase("right")) variant = true;
    else return false;
  } else if (lane == 1) {
    if (!text.equalsIgnoreCase("tap") && !text.equalsIgnoreCase("same"))
      return false;
    variant = false;
  } else {
    if (text.equalsIgnoreCase("half")) variant = false;
    else if (text.equalsIgnoreCase("full")) variant = true;
    else return false;
  }
  return true;
}

bool parseGimmickType(const String &text, GimmickType &type) {
  if (text.equalsIgnoreCase("wind")) type = GimmickType::Wind;
  else if (text.equalsIgnoreCase("screen_flash"))
    type = GimmickType::ScreenFlash;
  else if (text.equalsIgnoreCase("lane_pulse"))
    type = GimmickType::LanePulse;
  else return false;
  return true;
}

bool parseGimmickTarget(const String &text, int8_t &target) {
  if (text.isEmpty() || text.equalsIgnoreCase("all")) {
    target = -1;
    return true;
  }
  uint8_t lane = 0;
  if (parseLane(text, lane)) {
    target = static_cast<int8_t>(lane);
    return true;
  }
  if (text.length() == 1 && text[0] >= '0' && text[0] <= '2') {
    target = static_cast<int8_t>(text[0] - '0');
    return true;
  }
  return false;
}

bool parseRgbColor(const String &text, uint32_t &color) {
  const int firstSeparator = text.indexOf(':');
  const int secondSeparator = text.indexOf(':', firstSeparator + 1);
  if (firstSeparator <= 0 || secondSeparator <= firstSeparator + 1 ||
      text.indexOf(':', secondSeparator + 1) >= 0) return false;
  String channels[3] = {
    text.substring(0, firstSeparator),
    text.substring(firstSeparator + 1, secondSeparator),
    text.substring(secondSeparator + 1)
  };
  uint8_t parsed[3]{};
  for (size_t i = 0; i < 3; ++i) {
    channels[i].trim();
    const long value = channels[i].toInt();
    if (value < 0 || value > 255 || String(value) != channels[i]) return false;
    parsed[i] = static_cast<uint8_t>(value);
  }
  color = (static_cast<uint32_t>(parsed[0]) << 16) |
          (static_cast<uint32_t>(parsed[1]) << 8) | parsed[2];
  return true;
}

bool parseFloatParameter(const String &text, float minimum, float maximum,
                         float &value) {
  char *end = nullptr;
  const float parsed = strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !isfinite(parsed) ||
      parsed < minimum || parsed > maximum) return false;
  value = parsed;
  return true;
}

bool parsePattern(const String &text, GimmickPattern &pattern) {
  if (text.equalsIgnoreCase("solid")) pattern = GimmickPattern::Solid;
  else if (text.equalsIgnoreCase("stripes"))
    pattern = GimmickPattern::Stripes;
  else if (text.equalsIgnoreCase("checker"))
    pattern = GimmickPattern::Checker;
  else return false;
  return true;
}

bool readMetadata(const char *path, Song &out) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;

  strlcpy(out.title, "UNTITLED", sizeof(out.title));
  strlcpy(out.artist, "UNKNOWN", sizeof(out.artist));
  out.audioPath[0] = '\0';
  out.coverPath[0] = '\0';
  out.bpm = 120;
  out.color = 0x00EAFF;
  out.durationMs = 0;
  out.audioStartMs = 0;
  out.audioLoop = false;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    const String value = valueAfterEquals(line);
    if (line.startsWith("title="))
      strlcpy(out.title, value.c_str(), sizeof(out.title));
    else if (line.startsWith("artist="))
      strlcpy(out.artist, value.c_str(), sizeof(out.artist));
    else if (line.startsWith("audio="))
      strlcpy(out.audioPath, value.c_str(), sizeof(out.audioPath));
    else if (line.startsWith("cover="))
      strlcpy(out.coverPath, value.c_str(), sizeof(out.coverPath));
    else if (line.startsWith("bpm="))
      out.bpm = constrain(value.toInt(), 30, 300);
    else if (line.startsWith("color="))
      out.color = parseColor(value);
    else if (line.startsWith("duration="))
      out.durationMs = max(0L, value.toInt());
    else if (line.startsWith("audio_start="))
      out.audioStartMs = max(0L, value.toInt());
    else if (line.startsWith("audio_loop="))
      out.audioLoop = value.toInt() != 0;
  }
  file.close();
  return true;
}

bool parseNote(const String &value, uint16_t generatedId, ChartNote &note) {
  String field[7];
  const size_t count = splitCsv(value, field, 7);
  if (count < 3 || !parseLane(field[1], note.lane)) return false;
  if (!parseVariant(note.lane, field[2], note.variant)) return false;

  note.id = generatedId;
  note.hitMs = max(0L, field[0].toInt());
  note.holdMs = count > 3 ? constrain(field[3].toInt(), 0, 60000) : 0;
  note.endVariant = note.variant;
  if (count > 4 && !field[4].isEmpty() &&
      !field[4].equalsIgnoreCase("same") &&
      !parseVariant(note.lane, field[4], note.endVariant)) return false;
  note.transitionMs = count > 5
                          ? constrain(field[5].toInt(), 0, note.holdMs) : 0;
  note.bonus = count > 6 && field[6].toInt() != 0;

  if (note.lane != 2) {
    note.endVariant = note.variant;
    note.transitionMs = 0;
  }
  if (note.transitionMs >= note.holdMs) note.transitionMs = 0;
  return true;
}

bool parseGimmick(const String &value, ChartGimmick &gimmick) {
  String field[12];
  const size_t count = splitCsv(value, field, 12);
  if (count < 4 || !parseGimmickType(field[1], gimmick.type)) return false;
  const long parsedId = field[0].toInt();
  const long parsedStart = field[2].toInt();
  const long parsedDuration = field[3].toInt();
  if (parsedId <= 0 || parsedId > 65535 || parsedStart < 0 ||
      parsedDuration <= 0) return false;
  gimmick.id = static_cast<uint16_t>(parsedId);
  gimmick.startMs = static_cast<uint32_t>(parsedStart);
  gimmick.durationMs = static_cast<uint32_t>(parsedDuration);
  gimmick.target = -1;
  gimmick.color = gimmick.type == GimmickType::Wind
      ? (35UL << 16) | (130UL << 8) | 180UL
      : gimmick.type == GimmickType::ScreenFlash
          ? (255UL << 16) | (255UL << 8) | 255UL
          : (45UL << 16) | (60UL << 8) | 120UL;
  gimmick.brightness = gimmick.type == GimmickType::Wind ? 1.0f :
                        gimmick.type == GimmickType::ScreenFlash ? 0.14f :
                                                                   0.18f;
  gimmick.rateHz = 0.0f;
  gimmick.speed = 1.0f;
  gimmick.direction = 1;
  gimmick.density = 4;
  gimmick.pattern = gimmick.type == GimmickType::LanePulse
                        ? GimmickPattern::Solid
                        : GimmickPattern::Stripes;
  for (size_t i = 4; i < count; ++i) {
    const int separator = field[i].indexOf('=');
    if (separator <= 0) return false;
    String key = field[i].substring(0, separator);
    String parameter = field[i].substring(separator + 1);
    key.trim();
    parameter.trim();
    if (key.equalsIgnoreCase("target")) {
      if (!parseGimmickTarget(parameter, gimmick.target)) return false;
    } else if (key.equalsIgnoreCase("color")) {
      if (!parseRgbColor(parameter, gimmick.color)) return false;
    } else if (key.equalsIgnoreCase("brightness")) {
      if (!parseFloatParameter(parameter, 0.0f, 1.0f,
                               gimmick.brightness)) return false;
    } else if (key.equalsIgnoreCase("rate")) {
      if (!parseFloatParameter(parameter, 0.0f, 20.0f,
                               gimmick.rateHz)) return false;
    } else if (key.equalsIgnoreCase("speed")) {
      if (!parseFloatParameter(parameter, 0.25f, 4.0f,
                               gimmick.speed)) return false;
    } else if (key.equalsIgnoreCase("direction")) {
      if (parameter.equalsIgnoreCase("left")) gimmick.direction = -1;
      else if (parameter.equalsIgnoreCase("right")) gimmick.direction = 1;
      else return false;
    } else if (key.equalsIgnoreCase("density")) {
      const long density = parameter.toInt();
      if (density < 1 || density > 8 || String(density) != parameter)
        return false;
      gimmick.density = static_cast<uint8_t>(density);
    } else if (key.equalsIgnoreCase("pattern")) {
      if (!parsePattern(parameter, gimmick.pattern)) return false;
    } else return false;
  }
  return true;
}

bool parseGimmickNoteEffect(const String &value,
                            ChartGimmickNoteEffect &effect) {
  String field[6];
  if (splitCsv(value, field, 6) != 6 ||
      !field[2].equalsIgnoreCase("column")) return false;
  const long gimmickId = field[0].toInt();
  const long noteId = field[1].toInt();
  const long targetColumn = field[3].toInt();
  const long startOffset = field[4].toInt();
  const long endOffset = field[5].toInt();
  if (gimmickId <= 0 || gimmickId > 65535 || noteId <= 0 ||
      noteId > 65535 || targetColumn < 0 || targetColumn > 2 ||
      startOffset < 0 || startOffset > 65535 ||
      endOffset <= startOffset || endOffset > 65535) return false;
  effect.gimmickId = static_cast<uint16_t>(gimmickId);
  effect.noteId = static_cast<uint16_t>(noteId);
  effect.targetColumn = static_cast<uint8_t>(targetColumn);
  effect.startOffsetMs = static_cast<uint16_t>(startOffset);
  effect.endOffsetMs = static_cast<uint16_t>(endOffset);
  return true;
}

uint16_t readU16(File &file) {
  uint8_t bytes[2];
  if (file.read(bytes, 2) != 2) return 0;
  return static_cast<uint16_t>(bytes[0]) |
         (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readU32(File &file) {
  uint8_t bytes[4];
  if (file.read(bytes, 4) != 4) return 0;
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

}  // namespace

bool begin() {
  loadedSongCount = 0;
  filesystemMounted = LittleFS.begin(false);
  if (!filesystemMounted) return false;

  File index = LittleFS.open("/songs/index.txt", "r");
  if (!index) return true;
  while (index.available() && loadedSongCount < MAX_SONGS) {
    String path = index.readStringUntil('\n');
    path.trim();
    if (path.isEmpty() || path.startsWith("#")) continue;
    if (!path.startsWith("/")) path = "/songs/" + path;

    Song candidate{};
    strlcpy(candidate.chartPath, path.c_str(), sizeof(candidate.chartPath));
    if (readMetadata(candidate.chartPath, candidate)) {
      songs[loadedSongCount++] = candidate;
    } else {
      Serial.printf("Skipping missing or unsupported chart: %s\n",
                    candidate.chartPath);
    }
  }
  index.close();
  return true;
}

size_t songCount() { return loadedSongCount; }

const Song *song(size_t index) {
  return index < loadedSongCount ? &songs[index] : nullptr;
}

bool loadCover(size_t index, uint8_t *destination, size_t byteCapacity) {
  const Song *metadata = song(index);
  if (metadata == nullptr || destination == nullptr ||
      byteCapacity < COVER_RGB888_BYTES || metadata->coverPath[0] == '\0')
    return false;
  File file = LittleFS.open(metadata->coverPath, "r");
  if (!file) return false;
  if (file.size() != COVER_RGB888_BYTES) {
    file.close();
    return false;
  }
  const size_t read = file.read(destination, COVER_RGB888_BYTES);
  file.close();
  return read == COVER_RGB888_BYTES;
}

bool loadChart(size_t index, uint8_t mapping, Chart &out) {
  const Song *metadata = song(index);
  if (metadata == nullptr || !filesystemMounted || mapping > 2) return false;
  File file = LittleFS.open(metadata->chartPath, "r");
  if (!file) return false;

  out.noteCount = 0;
  out.gimmickCount = 0;
  out.gimmickNoteEffectCount = 0;
  out.durationMs = metadata->durationMs;
  uint32_t inferredDuration = 0;
  bool parseError = false;
  int8_t activeMapping = -1;
  bool selectedMappingFound = false;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    if (line.startsWith("mapping=")) {
      const String value = valueAfterEquals(line);
      if (value.length() != 1 || value[0] < '0' || value[0] > '2') {
        parseError = true;
        continue;
      }
      activeMapping = static_cast<int8_t>(value[0] - '0');
      if (activeMapping == mapping) {
        if (selectedMappingFound) parseError = true;
        selectedMappingFound = true;
      }
      continue;
    }
    if (activeMapping != mapping) continue;
    if (line.startsWith("note=")) {
      ChartNote parsed{};
      if (out.noteCount < MAX_NOTES &&
          parseNote(valueAfterEquals(line), out.noteCount + 1, parsed)) {
        out.notes[out.noteCount++] = parsed;
        const uint32_t noteEnd = parsed.hitMs + parsed.holdMs +
                                 static_cast<uint32_t>(1000);
        inferredDuration = max(inferredDuration, noteEnd);
      } else parseError = true;
    } else if (line.startsWith("gimmick=")) {
      ChartGimmick parsed{};
      if (out.gimmickCount < MAX_GIMMICKS &&
          parseGimmick(valueAfterEquals(line), parsed))
        out.gimmicks[out.gimmickCount++] = parsed;
      else parseError = true;
    } else if (line.startsWith("gimmick_note=")) {
      ChartGimmickNoteEffect parsed{};
      if (out.gimmickNoteEffectCount < MAX_GIMMICK_NOTE_EFFECTS &&
          parseGimmickNoteEffect(valueAfterEquals(line), parsed))
        out.gimmickNoteEffects[out.gimmickNoteEffectCount++] = parsed;
      else parseError = true;
    }
  }
  file.close();
  if (parseError || !selectedMappingFound || out.noteCount == 0) return false;
  for (size_t i = 0; i < out.gimmickCount; ++i) {
    for (size_t j = i + 1; j < out.gimmickCount; ++j)
      if (out.gimmicks[i].id == out.gimmicks[j].id) return false;
  }
  for (size_t i = 0; i < out.gimmickNoteEffectCount; ++i) {
    const ChartGimmickNoteEffect &effect = out.gimmickNoteEffects[i];
    const ChartGimmick *gimmick = nullptr;
    bool noteFound = false;
    for (size_t j = 0; j < out.gimmickCount; ++j)
      if (out.gimmicks[j].id == effect.gimmickId) {
        gimmick = &out.gimmicks[j];
        break;
      }
    for (size_t j = 0; j < out.noteCount; ++j)
      if (out.notes[j].id == effect.noteId) {
        noteFound = true;
        break;
      }
    if (gimmick == nullptr || !noteFound ||
        gimmick->type != GimmickType::Wind ||
        effect.endOffsetMs > gimmick->durationMs) return false;
    for (size_t j = i + 1; j < out.gimmickNoteEffectCount; ++j)
      if (out.gimmickNoteEffects[j].noteId == effect.noteId) return false;
  }
  if (out.durationMs == 0) out.durationMs = inferredDuration;
  return true;
}

bool WavReader::open(const char *path) {
  close();
  if (!filesystemMounted || path == nullptr || path[0] == '\0') return false;
  file_ = LittleFS.open(path, "r");
  if (!file_) return false;

  char id[4];
  if (file_.read(reinterpret_cast<uint8_t *>(id), 4) != 4 ||
      memcmp(id, "RIFF", 4) != 0) {
    close();
    return false;
  }
  readU32(file_);
  if (file_.read(reinterpret_cast<uint8_t *>(id), 4) != 4 ||
      memcmp(id, "WAVE", 4) != 0) {
    close();
    return false;
  }

  bool formatValid = false;
  uint32_t dataStart = 0;
  while (file_.available()) {
    if (file_.read(reinterpret_cast<uint8_t *>(id), 4) != 4) break;
    const uint32_t chunkSize = readU32(file_);
    const uint32_t chunkData = file_.position();
    if (memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
      const uint16_t encoding = readU16(file_);
      channels_ = readU16(file_);
      const uint32_t sampleRate = readU32(file_);
      readU32(file_);
      readU16(file_);
      const uint16_t bits = readU16(file_);
      bitsPerSample_ = bits;
      repeatFactor_ = sampleRate > 0 && 44100 % sampleRate == 0
                          ? 44100 / sampleRate : 0;
      formatValid = encoding == 1 && (channels_ == 1 || channels_ == 2) &&
                    (bits == 8 || bits == 16) &&
                    (repeatFactor_ == 1 || repeatFactor_ == 2 ||
                     repeatFactor_ == 4);
    } else if (memcmp(id, "data", 4) == 0) {
      dataStart = chunkData;
      dataRemaining_ = chunkSize;
    }
    file_.seek(chunkData + chunkSize + (chunkSize & 1));
    if (formatValid && dataStart != 0) break;
  }
  if (!formatValid || dataStart == 0) {
    close();
    return false;
  }
  dataStart_ = dataStart;
  dataSize_ = dataRemaining_;
  repeatsRemaining_ = 0;
  readBufferPosition_ = readBufferSize_ = 0;
  file_.seek(dataStart_);
  return true;
}

void WavReader::close() {
  if (file_) file_.close();
  dataRemaining_ = 0;
  dataStart_ = 0;
  dataSize_ = 0;
  channels_ = 0;
  bitsPerSample_ = 0;
  repeatFactor_ = 1;
  repeatsRemaining_ = 0;
  readBufferPosition_ = readBufferSize_ = 0;
}

bool WavReader::isOpen() const { return static_cast<bool>(file_); }

bool WavReader::rewind() {
  if (!file_ || dataStart_ == 0 || dataSize_ == 0) return false;
  if (!file_.seek(dataStart_)) return false;
  dataRemaining_ = dataSize_;
  repeatsRemaining_ = 0;
  readBufferPosition_ = readBufferSize_ = 0;
  return true;
}

bool WavReader::readDataByte(uint8_t &value) {
  if (readBufferPosition_ >= readBufferSize_) {
    if (dataRemaining_ == 0) return false;
    const size_t requested = min(dataRemaining_,
                                 static_cast<uint32_t>(sizeof(readBuffer_)));
    readBufferSize_ = file_.read(readBuffer_, requested);
    readBufferPosition_ = 0;
    dataRemaining_ -= readBufferSize_;
    if (readBufferSize_ == 0) return false;
  }
  value = readBuffer_[readBufferPosition_++];
  return true;
}

bool WavReader::readSourceFrame() {
  auto readChannel = [&](int16_t &sample) {
    uint8_t low = 0;
    if (!readDataByte(low)) return false;
    if (bitsPerSample_ == 8) {
      sample = static_cast<int16_t>((static_cast<int>(low) - 128) << 8);
      return true;
    }
    uint8_t high = 0;
    if (!readDataByte(high)) return false;
    sample = static_cast<int16_t>(static_cast<uint16_t>(low) |
                                  (static_cast<uint16_t>(high) << 8));
    return true;
  };

  if (!readChannel(currentLeft_)) return false;
  if (channels_ == 2) {
    if (!readChannel(currentRight_)) return false;
  } else {
    currentRight_ = currentLeft_;
  }
  repeatsRemaining_ = repeatFactor_;
  return true;
}

size_t WavReader::readStereo(int16_t *destination, size_t frameCount) {
  if (!file_ || destination == nullptr || frameCount == 0) return 0;
  frameCount = min(frameCount, WAV_BLOCK_FRAMES);
  size_t produced = 0;
  while (produced < frameCount) {
    if (repeatsRemaining_ == 0 && !readSourceFrame()) break;
    destination[produced * 2] = currentLeft_;
    destination[produced * 2 + 1] = currentRight_;
    --repeatsRemaining_;
    ++produced;
  }
  return produced;
}

}  // namespace BopImport

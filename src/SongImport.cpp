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

bool readMetadata(const char *path, Song &out) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  bool versionSupported = false;

  strlcpy(out.title, "UNTITLED", sizeof(out.title));
  strlcpy(out.artist, "UNKNOWN", sizeof(out.artist));
  out.audioPath[0] = '\0';
  out.coverPath[0] = '\0';
  out.bpm = 120;
  out.difficulty = 0;
  out.color = 0x00EAFF;
  out.durationMs = 0;
  out.audioStartMs = 0;
  out.audioLoop = false;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    const String value = valueAfterEquals(line);
    if (line.startsWith("version="))
      versionSupported = value.equalsIgnoreCase("BOP1");
    else if (line.startsWith("title="))
      strlcpy(out.title, value.c_str(), sizeof(out.title));
    else if (line.startsWith("artist="))
      strlcpy(out.artist, value.c_str(), sizeof(out.artist));
    else if (line.startsWith("audio="))
      strlcpy(out.audioPath, value.c_str(), sizeof(out.audioPath));
    else if (line.startsWith("cover="))
      strlcpy(out.coverPath, value.c_str(), sizeof(out.coverPath));
    else if (line.startsWith("bpm="))
      out.bpm = constrain(value.toInt(), 30, 300);
    else if (line.startsWith("difficulty="))
      out.difficulty = constrain(value.toInt(), 0, 2);
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
  return versionSupported;
}

bool parseNote(const String &value, ChartNote &note) {
  String field[11];
  const size_t count = splitCsv(value, field, 11);
  if (count < 4 || !parseLane(field[1], note.lane)) return false;
  if (!parseVariant(note.lane, field[2], note.variant)) return false;

  note.hitMs = max(0L, field[0].toInt());
  note.holdMs = constrain(field[3].toInt(), 0, 60000);
  note.endVariant = note.variant;
  if (count > 4 && !field[4].isEmpty() &&
      !field[4].equalsIgnoreCase("same") &&
      !parseVariant(note.lane, field[4], note.endVariant)) return false;
  note.transitionMs = count > 5
                          ? constrain(field[5].toInt(), 0, note.holdMs) : 0;
  note.bonus = count > 6 && field[6].toInt() != 0;
  note.startColumn = note.lane;
  note.endColumn = note.lane;
  note.shiftStartMs = 0;
  note.shiftEndMs = 0;
  if (count > 7 && !field[7].isEmpty())
    note.startColumn = constrain(field[7].toInt(), 0, 2);
  if (count > 8 && !field[8].isEmpty())
    note.endColumn = constrain(field[8].toInt(), 0, 2);
  if (count > 9) note.shiftStartMs = max(0L, field[9].toInt());
  if (count > 10) note.shiftEndMs = max(0L, field[10].toInt());

  if (note.lane != 2) {
    note.endVariant = note.variant;
    note.transitionMs = 0;
  }
  if (note.transitionMs >= note.holdMs) note.transitionMs = 0;
  if (note.startColumn != note.endColumn &&
      note.shiftEndMs <= note.shiftStartMs) return false;
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

bool loadChart(size_t index, Chart &out) {
  const Song *metadata = song(index);
  if (metadata == nullptr || !filesystemMounted) return false;
  File file = LittleFS.open(metadata->chartPath, "r");
  if (!file) return false;

  out.noteCount = 0;
  out.gimmickCount = 0;
  out.durationMs = metadata->durationMs;
  uint32_t inferredDuration = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    if (line.startsWith("note=") && out.noteCount < MAX_NOTES) {
      ChartNote parsed{};
      if (parseNote(valueAfterEquals(line), parsed)) {
        out.notes[out.noteCount++] = parsed;
        const uint32_t noteEnd = parsed.hitMs + parsed.holdMs +
                                 static_cast<uint32_t>(1000);
        inferredDuration = max(inferredDuration, noteEnd);
      }
    } else if (line.startsWith("wind=") &&
               out.gimmickCount < MAX_GIMMICKS) {
      String field[2];
      if (splitCsv(valueAfterEquals(line), field, 2) == 2) {
        ChartGimmick &gimmick = out.gimmicks[out.gimmickCount++];
        gimmick.startMs = max(0L, field[0].toInt());
        gimmick.durationMs = max(1L, field[1].toInt());
      }
    }
  }
  file.close();
  if (out.noteCount == 0) return false;
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

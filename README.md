# BOP Rhythm

BOP Rhythm is an ESP32-WROOM-32 rhythm game displayed on a 64×64 LED matrix
made from two daisy-chained 64×32 HUB75 panels. It uses a spring-return twist
control, a push button, a two-switch pull control, and a PCM5102A DAC feeding an
external amplifier and speaker.

## Hardware

- ESP32-WROOM-32 38-pin development board
- Two 64×32 HUB75 LED panels
- PCM5102A I²S DAC module
- 60 W audio amplifier and compatible speaker
- Twist-left and twist-right normally-open switches
- One normally-open push button
- Pull-rest and pull-full microswitches
- Four 10 kΩ pull-up resistors for GPIO34, GPIO35, GPIO36, and GPIO39
- Regulated panel, amplifier, and controller/DAC power supplies as required

Disconnect all power before changing wiring. Never power the LED panels,
amplifier, or speaker from an ESP32 GPIO pin.

## Complete ESP32 pin allocation

| Component | Signal | ESP32 GPIO | Wiring notes |
| --- | --- | ---: | --- |
| HUB75 | R1 | 25 | Panel data input |
| HUB75 | G1 | 26 | Panel data input |
| HUB75 | B1 | 27 | Panel data input |
| HUB75 | R2 | 14 | Panel data input |
| HUB75 | G2 | 13 | Panel data input |
| HUB75 | B2 | 33 | Panel data input |
| HUB75 | A | 23 | Row address |
| HUB75 | B | 19 | Row address |
| HUB75 | C | 18 | Row address |
| HUB75 | D | 17 | Row address |
| HUB75 | E | Not connected | 1/16-scan 64×32 panels do not use E |
| HUB75 | LAT / STB | 32 | Latch |
| HUB75 | OE | 21 | Output enable |
| HUB75 | CLK | 22 | Pixel clock |
| Twist control | Left switch | 34 | Active-low; external 10 kΩ pull-up required |
| Twist control | Right switch | 35 | Active-low; external 10 kΩ pull-up required |
| Push control | Push button | 36 | Active-low; external 10 kΩ pull-up required |
| Pull control | Rest microswitch | 39 | Active-low; external 10 kΩ pull-up required |
| Pull control | Full microswitch | 16 | Active-low; firmware enables internal pull-up |
| PCM5102A | BCK / BCLK | 4 | I²S bit clock |
| PCM5102A | LCK / LRCK / WS | 5 | I²S left/right word clock |
| PCM5102A | DIN / DATA | 15 | I²S audio data from ESP32 |
| All low-voltage modules | GND | GND | All signal grounds must share a reference |

The HUB75 mapping is fixed by the firmware. GPIO6 through GPIO11 are connected
to the ESP32 module's flash and must not be used.

GPIO4, GPIO5, and GPIO15 are ESP32 boot-strapping pins. A PCM5102A normally
presents high-impedance inputs and should not disturb boot, but do not add
pull-up or pull-down resistors to these three I²S lines. If the board fails to
boot, disconnect the DAC while diagnosing its breakout-board circuitry.

## HUB75 panels

Connect the ESP32 HUB75 signals in the table to the **IN** connector of the
first panel. Connect that panel's **OUT** connector to the second panel's
**IN** connector with a HUB75 ribbon cable. The firmware is configured for two
panels stacked vertically using `CHAIN_TOP_RIGHT_DOWN`, with the top panel first
in the chain.

Power both panels from a separate regulated 5 V supply sized for their combined
maximum current. Use adequately sized power wiring and inject power into both
panels rather than carrying the second panel's full current through the first
panel. Fit branch fuses when appropriate. Connect the panel power-supply ground
to ESP32 ground, but do not feed panel power through the ESP32 USB connector or
5 V pin.

If the image is mirrored, rotated, or the panel order is reversed, correct the
physical orientation or the `PANEL_CHAIN_TYPE` setting; do not change the GPIO
mapping to fix geometry.

## Controls

Every switch is wired active-low: one terminal connects to its assigned GPIO
and the other terminal connects to GND. Never apply 5 V to an ESP32 input.

GPIO34, GPIO35, GPIO36, and GPIO39 do not have internal pull-ups. For each of
these pins, connect a 10 kΩ resistor between the GPIO and ESP32 3.3 V. GPIO16
uses the ESP32's internal pull-up, although an external 10 kΩ pull-up may also
be fitted for consistent noise immunity.

### Twist block

- Centre/neutral: neither switch is closed.
- Twist left: the GPIO34 switch closes to GND.
- Twist right: the GPIO35 switch closes to GND.
- The spring mechanism must return the block to centre when released.
- The mechanics should prevent both twist switches closing simultaneously.
- On song selection, twist moves through the circular song carousel. In the
  difficulty popup it moves through EASY, NORM, and HARD without wrapping past
  either end.

### Push button

Use a normally-open momentary button between GPIO36 and GND. On the song-select
screen it opens the selected song's difficulty popup. Press it again to confirm
the highlighted difficulty and start the song. During gameplay it controls the
push lane.

### Pull mechanism

- At 0% travel/rest, the GPIO39 rest microswitch is closed.
- During a half pull, neither microswitch is closed.
- At 100% travel/full pull, the GPIO16 full microswitch is closed.
- Both switches closed at once is treated as a pull-mechanism fault.

The firmware detects a half-pull action when the mechanism leaves the rest
switch and detects a full-pull action when the full switch closes. Position the
switches so the rest switch releases before the full switch can close.
A full/hard pull in the difficulty popup cancels it and returns to the song
carousel without starting the song.

Pull notes use two visual states. A solid blue note requests full extension; a
blue note that becomes noticeably darker toward its centre requests the
half-pull position. Long pull notes can change between these appearances along
their trail. Move from half to full, or full to half, when that boundary reaches
the timing line. The normal hit tolerance is applied around the transition.

### Start-of-song cinematic

During gameplay the commander remains beyond the upper edge unless a
`commander` gimmick stages it again. Lane-column tops and note launch rows are
derived from the moving turret position each frame, so they follow every
commander entrance and withdrawal.

## PCM5102A, amplifier, and speaker

Wire the digital side of the PCM5102A as follows:

| PCM5102A pin | Connect to |
| --- | --- |
| BCK / BCLK | ESP32 GPIO4 |
| LCK / LRCK / WS | ESP32 GPIO5 |
| DIN / DATA | ESP32 GPIO15 |
| GND | ESP32 GND |
| VIN | Supply voltage required by the specific breakout board |

The ESP32 sends 44.1 kHz, 16-bit stereo I²S audio. The same program audio is
placed on the left and right channels. The HUB75 driver uses ESP32 I²S1 and the
PCM5102A uses I²S0, so both can operate simultaneously.

PCM5102A breakout-board power and configuration pins vary. Check the module's
schematic before applying power: some boards accept 5 V at VIN through an
on-board regulator, while others require 3.3 V. On common modules, SCK can be
tied low to use the internal PLL, XSMT must be high to unmute, and FLT/DEMP can
be tied low for normal filtering with de-emphasis disabled. Do not assume those
connections without checking the exact module.

Connect PCM5102A `LOUT`, `ROUT`, and audio ground to the amplifier's **line
input**. Do not connect a PCM5102A output directly to the speaker or to an
amplifier's speaker output. For a mono amplifier, use one DAC channel or combine
left and right through separate mixing resistors (typically 1–10 kΩ each); never
short `LOUT` and `ROUT` directly together.

Power the 60 W amplifier from its own correctly rated supply. Confirm that the
speaker impedance and power rating are supported by the amplifier. Connect the
speaker only to the amplifier's speaker-output terminals. Begin testing with
the amplifier gain at minimum and increase it gradually.

Use a common signal-ground reference between the ESP32, PCM5102A, and amplifier
input. Keep high-current panel and speaker wiring away from the I²S and input
switch wiring. A star-ground arrangement near the power entry is preferable if
panel noise is audible.

## Power checklist

- ESP32: USB or a suitable regulated board supply.
- HUB75 panels: separate regulated 5 V high-current supply.
- PCM5102A: the voltage required by its particular breakout board.
- Amplifier: separate supply matching the amplifier's voltage/current rating.
- Speaker: connect only to the amplifier output.
- Grounds: join ESP32, panel, DAC, and amplifier signal grounds at a controlled
  common point.

Do not connect the positive outputs of independent supplies together unless the
power design explicitly requires it. Only their ground references normally need
to be common for the signals used here.

## Importing songs and charts

All songs are loaded from the ESP32's LittleFS partition. There are no
firmware-compiled songs: if the filesystem is missing or contains no valid
charts, the display shows `NO SONG FILES` instead of starting an empty game.
The importer supports up to eight external songs. Each of a song's three
difficulty mappings may contain up to 220 notes, 24 gimmicks, and 128
gimmick-to-note effects.

The former `TEST GRID`, `SYNC STEP`, and `PULL RUSH` demonstrations are now
ordinary imported charts with accompanying loopable mono WAV files in
`data/songs/`. Their source generator is `tools/generate_demo_assets.py`; run it
after intentionally changing those generated charts or sounds:

```sh
python3 tools/generate_demo_assets.py
```

Place song files in `data/songs/`. Add each chart filename to
`data/songs/index.txt`, one per line, in the order it should appear in the song
carousel:

```text
my-song.bop
another-song.bop
```

Lines beginning with `#` and blank lines are ignored. Upload firmware and the
filesystem image separately:

```sh
pio run -t upload
pio run -t uploadfs
```

The serial monitor reports whether LittleFS mounted, how many charts were
found, chart-load failures, and unsupported WAV files.

### Chart format

Charts are UTF-8 text files using the BOP chart format. It has no version
field; its record types are validated when the chart is loaded. A complete template is
provided in `examples/songs/example.bop`; it is kept outside `data/` so it is
not copied into the device filesystem.

```text
title=EXAMPLE SONG
artist=YOUR NAME
bpm=120
color=#8A32FF
audio=/songs/example.wav
cover=/songs/example.rgb888
audio_start=0
audio_loop=0
duration=12000

mapping=0
note=2000,twist,left

mapping=1
note=2000,twist,left
note=2500,push,tap

mapping=2
note=2000,twist,left
note=2250,pull,half
note=2500,push,tap
```

- `title` and `artist` are limited to 16 characters.
- `color` is the six-digit RGB cover-art colour.
- `cover` is the absolute path to an exact 36×36 raw RGB888 cover asset.
  The colour is retained as the carousel border and missing-cover fallback.
- `duration` is the total chart duration in milliseconds. If it is zero or
  omitted, the importer uses the last note plus one second.
- `audio` is an absolute LittleFS path to the accompanying WAV file.
- `audio_start` delays WAV playback by the given song-time milliseconds.
- `audio_loop=1` loops the WAV until the chart ends. This permits a short music
  loop to accompany a longer chart without exhausting onboard flash.

Every chart must contain `mapping=0`, `mapping=1`, and `mapping=2` sections,
shown as EASY, NORM, and HARD. Push opens the difficulty popup, twist highlights
a bounded option, and push confirms it. A full/hard pull cancels the popup.
Difficulty does not shrink the judgement windows: every mapping uses the same
70 ms Perfect and 130 ms Good windows. Each successive mapping instead contains
a denser transcription of the music's rhythmic and spectral cues.

Each note uses this comma-separated layout:

```text
note=hit_ms,lane,action[,hold_ms,end_action,transition_ms,bonus]
```

The first three fields are required. Optional hold fields are omitted entirely
for ordinary notes. Within each mapping, notes receive automatic 1-based IDs in
file order: the first `note=` line is note 1, the second is note 2, and so on.
IDs restart at 1 in the next mapping. Gimmicks and their `gimmick_note`
references belong to the mapping in which they appear, so rearranging note
lines requires updating references in that mapping.

| Field | Accepted values |
|---|---|
| `lane` | `twist`, `push`, or `pull` |
| Twist `action` | `left` or `right` |
| Push `action` | `tap` |
| Pull `action` | `half` or `full` |
| `hold_ms` | `0` for a normal note; otherwise the hold duration |
| `end_action` | `same`, `half`, or `full`; pull holds only |
| `transition_ms` | Offset from the start of a pull hold where its state changes |
| `bonus` | `0` or `1` |

Examples:

```text
note=2000,twist,left
note=3000,pull,half
note=5000,pull,half,1600,full,800,0
note=8000,pull,full,1600,half,800,1
```

The logical `lane` determines both the physical control and default display
column. Notes no longer carry redundant start/end columns or absolute shift
times. Any temporary change is owned by a referenced gimmick.

Typed gimmicks use this layout:

```text
gimmick=id,type,start_ms,duration_ms[,key=value...]
```

Gimmick IDs remain explicit because note effects refer to them. IDs must be
unique positive integers. Optional named parameters may appear in any order:

| Parameter | Values and behavior |
|---|---|
| `target` | `all`, `twist`, `push`, `pull`, or display column `0`–`2` |
| `color` | Decimal `R:G:B`, with each channel from `0`–`255`, such as `255:128:64` |
| `brightness` | `0.0`–`1.0`, multiplied into the selected colour |
| `rate` | Flash/pulse effects: `0.0`–`20.0` pulses per second; zero produces one fade-in/fade-out envelope over the full duration |
| `pattern` | Flash/pulse effects: `solid`, `stripes`, or `checker` |
| `direction` | Wind only: `left` or `right` |
| `speed` | Wind only: `0.25`–`4.0` movement multiplier |
| `density` | Wind only: `1`–`8` simultaneous streak rows |

Wind `speed` controls only how quickly the visible streaks cross the matrix:
`0.5` is half speed, `1.0` is the default, and `2.0` is twice as fast. It does
not alter the song tempo, note descent, hit time, or column-shift duration.
Wind `density` is the number of streak rows distributed through the upper
gimmick region. Increasing it makes the gust look fuller but does not move
additional notes. Which notes move, where they move, and for how long remain
defined exclusively by the associated `gimmick_note` records. `direction`
also controls the streak animation only; chart authors should choose matching
target columns when they want every moved note to follow the visual direction.

Parameters not supplied use type-specific defaults:

| Type | Default colour | Brightness | Pattern | Additional defaults |
|---|---|---:|---|---|
| `wind` | `35:130:180` | `1.0` | `stripes` | right, speed `1.0`, density `4` |
| `screen_flash` | `255:255:255` | `0.14` | `stripes` | target `all`, rate `0` |
| `lane_pulse` | `45:60:120` | `0.18` | `solid` | target `all`, rate `0` |
| `commander` | `60:255:35` | `1.0` | `stripes` | target `all`, rate `0` |

The current renderer supports:

| Type | Effect |
|---|---|
| `wind` | Animated gust streaks with configurable colour, direction, speed, and density |
| `screen_flash` | A configurable colour/pattern pulse across the entire screen or selected lane |
| `lane_pulse` | A configurable full-height pulse behind one lane or all three lanes |
| `commander` | Brings the alien commander fully down from above the screen, holds it for the gimmick, then withdraws it |

Examples with additional parameters:

```text
gimmick=2,screen_flash,16000,1200,target=all,color=255:208:96,brightness=0.30,rate=4.0,pattern=checker
gimmick=3,lane_pulse,22000,2400,target=pull,color=150:60:255,brightness=0.24,rate=2.0,pattern=solid
gimmick=4,commander,28000,3200,color=80:255:35,brightness=1.0
```

The `commander` entrance and exit each take up to 520 ms. During ordinary
gameplay the commander looms partially beyond the top edge; this gimmick moves
the full sprite and its three turrets onto the matrix. `color` and `brightness`
control its staged appearance.

Both effects are visual-only and are rendered behind notes, timing bars,
separators, and the health bar. They may cover the full matrix because they do
not alter note positions or input mappings. The protected lower 60% restriction
continues to apply to note-changing effects such as wind column movement.

A gimmick changes a particular note by referring to its ID:

```text
gimmick_note=gimmick_id,note_id,column,target_column,start_offset_ms,end_offset_ms
```

For example:

```text
gimmick=1,wind,9000,1800,target=all,color=35:130:180,brightness=1.0,direction=right,speed=1.0,density=4
gimmick_note=1,5,column,1,0,1000
note=10500,twist,left
```

This gust starts at 9000 ms. Note 5 moves from its default twist column to
column 1 between offsets 0 and 1000 ms, equivalent to song times 9000–10000 ms.
It still requires the twist-left control. Effect offsets must remain inside the
referenced gimmick duration and should finish before the note enters the
protected lower 60% of the screen. The firmware validates IDs and resolves all
references once when loading the chart. In this example the twist note must be
the fifth `note=` record in the file.

Chart event records may be interleaved with notes. Generated charts are written
in chronological order: a gimmick declaration appears where it begins, and a
`gimmick_note` line is placed directly above the `note=` record it modifies.
Only `note=` records increment the automatic note ID.

### Cover art

Cover images are stored beside each chart and WAV rather than generated by the
firmware. Every current cover is exactly 36×36 pixels in raw RGB888 byte order
(`R`, `G`, `B`), making each file 3,888 bytes. The firmware loads and validates
all covers once at startup, caches them in RAM, and scales the cached pixels to
the focused or side-carousel size without reading flash during animation.
Other dimensions and pixel formats are rejected during import.

The HUB75 DMA framebuffer is deliberately rebuilt when its role changes. Song
selection allocates an eight-bit-per-channel double buffer and submits covers
through the library's RGB888 pixel path. Starting a song blanks and releases
that buffer before allocating a five-bit-per-channel gameplay buffer, restoring
the faster established game profile. Returning to song selection performs the
opposite transition. A short black interval during each rebuild is expected.
If the eight-bit selection allocation fails, firmware falls back once to a
six-bit selection buffer and reports that decision through Serial.
The selection screen presents completed buffers at 30 FPS while the HUB75 DMA
scan continues at its calculated hardware refresh rate. Gameplay retains its
faster profile-specific presentation interval.

Both display profiles currently call `setBrightness8(90)`. This is 90 on the
library's 0–255 scale, approximately 35.3% of its maximum setting rather than
90%. A gimmick's `brightness` parameter scales its RGB colour before that
global limit is applied. Revenge's `0.26` and `0.24` flashes therefore peak at
about 9.2% and 8.5% of the library's unbounded full-channel duty, while its
`0.18` lane pulses peak at about 6.4%, before panel and perceptual
nonlinearities.

When selection has to use its six-bit fallback, cover rendering applies a
stable 4×4 spatial dither to the two RGB888 bits below the physical DMA level.
The normal eight-bit selection profile submits the source values unchanged.
Neither expanded-colour path is applied during gameplay.

FFmpeg can convert an image into the required format. Keep this command on one
line; a space after a shell line-continuation backslash can make FFmpeg treat
`-f` as an output filename.

```sh
ffmpeg -i cover.png -vf "scale=36:36:flags=lanczos,curves=all='0/0 0.10/0.23 0.42/0.66 0.75/0.9 1/1',eq=saturation=1.3,unsharp=3:3:0.3" -f rawvideo -pix_fmt rgb24 data/songs/my-song.rgb888
```

This conversion deliberately lifts midtones, increases saturation, and adds a
small amount of sharpening. That compensates for dark-colour loss at the
matrix's low global brightness while keeping black pixels black. Images that
are already unusually bright or saturated may need milder filter values.

Then add the corresponding chart metadata:

```text
cover=/songs/my-song.rgb888
```

Editable source artwork can be kept under `assets/covers/`, which is not copied
to the ESP32 filesystem. The converted Revenge source is
`assets/covers/revenge.png`; its runtime file is
`data/songs/revenge.rgb888`. The generic importer converts source artwork to
the exact runtime format and rejects missing inputs or invalid metadata.
If a cover is missing, has the wrong size, or cannot be read, the carousel
shows a simple colour-based placeholder and reports the problem through Serial.

### Music files

Imported audio must be an uncompressed PCM WAV with these properties:

- 11,025, 22,050, or 44,100 Hz sample rate
- unsigned 8-bit or signed 16-bit PCM
- mono or stereo

Lower sample rates are repeated internally to the PCM5102A's fixed 44.1 kHz
output rate. Use 44.1 kHz/16-bit for quality when storage allows; use
11.025 kHz/8-bit mono when flash capacity is the priority.

For example, FFmpeg can produce a compatible stereo file:

```sh
ffmpeg -i input-file.mp3 -ar 44100 -ac 2 -c:a pcm_s16le data/songs/my-song.wav
```

Imported WAV playback is reduced to 35% digitally before reaching the
PCM5102A. Set amplifier gain low when first testing a new file. Uncompressed
WAV uses about 176 kB per second in stereo, so the onboard flash is suitable
primarily for short test tracks. The `SongImport` interface isolates storage
and streaming so an SD-card backend can be added later for full-length music
without changing the chart or gameplay structures.

`Revenge` is included as a 76.8-second imported track. Its supplied 44.1 kHz,
16-bit stereo source is 13.5 MB and cannot fit in the current LittleFS
partition. `tools/import_song.py` analyzes any compatible source and produces
an 11.025 kHz/8-bit mono game asset, synchronized contextual chart, strict
36×36 RGB888 cover, and manifest entry. Regenerate Revenge with:

```sh
python3 tools/import_song.py /path/to/revenge.wav --cover assets/covers/revenge.png --title Revenge --artist CaptainSparklez --slug revenge --color 42E66C
```

For another song, change the source, cover, title, artist, and optional
metadata. The slug defaults to a lowercase, hyphenated form of the title.
Titles and artists must each fit within 16 UTF-8 bytes. By default the importer
adds `<slug>.bop` to `data/songs/index.txt`; pass `--skip-index` when generating
files for inspection without registering them.

The generator estimates tempo and beat phase, detects strong offbeats and
sustained passages, maps low/mid/high spectral emphasis to pull/push/twist, and
places wind gusts at major energy changes. It creates all three mappings in one
pass: EASY follows a half-note grid, NORM a quarter-note grid, and HARD an
eighth-note grid. During the first 12 seconds it measures the relative onset
strength of an eight-slot/four-beat phrase and repeats the strongest two, four,
or six positions for EASY, NORM, or HARD. This preserves a recognizable opening
rhythm briefly instead of immediately reverting to a generic grid. It selects
multiple well-separated
screen flashes from strong isolated onsets and lane pulses from separate
sustained high-energy sections; the dominant low/mid/high band selects
pull/push/twist for each pulse. Revenge therefore flashes near 22.48 and 38.98
seconds and pulses the twist lane near 53.92 and 69.89 seconds rather than using
arbitrary intervals. It requires Python 3, NumPy, and FFmpeg.

## Startup self-test

The current testing build enables `STARTUP_SELF_TEST` near the top of
`src/1_SimpleTestShapes.ino`. After boot, it advances the cover-art carousel
up to three times at two-second intervals, without wrapping past Revenge. It
holds the Revenge cover for one second and then overlays the smaller difficulty
popup without clearing or rebuilding the song-selection screen. The original
Revenge title remains visible below it. The popup holds EASY for two seconds and
starts Revenge. After the result screen, the demo returns to the same Revenge
cover and popup for NORM, then repeats once more for HARD. Each popup remains
visible for two seconds before its run begins.

After the HARD Revenge result has remained visible for two seconds, the
repeating demo advances circularly to the next manifest song, holds its cover
for one second, opens its difficulty popup for two seconds, and starts it. This
later loop includes every valid manifest entry. Physical song and difficulty
selection remain locked while the deterministic demonstration is active.

Set the following value to `0` when testing is complete and normal manual song
selection should begin immediately at boot:

```cpp
#define STARTUP_SELF_TEST 0
```

## Build and upload

Install [PlatformIO](https://platformio.org/), connect the ESP32 board, and run:

```sh
pio run
pio run --target upload
pio device monitor
```

The serial monitor runs at 115200 baud.

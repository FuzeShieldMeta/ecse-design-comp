#!/usr/bin/env python3
"""Regenerate the three BOP LAB charts and their loopable PCM WAV assets."""

from __future__ import annotations

import math
import struct
import wave
from pathlib import Path


SAMPLE_RATE = 11_025
NOTE_TRAVEL_MS = 1800
NOTE_HIT_Y = 58
GIMMICK_SAFE_ZONE_Y = 26
SAFE_ZONE_LEAD_MS = (
    NOTE_TRAVEL_MS * (NOTE_HIT_Y - GIMMICK_SAFE_ZONE_Y) // NOTE_HIT_Y
)
HARD_MIN_NOTE_GAP_MS = 220
COLUMN_CLEARANCE_MS = 220
OUTPUT = Path(__file__).resolve().parents[1] / "data" / "songs"

SONGS = (
    {
        "slug": "test-grid", "title": "TEST GRID", "bpm": 96,
        "bars": 8, "color": "00EAFF",
        "melody": (72, -1, 72, 76, 67, -1, 67, 79, 72, -1, 76, 79, 67, 72, 79, -1),
        "bass": (48, -1, -1, -1, 48, -1, -1, -1, 53, -1, -1, -1, 55, -1, -1, -1),
    },
    {
        "slug": "sync-step", "title": "SYNC STEP", "bpm": 120,
        "bars": 10, "color": "FF28BA",
        "melody": (72, 76, 79, 76, 67, 72, 76, 79, 72, 79, 81, 79, 76, 72, 67, -1),
        "bass": (48, -1, 48, -1, 53, -1, 53, -1, 55, -1, 55, -1, 53, -1, 50, -1),
    },
    {
        "slug": "pull-rush", "title": "PULL RUSH", "bpm": 144,
        "bars": 12, "color": "FFB000",
        "melody": (72, 79, 76, 84, 79, 76, 72, 67, 72, 76, 79, 84, 81, 79, 76, 72),
        "bass": (48, -1, 48, 48, 53, -1, 53, 53, 55, -1, 55, 55, 58, 55, 53, 50),
    },
)


def midi_frequency(note: int) -> float:
    return 0.0 if note < 0 else 440.0 * 2.0 ** ((note - 69) / 12.0)


def render_cover(song: dict, style: int, destination: Path) -> None:
    size = 28
    output_size = 36
    accent_value = int(song["color"], 16)
    accent = ((accent_value >> 16) & 255, (accent_value >> 8) & 255,
              accent_value & 255)
    dark = tuple(channel // 12 for channel in accent)
    pixels = [[dark for _ in range(size)] for _ in range(size)]

    if style == 0:
        for y in range(3, size, 5):
            for x in range(size):
                pixels[y][x] = (0, 58, 78)
        for x in range(2, size, 5):
            for y in range(size):
                if (x + y) % 3 == 0:
                    pixels[y][x] = accent
        for y in range(7, 15):
            for x in range(10, 18):
                if (x - 14) ** 2 + (y - 11) ** 2 <= 15:
                    pixels[y][x] = (255, 55, 185)
    elif style == 1:
        for y in range(size):
            for x in range(size):
                if ((x // 4) + (y // 4)) & 1:
                    pixels[y][x] = (72, 5, 78)
                if abs(x - 14) + abs(y - 14) <= 9:
                    pixels[y][x] = accent if x >= 14 else (72, 0, 110)
    else:
        for y in range(size):
            for x in range(size):
                radius = (x - 14) ** 2 + (y - 14) ** 2
                if radius <= 90:
                    pixels[y][x] = accent
                if 10 <= y <= 18 and (x + y) % 4 == 0:
                    pixels[y][x] = (55, 0, 70)
        for offset in range(-1, 2):
            for y in range(4, 24):
                x = 14 + (y - 14) // 2 + offset
                if 0 <= x < size:
                    pixels[y][x] = (255, 45, 75)

    with destination.open("wb") as output:
        for output_y in range(output_size):
            source_y = output_y * size // output_size
            for output_x in range(output_size):
                source_x = output_x * size // output_size
                red, green, blue = pixels[source_y][source_x]
                output.write(bytes((red, green, blue)))


def render_loop(song: dict, destination: Path) -> None:
    step_ms = 60_000 // song["bpm"] // 4
    loop_ms = step_ms * 16
    frame_count = round(loop_ms * SAMPLE_RATE / 1000)
    melody_phase = bass_phase = kick_phase = 0.0
    frames = bytearray()

    for frame in range(frame_count):
        time_ms = frame * 1000.0 / SAMPLE_RATE
        step_number = int(time_ms // step_ms)
        pattern_step = step_number % 16
        step_age = time_ms % step_ms
        beat_ms = step_ms * 4
        beat_age = time_ms % beat_ms
        mix = 0.0

        melody_hz = midi_frequency(song["melody"][pattern_step])
        if melody_hz and step_age < step_ms * 0.78:
            melody_phase = (melody_phase + melody_hz / SAMPLE_RATE) % 1.0
            age = step_age / step_ms
            envelope = (
                age / 0.06 if age < 0.06
                else (0.78 - age) / 0.16 if age > 0.62
                else 1.0
            )
            mix += math.sin(melody_phase * math.tau) * 3900 * max(0.0, envelope)

        bass_hz = midi_frequency(song["bass"][pattern_step])
        if bass_hz and step_age < step_ms * 0.88:
            bass_phase = (bass_phase + bass_hz / SAMPLE_RATE) % 1.0
            mix += math.sin(bass_phase * math.tau) * 2300

        if beat_age < 55.0:
            kick_hz = max(45.0, 130.0 - beat_age * 1.5)
            kick_phase = (kick_phase + kick_hz / SAMPLE_RATE) % 1.0
            mix += math.sin(kick_phase * math.tau) * 2600 * (1.0 - beat_age / 55.0)

        # The firmware applies 35% gain. Scale the stored loop so its resulting
        # level remains close to the former real-time synthesizer.
        sample = max(-32767, min(32767, round(mix * 2.5)))
        fade_frames = SAMPLE_RATE // 200  # Five milliseconds at each seam.
        if frame < fade_frames:
            sample = round(sample * frame / fade_frames)
        elif frame >= frame_count - fade_frames:
            sample = round(sample * (frame_count - frame - 1) / fade_frames)
        frames.append(max(0, min(255, (int(sample) >> 8) + 128)))

    with wave.open(str(destination), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(1)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(frames)


def build_notes(song_index: int, song: dict,
                difficulty: int) -> tuple[list[dict], int, list[tuple[int, int]]]:
    step_ms = 60_000 // song["bpm"] // 4
    total_steps = song["bars"] * 16
    notes: list[dict] = []
    twist_count = pull_count = transition_count = 0

    for step in range(total_steps):
        pattern_step = step % 16
        melody = song["melody"][pattern_step]
        bass = song["bass"][pattern_step]
        if difficulty == 0 and step % 4 != 0:
            continue
        if difficulty == 1 and step % 2 != 0:
            continue
        if difficulty == 2 and melody < 0 and bass < 0:
            continue

        # Bass attacks use pull, upper melody uses twist, and the remaining
        # melodic rhythm uses push. This makes the charts follow each loop.
        if bass >= 0 and (difficulty < 2 or melody < 0 or step % 4 == 0):
            lane = 2
        elif melody >= 79:
            lane = 0
        else:
            lane = 1
        long_hold = False
        if lane == 0:
            twist_count += 1
            long_hold = twist_count % 6 == 4
        elif lane == 2:
            pull_count += 1
            long_hold = pull_count % 6 == 4
        hold_ms = step_ms * 3 if long_hold else 0
        variant = bool((step + song_index) & 1)
        end_variant = variant
        transition_ms = 0
        if lane == 2 and long_hold:
            variant = bool(transition_count & 1)
            end_variant = not variant
            transition_ms = hold_ms // 2
            transition_count += 1
        notes.append({
            "hit": 2000 + step * step_ms, "lane": lane,
            "variant": variant, "bonus": step >= total_steps * 3 // 4,
            "hold": hold_ms, "end_variant": end_variant,
            "transition": transition_ms, "end_col": lane,
            "shift_start": 0, "shift_end": 0, "gimmick_id": 0,
        })
        if (difficulty == 2 and bass >= 0 and melody >= 0 and lane != 2 and
                step % 4 == 0):
            second = 2
            second_variant = bool((step // 4) & 1)
            notes.append({
                "hit": 2000 + step * step_ms, "lane": second,
                "variant": second_variant, "bonus": True, "hold": 0,
                "end_variant": second_variant, "transition": 0,
                "end_col": second, "shift_start": 0, "shift_end": 0,
                "gimmick_id": 0,
            })

    notes.sort(key=lambda note: (note["hit"], note["lane"]))
    control_held_until = [0, 0, 0]
    filtered: list[dict] = []
    for note in notes:
        if note["hit"] < control_held_until[note["lane"]]:
            continue
        filtered.append(note)
        control_held_until[note["lane"]] = note["hit"] + note["hold"]
    notes = filtered

    if difficulty == 2:
        # HARD may follow the sixteenth-note analysis grid, but the physical
        # spring controls and five-row note sprites still require separation.
        # Keep the first musical cue in each too-dense group and give a hold
        # exclusive ownership of the action stream until it is released.
        playable: list[dict] = []
        previous_hit = -HARD_MIN_NOTE_GAP_MS
        hold_blocked_until = 0
        for note in notes:
            if (note["hit"] - previous_hit < HARD_MIN_NOTE_GAP_MS or
                    note["hit"] < hold_blocked_until):
                continue
            playable.append(note)
            previous_hit = note["hit"]
            if note["hold"]:
                hold_blocked_until = (note["hit"] + note["hold"] +
                                      HARD_MIN_NOTE_GAP_MS)
        notes = playable

    duration = 2000 + total_steps * step_ms
    gusts = ((duration // 3, 2800), (duration * 2 // 3, 2800))
    for index, note in enumerate(notes):
        # A moving sustain can sweep through multiple incoming notes. Long
        # rails remain anchored to the column matching their physical control.
        if note["hold"]:
            continue
        upper_start = max(0, note["hit"] - NOTE_TRAVEL_MS)
        safe_entry = max(0, note["hit"] - SAFE_ZONE_LEAD_MS)
        for gimmick_id, (gust_start, gust_duration) in enumerate(gusts, start=1):
            if upper_start < gust_start + gust_duration and safe_entry > gust_start:
                lane = note["lane"]
                shift = 1 if lane == 0 else -1 if lane == 2 else (
                    1 if ((index + song_index) & 1) else -1
                )
                note["end_col"] = max(0, min(2, lane + shift))
                note["shift_start"] = max(upper_start, gust_start)
                note["shift_end"] = min(safe_entry, gust_start + gust_duration)
                note["gimmick_id"] = gimmick_id
                break

    occupied_until = [0, 0, 0]
    for note in notes:
        desired = note["end_col"]
        choices = [desired]
        for distance in (1, 2):
            choices.extend((desired - distance, desired + distance))
        chosen = next(
            (column for column in choices
             if 0 <= column < 3 and note["hit"] >= occupied_until[column]),
            note["lane"],
        )
        if note["shift_end"] > 0:
            note["end_col"] = chosen
            if chosen == note["lane"]:
                note["shift_start"] = 0
                note["shift_end"] = 0
                note["gimmick_id"] = 0
        else:
            chosen = note["lane"]
        occupied_until[chosen] = (note["hit"] + note["hold"] +
                                  COLUMN_CLEARANCE_MS)
    return notes, duration, list(gusts)


def action(lane: int, variant: bool) -> str:
    return ("right" if variant else "left") if lane == 0 else (
        "tap" if lane == 1 else "full" if variant else "half"
    )


def write_chart(song_index: int, song: dict, destination: Path) -> None:
    mappings = [build_notes(song_index, song, difficulty)[0]
                for difficulty in range(3)]
    _, duration, gusts = build_notes(song_index, song, 0)
    lines = [
        "# Generated by tools/generate_demo_assets.py",
        f"title={song['title']}",
        "artist=BOP LAB",
        f"bpm={song['bpm']}",
        f"color=#{song['color']}",
        f"audio=/songs/{song['slug']}.wav",
        f"cover=/songs/{song['slug']}.rgb888",
        "audio_start=2000",
        "audio_loop=1",
        f"duration={duration}",
        "",
    ]
    for mapping, notes in enumerate(mappings):
        lines.extend((f"mapping={mapping}", ""))
        for note_id, note in enumerate(notes, start=1):
            note["id"] = note_id
        gimmicks: list[dict] = []
        for gimmick_id, (start, length) in enumerate(gusts, start=1):
            gimmicks.append({
                "wind_index": gimmick_id, "type": "wind", "start": start,
                "duration": length,
                "parameters": ("target=all,color=35:130:180,brightness=1.0,"
                               "direction=right,speed=1.0,density=4"),
            })
        pulse_targets = ("push", "twist", "pull")
        pulse_colors = ("64:255:96", "255:64:64", "64:96:255")
        gimmicks.extend((
            {
                "type": "screen_flash", "start": duration // 2,
                "duration": 900,
                "parameters": ("target=all,color=255:208:96,brightness=0.24,"
                               "rate=4.0,pattern=checker"),
            },
            {
                "type": "lane_pulse", "start": duration * 5 // 6,
                "duration": 2200,
                "parameters": (f"target={pulse_targets[song_index]},"
                               f"color={pulse_colors[song_index]},brightness=0.18,"
                               "rate=2.0,pattern=solid"),
            },
        ))
        gimmicks.sort(key=lambda gimmick: gimmick["start"])
        for gimmick_id, gimmick in enumerate(gimmicks, start=1):
            gimmick["id"] = gimmick_id
        wind_ids = {
            gimmick["wind_index"]: gimmick["id"] for gimmick in gimmicks
            if "wind_index" in gimmick
        }

        event_blocks: list[tuple[int, int, list[str], bool]] = []
        for gimmick in gimmicks:
            event_blocks.append((
                gimmick["start"], 0,
                [f"gimmick={gimmick['id']},{gimmick['type']},"
                 f"{gimmick['start']},{gimmick['duration']},"
                 f"{gimmick['parameters']}"], True,
            ))
        for note in notes:
            lane_name = ("twist", "push", "pull")[note["lane"]]
            end = (action(note["lane"], note["end_variant"])
                   if note["end_variant"] != note["variant"] else "same")
            fields = [str(note["hit"]), lane_name,
                      action(note["lane"], note["variant"])]
            if (note["hold"] or end != "same" or note["transition"] or
                    note["bonus"]):
                fields.extend((str(note["hold"]), end,
                               str(note["transition"]),
                               str(int(note["bonus"]))))
            block: list[str] = []
            if note["gimmick_id"]:
                gust_start = gusts[note["gimmick_id"] - 1][0]
                block.append(
                    f"gimmick_note={wind_ids[note['gimmick_id']]},"
                    f"{note['id']},column,"
                    f"{note['end_col']},{note['shift_start'] - gust_start},"
                    f"{note['shift_end'] - gust_start}"
                )
            block.append("note=" + ",".join(fields))
            event_blocks.append((note["hit"], 1, block,
                                 bool(note["gimmick_id"])))

        for _, _, block, separated in sorted(event_blocks):
            if separated and lines[-1] != "":
                lines.append("")
            lines.extend(block)
            if separated:
                lines.append("")
    while lines and lines[-1] == "":
        lines.pop()
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for index, song in enumerate(SONGS):
        write_chart(index, song, OUTPUT / f"{song['slug']}.bop")
        render_loop(song, OUTPUT / f"{song['slug']}.wav")
        render_cover(song, index, OUTPUT / f"{song['slug']}.rgb888")


if __name__ == "__main__":
    main()

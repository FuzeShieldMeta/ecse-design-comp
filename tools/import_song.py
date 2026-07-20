#!/usr/bin/env python3
"""Import a PCM WAV and cover image as a synchronized BOP1 song bundle."""

from __future__ import annotations

import argparse
import math
import re
import subprocess
import wave
from pathlib import Path

import numpy as np


ANALYSIS_RATE = 11_025
HOP = 128
WINDOW = 512
NOTE_TRAVEL_MS = 1800
SAFE_ZONE_LEAD_MS = NOTE_TRAVEL_MS * (58 - 26) // 58


def load_analysis_audio(path: Path) -> tuple[np.ndarray, float]:
    with wave.open(str(path), "rb") as source:
        if source.getsampwidth() != 2:
            raise ValueError("source must be 16-bit PCM WAV")
        sample_rate = source.getframerate()
        channels = source.getnchannels()
        raw = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2")
    audio = raw.reshape(-1, channels).mean(axis=1).astype(np.float32) / 32768.0
    if sample_rate % ANALYSIS_RATE != 0:
        raise ValueError("source rate must be an integer multiple of 11025 Hz")
    audio = audio[:: sample_rate // ANALYSIS_RATE]
    return audio, len(audio) / ANALYSIS_RATE


def extract_features(audio: np.ndarray) -> dict[str, np.ndarray]:
    frame_count = 1 + max(0, (len(audio) - WINDOW) // HOP)
    window = np.hanning(WINDOW)
    frequencies = np.fft.rfftfreq(WINDOW, 1.0 / ANALYSIS_RATE)
    low_mask = frequencies < 300
    mid_mask = (frequencies >= 300) & (frequencies < 2200)
    high_mask = frequencies >= 2200
    rms = np.zeros(frame_count)
    flux = np.zeros(frame_count)
    bands = np.zeros((frame_count, 3))
    previous = None
    for index in range(frame_count):
        frame = audio[index * HOP:index * HOP + WINDOW] * window
        magnitude = np.abs(np.fft.rfft(frame))
        log_magnitude = np.log1p(magnitude)
        rms[index] = math.sqrt(float(np.mean(frame * frame)))
        if previous is not None:
            flux[index] = np.maximum(log_magnitude - previous, 0).sum()
        previous = log_magnitude
        power = magnitude * magnitude
        bands[index] = (power[low_mask].sum(), power[mid_mask].sum(),
                        power[high_mask].sum())
    flux = np.convolve(flux, np.ones(3) / 3.0, mode="same")
    return {"rms": rms, "flux": flux, "bands": bands}


def detect_tempo_and_phase(flux: np.ndarray) -> tuple[float, float]:
    onset = np.maximum(flux - np.median(flux), 0)
    best_score = -1.0
    best_bpm = 120.0
    for bpm in np.arange(90.0, 150.01, 0.1):
        lag = round(60.0 * ANALYSIS_RATE / (bpm * HOP))
        score = float(np.dot(onset[lag:], onset[:-lag]))
        if score > best_score:
            best_score, best_bpm = score, float(bpm)

    period_seconds = 60.0 / best_bpm
    phases = np.linspace(0.0, period_seconds, 200, endpoint=False)
    times = np.arange(len(onset)) * HOP / ANALYSIS_RATE
    best_phase = 0.0
    best_score = -1.0
    duration = times[-1]
    for phase in phases:
        grid = np.arange(phase, duration, period_seconds)
        score = float(np.interp(grid, times, onset).sum())
        if score > best_score:
            best_score, best_phase = score, float(phase)
    return best_bpm, best_phase


def feature_at(features: dict[str, np.ndarray], time_seconds: float) -> tuple:
    index = int(round(time_seconds * ANALYSIS_RATE / HOP))
    index = max(0, min(len(features["rms"]) - 1, index))
    return features["rms"][index], features["flux"][index], features["bands"][index]


def contextual_notes(features: dict[str, np.ndarray], duration: float,
                     bpm: float, phase: float) -> list[dict]:
    period = 60.0 / bpm
    flux = features["flux"]
    strong_threshold = float(np.quantile(flux, 0.88))
    rms_median = float(np.median(features["rms"]))
    band_reference = np.maximum(np.median(features["bands"], axis=0), 1e-9)

    first_beat = phase
    while first_beat < 1.8:
        first_beat += period
    candidates = list(np.arange(first_beat, duration - 0.45, period))
    for offbeat in np.arange(first_beat + period / 2, duration - 0.45, period):
        _, onset, _ = feature_at(features, float(offbeat))
        if onset >= strong_threshold:
            candidates.append(float(offbeat))
    candidates.sort()

    notes: list[dict] = []
    lane_counts = [0, 0, 0]
    previous_lane = -1
    repeated_lane = 0
    for grid_time in candidates[:220]:
        frame = int(round(grid_time * ANALYSIS_RATE / HOP))
        radius = max(1, round(0.07 * ANALYSIS_RATE / HOP))
        start = max(0, frame - radius)
        end = min(len(flux), frame + radius + 1)
        local = start + int(np.argmax(flux[start:end]))
        onset_time = local * HOP / ANALYSIS_RATE
        rms, onset, bands = feature_at(features, onset_time)
        scores = bands / band_reference
        lane_order = [2, 1, 0]  # Low=pull, mid=push, high=twist.
        ranked = sorted(lane_order, key=lambda lane: scores[2 - lane], reverse=True)
        lane = ranked[0]
        if lane == previous_lane and repeated_lane >= 2:
            lane = ranked[1]
        repeated_lane = repeated_lane + 1 if lane == previous_lane else 1
        previous_lane = lane

        variant = False
        if lane == 0:
            variant = bool(lane_counts[lane] & 1)
        elif lane == 2:
            variant = rms > rms_median or onset > strong_threshold
        lane_counts[lane] += 1
        notes.append({
            "hit": round(onset_time * 1000), "lane": lane,
            "variant": variant, "end_variant": variant, "hold": 0,
            "transition": 0, "bonus": onset_time >= duration * 0.75,
            "start_col": lane, "end_col": lane,
            "shift_start": 0, "shift_end": 0,
            "onset": onset, "rms": rms,
        })

    # Sustained, energetic phrase points become two-beat twist/pull holds.
    last_hold = -10.0
    pull_transition = False
    for note in notes:
        time_seconds = note["hit"] / 1000.0
        if time_seconds - last_hold < 8.0 or note["lane"] == 1:
            continue
        future_flux = [feature_at(features, time_seconds + period * step)[1]
                       for step in (0.5, 1.0, 1.5)]
        if note["rms"] > rms_median and np.mean(future_flux) < strong_threshold:
            note["hold"] = round(period * 2 * 1000)
            if note["lane"] == 2:
                note["variant"] = pull_transition
                note["end_variant"] = not pull_transition
                note["transition"] = note["hold"] // 2
                pull_transition = not pull_transition
            last_hold = time_seconds

    # Never schedule a second action on a control while its hold is active.
    hold_until = [0, 0, 0]
    filtered = []
    for note in notes:
        if note["hit"] < hold_until[note["lane"]]:
            continue
        filtered.append(note)
        hold_until[note["lane"]] = note["hit"] + note["hold"]
    return filtered


def section_gusts(features: dict[str, np.ndarray], duration: float) -> list[tuple[int, int]]:
    block_seconds = 4.0
    block_frames = max(1, round(block_seconds * ANALYSIS_RATE / HOP))
    rms = features["rms"]
    block_energy = np.array([
        rms[start:start + block_frames].mean()
        for start in range(0, len(rms), block_frames)
    ])
    changes = np.abs(np.diff(block_energy))
    candidates = sorted(range(len(changes)), key=lambda i: changes[i], reverse=True)
    selected: list[float] = []
    for index in candidates:
        time_seconds = (index + 1) * block_seconds
        if 10 < time_seconds < duration - 10 and all(
                abs(time_seconds - other) >= 16 for other in selected):
            selected.append(time_seconds)
        if len(selected) == 2:
            break
    return sorted((round(time * 1000), 2800) for time in selected)


def apply_wind_and_columns(notes: list[dict], gusts: list[tuple[int, int]]) -> None:
    for index, note in enumerate(notes):
        upper_start = max(0, note["hit"] - NOTE_TRAVEL_MS)
        safe_entry = max(0, note["hit"] - SAFE_ZONE_LEAD_MS)
        for gust_start, duration in gusts:
            if upper_start < gust_start + duration and safe_entry > gust_start:
                lane = note["lane"]
                shift = 1 if lane == 0 else -1 if lane == 2 else (
                    1 if index & 1 else -1
                )
                note["end_col"] = max(0, min(2, lane + shift))
                note["shift_start"] = max(upper_start, gust_start)
                note["shift_end"] = min(safe_entry, gust_start + duration)
                break

    occupied_until = [0, 0, 0]
    for note in notes:
        desired = note["end_col"]
        choices = [desired]
        for distance in (1, 2):
            choices.extend((desired - distance, desired + distance))
        chosen = next((column for column in choices if 0 <= column < 3 and
                       note["hit"] > occupied_until[column]), desired)
        if note["shift_end"] == 0:
            note["start_col"] = chosen
        note["end_col"] = chosen
        occupied_until[chosen] = note["hit"] + note["hold"]


def action(lane: int, variant: bool) -> str:
    if lane == 0:
        return "right" if variant else "left"
    if lane == 1:
        return "tap"
    return "full" if variant else "half"


def write_chart(destination: Path, notes: list[dict], gusts: list[tuple[int, int]],
                duration: float, bpm: float, *, title: str, artist: str,
                slug: str, difficulty: int, color: str,
                audio_start: int) -> None:
    lines = [
        "# Beat/onset/spectrum analysis generated by tools/import_song.py",
        "version=BOP1", f"title={title}", f"artist={artist}",
        f"bpm={round(bpm)}", f"difficulty={difficulty}", f"color=#{color}",
        f"audio=/songs/{slug}.wav", f"cover=/songs/{slug}.rgb888",
        f"audio_start={audio_start}", "audio_loop=0",
        f"duration={round(duration * 1000)}", "",
    ]
    lines.extend(f"wind={start},{length}" for start, length in gusts)
    lines.append("")
    for note in notes:
        end = (action(note["lane"], note["end_variant"])
               if note["end_variant"] != note["variant"] else "same")
        lines.append(
            f"note={note['hit']},{('twist','push','pull')[note['lane']]},"
            f"{action(note['lane'], note['variant'])},{note['hold']},{end},"
            f"{note['transition']},{int(note['bonus'])},{note['start_col']},"
            f"{note['end_col']},{note['shift_start']},{note['shift_end']}"
        )
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")


def normalized_slug(value: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")
    if not slug:
        raise ValueError("slug must contain at least one letter or digit")
    if len(slug) > 32:
        raise ValueError("slug must be at most 32 characters")
    return slug


def validate_metadata(title: str, artist: str, color: str) -> str:
    for label, value in (("title", title), ("artist", artist)):
        if not value or len(value.encode("utf-8")) > 16:
            raise ValueError(f"{label} must occupy 1 to 16 UTF-8 bytes")
        if "\n" in value or "\r" in value:
            raise ValueError(f"{label} cannot contain a newline")
    normalized_color = color.removeprefix("#").upper()
    if not re.fullmatch(r"[0-9A-F]{6}", normalized_color):
        raise ValueError("color must be a six-digit RGB hex value")
    return normalized_color


def convert_cover(source: Path, destination: Path) -> None:
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(source), "-vf",
        "scale=36:36:force_original_aspect_ratio=increase:flags=lanczos,"
        "crop=36:36,curves=all='0/0 0.10/0.23 0.42/0.66 0.75/0.9 1/1',"
        "eq=saturation=1.3,unsharp=3:3:0.3",
        "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24",
        str(destination),
    ], check=True)
    expected_size = 36 * 36 * 3
    if destination.stat().st_size != expected_size:
        raise RuntimeError(
            f"cover conversion wrote {destination.stat().st_size} bytes; "
            f"expected {expected_size}"
        )


def convert_audio(source: Path, destination: Path) -> None:
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(source), "-ac", "1", "-ar", "11025",
        "-c:a", "pcm_u8", str(destination),
    ], check=True)


def register_chart(index_path: Path, chart_name: str) -> None:
    existing = index_path.read_text(encoding="utf-8") if index_path.exists() else ""
    registered = {
        line.strip().lstrip("/").removeprefix("songs/")
        for line in existing.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    if chart_name in registered:
        return
    separator = "" if not existing or existing.endswith("\n") else "\n"
    index_path.write_text(existing + separator + chart_name + "\n",
                          encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Analyze a 16-bit PCM WAV and create an importable BOP song"
    )
    parser.add_argument("source", type=Path, help="16-bit PCM source WAV")
    parser.add_argument("--cover", type=Path, required=True,
                        help="source PNG/JPEG cover image")
    parser.add_argument("--title", required=True,
                        help="display title, at most 16 UTF-8 bytes")
    parser.add_argument("--artist", required=True,
                        help="display artist, at most 16 UTF-8 bytes")
    parser.add_argument("--slug",
                        help="filesystem name; defaults to normalized title")
    parser.add_argument("--difficulty", type=int, choices=(0, 1, 2), default=1)
    parser.add_argument("--color", default="00EAFF",
                        help="six-digit RGB carousel accent")
    parser.add_argument("--audio-start", type=int, default=0,
                        help="non-negative chart time before audio starts")
    parser.add_argument("--skip-index", action="store_true",
                        help="do not add the generated chart to index.txt")
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[1] / "data" / "songs")
    args = parser.parse_args()
    if not args.source.is_file():
        parser.error(f"source WAV does not exist: {args.source}")
    if not args.cover.is_file():
        parser.error(f"cover image does not exist: {args.cover}")
    if args.audio_start < 0:
        parser.error("--audio-start must be non-negative")
    try:
        color = validate_metadata(args.title, args.artist, args.color)
        slug = normalized_slug(args.slug or args.title)
    except ValueError as error:
        parser.error(str(error))
    args.output.mkdir(parents=True, exist_ok=True)

    audio, duration = load_analysis_audio(args.source)
    features = extract_features(audio)
    bpm, phase = detect_tempo_and_phase(features["flux"])
    notes = contextual_notes(features, duration, bpm, phase)
    gusts = section_gusts(features, duration)
    apply_wind_and_columns(notes, gusts)
    cover_path = args.output / f"{slug}.rgb888"
    audio_path = args.output / f"{slug}.wav"
    chart_path = args.output / f"{slug}.bop"
    convert_cover(args.cover, cover_path)
    convert_audio(args.source, audio_path)
    write_chart(chart_path, notes, gusts, duration, bpm,
                title=args.title, artist=args.artist, slug=slug,
                difficulty=args.difficulty, color=color,
                audio_start=args.audio_start)
    if not args.skip_index:
        register_chart(args.output / "index.txt", chart_path.name)
    print(
        f"Imported {args.title} as {slug}: detected {bpm:.1f} BPM; "
        f"wrote {len(notes)} notes and {len(gusts)} gusts"
    )


if __name__ == "__main__":
    main()

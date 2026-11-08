#!/usr/bin/env python3
"""Composes and renders the PS5 home screen's music: an original piece written for this port.

Dark ambient on a rainy night: slow synth pads swelling through Dm9, Bbmaj7, Gm9 and A7sus and back,
a low D drone under them and a breath of wind, in a long reverb,
with soft rain, scattered drips and two distant rolls of thunder. About 40 seconds, fading in and out. Every
note is chosen below and every sound synthesised here, so it is the project's own and can be shipped
(the game's music never is).

    python3 tools/ps5/make_home_music.py out.wav

Writes 48 kHz 16-bit stereo, what the home screen's sce_sys/snd0.at9 is encoded from (an ATRAC9 encoder
is needed for that step; see docs/building-ps5.md). Needs NumPy.
"""
import sys
import wave

import numpy as np

RATE = 48000
CHORD_SECONDS = 8.0
FADE_IN, FADE_OUT = 3.0, 5.0
rng = np.random.default_rng(1979)

# The chords, a pad voicing each (MIDI), CHORD_SECONDS apiece, each swelling into the next.
CHORDS = [
    ("Dm9", [50, 57, 60, 64, 65]),       # D3 A3 C4 E4 F4
    ("Bbmaj7", [46, 53, 57, 62, 65]),    # Bb2 F3 A3 D4 F4
    ("Gm9", [43, 50, 57, 58, 62]),       # G2 D3 A3 Bb3 D4
    ("A7sus", [45, 52, 55, 62, 64]),     # A2 E3 G3 D4 E4
    ("Dm9", [50, 57, 60, 64, 69]),       # back home, the top opened up
]
# No bells: the rain and thunder carry the top of it.
BELLS: list[tuple[float, int]] = []
DRONE = 38  # D2, and an octave below it


def hz(note: float) -> float:
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


def pad_voice(note: int, seconds: float, detune_cents: float) -> np.ndarray:
    """A warm saw: its harmonics rolled off above about 900 Hz (a soft low-pass), slightly detuned."""
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    f = hz(note + detune_cents / 100.0)
    out = np.zeros(n)
    k = 1
    while f * k < 2500:
        rolloff = 1.0 / (1.0 + (f * k / 900.0) ** 3)
        out += np.sin(2 * np.pi * f * k * t + rng.uniform(0, 2 * np.pi)) / k * rolloff
        k += 1
    return out


def pad_chord(notes: list[int], seconds: float) -> np.ndarray:
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    chord = np.zeros((n, 2))
    for i, note in enumerate(notes):
        for d, pan in ((-7.0, -0.6), (0.0, 0.0), (7.0, 0.6)):
            voice = pad_voice(note, seconds, d + rng.uniform(-1.5, 1.5))
            chord[:, 0] += voice * (1.0 - pan) ** 0.5
            chord[:, 1] += voice * (1.0 + pan) ** 0.5
    # A slow swell in and out, and a gentle breathing.
    attack, release = seconds * 0.35, seconds * 0.45
    env = np.minimum(1.0, t / attack) * np.minimum(1.0, (seconds - t) / release)
    env = np.clip(env, 0.0, 1.0) ** 1.5
    env *= 0.85 + 0.15 * np.sin(2 * np.pi * 0.11 * t + rng.uniform(0, 6))
    return chord * env[:, None] / len(notes) * 0.28


def drone(seconds: float) -> np.ndarray:
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    f = hz(DRONE)
    left = np.sin(2 * np.pi * f * t) + 0.6 * np.sin(2 * np.pi * f / 2 * t) + 0.15 * np.sin(2 * np.pi * f * 2.002 * t)
    right = np.sin(2 * np.pi * f * 1.0015 * t) + 0.6 * np.sin(2 * np.pi * f / 2 * 1.001 * t) + 0.15 * np.sin(2 * np.pi * f * 2 * t)
    swell = 0.8 + 0.2 * np.sin(2 * np.pi * 0.05 * t)
    return np.stack([left, right], axis=1) * swell[:, None] * 0.055


def bell(note: int, seconds: float = 6.0) -> np.ndarray:
    """A glassy FM bell: its brightness falling away faster than its loudness."""
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    f = hz(note)
    index = 2.2 * np.exp(-t * 1.6)
    tone = np.sin(2 * np.pi * f * t + index * np.sin(2 * np.pi * f * 3.5 * t))
    return tone * np.minimum(1.0, t / 0.004) * np.exp(-t * 0.9) * 0.07


def wind(seconds: float) -> np.ndarray:
    n = int(seconds * RATE)
    out = []
    for c in range(2):
        noise = rng.standard_normal(n)
        spectrum = np.fft.rfft(noise)
        freqs = np.fft.rfftfreq(n, 1.0 / RATE)
        spectrum *= 1.0 / (1.0 + (freqs / 500.0) ** 2) * (freqs > 80)  # dark, no rumble
        band = np.fft.irfft(spectrum, n)
        band /= np.abs(band).max()
        t = np.arange(n) / RATE
        gust = 0.5 + 0.5 * np.sin(2 * np.pi * (0.07 + 0.02 * c) * t + c * 2.1)
        out.append(band * gust ** 2)
    return np.stack(out, axis=1) * 0.035


def shaped_noise(n: int, low: float, high: float) -> np.ndarray:
    """Noise kept between about `low` and `high` Hz, scaled to a peak of 1."""
    spectrum = np.fft.rfft(rng.standard_normal(n))
    freqs = np.fft.rfftfreq(n, 1.0 / RATE)
    spectrum *= 1.0 / (1.0 + (low / np.maximum(freqs, 1.0)) ** 2) / (1.0 + (freqs / high) ** 2)
    band = np.fft.irfft(spectrum, n)
    return band / np.abs(band).max()


def rain(seconds: float) -> np.ndarray:
    """A steady soft rain: airy hiss, its loudness drifting slowly and differently in each ear."""
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    out = []
    for c in range(2):
        hiss = shaped_noise(n, 900.0, 6000.0)
        drift = 0.8 + 0.2 * np.sin(2 * np.pi * (0.03 + 0.017 * c) * t + c * 1.3)
        out.append(hiss * drift)
    return np.stack(out, axis=1) * 0.24


def drops(seconds: float, count: int = 420) -> np.ndarray:
    """Single drips and patter: short bright ticks scattered in time and across the stereo field."""
    n = int(seconds * RATE)
    out = np.zeros((n, 2))
    length = int(0.03 * RATE)
    t = np.arange(length) / RATE
    for _ in range(count):
        start = int(rng.uniform(0, seconds - 0.05) * RATE)
        pitch = rng.uniform(1800, 5200)
        tick = np.sin(2 * np.pi * pitch * t * (1.0 - 0.6 * t / t[-1])) * np.exp(-t * rng.uniform(140, 320))
        tick *= rng.uniform(0.2, 1.0) ** 2 * 0.17
        pan = rng.uniform(-0.9, 0.9)
        out[start:start + length, 0] += tick * (1.0 - pan) ** 0.5
        out[start:start + length, 1] += tick * (1.0 + pan) ** 0.5
    return out


def thunder(seconds: float, when: float, strength: float = 1.0) -> np.ndarray:
    """A distant roll: low rumble, a slow rise, a long uneven fall."""
    n = int(seconds * RATE)
    length = int(9.0 * RATE)
    t = np.arange(length) / RATE
    # Low enough to rumble, wide enough to be heard on small speakers, with a crack as it starts.
    rumble = shaped_noise(length, 40.0, 450.0) + 0.5 * shaped_noise(length, 30.0, 120.0)
    crack = shaped_noise(length, 200.0, 2500.0) * np.exp(-np.maximum(t - 1.0, 0.0) * 4.0) * np.minimum(1.0, t / 0.9) ** 3
    flutter = 0.6 + 0.4 * np.abs(np.sin(2 * np.pi * 0.9 * t + rng.uniform(0, 3))) * np.exp(-t * 0.3)
    env = np.minimum(1.0, t / 1.2) ** 2 * np.exp(-np.maximum(t - 1.2, 0.0) * 0.55) * flutter
    roll = (rumble * env + 0.35 * crack * (t < 3.0)) * 1.7 * strength
    out = np.zeros((n, 2))
    start = int(when * RATE)
    end = min(n, start + length)
    out[start:end, 0] = roll[: end - start]
    out[start:end, 1] = np.roll(roll, int(0.012 * RATE))[: end - start] * 0.9
    return out


def reverb(dry: np.ndarray, seconds: float = 4.5, mix: float = 0.45) -> np.ndarray:
    tail_n = int(seconds * RATE)
    out = np.empty_like(dry)
    for c in range(2):
        tail = rng.standard_normal(tail_n) * np.exp(-np.arange(tail_n) / RATE * (6.9 / seconds))
        tail[: int(0.03 * RATE)] = 0.0
        size = 1 << int(np.ceil(np.log2(len(dry) + tail_n)))
        wet = np.fft.irfft(np.fft.rfft(dry[:, c], size) * np.fft.rfft(tail, size), size)[: len(dry)]
        wet *= np.abs(dry[:, c]).max() / max(np.abs(wet).max(), 1e-9)
        out[:, c] = dry[:, c] * (1.0 - mix) + wet * mix
    return out


def render() -> np.ndarray:
    total_seconds = CHORD_SECONDS * len(CHORDS) + 2.0
    n = int(total_seconds * RATE)
    mix = np.zeros((n, 2))
    # Each chord lasts a little longer than its slot, so it swells into the next.
    for i, (_, notes) in enumerate(CHORDS):
        start = int(i * CHORD_SECONDS * RATE)
        chord = pad_chord(notes, CHORD_SECONDS * 1.5)
        end = min(n, start + len(chord))
        mix[start:end] += chord[: end - start]
    mix += drone(total_seconds)
    mix += wind(total_seconds) * 0.6
    mix += thunder(total_seconds, 9.0, 1.0) + thunder(total_seconds, 27.5, 0.7)
    for when, note in BELLS:
        sound = bell(note)
        start = int(when * RATE)
        end = min(n, start + len(sound))
        pan = rng.uniform(-0.7, 0.7)
        mix[start:end, 0] += sound[: end - start] * (1.0 - pan) ** 0.5
        mix[start:end, 1] += sound[: end - start] * (1.0 + pan) ** 0.5
    mix = reverb(mix)
    # The rain outside the room: added after the reverb, so it stays close and crisp.
    mix += rain(total_seconds) + drops(total_seconds)
    t = np.arange(n) / RATE
    fade = np.minimum(1.0, t / FADE_IN) * np.clip((total_seconds - t) / FADE_OUT, 0.0, 1.0)
    mix *= fade[:, None]
    mix *= 0.8 / np.abs(mix).max()
    return mix


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_home_music.py out.wav")
    mix = render()
    with wave.open(sys.argv[1], "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes((mix * 32767).astype("<i2").tobytes())
    print(f"wrote {sys.argv[1]} ({len(mix) / RATE:.1f} s)")


if __name__ == "__main__":
    main()

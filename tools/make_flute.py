#!/usr/bin/env python3
# Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
"""Synthesizes examples/sounds/flute.wav: a short flute-like phrase (few
soft harmonics, delayed vibrato, breath noise, a small room). It is smooth
material for the granulator and the phase vocoder.

    python3 tools/make_flute.py examples/sounds/flute.wav
"""
import sys, wave
import numpy as np
from scipy.signal import lfilter, butter

sr = 44100
rng = np.random.default_rng(7)
# (start s, duration s, MIDI note)
notes = [(0.00, 0.90, 74), (0.90, 0.45, 77), (1.35, 0.45, 76), (1.80, 1.15, 72),
         (2.95, 0.55, 69), (3.50, 1.70, 74)]
dur = 5.8
n = int(sr * dur)
t = np.arange(n) / sr
out = np.zeros(n)

for start, length, midi in notes:
    f0 = 440.0 * 2 ** ((midi - 69) / 12)
    i0, i1 = int(start * sr), int(min(dur, start + length + 0.25) * sr)
    tt = np.arange(i1 - i0) / sr
    # delayed vibrato and a slow, tiny pitch wander
    vib = 0.0035 * np.sin(2 * np.pi * 5.1 * tt) * np.clip((tt - 0.18) / 0.35, 0, 1)
    wander = 0.0015 * np.sin(2 * np.pi * 0.7 * tt + rng.uniform(0, 6))
    phase = 2 * np.pi * np.cumsum(f0 * (1 + vib + wander)) / sr
    tone = np.zeros_like(tt)
    for h, amp in enumerate([1.0, 0.30, 0.11, 0.05, 0.02], start=1):
        flutter = 1 + 0.06 * np.sin(2 * np.pi * rng.uniform(0.3, 1.2) * tt + rng.uniform(0, 6))
        tone += amp * flutter * np.sin(h * phase + rng.uniform(0, 6))
    # breath: band-passed noise around the fundamental and a chiff at the onset
    b, a = butter(2, [f0 * 0.8 / (sr / 2), min(0.99, f0 * 3.0 / (sr / 2))], btype="band")
    breath = lfilter(b, a, rng.standard_normal(len(tt)))
    breath /= np.max(np.abs(breath)) + 1e-9
    chiff = np.exp(-tt / 0.04)
    # envelope: soft attack, slight swell, release
    rel = length
    env = np.clip(tt / 0.07, 0, 1) * (0.85 + 0.15 * np.sin(np.pi * np.clip(tt / rel, 0, 1)))
    env *= np.clip((rel + 0.18 - tt) / 0.18, 0, 1)
    out[i0:i1] += env * (tone + breath * (0.05 + 0.25 * chiff))

# a small room: decaying, low-passed noise impulse response
L = int(1.3 * sr)
ir = rng.standard_normal(L) * np.exp(-np.arange(L) / (0.35 * sr))
b, a = butter(2, 5000 / (sr / 2))
ir = lfilter(b, a, ir)
ir /= np.sqrt(np.sum(ir ** 2))
wet = np.convolve(out, ir)[:n]
out = 0.8 * out + 0.35 * wet
out *= 0.6 / np.max(np.abs(out))
fade = np.clip((dur - t) / 0.3, 0, 1)
out *= fade
with wave.open(sys.argv[1] if len(sys.argv) > 1 else "examples/sounds/flute.wav", "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes((np.clip(out, -1, 1) * 32767).astype("<i2").tobytes())
print("peak %.2f rms %.3f" % (np.max(np.abs(out)), np.sqrt(np.mean(out ** 2))))

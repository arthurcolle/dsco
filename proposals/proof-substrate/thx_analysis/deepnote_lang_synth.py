#!/usr/bin/env python3
"""NOVA / Deepnote-derived agent language synthesizer.
Original synthesis inspired by measured chaos->order acoustic principles; does NOT
sample or reuse THX audio. Generates a compact spoken-agent utterance as WAV.

Core idea:
- Vowels = stable spectral chords anchored near measured converged partial families.
- Consonants = short chirp/noise gestures that indicate speech act.
- Prosody = entropy contour: disorder->order for verify/commit, order->disorder for warn.
"""
import numpy as np
from scipy.io.wavfile import write
from scipy.signal import chirp

SR = 48000

# Stable spectral vowels. Names are language phonemes, not English vowels.
# Frequencies derived as families from measured converged partials but recomposed.
VOWELS = {
    "A": ([82, 164, 252, 504],        [0.55, 0.7, 1.0, 0.35]),   # grounded / evidence
    "E": ([129, 258, 334, 674],       [0.7, 0.35, 1.0, 0.30]),   # proposal / hypothesis
    "I": ([170, 334, 674, 1008],      [0.45, 0.65, 0.8, 0.45]),  # verification / proof
    "O": ([41, 82, 170, 252, 504],    [0.7, 0.55, 0.45, 1.0, .25]), # commit / convergence
    "U": ([82, 129, 210, 421, 843],   [0.65, 0.65, 0.45, .5, .25]), # rollback / uncertainty
}

# Consonants: short gestures. Each is a protocol marker.
CONSONANTS = {
    "h": ("noise", 0.055),       # observe / attention breath
    "r": ("riser", 0.070),       # request / open channel
    "k": ("click", 0.030),       # commit boundary
    "v": ("fall", 0.080),        # verify / collapse uncertainty
    "x": ("burst", 0.050),       # warning / interrupt
    "m": ("hum", 0.060),         # memory / context
    "p": ("pulse", 0.040),       # proof token
    "s": ("sweep", 0.065),       # search / propose
}

LEXICON = {
    # word -> phoneme sequence (CV/CVC). Compact, agent-useful.
    "observe": "hA",
    "propose": "sE",
    "verify": "vI",
    "commit": "kO",
    "rollback": "rU",
    "help": "rA",
    "warn": "xU",
    "prove": "pI",
    "memory": "mA",
    "cost": "kU",
    "success": "vOkO",
}

def env(n, attack=0.01, release=0.03):
    a = max(1, int(attack*SR)); r = max(1, int(release*SR))
    e = np.ones(n)
    e[:min(a,n)] *= np.linspace(0,1,min(a,n))
    e[-min(r,n):] *= np.linspace(1,0,min(r,n))
    return e

def vowel(name, dur=0.18, entropy=0.15):
    freqs, amps = VOWELS[name]
    n = int(dur*SR); t = np.arange(n)/SR
    y = np.zeros(n)
    # entropy adds controlled detuning that relaxes during the vowel
    for f,a in zip(freqs, amps):
        det = (np.random.default_rng(int(f*1000)).normal(0, entropy*7, n))
        relax = np.linspace(1.0, 0.05, n)
        phase = 2*np.pi*np.cumsum((f + det*relax)/SR)
        y += a*np.sin(phase)
    y *= env(n, 0.018, 0.055)
    return y / (np.max(np.abs(y))+1e-9) * 0.28

def consonant(name):
    kind, dur = CONSONANTS[name]
    n = int(dur*SR); t = np.arange(n)/SR
    rng = np.random.default_rng(ord(name))
    if kind == "noise": y = rng.normal(0,1,n) * np.exp(-t*25)
    elif kind == "riser": y = chirp(t, 120, dur, 1200, method='logarithmic')
    elif kind == "fall": y = chirp(t, 1800, dur, 160, method='logarithmic')
    elif kind == "click": y = np.zeros(n); y[:80] = np.hanning(80)*rng.normal(0,1,80)
    elif kind == "burst": y = rng.normal(0,1,n) * np.hanning(n)
    elif kind == "hum": y = np.sin(2*np.pi*82*t) + .4*np.sin(2*np.pi*164*t)
    elif kind == "pulse": y = np.sin(2*np.pi*252*t) * (np.sin(2*np.pi*18*t)>0).astype(float)
    elif kind == "sweep": y = chirp(t, 900, dur, 250, method='linear') * np.hanning(n)
    else: y = np.zeros(n)
    y *= env(n, 0.003, 0.015)
    return y/(np.max(np.abs(y))+1e-9)*0.22

def synth_phonemes(seq, entropy=0.2):
    parts=[]
    for ch in seq:
        if ch in VOWELS: parts.append(vowel(ch, entropy=entropy))
        elif ch in CONSONANTS: parts.append(consonant(ch))
        else: parts.append(np.zeros(int(0.03*SR)))
        parts.append(np.zeros(int(0.018*SR)))
    return np.concatenate(parts) if parts else np.zeros(1)

def synth_message(words):
    parts=[]
    # message prosody: entropy decreases from first word to last = coordination converges
    for i,w in enumerate(words):
        ent = np.interp(i, [0, max(1,len(words)-1)], [0.45, 0.05])
        parts.append(synth_phonemes(LEXICON[w], entropy=ent))
        parts.append(np.zeros(int(0.08*SR)))
    y=np.concatenate(parts)
    y *= 0.95/(np.max(np.abs(y))+1e-9)
    return (y*32767).astype(np.int16)

if __name__ == "__main__":
    # Example: "observe propose verify prove commit success"
    msg = ["observe", "propose", "verify", "prove", "commit", "success"]
    y = synth_message(msg)
    write("deepnote_agent_sentence.wav", SR, y)
    print("message:", " ".join(msg))
    print("phonemes:", " ".join(LEXICON[w] for w in msg))
    print("wrote deepnote_agent_sentence.wav")

#!/usr/bin/env python3
"""Mine the THX Deep Note for acoustic primitives that will ground a language.
We extract: (1) the converged chord's partials (the 'vowel space'),
(2) the entropy/chaos->order curve (the 'prosody engine'),
(3) spectral-centroid & bandwidth trajectories (the 'consonant/onset space')."""
import numpy as np
from scipy.io import wavfile
from scipy.signal import spectrogram, get_window

sr, x = wavfile.read('thx.wav')
if x.ndim == 2: x = x.astype(np.float64).mean(axis=1)
else: x = x.astype(np.float64)
x /= (np.max(np.abs(x)) + 1e-12)

nperseg, noverlap = 8192, 7168
f, t, S = spectrogram(x, fs=sr, window=get_window('hann', nperseg),
                      nperseg=nperseg, noverlap=noverlap, mode='magnitude')
P = S**2

# 0) Find ACTIVE region (drop silence padding)
frame_energy = P.sum(axis=0)
thr = frame_energy.max() * 0.02
active = frame_energy > thr
t_lo, t_hi = t[active][0], t[active][-1]
print(f"[active region] {t_lo:.2f}s -> {t_hi:.2f}s of {t[-1]:.2f}s")

# 1) SPECTRAL ENTROPY over active arc == the chaos->order curve
def spec_entropy(col):
    p = col / (col.sum() + 1e-12)
    p = p[p > 0]
    return float(-(p*np.log2(p)).sum())
ent = np.array([spec_entropy(P[:,j]) if active[j] else np.nan
                for j in range(P.shape[1])])
va = ent[~np.isnan(ent)]
ent_n = ent.copy()
ent_n[~np.isnan(ent)] = (va - va.min())/(va.max()-va.min()+1e-12)

# 2) CONVERGED CHORD: last 2s of ACTIVE region (peak convergence)
conv_mask = active & (t >= (t_hi - 2.0))
tail = P[:, conv_mask].mean(axis=1)
# peak-pick
peaks = []
for i in range(2, len(tail)-2):
    if tail[i] > tail[i-1] and tail[i] >= tail[i+1] and tail[i] > tail.max()*0.02:
        peaks.append((f[i], tail[i]))
peaks.sort(key=lambda p: -p[1])
top = sorted(peaks[:16], key=lambda p: p[0])

# map freq -> nearest note
A4=440.0
def note(fr):
    if fr<=0: return "-"
    n=round(12*np.log2(fr/A4))
    names=["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]
    return names[(n+9)%12]+str(4+(n+9)//12)

print("=== CONVERGED CHORD (vowel anchors) ===")
for fr,mag in top:
    print(f"  {fr:8.1f} Hz  {note(fr):>4}  rel={mag/tail.max():.3f}")

# 3) spectral centroid trajectory (brightness -> onset articulation)
cent = (f[:,None]*P).sum(0)/(P.sum(0)+1e-12)
print("\n=== ARC SUMMARY ===")
print(f"duration_s        : {len(x)/sr:.2f}")
print(f"entropy_start     : {np.nanmean(ent_n[active][:5]):.3f} (chaos)")
print(f"entropy_end       : {np.nanmean(ent_n[active][-5:]):.3f} (order)")
print(f"entropy_drop      : {np.nanmean(ent_n[active][:5])-np.nanmean(ent_n[active][-5:]):.3f}")
print(f"centroid_start_hz : {cent[:5].mean():.1f}")
print(f"centroid_end_hz   : {cent[-5:].mean():.1f}")

# save arc for the prosody engine
np.savez('thx_arc.npz', t=t, entropy=ent_n, centroid=cent,
         chord_freqs=np.array([p[0] for p in top]),
         chord_mags=np.array([p[1] for p in top]))
print("\nwrote thx_arc.npz")

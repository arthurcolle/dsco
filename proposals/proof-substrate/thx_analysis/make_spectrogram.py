#!/usr/bin/env python3
import numpy as np
import matplotlib.pyplot as plt
from scipy.io import wavfile
from scipy.signal import spectrogram, get_window
from pathlib import Path

wav = Path('thx.wav')
sr, x = wavfile.read(wav)
if x.ndim == 2:
    x = x.astype(np.float64).mean(axis=1)
else:
    x = x.astype(np.float64)
# Normalize for display only
if np.max(np.abs(x)) > 0:
    x = x / np.max(np.abs(x))

# High-resolution STFT: good for THX-style clustered glissandi
nperseg = 4096
noverlap = 3840
f, t, S = spectrogram(
    x, fs=sr, window=get_window('hann', nperseg),
    nperseg=nperseg, noverlap=noverlap,
    scaling='spectrum', mode='magnitude'
)
S_db = 20 * np.log10(S + 1e-10)
# Clip dynamic range to emphasize visible partials
vmax = np.percentile(S_db, 99.9)
vmin = vmax - 90

# Linear frequency overview
fig, ax = plt.subplots(figsize=(16, 9), dpi=220)
im = ax.pcolormesh(t, f, S_db, shading='auto', cmap='magma', vmin=vmin, vmax=vmax)
ax.set_title('THX Intro Sound — Spectrogram (linear frequency)', fontsize=16, weight='bold')
ax.set_xlabel('Time (s)')
ax.set_ylabel('Frequency (Hz)')
ax.set_ylim(0, 12000)
ax.grid(alpha=0.12, color='white')
cbar = fig.colorbar(im, ax=ax, pad=0.01)
cbar.set_label('Magnitude (dB, clipped 90 dB range)')
fig.tight_layout()
fig.savefig('thx_spectrogram_linear.png')
plt.close(fig)

# Log-frequency view — more musically useful
mask = f >= 20
fig, ax = plt.subplots(figsize=(16, 9), dpi=220)
im = ax.pcolormesh(t, f[mask], S_db[mask], shading='auto', cmap='turbo', vmin=vmin, vmax=vmax)
ax.set_title('THX Intro Sound — Spectrogram (log frequency)', fontsize=16, weight='bold')
ax.set_xlabel('Time (s)')
ax.set_ylabel('Frequency (Hz, log scale)')
ax.set_yscale('log')
ax.set_ylim(20, 12000)
# Musical-ish reference ticks
_ticks = [20, 30, 50, 80, 100, 150, 220, 330, 440, 660, 1000, 1500, 2200, 3300, 5000, 8000, 12000]
ax.set_yticks(_ticks)
ax.get_yaxis().set_major_formatter(plt.ScalarFormatter())
ax.grid(alpha=0.16, color='white', which='both')
cbar = fig.colorbar(im, ax=ax, pad=0.01)
cbar.set_label('Magnitude (dB, clipped 90 dB range)')
fig.tight_layout()
fig.savefig('thx_spectrogram_log.png')
plt.close(fig)

# Extract coarse peak trajectories for analysis text
# For each time slice, find strongest frequency bin above 30Hz and below 10kHz.
lo, hi = np.searchsorted(f, 30), np.searchsorted(f, 10000)
peaks = []
for j in range(S_db.shape[1]):
    col = S_db[lo:hi, j]
    if col.size == 0: continue
    idx = int(np.argmax(col)) + lo
    peaks.append((float(t[j]), float(f[idx]), float(S_db[idx, j])))
# Sample peak every ~1s for a compact CSV
sampled = []
for sec in np.arange(0, t[-1], 1.0):
    k = min(range(len(peaks)), key=lambda i: abs(peaks[i][0] - sec))
    sampled.append(peaks[k])
with open('thx_peak_trace.csv', 'w') as fp:
    fp.write('time_s,dominant_freq_hz,mag_db\n')
    for a,b,c in sampled:
        fp.write(f'{a:.3f},{b:.2f},{c:.2f}\n')

print('wrote thx_spectrogram_linear.png')
print('wrote thx_spectrogram_log.png')
print('wrote thx_peak_trace.csv')
print(f'sr={sr} samples={len(x)} duration={len(x)/sr:.3f}s')

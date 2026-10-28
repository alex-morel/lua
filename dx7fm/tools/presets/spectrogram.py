# [ADICIONADO em 2026-10-28] Espectrogramas (eixo log) + envelope RMS (ciano)
# dos .wav gravados por patch_tests. Uso:
#   python3 tools/presets/spectrogram.py output/patch_tests saida.png piano sino ...
import sys, numpy as np, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy.io import wavfile
from scipy.signal import spectrogram
names = sys.argv[3:]
fig, axes = plt.subplots(len(names), 1, figsize=(11, 2.6 * len(names)))
axes = np.atleast_1d(axes)
for ax, n in zip(axes, names):
    sr, x = wavfile.read(f"{sys.argv[1]}/{n}.wav")
    x = x[:, 0].astype(np.float32) / 32768.0
    f, t, S = spectrogram(x, sr, nperseg=2048, noverlap=1792)
    ax.pcolormesh(t, f, 10 * np.log10(S + 1e-12), vmin=-110, vmax=-30, shading="auto", cmap="inferno")
    ax.set_yscale("symlog", linthresh=200); ax.set_ylim(40, 16000)
    env = np.sqrt(np.convolve(x * x, np.ones(480) / 480, mode="same"))
    ax2 = ax.twinx(); ax2.plot(np.arange(len(x)) / sr, 20 * np.log10(env + 1e-6), color="cyan", lw=0.8); ax2.set_ylim(-70, 0)
    ax.set_title(n, fontsize=9, loc="left"); ax.tick_params(labelsize=7); ax2.tick_params(labelsize=7)
plt.tight_layout(); plt.savefig(sys.argv[2], dpi=70)

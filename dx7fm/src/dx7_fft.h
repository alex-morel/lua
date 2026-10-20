#pragma once
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

// FFT complexa in-place, iterativa (Cooley-Tukey radix-2). n potencia de 2.
// [MOVIDO em 2026-10-20 de app/spectrum.h] usada pelo espectrograma da UI e
// pela ressintese (dx7_resynth.h).
namespace dx7fm {

inline void fftInPlace(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { // permutacao bit-reversa
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            // twiddle acumulado em double: o erro de w *= wl cresce com len,
            // e a ressintese usa n ate 65536
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<float> wf(static_cast<float>(w.real()), static_cast<float>(w.imag()));
                std::complex<float> u = a[i + k], v = a[i + k + len / 2] * wf;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace dx7fm

#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// [ADICIONADO em 2026-10-18] "Grampo" na saida de audio pros visualizadores
// (osciloscopio e espectrograma). A audio thread grava cada bloco que acabou
// de gerar (push); a UI le as ultimas N amostras quando quiser (read).
//
// Buffer circular de 32768 quadros estereo, um escritor (audio thread) e um
// leitor (UI thread), sem trava: o escritor publica quantos quadros ja
// escreveu num contador atomico (release) e o leitor so le quadros
// anteriores a esse contador (acquire). O leitor pede no maximo alguns
// milhares de quadros, entao o escritor precisaria dar a volta no buffer
// inteiro (~0.7s de audio) DURANTE uma leitura (microssegundos) pra
// sobrescrever o que esta sendo lido.
namespace dx7fm_app {

class AudioTap {
public:
    static constexpr size_t kSize = 1u << 15; // quadros; potencia de 2

    AudioTap() : l_(kSize, 0.0f), r_(kSize, 0.0f) {}

    // audio thread: quadros intercalados L,R
    void push(const float* interleaved, int frames) {
        uint64_t w = written_.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i) {
            size_t idx = static_cast<size_t>(w + static_cast<uint64_t>(i)) & (kSize - 1);
            l_[idx] = interleaved[2 * i];
            r_[idx] = interleaved[2 * i + 1];
        }
        written_.store(w + static_cast<uint64_t>(frames), std::memory_order_release);
    }

    // total de quadros ja escritos desde o inicio
    uint64_t written() const { return written_.load(std::memory_order_acquire); }

    // Copia os n quadros que TERMINAM em `end` (exclusivo), como mono
    // (L+R)/2. Quadros anteriores ao inicio do audio saem como 0.
    void readMono(uint64_t end, int n, float* out) const {
        for (int i = 0; i < n; ++i) {
            int64_t f = static_cast<int64_t>(end) - n + i;
            if (f < 0) { out[i] = 0.0f; continue; }
            size_t idx = static_cast<size_t>(f) & (kSize - 1);
            out[i] = 0.5f * (l_[idx] + r_[idx]);
        }
    }

private:
    std::vector<float> l_, r_;
    std::atomic<uint64_t> written_{0};
};

} // namespace dx7fm_app

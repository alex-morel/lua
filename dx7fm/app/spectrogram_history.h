#pragma once
#include "audio_tap.h"
#include "spectrum.h"
#include <algorithm>
#include <cstdint>
#include <vector>

// [ADICIONADO em 2026-10-18] Historico do espectrograma (sem ImGui, testado
// no demo.cpp). Uma coluna nova a cada kHop amostras de AUDIO (nao por quadro
// de tela: a rolagem nao depende do FPS da UI). Cada coluna e a FFT de kFft
// amostras (Hann) reduzida a kBands faixas em escala log de kFMin a kFMax.
namespace dx7fm_app {

struct SpectrogramHistory {
    static constexpr int kCols = 160, kBands = 56, kHop = 1024, kFft = 4096;
    static constexpr float kFMin = 20.0f, kFMax = 20000.0f;
    static constexpr float kSilenceDb = -140.0f;
    std::vector<float> hist = std::vector<float>(static_cast<size_t>(kCols * kBands), kSilenceDb); // [coluna][faixa]
    int head = 0;          // proxima coluna a escrever (= a mais antiga)
    uint64_t lastEnd = 0;  // fim (em quadros do tap) da ultima coluna calculada
    SpectrumAnalyzer analyzer{kFft};
    std::vector<float> block, bins, bands;

    const float* column(int c) const { return &hist[static_cast<size_t>(((head + c) % kCols) * kBands)]; } // c=0: mais antiga

    void pushSilentColumn() {
        std::fill(hist.begin() + static_cast<long>(head) * kBands, hist.begin() + static_cast<long>(head + 1) * kBands, kSilenceDb);
        head = (head + 1) % kCols;
    }

    // Consome o audio novo do tap, uma coluna por kHop quadros.
    void update(const AudioTap& tap, double sampleRate) {
        uint64_t w = tap.written();
        if (w < lastEnd) lastEnd = 0; // tap reiniciado
        // UI parada por mais tempo que o historico inteiro: so o que cabe na tela
        if (w - lastEnd > static_cast<uint64_t>(kHop) * kCols) lastEnd = w - static_cast<uint64_t>(kHop) * kCols;
        // O tap so guarda kSize quadros. Uma coluna cujo bloco ja foi
        // sobrescrito NAO pode ser lida (o indice daria a volta e devolveria
        // audio recente no lugar do antigo): entra como silencio, mantendo o
        // eixo do tempo certo.
        const uint64_t oldestReadableEnd = w > AudioTap::kSize ? w - AudioTap::kSize + kFft : 0;
        block.resize(kFft);
        while (lastEnd + kHop <= w) {
            lastEnd += kHop;
            if (lastEnd < oldestReadableEnd) { pushSilentColumn(); continue; }
            tap.readMono(lastEnd, kFft, block.data());
            analyzer.magnitudeDb(block.data(), bins);
            logBands(bins, sampleRate, kFft, kBands, kFMin, kFMax, bands);
            std::copy(bands.begin(), bands.end(), hist.begin() + static_cast<long>(head) * kBands);
            head = (head + 1) % kCols;
        }
    }
};

} // namespace dx7fm_app

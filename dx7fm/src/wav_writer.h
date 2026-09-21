#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dx7fm {

// Escritor de WAV PCM16 estereo minimo. So pros demos/testes, nao e um
// writer de producao (sem dithering, sem outros formatos).
inline void writeWavStereo(const std::string& path, const std::vector<float>& left,
                            const std::vector<float>& right, int sampleRate) {
    size_t n = std::min(left.size(), right.size());
    std::vector<int16_t> interleaved(n * 2);
    for (size_t i = 0; i < n; ++i) {
        float l = std::clamp(left[i], -1.0f, 1.0f);
        float r = std::clamp(right[i], -1.0f, 1.0f);
        interleaved[2 * i]     = static_cast<int16_t>(l * 32767.0f);
        interleaved[2 * i + 1] = static_cast<int16_t>(r * 32767.0f);
    }

    uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(int16_t));
    uint32_t riffBytes = 36 + dataBytes;
    uint16_t numChannels = 2;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = static_cast<uint32_t>(sampleRate) * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;

    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riffBytes, 4, 1, f);
    std::fwrite("WAVE", 1, 4, f);

    std::fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;
    std::fwrite(&fmtSize, 4, 1, f);
    std::fwrite(&audioFormat, 2, 1, f);
    std::fwrite(&numChannels, 2, 1, f);
    uint32_t sr = static_cast<uint32_t>(sampleRate);
    std::fwrite(&sr, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&blockAlign, 2, 1, f);
    std::fwrite(&bitsPerSample, 2, 1, f);

    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);
    std::fwrite(interleaved.data(), sizeof(int16_t), interleaved.size(), f);

    std::fclose(f);
}

// Atalho: duplica o buffer mono nos dois canais (o WAV gerado sempre tem 2 canais).
inline void writeWavMono(const std::string& path, const std::vector<float>& mono, int sampleRate) {
    writeWavStereo(path, mono, mono, sampleRate);
}

} // namespace dx7fm

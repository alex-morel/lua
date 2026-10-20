#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-20] Leitor de WAV pra ressintese (dx7_resynth.h).
// Aceita PCM inteiro de 8 (sem sinal), 16, 24 e 32 bits, float de 32 e 64
// bits, e WAVE_FORMAT_EXTENSIBLE com esses subformatos; qualquer numero de
// canais (mixados em mono pela media). Le os chunks em qualquer ordem e
// ignora os desconhecidos (LIST, fact, cue...). x86-64 = little-endian, igual
// ao WAV, entao os campos sao copiados direto.
namespace dx7fm {

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    bool isFloat = false;
    std::vector<float> mono; // -1..1
    bool truncated = false;  // passou de maxSeconds e foi cortado
    std::string error;       // vazio = ok
    double seconds() const { return sampleRate > 0 ? static_cast<double>(mono.size()) / sampleRate : 0.0; }
};

inline WavData readWavMono(const std::string& path, double maxSeconds = 300.0) {
    WavData w;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { w.error = "nao foi possivel abrir o arquivo"; return w; }
    std::vector<uint8_t> bytes;
    {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0) {
            bytes.resize(static_cast<size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
        std::fclose(f);
    }
    auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, &bytes[at], 2); return v; };
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, &bytes[at], 4); return v; };
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(&bytes[8], "WAVE", 4) != 0) {
        w.error = "nao e um arquivo WAV (RIFF/WAVE)";
        return w;
    }

    int format = 0, blockAlign = 0;
    size_t dataAt = 0, dataBytes = 0;
    bool haveFmt = false;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        uint32_t len = u32(at + 4);
        size_t body = at + 8, avail = bytes.size() - body;
        if (std::memcmp(&bytes[at], "fmt ", 4) == 0 && len >= 16 && avail >= 16) {
            format = u16(body);
            w.channels = u16(body + 2);
            w.sampleRate = static_cast<int>(u32(body + 4));
            blockAlign = u16(body + 12);
            w.bitsPerSample = u16(body + 14);
            if (format == 0xFFFE && len >= 26 && avail >= 26) format = u16(body + 24); // subformato (GUID comeca com o codigo)
            haveFmt = true;
        } else if (std::memcmp(&bytes[at], "data", 4) == 0) {
            dataAt = body;
            dataBytes = std::min<size_t>(len, avail); // arquivo cortado: usa o que tem
        }
        at = body + len + (len & 1); // chunks tem tamanho par (byte de preenchimento)
        if (len > avail) break;
    }
    if (!haveFmt) { w.error = "WAV sem chunk fmt"; return w; }
    if (dataAt == 0) { w.error = "WAV sem chunk data"; return w; }
    const int bits = w.bitsPerSample, ch = w.channels;
    w.isFloat = format == 3;
    const bool okInt = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool okFloat = format == 3 && (bits == 32 || bits == 64);
    if (!okInt && !okFloat) {
        w.error = "formato nao suportado (codigo " + std::to_string(format) + ", " + std::to_string(bits) + " bits)";
        return w;
    }
    if (ch < 1 || w.sampleRate < 1000 || blockAlign != ch * (bits / 8)) { w.error = "cabecalho fmt invalido"; return w; }

    size_t frames = dataBytes / static_cast<size_t>(blockAlign);
    const size_t maxFrames = static_cast<size_t>(maxSeconds * w.sampleRate);
    if (frames > maxFrames) { frames = maxFrames; w.truncated = true; }
    w.mono.resize(frames);
    const int bytesPer = bits / 8;
    const float invCh = 1.0f / static_cast<float>(ch);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* p = &bytes[dataAt + i * static_cast<size_t>(blockAlign)];
        float sum = 0.0f;
        for (int c = 0; c < ch; ++c, p += bytesPer) {
            float v;
            if (okFloat && bits == 32) { float x; std::memcpy(&x, p, 4); v = x; }
            else if (okFloat) { double x; std::memcpy(&x, p, 8); v = static_cast<float>(x); }
            else if (bits == 8) v = (static_cast<float>(p[0]) - 128.0f) / 128.0f;
            else if (bits == 16) { int16_t x; std::memcpy(&x, p, 2); v = x / 32768.0f; }
            else if (bits == 24) {
                int32_t x = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                                                 static_cast<uint32_t>(p[2]) << 24) >> 8; // estende o sinal
                v = static_cast<float>(x) / 8388608.0f;
            } else { int32_t x; std::memcpy(&x, p, 4); v = static_cast<float>(x / 2147483648.0); }
            sum += v;
        }
        w.mono[i] = sum * invCh;
    }
    if (frames == 0) w.error = "WAV sem amostras";
    return w;
}

} // namespace dx7fm

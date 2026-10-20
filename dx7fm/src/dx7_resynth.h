#pragma once
#include "dx7_fft.h"
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <tuple>
#include <vector>

// [ADICIONADO em 2026-10-20] Ressintese: transforma um som gravado (WAV,
// ver wav_reader.h) num operador Additive.
//
//  1. Envelope (RMS em quadros de 10ms) -> inicio do som (onset), pico, fim.
//  2. Dois instantes do som: A (ataque) e B (sustain). Em cada um, um
//     quadro de FFT com janela Blackman-Harris (lobulos laterais a -92 dB,
//     entao picos falsos de vazamento ficam abaixo do limiar) e zero-padding
//     4x; cada maximo local acima do limiar vira um parcial, com frequencia e
//     amplitude refinadas por interpolacao parabolica (em dB).
//  3. Frequencia de referencia (o "1x" das razoes): a fundamental (ver
//     detectReference). Som inarmonico (sino) sem nenhuma fundamental que
//     explique bem o espectro: usa o parcial forte mais grave.
//  4. Pareamento A <-> B: o morph interpola o parcial k de A com o parcial k
//     de B (amplitude E razao). Se o slot k tivesse frequencias diferentes em
//     A e B, o morph faria o parcial DESLIZAR de uma pra outra. Entao os picos
//     de A e B sao pareados por frequencia (< 3%); pico sem par fica com a
//     mesma razao nos dois e amplitude 0 do lado que falta (entra/sai em fade).
//  5. Envelopes do operador: fxEG leva o morph de 0 (A) a 1 (B) entre os dois
//     instantes; opcionalmente o Amp EG imita o envelope de volume do som
//     (EG do DX7 = segmentos lineares em dB, 0.75 dB por unidade de level).
namespace dx7fm::resynth {

struct Peak {
    float hz = 0.0f;
    float amp = 0.0f; // amplitude de pico de uma senoide (1 = escala cheia)
};

struct Options {
    int windowSize = 8192;       // amostras do quadro de FFT (potencia de 2)
    float thresholdDb = -60.0f;  // picos abaixo disso (relativo ao mais forte) sao ignorados
    int maxPartials = kNumPartials;
    float minHz = 20.0f;
};

// --- 1. envelope ---------------------------------------------------------------
struct Envelope {
    double hopSec = 0.005;
    std::vector<float> db; // por quadro, relativo ao quadro mais forte (0 dB)
    int onset = 0, peak = 0, end = 0; // indices de quadro
    double timeOf(int frame) const { return frame * hopSec; }
    // dB no instante t (s), interpolado
    float dbAt(double t) const {
        if (db.empty()) return -120.0f;
        double fi = t / hopSec;
        int i = std::clamp(static_cast<int>(fi), 0, static_cast<int>(db.size()) - 1);
        int j = std::min(i + 1, static_cast<int>(db.size()) - 1);
        float u = static_cast<float>(std::clamp(fi - i, 0.0, 1.0));
        return db[static_cast<size_t>(i)] * (1.0f - u) + db[static_cast<size_t>(j)] * u;
    }
};

inline Envelope analyzeEnvelope(const float* x, size_t n, double sr) {
    Envelope e;
    const size_t hop = std::max<size_t>(1, static_cast<size_t>(e.hopSec * sr)), win = 2 * hop;
    e.hopSec = static_cast<double>(hop) / sr;
    float maxDb = -200.0f;
    for (size_t start = 0; start < n; start += hop) {
        double sum = 0.0;
        size_t end = std::min(n, start + win);
        for (size_t i = start; i < end; ++i) sum += static_cast<double>(x[i]) * x[i];
        float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(win)));
        float db = 20.0f * std::log10(std::max(rms, 1e-9f));
        e.db.push_back(db);
        maxDb = std::max(maxDb, db);
    }
    for (auto& d : e.db) d -= maxDb;
    const int nf = static_cast<int>(e.db.size());
    for (int i = 0; i < nf; ++i) if (e.db[static_cast<size_t>(i)] > -50.0f) { e.onset = i; break; }
    for (int i = 0; i < nf; ++i) if (e.db[static_cast<size_t>(i)] > e.db[static_cast<size_t>(e.peak)]) e.peak = i;
    e.end = e.peak;
    for (int i = nf - 1; i > e.peak; --i) if (e.db[static_cast<size_t>(i)] > -60.0f) { e.end = i; break; }
    return e;
}

// --- 2. picos de um quadro -------------------------------------------------------
// Quadro centrado na amostra `center` (fora do audio conta como silencio).
inline std::vector<Peak> findPeaks(const float* x, size_t n, double sr, double centerSec, const Options& o) {
    const int N = o.windowSize, pad = 4, M = N * pad;
    std::vector<std::complex<float>> buf(static_cast<size_t>(M));
    const long start = static_cast<long>(centerSec * sr) - N / 2;
    const double tau = 2.0 * 3.14159265358979323846 / N;
    for (int i = 0; i < N; ++i) {
        long at = start + i;
        float s = at >= 0 && at < static_cast<long>(n) ? x[at] : 0.0f;
        double w = 0.35875 - 0.48829 * std::cos(tau * i) + 0.14128 * std::cos(2 * tau * i) - 0.01168 * std::cos(3 * tau * i);
        buf[static_cast<size_t>(i)] = static_cast<float>(s * w);
    }
    fftInPlace(buf);
    const int half = M / 2;
    std::vector<float> db(static_cast<size_t>(half + 1));
    const float norm = 2.0f / (static_cast<float>(N) * 0.35875f); // seno de amplitude A -> A
    float maxDb = -200.0f;
    for (int k = 0; k <= half; ++k) {
        db[static_cast<size_t>(k)] = 20.0f * std::log10(std::max(std::abs(buf[static_cast<size_t>(k)]) * norm, 1e-10f));
        maxDb = std::max(maxDb, db[static_cast<size_t>(k)]);
    }
    const float binHz = static_cast<float>(sr / M);
    const float floorDb = std::max(maxDb + o.thresholdDb, -100.0f);
    const int nb = 2 * pad; // maximo local em +-2 bins da janela sem padding
    const int kMin = std::max(nb, static_cast<int>(o.minHz / binHz));
    std::vector<Peak> peaks;
    for (int k = kMin; k < half - nb; ++k) {
        const float v = db[static_cast<size_t>(k)];
        if (v < floorDb) continue;
        bool isMax = true;
        for (int d = 1; d <= nb && isMax; ++d)
            isMax = v > db[static_cast<size_t>(k - d)] && v >= db[static_cast<size_t>(k + d)];
        if (!isMax) continue;
        float a = db[static_cast<size_t>(k - 1)], c = db[static_cast<size_t>(k + 1)];
        float den = a - 2.0f * v + c;
        float p = den < 0.0f ? 0.5f * (a - c) / den : 0.0f;
        peaks.push_back({(static_cast<float>(k) + p) * binHz, std::pow(10.0f, (v - 0.25f * (a - c) * p) / 20.0f)});
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& l, const Peak& r) { return l.amp > r.amp; });
    if (static_cast<int>(peaks.size()) > o.maxPartials) peaks.resize(static_cast<size_t>(o.maxPartials));
    return peaks;
}

// --- 3. frequencia de referencia ---------------------------------------------------
// Pra cada candidato f = pico/k (k = 1..8, entre os 10 picos mais fortes),
// soma o peso dos picos que caem num harmonico de f, com o peso dividido por
// sqrt(numero do harmonico): f/2 tambem "explica" tudo que f explica, mas com
// harmonicos de numero maior, entao perde; e um f baixo que acerta picos
// inarmonicos por acaso (h alto) quase nao ganha nada com isso. Tolerancia:
// 2% do harmonico + 2% de f (instrumentos reais tem harmonicos levemente
// esticados). Se o melhor candidato nao explica nem 60% do espectro (sino),
// a referencia e o parcial forte mais grave.
inline float detectReference(const std::vector<Peak>& peaks) {
    if (peaks.empty()) return 0.0f;
    auto weight = [](const Peak& p) { return std::sqrt(p.amp); }; // parciais fracos tambem votam
    auto harmonic = [](float hz, float f) {
        float h = hz / f, hr = std::round(h);
        return hr >= 1.0f && hr <= static_cast<float>(kMaxPartialRatio) && std::fabs(h - hr) < 0.02f * hr + 0.02f ? hr : 0.0f;
    };
    float total = 0.0f;
    for (const auto& p : peaks) total += weight(p);
    float bestF = 0.0f, bestScore = -1.0f;
    const size_t top = std::min<size_t>(peaks.size(), 10);
    for (size_t i = 0; i < top; ++i)
        for (int k = 1; k <= 8; ++k) {
            float f = peaks[i].hz / static_cast<float>(k);
            if (f < 25.0f || f > 5000.0f) continue;
            float sc = 0.0f;
            for (const auto& p : peaks)
                if (float hr = harmonic(p.hz, f)) sc += weight(p) / std::sqrt(hr);
            if (sc > bestScore) { bestScore = sc; bestF = f; }
        }
    float explained = 0.0f, num = 0.0f, den = 0.0f;
    for (const auto& p : peaks)
        if (float hr = harmonic(p.hz, bestF)) { explained += weight(p); num += weight(p) * p.hz / hr; den += weight(p); }
    if (explained < 0.6f * total) { // inarmonico: o parcial forte mais grave
        float lowest = 0.0f;
        for (const auto& p : peaks)
            if (p.amp >= 0.1f * peaks[0].amp && (lowest == 0.0f || p.hz < lowest)) lowest = p.hz;
        return lowest;
    }
    return num / den; // refinada: media ponderada de pico/harmonico
}

// --- 4 + 5. resultado ---------------------------------------------------------------
struct Result {
    bool ok = false;
    float refHz = 0.0f;           // frequencia que vira razao 1
    float detectedHz = 0.0f;      // o que detectReference achou (antes de um override)
    double onsetSec = 0.0, aSec = 0.0, bSec = 0.0;
    std::vector<Peak> peaksA, peaksB;
    OperatorParams::PartialSet a, b;
    int partialsA = 0, partialsB = 0, paired = 0; // paired: slots com parcial em A E em B
    EGParams fxEG, ampEG;
    Envelope env;
};

// Nivel de EG (0..99) que da `db` (<= 0) dB: egLevelToAmplitude = 0.75 dB por unidade.
inline uint8_t egLevelForDb(float db) {
    return static_cast<uint8_t>(std::lround(std::clamp(99.0f + db / 0.75f, 0.0f, 99.0f)));
}

// Rate (0..99) que leva o EG de `from` a `to` em `seconds` (inverso de egRateToSpeed).
inline uint8_t egRateFor(float from, float to, double seconds) {
    float delta = std::fabs(to - from);
    if (delta <= 0.0f) return 99;
    float speed = delta / static_cast<float>(std::max(seconds, 1e-4));
    return static_cast<uint8_t>(std::lround(std::clamp(std::log(speed / 5.0f) / std::log(1.0897f), 0.0f, 99.0f)));
}

// Valores padrao dos instantes A e B pra um som: A no comeco (quadro comecando
// no onset), B 0.5 s depois (ou o mais tarde que o som permitir).
inline void defaultTimes(const Envelope& env, double durationSec, const Options& o, double sr, double& aSec, double& bSec) {
    const double halfWin = 0.5 * o.windowSize / sr;
    const double onset = env.timeOf(env.onset), end = std::min(durationSec, env.timeOf(env.end));
    aSec = std::min(onset + halfWin, std::max(onset, durationSec - halfWin));
    bSec = std::clamp(onset + 0.5, aSec, std::max(aSec, end - halfWin));
}

inline Result analyze(const float* x, size_t n, double sr, double aSec, double bSec, const Options& o, float refOverrideHz = 0.0f) {
    Result r;
    r.env = analyzeEnvelope(x, n, sr);
    r.onsetSec = r.env.timeOf(r.env.onset);
    r.aSec = aSec;
    r.bSec = std::max(aSec, bSec);
    r.peaksA = findPeaks(x, n, sr, r.aSec, o);
    r.peaksB = findPeaks(x, n, sr, r.bSec, o);
    r.detectedHz = detectReference(r.peaksB.empty() ? r.peaksA : r.peaksB);
    r.refHz = refOverrideHz > 0.0f ? refOverrideHz : r.detectedHz;
    if (r.refHz <= 0.0f) return r;

    // pareamento por frequencia (guloso, dos pares mais proximos primeiro)
    struct Slot { float ampA, ratioA, ampB, ratioB; };
    std::vector<Slot> slots;
    float maxA = 0.0f, maxB = 0.0f;
    for (const auto& p : r.peaksA) maxA = std::max(maxA, p.amp);
    for (const auto& p : r.peaksB) maxB = std::max(maxB, p.amp);
    std::vector<std::tuple<float, int, int>> pairs; // (distancia log, ia, ib)
    for (int i = 0; i < static_cast<int>(r.peaksA.size()); ++i)
        for (int j = 0; j < static_cast<int>(r.peaksB.size()); ++j) {
            float d = std::fabs(std::log(r.peaksA[static_cast<size_t>(i)].hz / r.peaksB[static_cast<size_t>(j)].hz));
            if (d < 0.03f) pairs.push_back({d, i, j});
        }
    std::sort(pairs.begin(), pairs.end());
    std::vector<char> usedA(r.peaksA.size(), 0), usedB(r.peaksB.size(), 0);
    for (const auto& [d, i, j] : pairs) {
        if (usedA[static_cast<size_t>(i)] || usedB[static_cast<size_t>(j)]) continue;
        usedA[static_cast<size_t>(i)] = usedB[static_cast<size_t>(j)] = 1;
        const auto &pa = r.peaksA[static_cast<size_t>(i)], &pb = r.peaksB[static_cast<size_t>(j)];
        slots.push_back({pa.amp / maxA, pa.hz / r.refHz, pb.amp / maxB, pb.hz / r.refHz});
    }
    for (size_t i = 0; i < r.peaksA.size(); ++i)
        if (!usedA[i]) slots.push_back({r.peaksA[i].amp / maxA, r.peaksA[i].hz / r.refHz, 0.0f, r.peaksA[i].hz / r.refHz});
    for (size_t j = 0; j < r.peaksB.size(); ++j)
        if (!usedB[j]) slots.push_back({0.0f, r.peaksB[j].hz / r.refHz, r.peaksB[j].amp / maxB, r.peaksB[j].hz / r.refHz});
    // fora da faixa de razao do parametro: descarta
    slots.erase(std::remove_if(slots.begin(), slots.end(), [](const Slot& s) {
                    return std::max(s.ratioA, s.ratioB) > kMaxPartialRatio || std::min(s.ratioA, s.ratioB) < 0.01f;
                }), slots.end());
    // mais de 64 (ou de maxPartials): ficam os mais fortes; depois ordem de frequencia
    const size_t cap = static_cast<size_t>(std::clamp(o.maxPartials, 1, kNumPartials));
    std::sort(slots.begin(), slots.end(), [](const Slot& l, const Slot& s) { return std::max(l.ampA, l.ampB) > std::max(s.ampA, s.ampB); });
    if (slots.size() > cap) slots.resize(cap);
    std::sort(slots.begin(), slots.end(), [](const Slot& l, const Slot& s) { return l.ratioA < s.ratioA; });
    for (int k = 0; k < kNumPartials; ++k) {
        r.a.amp[k] = r.b.amp[k] = 0.0f;
        r.a.ratio[k] = r.b.ratio[k] = static_cast<float>(k + 1);
    }
    for (size_t k = 0; k < slots.size(); ++k) {
        r.a.amp[k] = std::clamp(slots[k].ampA, 0.0f, 1.0f);
        r.b.amp[k] = std::clamp(slots[k].ampB, 0.0f, 1.0f);
        r.a.ratio[k] = slots[k].ratioA;
        r.b.ratio[k] = slots[k].ratioB;
        r.partialsA += r.a.amp[k] > 0.0f;
        r.partialsB += r.b.amp[k] > 0.0f;
        r.paired += r.a.amp[k] > 0.0f && r.b.amp[k] > 0.0f;
    }

    // fxEG (morph): ~0 ate o instante A, sobe linear ate 99 no instante B e fica.
    // (EG nao tem "espera": o 1o estagio vai so ate o level 1, devagar, = morph 0.01)
    const double tA = std::max(0.0, r.aSec - r.onsetSec), tB = std::max(tA, r.bSec - r.onsetSec);
    r.fxEG.level[0] = 1;
    r.fxEG.rate[0] = egRateFor(0.0f, 1.0f, tA);
    r.fxEG.level[1] = 99;
    r.fxEG.rate[1] = egRateFor(1.0f, 99.0f, tB - tA);
    r.fxEG.level[2] = 99;
    r.fxEG.rate[2] = 99;
    r.fxEG.level[3] = 99; // release: continua em B
    r.fxEG.rate[3] = 99;

    // Amp EG: ataque ate o pico, decai ate o nivel do instante B, depois segue a
    // inclinacao do resto do som (ate 0 se ele morre dentro do arquivo).
    const auto& env = r.env;
    const double tPeak = std::max(0.0, env.timeOf(env.peak) - r.onsetSec);
    const double tBabs = std::max(r.bSec, env.timeOf(env.peak));
    const uint8_t lB = egLevelForDb(env.dbAt(tBabs));
    r.ampEG.level[0] = 99;
    r.ampEG.rate[0] = egRateFor(0.0f, 99.0f, tPeak);
    r.ampEG.level[1] = lB;
    r.ampEG.rate[1] = egRateFor(99.0f, lB, tBabs - env.timeOf(env.peak));
    const double fileEnd = static_cast<double>(n) / sr, tEnd = env.timeOf(env.end);
    if (tEnd > tBabs + 0.05) {
        float lEnd = egLevelForDb(env.dbAt(tEnd));
        // decai (morre dentro do arquivo, ou cai > 6 dB/s ate o fim dele: sino,
        // piano, corda dedilhada) ou sustenta (orgao, sopro, pad)?
        const double slopeDb = (env.dbAt(tBabs) - env.dbAt(tEnd)) / (tEnd - tBabs);
        const bool decays = tEnd < fileEnd - 0.1 || slopeDb > 6.0;
        if (decays) { // extrapola a inclinacao ate o level 0
            double slope = (lB - lEnd) / (tEnd - tBabs); // levels por segundo
            r.ampEG.level[2] = 0;
            r.ampEG.rate[2] = slope > 0.0 ? egRateFor(lB, 0.0f, lB / slope) : 0;
        } else {
            r.ampEG.level[2] = static_cast<uint8_t>(lEnd);
            r.ampEG.rate[2] = egRateFor(lB, lEnd, tEnd - tBabs);
        }
    } else {
        r.ampEG.level[2] = lB;
        r.ampEG.rate[2] = 99;
    }
    r.ampEG.level[3] = 0;
    r.ampEG.rate[3] = r.ampEG.level[2] == 0 ? r.ampEG.rate[2] : egRateFor(r.ampEG.level[2], 0.0f, 0.3);
    r.ok = r.partialsA + r.partialsB > 0;
    return r;
}

struct ApplyOptions {
    bool morph = true;       // false: so o espectro A (B = A, sem morph)
    bool ampEG = true;       // copia o envelope de volume do som pro Amp EG
    bool resetTuning = true; // operador em razao 1x, sem fine/detune (razoes ja sao relativas a refHz)
};

inline void apply(OperatorParams& op, const Result& r, const ApplyOptions& ao) {
    op.waveform = Waveform::Additive;
    op.additiveA = r.a;
    op.additiveB = ao.morph ? r.b : r.a;
    op.morphDepth = ao.morph ? 1.0f : 0.0f;
    if (ao.morph) op.fxEG = r.fxEG;
    if (ao.ampEG) op.ampEG = r.ampEG;
    if (ao.resetTuning) {
        op.fixedFreq = false;
        op.coarseRatio = 1.0f;
        op.fineCents = 0.0f;
        op.detuneCents = 0.0f;
    }
}

} // namespace dx7fm::resynth

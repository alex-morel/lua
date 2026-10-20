#include "dx7_params.h"
#include "dx7_dsp.h"
#include "dx7_engine.h"
#include "dx7_alloc.h"
#include "dx7_patch_io.h"
#include "dx7_algorithms.h"
#include "wav_writer.h"
#include "wav_reader.h"
#include "dx7_resynth.h"
#include "../app/event_queue.h"
#include "../app/patch_browser.h"
#include "../app/envelope_math.h"
#include "../app/audio_tap.h"
#include "../app/spectrum.h"
#include "../app/spectrogram_history.h"
#include "../app/additive_presets.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

using namespace dx7fm;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name) {
    if (cond) { std::printf("[PASS] %s\n", name); ++g_pass; }
    else      { std::printf("[FAIL] %s\n", name); ++g_fail; }
}

static float computeRms(const std::vector<float>& buf, size_t start, size_t count) {
    size_t end = std::min(buf.size(), start + count);
    if (end <= start) return 0.0f;
    double sum = 0.0;
    for (size_t i = start; i < end; ++i) sum += static_cast<double>(buf[i]) * buf[i];
    return static_cast<float>(std::sqrt(sum / static_cast<double>(end - start)));
}

// Goertzel: magnitude de uma unica raia de frequencia, sem precisar de FFT
// inteira. Usado pra medir energia aliasada numa frequencia especifica.
static float goertzelMagnitude(const std::vector<float>& x, double targetFreq, double sampleRate) {
    const double kPiD = 3.14159265358979323846;
    int n = static_cast<int>(x.size());
    double k = std::round((n * targetFreq) / sampleRate);
    double w = (2.0 * kPiD / n) * k;
    double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double real = s1 - s2 * std::cos(w);
    double imag = s2 * std::sin(w);
    return static_cast<float>(std::sqrt(real * real + imag * imag) / (n / 2.0));
}

// Patch minimo de FM de 2 operadores (usa o default de VoiceParams: op0
// portadora, op1 modulador) com os operadores nao usados zerados para ficarem
// idle e nao desperdicar CPU.
static VoiceParams makeSimpleFmPatch() {
    VoiceParams v;
    for (int i = 2; i < kNumOps; ++i) {
        v.op[i].ampEG.level[0] = 0;
        v.op[i].ampEG.level[1] = 0;
        v.op[i].ampEG.level[2] = 0;
        v.op[i].ampEG.level[3] = 0;
    }
    return v;
}

// --- testes de sanidade ------------------------------------------------

static void testEnvelopeShape() {
    SegmentEG eg;
    uint8_t rate[4]  = {80, 80, 80, 80};
    uint8_t level[4] = {99, 90, 70, 0};
    eg.configure(rate, level);
    eg.noteOn(0.0f);

    double sr = 48000.0;
    float peak = 0.0f;
    for (int i = 0; i < static_cast<int>(sr * 0.2); ++i) peak = std::max(peak, eg.process(sr));
    check(peak > 60.0f, "EG sobe perto do pico apos o ataque");

    eg.noteOff();
    for (int i = 0; i < static_cast<int>(sr * 1.0); ++i) eg.process(sr);
    check(eg.isIdle(), "EG fica idle apos noteOff + release completo");
}

static void testPegCentered() {
    SegmentEG peg;
    uint8_t rate[4]  = {99, 99, 99, 99};
    uint8_t level[4] = {50, 50, 50, 50};
    peg.configure(rate, level);
    peg.noteOn(0.0f);

    double sr = 48000.0;
    float v = 0.0f;
    for (int i = 0; i < 1000; ++i) v = peg.process(sr);
    check(std::abs(v - 50.0f) < 0.5f, "PEG default (todos levels=50) fica parado no centro");
}

// --- [ADICIONADO em 2026-09-21] bateria mais rigorosa de envelopes -------

static void testEgRateToSpeedMatchesDocumentedTiming() {
    // ARQUITETURA.txt/dx7_dsp.h documentam: rate=99 cruza as 99 unidades em
    // ~4ms; rate=0 cruza em ~20s. Confirma que a formula bate com isso.
    float speed99 = egRateToSpeed(99.0f);
    float time99Ms = (99.0f / speed99) * 1000.0f;
    float speed0 = egRateToSpeed(0.0f);
    float time0Sec = 99.0f / speed0;

    std::printf("  (rate=99 cruza 99 unidades em %.2fms; rate=0 em %.2fs)\n", time99Ms, time0Sec);
    check(time99Ms > 2.0f && time99Ms < 8.0f, "egRateToSpeed(99) cruza 99 unidades em ~4ms, como documentado");
    check(time0Sec > 15.0f && time0Sec < 25.0f, "egRateToSpeed(0) cruza 99 unidades em ~20s, como documentado");
}

static void testEgLevelToAmplitudeBoundaries() {
    check(egLevelToAmplitude(0.0f) == 0.0f, "egLevelToAmplitude(0) e silencio EXATO (nao so muito baixo)");
    check(std::abs(egLevelToAmplitude(99.0f) - 1.0f) < 0.001f, "egLevelToAmplitude(99) e ~1.0 (0dB, unidade)");
    float a50 = egLevelToAmplitude(50.0f);
    float a90 = egLevelToAmplitude(90.0f);
    check(a50 > 0.0f && a50 < a90 && a90 < 1.0f, "egLevelToAmplitude e monotonica crescente entre os extremos");
}

static void testEnvelopeStageProgression() {
    // ataque com OVERSHOOT (sobe ate 80), decay com UNDERSHOOT (desce ate
    // 40), sustain fica em 60 -- forma deliberada pra confirmar que o EG
    // realmente VISITA os 2 primeiros estagios antes de assentar no 3o,
    // nao so "chega perto do valor final" por sorte.
    SegmentEG eg;
    uint8_t rate[4]  = {99, 99, 99, 99};
    uint8_t level[4] = {80, 40, 60, 0};
    eg.configure(rate, level);
    eg.noteOn(0.0f);

    double sr = 48000.0;
    float maxSeen = 0.0f, minAfterPeak = 200.0f;
    bool passedPeak = false;
    float v = 0.0f;
    for (int i = 0; i < 3000; ++i) {
        v = eg.process(sr);
        maxSeen = std::max(maxSeen, v);
        if (v >= 79.0f) passedPeak = true;
        if (passedPeak) minAfterPeak = std::min(minAfterPeak, v);
    }
    check(maxSeen > 79.0f, "EG visita o nivel do estagio 0 (overshoot ate ~80) antes de assentar");
    check(minAfterPeak < 41.0f, "EG visita o nivel do estagio 1 (undershoot ate ~40) depois do overshoot");

    for (int i = 0; i < 5000; ++i) v = eg.process(sr);
    check(std::abs(v - 60.0f) < 0.5f, "EG assenta EXATAMENTE no nivel do estagio 2 (sustain)");
}

static void testEnvelopeSustainNeverDecaysWithoutNoteOff() {
    SegmentEG eg;
    uint8_t rate[4]  = {50, 50, 50, 50};
    uint8_t level[4] = {99, 99, 70, 0};
    eg.configure(rate, level);
    eg.noteOn(0.0f);
    double sr = 48000.0;

    float v = 0.0f;
    for (int i = 0; i < 48000; ++i) v = eg.process(sr); // 1s: da tempo de sobra de assentar no sustain
    check(std::abs(v - 70.0f) < 0.5f, "EG assentou no sustain (70) apos 1s simulado");

    // 30s simulados SEM noteOff -- sustain tem que segurar o tempo TODO
    for (int i = 0; i < 48000 * 30; ++i) v = eg.process(sr);
    check(std::abs(v - 70.0f) < 0.5f, "sustain ainda EXATO apos 30s simulados sem noteOff");
    check(!eg.isIdle(), "EG nunca fica idle sozinho -- so depois de um noteOff() explicito");
}

static void testEnvelopeNoteOffPreservesCurrentLevel() {
    // ataque deliberadamente LENTO (rate=10) pra conseguir "pegar" o EG no
    // meio do caminho, longe tanto de 0 quanto do alvo.
    SegmentEG eg;
    uint8_t rate[4]  = {10, 99, 99, 40};
    uint8_t level[4] = {99, 99, 99, 0};
    eg.configure(rate, level);
    eg.noteOn(0.0f);
    double sr = 48000.0;

    float v = 0.0f;
    for (int i = 0; i < 4800; ++i) v = eg.process(sr); // 0.1s do ataque lento
    check(v > 1.0f && v < 90.0f, "capturado no MEIO do ataque (nem perto de 0, nem do alvo)");
    float levelAtNoteOff = v;

    eg.noteOff();
    float justAfter = eg.process(sr);
    check(std::abs(justAfter - levelAtNoteOff) < 5.0f,
          "noteOff() nao pula pro nivel de release -- o release comeca DE ONDE o EG estava");

    for (int i = 0; i < 48000 * 2; ++i) v = eg.process(sr);
    check(v == 0.0f && eg.isIdle(), "release termina exatamente em 0 e fica idle");
}

static void testEnvelopeRateScalingSpeedsUpHighNotes() {
    auto samplesUntilLoud = [](int note) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v = makeSimpleFmPatch();
        v.op[0].rateScaling = 7;     // maximo: bastante aceleracao em notas agudas
        v.op[0].ampEG.rate[0] = 20;  // ataque moderado, pra a diferenca ficar visivel
        v.op[0].ampEG.level[0] = 99;
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, note, 100);

        float runningMax = 0.0f;
        for (int i = 0; i < 48000 * 2; ++i) {
            float l, r;
            synth.renderStereo(l, r);
            runningMax = std::max(runningMax, std::abs((l + r) * 0.5f));
            if (runningMax > 0.3f) return i;
        }
        return 48000 * 2;
    };

    int samplesLowNote = samplesUntilLoud(36);  // rateScaling so afeta notas ACIMA de 60 (rateScaleAdd=0 aqui)
    int samplesHighNote = samplesUntilLoud(96); // bem acima de 60: rateScaleAdd grande

    std::printf("  (amostras ate ficar alto: nota grave=%d, nota aguda=%d)\n", samplesLowNote, samplesHighNote);
    check(samplesHighNote < samplesLowNote,
          "rate scaling: nota aguda atinge o limiar de amplitude mais rapido (EG acelerado pela tecla)");
}

static float estimateFrequencyByZeroCrossings(const std::vector<float>& buf, double sampleRate) {
    int crossings = 0;
    for (size_t i = 1; i < buf.size(); ++i) {
        bool up = buf[i - 1] < 0.0f && buf[i] >= 0.0f;
        bool down = buf[i - 1] >= 0.0f && buf[i] < 0.0f;
        if (up || down) ++crossings;
    }
    double durationSec = static_cast<double>(buf.size()) / sampleRate;
    return static_cast<float>((crossings / 2.0) / durationSec);
}

// Tolerancia RELATIVA (nao Hz absoluto): a estimativa por zero-crossing
// quantiza em passos de 1/(2*duracao_da_janela) Hz, entao o mesmo erro
// absoluto pesa mais em frequencias baixas que altas -- comparacoes entre
// frequencias de magnitudes diferentes (ex.: 260Hz vs 520Hz) precisam de
// tolerancia relativa pra nao falhar por pura quantizacao da medicao.
static bool nearlyEqualRel(float a, float b, float relTol = 0.03f) {
    return std::abs(a - b) < std::abs(b) * relTol;
}

static void testPegPushesPitchUpAndDown() {
    auto renderAtPegLevel = [](uint8_t pegLevel) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].waveform = Waveform::Sine;
        v.op[0].outputLevel = 99;
        v.op[0].pegDepth = 1.0f;
        for (int i = 1; i < kNumOps; ++i) {
            v.op[i].ampEG.level[0] = 0; v.op[i].ampEG.level[1] = 0;
            v.op[i].ampEG.level[2] = 0; v.op[i].ampEG.level[3] = 0;
        }
        v.peg.rangeOctaves = 1.0f;
        for (int i = 0; i < 4; ++i) { v.peg.rate[i] = 99; v.peg.level[i] = pegLevel; }
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);

        for (int i = 0; i < 500; ++i) { float l, r; synth.renderStereo(l, r); } // pula o transiente breve do PEG
        std::vector<float> mono;
        mono.reserve(4800);
        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
        return estimateFrequencyByZeroCrossings(mono, 48000.0);
    };

    float freqDown = renderAtPegLevel(0);    // -1 oitava (rangeOctaves=1.0)
    float freqCenter = renderAtPegLevel(50); // sem desvio
    float freqUp = renderAtPegLevel(99);     // quase +1 oitava

    std::printf("  (freq estimada por zero-crossing: PEG=0 -> %.1fHz, PEG=50 -> %.1fHz, PEG=99 -> %.1fHz)\n",
                freqDown, freqCenter, freqUp);
    check(std::abs(freqCenter - 261.6f) < 10.0f, "PEG level=50 fica no centro (~261.6Hz, nota 60 sem desvio)");
    check(freqDown < freqCenter * 0.6f, "PEG level=0 baixa a frequencia (~-1 oitava, rangeOctaves=1.0)");
    check(freqUp > freqCenter * 1.5f, "PEG level=99 sobe a frequencia (~+1 oitava, rangeOctaves=1.0)");
}

static void testEnvelopeLegatoReuseAffectsRetrigger() {
    auto peakRightAfterLegato = [](bool reuse) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v = makeSimpleFmPatch();
        // rate[0]=40 e deliberadamente MODERADO (nao rapido): rapido demais
        // faz o caso "reset" recuperar quase toda a amplitude DENTRO da
        // propria janela de 400 amostras usada pra medir o pico logo apos o
        // legato, escondendo a diferenca entre reuse=true/false. Com 40, o
        // pre-legato (40000 amostras = 0.83s) tem tempo de sobra pra assentar
        // no sustain, mas o pos-legato (so 400 amostras) nao.
        v.op[0].ampEG.rate[0] = 40;
        synth.part(0).voice = v;
        synth.part(0).voiceMode = VoiceMode::Mono;
        synth.part(0).monoLegatoEnvReuse = reuse;
        synth.recompile(0);

        synth.noteOn(0, 60, 100);
        for (int i = 0; i < 40000; ++i) { float l, r; synth.renderStereo(l, r); } // deixa assentar no sustain

        synth.noteOn(0, 64, 100); // legato: nova nota sem soltar a primeira

        float peak = 0.0f;
        for (int i = 0; i < 400; ++i) { // >1 ciclo completo da nota, pra nao perder o pico por fase
            float l, r;
            synth.renderStereo(l, r);
            peak = std::max(peak, std::abs((l + r) * 0.5f));
        }
        return peak;
    };

    float peakReuse = peakRightAfterLegato(true);
    float peakReset = peakRightAfterLegato(false);

    std::printf("  (pico logo apos legato: reuse=true -> %.4f, reuse=false -> %.4f)\n", peakReuse, peakReset);
    check(peakReuse > 0.2f, "monoLegatoEnvReuse=true: envelope NAO reseta no legato (som continua alto na hora)");
    check(peakReset < peakReuse * 0.5f, "monoLegatoEnvReuse=false: envelope RESETA no legato (comeca baixo de novo)");
}

static void testSimpleFmRender() {
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    synth.noteOn(0, 60, 100);

    std::vector<float> mono;
    mono.reserve(48000 * 4);
    for (int i = 0; i < 48000; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        mono.push_back((l + r) * 0.5f);
        if (i == 24000) synth.noteOff(0, 60);
    }
    float rms = computeRms(mono, 0, 24000);
    check(rms > 0.01f, "patch de 2 operadores gera sinal audivel (RMS > 0.01)");

    for (int i = 0; i < 48000 * 3 && synth.activeVoiceCount() > 0; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        mono.push_back((l + r) * 0.5f);
    }
    check(synth.activeVoiceCount() == 0, "fica idle apos o release");

    writeWavMono("output/01_simple_fm.wav", mono, 48000);
}

static void testFilteredModulatorFx() {
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v = makeSimpleFmPatch();
    v.op[1].fxType = FxType::LowPass;
    v.op[1].fxParam1 = 2000.0f;
    v.op[1].fxParam2 = 0.7f;
    v.op[1].fxEG.rate[0] = 40;  v.op[1].fxEG.level[0] = 99;
    v.op[1].fxEG.rate[1] = 40;  v.op[1].fxEG.level[1] = 20;
    v.op[1].fxEG.rate[2] = 40;  v.op[1].fxEG.level[2] = 20;
    v.op[1].fxEG.rate[3] = 40;  v.op[1].fxEG.level[3] = 0;
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 48, 100);

    std::vector<float> mono;
    mono.reserve(48000 * 2);
    bool allFinite = true;
    for (int i = 0; i < 48000 * 2; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        float m = (l + r) * 0.5f;
        if (!std::isfinite(m)) allFinite = false;
        mono.push_back(m);
        if (i == 48000) synth.noteOff(0, 48);
    }
    check(allFinite, "filtro TPT com fxEG modulando o corte nao produz NaN/Inf");
    check(computeRms(mono, 0, 48000) > 0.001f, "sinal com filtro no modulador continua audivel");

    writeWavMono("output/02_filtered_modulator.wav", mono, 48000);
}

static void testMonoGlide() {
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.part(0).voiceMode = VoiceMode::Mono;
    synth.part(0).glideEnabled = true;
    synth.part(0).glideTimeSec = 0.3f;
    synth.recompile(0);

    std::vector<float> mono;
    synth.noteOn(0, 48, 100);
    for (int i = 0; i < 48000; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        mono.push_back((l + r) * 0.5f);
    }

    synth.noteOn(0, 60, 100); // legato (nota anterior ainda ativa): desliza
    float hzEarly = 0.0f, hzLate = 0.0f;
    int total = 48000 * 3;
    for (int i = 0; i < total; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        mono.push_back((l + r) * 0.5f);
        if (i == static_cast<int>(48000 * 0.05)) hzEarly = synth.debugVoiceOfPart(0).debugCurrentHz();
        if (i == total - 1) hzLate = synth.debugVoiceOfPart(0).debugCurrentHz();
    }
    synth.noteOff(0, 60);
    for (int i = 0; i < 48000; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        mono.push_back((l + r) * 0.5f);
    }

    const float startHz = 130.8f;  // nota 48
    const float targetHz = 261.6f; // nota 60
    check(hzEarly > startHz + 1.0f && hzEarly < targetHz - 1.0f,
          "glide desliza gradualmente (nao pula direto pro alvo)");
    check(std::abs(hzLate - targetHz) < 2.0f,
          "glide converge para o alvo apos ~3s (tolerancia 2Hz)");

    writeWavMono("output/03_mono_glide.wav", mono, 48000);
}

static void testUnison() {
    Synth synth;
    synth.init(48000.0, 16);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.part(0).voiceMode = VoiceMode::Unison;
    synth.part(0).unisonCount = 5;
    synth.part(0).unisonDetuneCents = 15.0f;
    synth.part(0).unisonSpread = 0.9f;
    synth.recompile(0);

    synth.noteOn(0, 60, 100);
    check(synth.activeVoiceCount() == 5, "Unison consome exatamente unisonCount (5) vozes");

    std::vector<float> left, right;
    for (int i = 0; i < 48000; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        left.push_back(l);
        right.push_back(r);
        if (i == 24000) synth.noteOff(0, 60);
    }
    for (int i = 0; i < 48000 * 3 && synth.activeVoiceCount() > 0; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        left.push_back(l);
        right.push_back(r);
    }
    check(synth.activeVoiceCount() == 0, "todas as vozes do Unison liberam apos o release");

    writeWavStereo("output/04_unison.wav", left, right, 48000);
}

static void testSustainPedal() {
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);

    synth.sustainPedal(0, true);
    synth.noteOn(0, 60, 100);
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); }
    synth.noteOff(0, 60); // pedal ainda pressionado
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); }
    check(synth.activeVoiceCount() == 1, "nota com pedal pressionado continua soando apos noteOff");

    synth.sustainPedal(0, false); // solta o pedal, libera de verdade
    for (int i = 0; i < 48000 * 3 && synth.activeVoiceCount() > 0; ++i) { float l, r; synth.renderStereo(l, r); }
    check(synth.activeVoiceCount() == 0, "ao soltar o pedal a nota presa e liberada de verdade");
}

static void testPolyAftertouch() {
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v = makeSimpleFmPatch();
    v.op[1].fxType = FxType::LowPass;
    v.op[1].fxParam1 = 200.0f;  // abaixo da fundamental (~261Hz): moduladdor bem filtrado antes do AT
    v.op[1].fxParam2 = 4.0f;    // Q alto: a varredura de corte cruza a fundamental com um pico ressonante
    v.modMatrix[0] = ModRoute{ModSource::PolyAT, ModDest::OperatorFx1, 1, 1.0f}; // AT=1.0 dobra o corte, abrindo o filtro
    synth.part(0).voice = v;
    synth.recompile(0);

    synth.noteOn(0, 60, 100);
    std::vector<float> before;
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); before.push_back((l + r) * 0.5f); }
    float rmsBefore = computeRms(before, 0, before.size());

    synth.polyAftertouch(0, 60, 1.0f);
    std::vector<float> after;
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); after.push_back((l + r) * 0.5f); }
    float rmsAfter = computeRms(after, 0, after.size());

    std::printf("  (RMS antes=%.5f, depois=%.5f)\n", rmsBefore, rmsAfter);
    check(std::abs(rmsBefore - rmsAfter) > 0.001f,
          "poly aftertouch via ModRoute muda o timbre (RMS antes != depois)");
}

static void testOperatorFeedbackModulation() {
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.matrix.mod[0][0] = 1.0f; // op0 tem auto-modulacao (diagonal)
    v.op[0].feedback = 0.1f;   // feedback base pequeno
    v.op[0].outputLevel = 99;
    for (int i = 1; i < kNumOps; ++i) {
        v.op[i].ampEG.level[0] = 0; v.op[i].ampEG.level[1] = 0;
        v.op[i].ampEG.level[2] = 0; v.op[i].ampEG.level[3] = 0;
    }
    v.modMatrix[0] = ModRoute{ModSource::ModWheel, ModDest::OperatorFeedback, 0, 0.9f};
    synth.part(0).voice = v;
    synth.recompile(0);

    synth.noteOn(0, 60, 100);
    std::vector<float> low;
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); low.push_back((l + r) * 0.5f); }
    float rmsLow = computeRms(low, 0, low.size());

    synth.modWheel(0, 1.0f); // fbTrim=0.9 -> effectiveFeedback ~1.0 (clamped)
    std::vector<float> high;
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); high.push_back((l + r) * 0.5f); }
    float rmsHigh = computeRms(high, 0, high.size());

    std::printf("  (RMS feedback baixo=%.5f, alto=%.5f)\n", rmsLow, rmsHigh);
    check(std::abs(rmsLow - rmsHigh) > 0.005f,
          "ModDest::OperatorFeedback (fbTrim) altera o timbre via auto-modulacao");
}

static void testFxEGReleaseFollowsAmp() {
    auto makePatch = [](bool followRelease) {
        VoiceParams v = makeSimpleFmPatch();
        v.op[0].fxType = FxType::LowPass;
        v.op[0].fxParam1 = 2000.0f;
        v.op[0].fxEGReleaseWithAmp = followRelease;
        uint8_t r[4] = {99, 99, 99, 99};
        uint8_t l[4] = {99, 70, 70, 0};
        for (int k = 0; k < 4; ++k) { v.op[0].fxEG.rate[k] = r[k]; v.op[0].fxEG.level[k] = l[k]; }
        v.op[0].ampEG.rate[3] = 5; // release LENTO: mantem a voz ativa por tempo suficiente pra observar o fxEG
        return v;
    };

    Synth synthOn;
    synthOn.init(48000.0, 4);
    synthOn.part(0).voice = makePatch(true);
    synthOn.recompile(0);
    synthOn.noteOn(0, 60, 100);
    for (int i = 0; i < 2000; ++i) { float l, r; synthOn.renderStereo(l, r); }
    synthOn.noteOff(0, 60);
    for (int i = 0; i < 2000; ++i) { float l, r; synthOn.renderStereo(l, r); }
    check(synthOn.activeVoiceCount() == 1 && synthOn.debugVoiceOfPart(0).debugFxEGIdle(0),
          "fxEGReleaseWithAmp=true: fxEG recebe noteOff e acompanha o release");

    Synth synthOff;
    synthOff.init(48000.0, 4);
    synthOff.part(0).voice = makePatch(false);
    synthOff.recompile(0);
    synthOff.noteOn(0, 60, 100);
    for (int i = 0; i < 2000; ++i) { float l, r; synthOff.renderStereo(l, r); }
    synthOff.noteOff(0, 60);
    for (int i = 0; i < 2000; ++i) { float l, r; synthOff.renderStereo(l, r); }
    check(synthOff.activeVoiceCount() == 1 && !synthOff.debugVoiceOfPart(0).debugFxEGIdle(0),
          "fxEGReleaseWithAmp=false: fxEG fica preso no sustain, independente do noteOff");
}

static void testOversamplingReducesAliasing() {
    // Square wave fixa em 5000Hz: o 9o harmonico (45000Hz) nao existe no
    // sinal "ideal" (banda ilimitada, mas so importa o que sobrevive ate
    // 48kHz), porem um oscilador NAO band-limited (osc.h nao faz anti-
    // aliasing por si) reflete esse harmonico de volta pra dentro da banda
    // audivel ao ser amostrado sem cuidado: |45000 - 48000| = 3000Hz.
    // Sem oversampling, essa energia aliasada aparece em 3000Hz. Com 4x de
    // oversampling, o harmonico de 45kHz e gerado (sem aliasing) a 192kHz
    // e depois removido pelo filtro half-band da decimacao (corte final
    // em 24kHz) antes de poder dobrar pra 3kHz.
    auto renderAliasProbe = [](int oversample) {
        Synth synth;
        synth.init(48000.0, 4, oversample);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].fixedFreq = true;
        v.op[0].fixedHz = 5000.0f;
        v.op[0].waveform = Waveform::Square;
        v.op[0].outputLevel = 99;
        for (int i = 1; i < kNumOps; ++i) {
            v.op[i].ampEG.level[0] = 0; v.op[i].ampEG.level[1] = 0;
            v.op[i].ampEG.level[2] = 0; v.op[i].ampEG.level[3] = 0;
        }
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100); // nota so dispara o EG; a freq real vem de fixedHz

        for (int i = 0; i < 2400; ++i) { float l, r; synth.renderStereo(l, r); } // descarta o ataque
        std::vector<float> probe;
        probe.reserve(4800);
        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); probe.push_back((l + r) * 0.5f); }
        return probe;
    };

    float aliasNo = goertzelMagnitude(renderAliasProbe(1), 3000.0, 48000.0);
    float aliasWith = goertzelMagnitude(renderAliasProbe(4), 3000.0, 48000.0);

    std::printf("  (energia aliasada em 3kHz: sem oversampling=%.5f, com 4x=%.5f)\n", aliasNo, aliasWith);
    check(aliasNo > 0.02f,
          "sem oversampling, o 9o harmonico do square em 5kHz alia mensuravelmente pra 3kHz");
    check(aliasWith < aliasNo * 0.3f,
          "com oversampling 4x, o filtro half-band derruba a energia aliasada em 3kHz");
}

static bool nearlyEqual(float a, float b, float eps = 1e-4f) { return std::abs(a - b) < eps; }

static void testPatchJsonRoundTrip() {
    VoiceParams v;
    v.op[0].waveform = Waveform::Saw;
    v.op[0].coarseRatio = 2.5f;
    v.op[0].fixedFreq = true;
    v.op[0].fixedHz = 333.25f;
    v.op[0].fxType = FxType::Comb;
    v.op[0].fxParam1 = 1234.5f;
    v.op[0].fxParam2 = 0.42f;
    v.op[0].fxEGReleaseWithAmp = false;
    v.op[0].levelScaling.breakpoint = 72;
    v.op[0].levelScaling.leftDepth = -5;
    v.op[0].levelScaling.rightExp = true;
    v.op[3].feedback = 0.66f;
    v.matrix.mod[3][3] = 1.0f;
    v.matrix.ring[2][1] = true;
    v.matrix.outputMix[3] = 0.8f;
    v.peg.rangeOctaves = 2.5f;
    v.lfo1.wave = LfoWave::SampleHold;
    v.lfo1.rate = 3.3f;
    v.modMatrix[0] = ModRoute{ModSource::ModWheel, ModDest::OperatorFeedback, 3, -0.5f};

    std::string json = voiceParamsToJson(v).dump();
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(json));

    bool ok = back.op[0].waveform == Waveform::Saw
        && nearlyEqual(back.op[0].coarseRatio, 2.5f)
        && back.op[0].fixedFreq
        && nearlyEqual(back.op[0].fixedHz, 333.25f)
        && back.op[0].fxType == FxType::Comb
        && nearlyEqual(back.op[0].fxParam1, 1234.5f)
        && nearlyEqual(back.op[0].fxParam2, 0.42f)
        && !back.op[0].fxEGReleaseWithAmp
        && back.op[0].levelScaling.breakpoint == 72
        && back.op[0].levelScaling.leftDepth == -5
        && back.op[0].levelScaling.rightExp
        && nearlyEqual(back.op[3].feedback, 0.66f)
        && nearlyEqual(back.matrix.mod[3][3], 1.0f)
        && back.matrix.ring[2][1]
        && nearlyEqual(back.matrix.outputMix[3], 0.8f)
        && nearlyEqual(back.peg.rangeOctaves, 2.5f)
        && back.lfo1.wave == LfoWave::SampleHold
        && nearlyEqual(back.lfo1.rate, 3.3f)
        && back.modMatrix[0].src == ModSource::ModWheel
        && back.modMatrix[0].dest == ModDest::OperatorFeedback
        && back.modMatrix[0].opIndex == 3
        && nearlyEqual(back.modMatrix[0].depth, -0.5f);
    check(ok, "VoiceParams -> JSON -> VoiceParams preserva todos os campos testados");

    VoiceParams fromEmpty = voiceParamsFromJson(JsonValue::parse("{}"));
    check(fromEmpty.matrix.outputMix[0] == 1.0f && fromEmpty.op[1].outputLevel == 60,
          "JSON vazio/incompleto nao quebra o parser (cai nos defaults do construtor)");
}

static void testPatchFileSaveLoad() {
    PartParams p;
    p.name = "Meu Sino";
    p.voiceMode = VoiceMode::Unison;
    p.unisonCount = 3;
    p.transpose = -12;
    p.voice.op[1].waveform = Waveform::Triangle;
    p.voice.op[1].outputLevel = 42;

    const std::string path = "output/test_patch.json";
    check(savePartFile(path, p), "savePartFile grava o arquivo com sucesso");

    PartParams loaded;
    check(loadPartFile(path, loaded)
          && loaded.name == "Meu Sino"
          && loaded.voiceMode == VoiceMode::Unison
          && loaded.unisonCount == 3
          && loaded.transpose == -12
          && loaded.voice.op[1].waveform == Waveform::Triangle
          && loaded.voice.op[1].outputLevel == 42,
          "loadPartFile le de volta exatamente o que foi salvo");
}

static void testLivePatchEditPropagatesToSoundingVoice() {
    Synth synth;
    synth.init(48000.0, 4);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    synth.noteOn(0, 60, 100);

    std::vector<float> before;
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); before.push_back((l + r) * 0.5f); }
    float rmsBefore = computeRms(before, 0, before.size());

    // edita o timbre AO VIVO, com a voz ja soando, sem noteOff/noteOn novo --
    // exatamente o cenario que o mecanismo de troca atomica de patch precisa
    // preservar (ver PartRuntime::live()/Synth::recompile() em dx7_alloc.h).
    VoiceParams edited = synth.part(0).voice;
    edited.op[0].outputLevel = 20;
    synth.part(0).voice = edited;
    synth.recompile(0);

    std::vector<float> after;
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); after.push_back((l + r) * 0.5f); }
    float rmsAfter = computeRms(after, 0, after.size());

    std::printf("  (RMS antes da edicao ao vivo=%.5f, depois=%.5f)\n", rmsBefore, rmsAfter);
    check(rmsAfter < rmsBefore * 0.5f,
          "editar part(idx).voice + recompile() com a voz ja soando muda o timbre na hora (edicao ao vivo preservada)");
}

static void testConcurrentPatchEditNoCrash() {
    // Stress test real de concorrencia: uma thread "UI" fica editando o
    // patch (part().voice = X; recompile();) sem parar, enquanto a thread
    // principal renderiza audio continuamente, como um host faria com
    // audio numa thread e edicao de parametro noutra. Nao prova ausencia
    // de data race no sentido formal (pra isso, rodar com -fsanitize=thread),
    // mas confirma que 2s de audio sob edicao concorrente pesada nao produz
    // NaN/Inf nem crasha.
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    synth.noteOn(0, 60, 100);

    std::atomic<bool> stop{false};
    std::atomic<bool> anyNonFinite{false};
    std::atomic<long> editCount{0};

    std::thread writer([&]() {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> distLevel(0, 99);
        std::uniform_real_distribution<float> distRatio(0.5f, 8.0f);
        while (!stop.load(std::memory_order_relaxed)) {
            VoiceParams v = synth.part(0).voice; // parte da ultima staging escrita por esta mesma thread
            v.op[0].outputLevel = static_cast<uint8_t>(distLevel(rng));
            v.op[1].coarseRatio = distRatio(rng);
            v.matrix.mod[0][1] = distRatio(rng) / 8.0f;
            synth.part(0).voice = v;
            synth.recompile(0);
            editCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 48000 * 2; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        if (!std::isfinite(l) || !std::isfinite(r)) anyNonFinite.store(true, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    std::printf("  (%ld edicoes de patch concorrentes durante 2s de audio)\n", editCount.load());
    check(!anyNonFinite.load(),
          "2s de audio com outra thread editando o patch sem parar: sem NaN/Inf (nenhuma corrupcao observada)");
}

static void testChorusChangesStereoImage() {
    auto render = [](bool chorusOn) {
        Synth synth;
        synth.init(48000.0, 4);
        synth.part(0).voice = makeSimpleFmPatch();
        synth.recompile(0);
        synth.globalFx().chorus.enabled = chorusOn;
        synth.globalFx().chorus.mix = 0.5f;
        synth.globalFx().chorus.depth = 0.6f;
        synth.globalFx().chorus.rateHz = 1.2f;
        synth.noteOn(0, 60, 100);

        std::vector<float> left, right;
        for (int i = 0; i < 9600; ++i) {
            float l, r;
            synth.renderStereo(l, r);
            left.push_back(l);
            right.push_back(r);
        }
        return std::make_pair(left, right);
    };

    auto [dryL, dryR] = render(false);
    auto [wetL, wetR] = render(true);

    // Sem chorus, a fonte e mono (mesmo patch, sem pan): L e R ficam
    // identicos amostra a amostra. Com chorus, os LFOs L/R defasados 90
    // graus devem descorrelacionar os canais.
    double diffDry = 0.0, diffWet = 0.0;
    for (size_t i = 0; i < dryL.size(); ++i) {
        diffDry += std::abs(static_cast<double>(dryL[i]) - dryR[i]);
        diffWet += std::abs(static_cast<double>(wetL[i]) - wetR[i]);
    }
    std::printf("  (diferenca L/R acumulada: sem chorus=%.5f, com chorus=%.5f)\n", diffDry, diffWet);
    check(diffDry < 1e-6, "sem chorus, fonte mono produz L==R (confirma a baseline do teste)");
    check(diffWet > diffDry + 1.0, "chorus liga descorrelaciona L/R (largura estereo mensuravel)");
}

static void testDelayProducesEchoAtExpectedTime() {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v = makeSimpleFmPatch();
    for (int i = 0; i < kNumOps; ++i) v.op[i].ampEG.rate[3] = 99; // release RAPIDO: o "seco" morre logo
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.globalFx().delay.enabled = true;
    const float delaySec = 0.2f;
    synth.globalFx().delay.timeLeftSec = delaySec;
    synth.globalFx().delay.timeRightSec = delaySec;
    synth.globalFx().delay.feedback = 0.0f; // 1 eco so, sem repeticoes -- mais facil de medir
    synth.globalFx().delay.mix = 0.9f;

    const int noteOnSamples = 480; // ~10ms de nota bem curta (o "impulso" que vai ecoar)
    synth.noteOn(0, 60, 100);
    for (int i = 0; i < noteOnSamples; ++i) { float l, r; synth.renderStereo(l, r); }
    synth.noteOff(0, 60);

    // A partir daqui, mono[0] == a amostra do instante do noteOff. O eco da
    // rajada de 10ms original (que ocorreu ANTES do noteOff) aparece na
    // saida do delay exatamente na janela [delaySamples-noteOnSamples,
    // delaySamples) relativa a este ponto -- matematica exata, nao um
    // numero redondo chutado.
    const int delaySamples = static_cast<int>(delaySec * 48000.0f);
    const int echoStart = delaySamples - noteOnSamples;
    const int total = delaySamples + 4800;
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }

    float rmsBeforeEcho = computeRms(mono, 1000, static_cast<size_t>(echoStart - 2000)); // bem antes do eco
    float rmsAtEcho = computeRms(mono, static_cast<size_t>(echoStart), static_cast<size_t>(noteOnSamples));

    std::printf("  (RMS antes do eco=%.5f, RMS na janela exata do eco=%.5f)\n", rmsBeforeEcho, rmsAtEcho);
    check(rmsBeforeEcho < 0.01f, "seco decai a quase silencio antes do tempo do delay (release rapido)");
    check(rmsAtEcho > rmsBeforeEcho * 3.0f, "energia reaparece exatamente na janela do tempo de delay configurado (eco real)");
}

static void testReverbAddsDecayTail() {
    auto renderTailRms = [](bool reverbOn) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v = makeSimpleFmPatch();
        for (int i = 0; i < kNumOps; ++i) v.op[i].ampEG.rate[3] = 99; // release RAPIDO
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.globalFx().reverb.enabled = reverbOn;
        synth.globalFx().reverb.mix = 0.6f;
        synth.globalFx().reverb.roomSize = 0.8f;
        synth.globalFx().reverb.damping = 0.2f;

        synth.noteOn(0, 60, 100);
        for (int i = 0; i < 480; ++i) { float l, r; synth.renderStereo(l, r); } // ~10ms de nota
        synth.noteOff(0, 60);

        std::vector<float> mono;
        int total = 24000; // 0.5s
        mono.reserve(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
        return computeRms(mono, 9600, 9600); // janela ~0.2s..0.4s, bem depois do release rapido do seco
    };

    float tailDry = renderTailRms(false);
    float tailWet = renderTailRms(true);

    std::printf("  (RMS da cauda: sem reverb=%.5f, com reverb=%.5f)\n", tailDry, tailWet);
    check(tailDry < 0.005f, "sem reverb, o sinal decai a quase silencio logo apos o release rapido");
    check(tailWet > tailDry + 0.01f, "com reverb ligado, sobra uma cauda de decaimento mensuravel na mesma janela");
}

static void testAfmWaveformsAreBoundedAndDistinct() {
    // As 8 formas de onda do motor AFM (Sine + as 7 novas). Renderiza cada
    // uma isoladamente (op0, sem modulacao) e confirma: fica dentro de
    // -1..1 (nenhum overshoot de retificacao/dobra de fase) e produz um
    // sinal MENSURAVELMENTE diferente das outras (nao e so um alias
    // silencioso de outra forma de onda ja existente).
    static const Waveform kWaves[] = {
        Waveform::Sine, Waveform::HalfSine, Waveform::AbsSine, Waveform::QuarterSine,
        Waveform::AlternatingSine, Waveform::CamelSine, Waveform::SquaredSine, Waveform::LogSaw,
    };
    static const char* kNames[] = {
        "Sine", "HalfSine", "AbsSine", "QuarterSine",
        "AlternatingSine", "CamelSine", "SquaredSine", "LogSaw",
    };
    constexpr int kCount = 8;

    std::vector<std::vector<float>> renders(kCount);
    bool allBounded = true;

    for (int w = 0; w < kCount; ++w) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].waveform = kWaves[w];
        v.op[0].outputLevel = 99;
        for (int i = 1; i < kNumOps; ++i) {
            v.op[i].ampEG.level[0] = 0; v.op[i].ampEG.level[1] = 0;
            v.op[i].ampEG.level[2] = 0; v.op[i].ampEG.level[3] = 0;
        }
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);

        auto& buf = renders[w];
        buf.reserve(2400);
        for (int i = 0; i < 2400; ++i) {
            float l, r;
            synth.renderStereo(l, r);
            float m = (l + r) * 0.5f;
            if (m < -1.0001f || m > 1.0001f) allBounded = false;
            buf.push_back(m);
        }
    }
    check(allBounded, "todas as 8 formas de onda AFM ficam dentro de -1..1 (sem overshoot)");

    bool allDistinct = true;
    for (int a = 0; a < kCount && allDistinct; ++a) {
        for (int b = a + 1; b < kCount; ++b) {
            double diff = 0.0;
            for (size_t i = 0; i < renders[a].size(); ++i) diff += std::abs(renders[a][i] - renders[b][i]);
            diff /= static_cast<double>(renders[a].size());
            if (diff < 0.02) {
                std::printf("  (%s vs %s: diferenca media = %.5f -- parecidas demais)\n", kNames[a], kNames[b], diff);
                allDistinct = false;
            }
        }
    }
    check(allDistinct, "as 8 formas de onda AFM sao mutuamente distintas (nenhum par quase identico)");
}

// --- [ADICIONADO em 2026-09-21] bateria de operadores/oscilador ----------

// Renderiza uma nota isolada (op0 sozinho, Sine por padrao -- confiavel
// pra estimativa de frequencia por zero-crossing) e devolve a frequencia
// medida. Reusa estimateFrequencyByZeroCrossings() da bateria de envelopes.
static float renderCarrierFrequency(int note, float coarseRatio = 1.0f, float fineCents = 0.0f,
                                     float detuneCents = 0.0f, bool fixedFreq = false, float fixedHz = 440.0f) {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].waveform = Waveform::Sine;
    v.op[0].outputLevel = 99;
    v.op[0].fixedFreq = fixedFreq;
    v.op[0].fixedHz = fixedHz;
    v.op[0].coarseRatio = coarseRatio;
    v.op[0].fineCents = fineCents;
    v.op[0].detuneCents = detuneCents;
    for (int i = 1; i < kNumOps; ++i) {
        v.op[i].ampEG.level[0] = 0; v.op[i].ampEG.level[1] = 0;
        v.op[i].ampEG.level[2] = 0; v.op[i].ampEG.level[3] = 0;
    }
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, note, 100);

    for (int i = 0; i < 500; ++i) { float l, r; synth.renderStereo(l, r); } // pula o transiente do ataque
    // janela de 0.2s (9600 amostras): zero-crossing quantiza a estimativa em
    // passos de 1/(2*duracao) Hz -- janela maior = passos menores = mais
    // precisao, importante pra comparacoes de frequencia com tolerancia apertada.
    std::vector<float> mono;
    mono.reserve(9600);
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
    return estimateFrequencyByZeroCrossings(mono, 48000.0);
}

static void testNoteToHzStandardTuning() {
    float freqA4 = renderCarrierFrequency(69); // A4 = 440Hz por convencao (afinacao padrao)
    float freqC4 = renderCarrierFrequency(60); // C4 = ~261.63Hz
    float freqA5 = renderCarrierFrequency(81); // uma oitava acima de A4 = 880Hz

    std::printf("  (nota 69=%.2fHz, nota 60=%.2fHz, nota 81=%.2fHz)\n", freqA4, freqC4, freqA5);
    check(std::abs(freqA4 - 440.0f) < 3.0f, "nota MIDI 69 (A4) produz ~440Hz (afinacao padrao)");
    check(std::abs(freqC4 - 261.63f) < 3.0f, "nota MIDI 60 (C4) produz ~261.63Hz");
    check(std::abs(freqA5 - 880.0f) < 6.0f, "nota MIDI 81 (uma oitava acima de A4) produz ~880Hz (dobro exato)");
}

static void testCoarseRatioScalesFrequencyLinearly() {
    float freqRatio1 = renderCarrierFrequency(60, 1.0f);
    float freqRatio2 = renderCarrierFrequency(60, 2.0f);
    float freqRatioHalf = renderCarrierFrequency(60, 0.5f);

    std::printf("  (ratio=0.5 -> %.2fHz, ratio=1.0 -> %.2fHz, ratio=2.0 -> %.2fHz)\n",
                freqRatioHalf, freqRatio1, freqRatio2);
    check(nearlyEqualRel(freqRatio2, freqRatio1 * 2.0f),
          "coarseRatio=2.0 produz exatamente o dobro da frequencia de ratio=1.0");
    check(nearlyEqualRel(freqRatioHalf, freqRatio1 * 0.5f),
          "coarseRatio=0.5 produz exatamente metade da frequencia de ratio=1.0");
}

static void testFixedFreqIgnoresNote() {
    float freqLowNote = renderCarrierFrequency(48, 1.0f, 0.0f, 0.0f, true, 440.0f);
    float freqHighNote = renderCarrierFrequency(84, 1.0f, 0.0f, 0.0f, true, 440.0f);

    std::printf("  (fixedHz=440, nota 48 -> %.2fHz, nota 84 -> %.2fHz)\n", freqLowNote, freqHighNote);
    check(std::abs(freqLowNote - 440.0f) < 3.0f && std::abs(freqHighNote - 440.0f) < 3.0f,
          "fixedFreq=true ignora a nota tocada -- mesma frequencia (440Hz) em notas bem diferentes");
}

static void testFineAndDetuneCentsShiftPitch() {
    float freqBase = renderCarrierFrequency(60, 1.0f, 0.0f, 0.0f);
    float freqFineOctave = renderCarrierFrequency(60, 1.0f, 1200.0f, 0.0f);   // 1200 cents = 1 oitava
    float freqDetuneOctave = renderCarrierFrequency(60, 1.0f, 0.0f, 1200.0f);

    std::printf("  (base=%.2fHz, fine=+1200c -> %.2fHz, detune=+1200c -> %.2fHz)\n",
                freqBase, freqFineOctave, freqDetuneOctave);
    check(nearlyEqualRel(freqFineOctave, freqBase * 2.0f), "fineCents=1200 (1 oitava) dobra a frequencia");
    check(nearlyEqualRel(freqDetuneOctave, freqBase * 2.0f), "detuneCents=1200 (1 oitava) tambem dobra a frequencia");
}

static void testClassicWaveformShapesMatchMath() {
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    check(std::abs(oscWave(0.25f, Waveform::Sine, rng, dist) - 1.0f) < 0.001f,
          "Sine em phase=0.25 (quarto de ciclo) vale exatamente +1");
    check(std::abs(oscWave(0.75f, Waveform::Sine, rng, dist) - (-1.0f)) < 0.001f,
          "Sine em phase=0.75 vale exatamente -1");
    check(oscWave(0.1f, Waveform::Square, rng, dist) == 1.0f && oscWave(0.9f, Waveform::Square, rng, dist) == -1.0f,
          "Square e exatamente +1 na primeira metade do ciclo e -1 na segunda");
    check(std::abs(oscWave(0.0f, Waveform::Saw, rng, dist) - (-1.0f)) < 0.001f &&
          std::abs(oscWave(0.999f, Waveform::Saw, rng, dist) - 1.0f) < 0.01f,
          "Saw rampa linearmente de -1 (inicio do ciclo) a +1 (fim do ciclo)");
    check(std::abs(oscWave(0.0f, Waveform::Triangle, rng, dist) - (-1.0f)) < 0.001f &&
          std::abs(oscWave(0.5f, Waveform::Triangle, rng, dist) - 1.0f) < 0.001f,
          "Triangle vai de -1 (inicio) a +1 (metade do ciclo)");
}

static void testAfmWaveformStructuralProperties() {
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    bool absSineAlwaysNonNegative = true;
    bool halfSineZeroInSecondHalf = true;
    for (int i = 0; i <= 100; ++i) {
        float p = i / 100.0f;
        if (oscWave(p, Waveform::AbsSine, rng, dist) < -0.0001f) absSineAlwaysNonNegative = false;
        if (p >= 0.5f && std::abs(oscWave(p, Waveform::HalfSine, rng, dist)) > 0.0001f) halfSineZeroInSecondHalf = false;
    }
    check(absSineAlwaysNonNegative, "AbsSine nunca fica negativo (retificacao de onda completa)");
    check(halfSineZeroInSecondHalf, "HalfSine fica em silencio EXATO na segunda metade do ciclo");
    check(std::abs(oscWave(0.35f, Waveform::CamelSine, rng, dist)) < 0.0001f,
          "CamelSine tem silencio EXATO no vao entre as 2 corcovas (ex.: phase=0.35)");
}

static void testPhaseModulationZeroMeansUnmodifiedCarrier() {
    auto renderWithMod = [](float modAmount) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.matrix.mod[0][1] = modAmount;
        v.op[0].outputLevel = 99;
        v.op[1].outputLevel = 99; // modulador com nivel audivel -- garante que mod=0 zera por CONTA da matriz, nao por o modulador estar quieto
        for (int i = 2; i < kNumOps; ++i) {
            v.op[i].ampEG.level[0] = 0; v.op[i].ampEG.level[1] = 0;
            v.op[i].ampEG.level[2] = 0; v.op[i].ampEG.level[3] = 0;
        }
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);
        std::vector<float> mono;
        mono.reserve(1000);
        for (int i = 0; i < 1000; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
        return mono;
    };

    auto unmodulated = renderWithMod(0.0f);

    Synth refSynth;
    refSynth.init(48000.0, 4);
    VoiceParams refV;
    refV.matrix = AlgorithmMatrix{};
    refV.matrix.outputMix[0] = 1.0f;
    refV.op[0].outputLevel = 99;
    for (int i = 1; i < kNumOps; ++i) {
        refV.op[i].ampEG.level[0] = 0; refV.op[i].ampEG.level[1] = 0;
        refV.op[i].ampEG.level[2] = 0; refV.op[i].ampEG.level[3] = 0;
    }
    refSynth.part(0).voice = refV;
    refSynth.recompile(0);
    refSynth.noteOn(0, 60, 100);
    std::vector<float> reference;
    reference.reserve(1000);
    for (int i = 0; i < 1000; ++i) { float l, r; refSynth.renderStereo(l, r); reference.push_back((l + r) * 0.5f); }

    double maxDiff = 0.0;
    for (size_t i = 0; i < unmodulated.size(); ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(unmodulated[i] - reference[i])));
    check(maxDiff < 0.0001, "matrix.mod=0.0 produz saida IDENTICA a nem ter o modulador no roteamento");

    auto modulated = renderWithMod(1.0f);
    double diffFromUnmodulated = 0.0;
    for (size_t i = 0; i < modulated.size(); ++i)
        diffFromUnmodulated = std::max(diffFromUnmodulated, static_cast<double>(std::abs(modulated[i] - unmodulated[i])));
    check(diffFromUnmodulated > 0.01, "matrix.mod=1.0 muda audivelmente a saida (a modulacao de fase tem efeito real)");
}

static void testKeyLevelScalingAffectsOutputBySide() {
    auto renderRms = [](int note) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v = makeSimpleFmPatch();
        v.op[0].outputLevel = 80;              // abaixo do maximo: o ganho do key scaling tem teto de 0dB (= level 99)
        v.op[0].levelScaling.breakpoint = 60;
        v.op[0].levelScaling.leftDepth = -12; // mais QUIETO abaixo do breakpoint
        v.op[0].levelScaling.rightDepth = 12; // mais ALTO acima do breakpoint
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, note, 100);
        std::vector<float> mono;
        mono.reserve(4800);
        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
        return computeRms(mono, 0, mono.size());
    };

    float rmsBelow = renderRms(36); // bem abaixo do breakpoint (60)
    float rmsAt = renderRms(60);    // no breakpoint
    float rmsAbove = renderRms(84); // bem acima do breakpoint

    std::printf("  (RMS: nota 36=%.5f, nota 60=%.5f, nota 84=%.5f)\n", rmsBelow, rmsAt, rmsAbove);
    check(rmsBelow < rmsAt, "leftDepth negativo: nota abaixo do breakpoint fica mais quieta");
    check(rmsAbove > rmsAt, "rightDepth positivo: nota acima do breakpoint fica mais alta");
}

static void testVelocitySensitivityAffectsLevel() {
    auto renderRms = [](int velSens, int velocity) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v = makeSimpleFmPatch();
        v.op[0].velocitySens = static_cast<uint8_t>(velSens);
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, velocity);
        std::vector<float> mono;
        mono.reserve(4800);
        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
        return computeRms(mono, 0, mono.size());
    };

    float rmsLoudHighSens = renderRms(7, 127);
    float rmsQuietHighSens = renderRms(7, 1);
    float rmsLoudNoSens = renderRms(0, 127);
    float rmsQuietNoSens = renderRms(0, 1);

    std::printf("  (velocitySens=7: vel127=%.5f vel1=%.5f | velocitySens=0: vel127=%.5f vel1=%.5f)\n",
                rmsLoudHighSens, rmsQuietHighSens, rmsLoudNoSens, rmsQuietNoSens);
    check(rmsLoudHighSens > rmsQuietHighSens * 1.5f,
          "velocitySens=7: velocity alta soa MUITO mais forte que velocity baixa");
    check(std::abs(rmsLoudNoSens - rmsQuietNoSens) < rmsLoudNoSens * 0.05f,
          "velocitySens=0: velocity nao afeta o nivel (diferenca desprezivel)");
}

static void testOutputLevelCurveMatchesFormula() {
    auto renderRms = [](int outputLevel) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].outputLevel = static_cast<uint8_t>(outputLevel);
        for (int i = 1; i < kNumOps; ++i) {
            v.op[i].ampEG.level[0] = 0; v.op[i].ampEG.level[1] = 0;
            v.op[i].ampEG.level[2] = 0; v.op[i].ampEG.level[3] = 0;
        }
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);
        std::vector<float> mono;
        mono.reserve(4800);
        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
        return computeRms(mono, 0, mono.size());
    };

    float rms99 = renderRms(99);
    float rms93 = renderRms(93);
    float expectedRatio = dbToLinear((93.0f - 99.0f) * 0.75f); // formula exata de OperatorParams.outputLevel

    float actualRatio = rms93 / rms99;
    std::printf("  (RMS(99)=%.5f, RMS(93)=%.5f, razao=%.4f, esperado=%.4f)\n", rms99, rms93, actualRatio, expectedRatio);
    check(std::abs(actualRatio - expectedRatio) < 0.02f,
          "outputLevel segue a formula documentada (level-99)*0.75dB, sem floor de silencio exato (diferente do EG)");
}

static void testAlgorithmPresetsUseEachOperatorExactlyOnce() {
    // Rede de seguranca contra erro de digitacao nos 32 algoritmos
    // desenhados a mao em dx7_algorithms.h: pra CADA algoritmo, a uniao de
    // todos os operadores citados em chains/forks/merges tem que ser
    // EXATAMENTE {0..7}, sem repeticao e sem omissao.
    bool allValid = true;
    for (int n = 1; n <= kNumAlgorithms; ++n) {
        const auto& t = algorithmTable();
        const AlgorithmDef& def = t[static_cast<size_t>(n - 1)];
        int seen[kNumOps] = {};
        auto mark = [&](int op) { if (op >= 0 && op < kNumOps) seen[op]++; };

        for (auto& chain : def.chains) for (int op : chain) mark(op);
        for (auto& fork : def.forks) {
            for (int op : fork.carriers) mark(op);
            for (int op : fork.modChain) mark(op);
        }
        for (auto& merge : def.merges) {
            mark(merge.carrier);
            for (auto& chain : merge.modChains) for (int op : chain) mark(op);
        }

        for (int i = 0; i < kNumOps; ++i) {
            if (seen[i] != 1) {
                std::printf("  (algoritmo %d \"%s\": operador %d visto %d vez(es), esperado 1)\n",
                            n, def.name, i, seen[i]);
                allValid = false;
            }
        }
    }
    check(allValid, "os 32 algoritmos preset usam cada operador (0-7) exatamente 1 vez, sem repeticao/omissao");
}

static void testAlgorithmPresetsRenderAudibly() {
    // Aplica CADA um dos 32 algoritmos num patch simples e confirma que
    // renderiza um sinal finito e audivel -- pega problemas estruturais
    // (ex.: nenhuma portadora setada, ciclo sem saida) que o teste anterior
    // (contagem de operadores) nao pegaria sozinho.
    bool allOk = true;
    for (int n = 1; n <= kNumAlgorithms; ++n) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        applyAlgorithm(v, n);
        for (int i = 0; i < kNumOps; ++i) v.op[i].outputLevel = 70; // nivel moderado, todo operador contribui se for portadora
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);

        std::vector<float> mono;
        mono.reserve(4800);
        bool finite = true;
        for (int i = 0; i < 4800; ++i) {
            float l, r;
            synth.renderStereo(l, r);
            if (!std::isfinite(l) || !std::isfinite(r)) finite = false;
            mono.push_back((l + r) * 0.5f);
        }
        float rms = computeRms(mono, 0, mono.size());
        if (!finite || rms <= 0.001f) {
            std::printf("  (algoritmo %d \"%s\": finite=%d RMS=%.5f)\n",
                        n, algorithmName(n), finite, rms);
            allOk = false;
        }
    }
    check(allOk, "os 32 algoritmos preset renderizam sinal finito e audivel (RMS > 0.001)");
}

static void testChordDoesNotClip() {
    // [ADICIONADO em 2026-09-21] Regressao pro bug reportado pelo usuario:
    // "acordes nao soam bem" -- uma UNICA nota ja chega perto de +-1.0 com
    // o patch default, entao um acorde de 3+ notas somava sem nenhuma
    // protecao e passava de +-3.0 (42% das amostras clipando). Corrigido
    // com softLimit() no master bus (ver dx7_dsp.h e Synth::renderStereo()).
    Synth synth;
    synth.init(48000.0, 8);
    synth.recompile(0);
    synth.noteOn(0, 60, 100); // triade de C maior
    synth.noteOn(0, 64, 100);
    synth.noteOn(0, 67, 100);

    float peak = 0.0f;
    int overCount = 0;
    for (int i = 0; i < 48000; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
        if (std::abs(l) > 1.0001f || std::abs(r) > 1.0001f) ++overCount;
    }

    std::printf("  (acorde de 3 notas: pico=%.4f, amostras acima de 1.0=%d/48000)\n", peak, overCount);
    check(peak <= 1.0001f, "acorde de 3 notas nunca ultrapassa +-1.0 na saida (sem clipping duro)");
    check(overCount == 0, "nenhuma amostra do acorde fica acima de +-1.0");
}

static void testMasterLimiterIsTransparentForSingleVoice() {
    // O limitador NAO deveria audivelmente colorir uma unica voz normal --
    // so entra em acao quando o sinal realmente passa do limiar (0.9).
    Synth synth;
    synth.init(48000.0, 8);
    synth.recompile(0);
    synth.noteOn(0, 60, 100);

    std::vector<float> mono;
    mono.reserve(9600);
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
    float rms = computeRms(mono, 0, mono.size());

    std::printf("  (RMS de 1 voz com limitador ativo=%.5f)\n", rms);
    check(rms > 0.4f, "limitador continua deixando 1 voz normal audivel e com nivel razoavel (nao esmaga o sinal)");
}

static void testPolyphonyGainKeepsTotalLoudnessRoughlyConstant() {
    // [ADICIONADO em 2026-09-21] Compensacao 1/sqrt(N): confirma que o RMS
    // total NAO cresce proporcionalmente ao numero de notas (o que causaria
    // o clipping original) nem cai proporcionalmente a 1/N (fraco demais) --
    // deve ficar dentro de uma faixa razoavelmente ESTREITA conforme N vai
    // de 1 a 8 notas.
    static const int kNotes[8] = {48, 52, 55, 60, 64, 67, 72, 76};
    auto renderRmsWithNVoices = [](int n) {
        Synth synth;
        synth.init(48000.0, 16);
        synth.recompile(0);
        for (int i = 0; i < n; ++i) synth.noteOn(0, kNotes[i], 100);

        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); } // deixa a suavizacao (~30ms) convergir

        std::vector<float> mono;
        mono.reserve(4800);
        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); mono.push_back((l + r) * 0.5f); }
        return computeRms(mono, 0, mono.size());
    };

    float rms1 = renderRmsWithNVoices(1);
    float rms4 = renderRmsWithNVoices(4);
    float rms8 = renderRmsWithNVoices(8);

    std::printf("  (RMS total: 1 nota=%.4f, 4 notas=%.4f, 8 notas=%.4f)\n", rms1, rms4, rms8);
    check(rms4 < rms1 * 2.0f && rms4 > rms1 * 0.5f,
          "RMS com 4 notas fica na mesma ORDEM DE GRANDEZA que com 1 (nao cresce 4x nem cai 4x)");
    check(rms8 < rms1 * 2.0f && rms8 > rms1 * 0.5f,
          "RMS com 8 notas fica na mesma ORDEM DE GRANDEZA que com 1 (compensacao 1/sqrt(N) funcionando)");
}

static void testPolyphonyGainIsSmoothedNoClick() {
    // Adicionar uma nota no meio de um acorde nao deveria produzir um
    // DEGRAU audivel de ganho (a compensacao e suavizada, nao instantanea).
    Synth synth;
    synth.init(48000.0, 8);
    synth.recompile(0);
    synth.noteOn(0, 60, 100);
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); } // assenta com 1 voz

    float prevSample = 0.0f;
    { float l, r; synth.renderStereo(l, r); prevSample = (l + r) * 0.5f; }

    synth.noteOn(0, 64, 100); // acorde ganha uma 2a nota AGORA

    float maxJump = 0.0f;
    for (int i = 0; i < 20; ++i) { // primeiras amostras logo apos a mudanca de polifonia
        float l, r;
        synth.renderStereo(l, r);
        float s = (l + r) * 0.5f;
        maxJump = std::max(maxJump, std::abs(s - prevSample));
        prevSample = s;
    }
    std::printf("  (maior salto amostra-a-amostra logo apos nova nota=%.4f)\n", maxJump);
    check(maxJump < 0.3f, "ganho de polifonia suavizado: nenhum degrau abrupto ao adicionar uma nota ao acorde");
}

static void testMultitimbral4Parts() {
    Synth synth;
    synth.init(48000.0, 64);
    for (int p = 0; p < 4; ++p) {
        synth.part(p).voice = makeSimpleFmPatch();
        synth.recompile(p);
    }
    synth.noteOn(0, 60, 100);
    synth.noteOn(1, 62, 100);
    synth.noteOn(2, 64, 100);
    synth.noteOn(3, 67, 100);
    check(synth.activeVoiceCount() == 4, "uma nota em cada uma das 4 partes soa do mesmo pool de 64");
}

static void testPolyphonyCap() {
    Synth synth;
    synth.init(48000.0, 64);
    VoiceParams v = makeSimpleFmPatch();
    for (int i = 0; i < kNumOps; ++i) v.op[i].ampEG.rate[3] = 5; // release lento, forca roubo de voz
    synth.part(0).voice = v;
    synth.recompile(0);

    int maxSeen = 0;
    for (int n = 0; n < 80; ++n) {
        synth.noteOn(0, 20 + (n % 90), 100);
        for (int i = 0; i < 200; ++i) { float l, r; synth.renderStereo(l, r); }
        maxSeen = std::max(maxSeen, synth.activeVoiceCount());
    }
    check(maxSeen <= 64, "activeVoiceCount() nunca excede o pool de 64 vozes");
}

// --- [ADICIONADO em 2026-09-23] regressoes da revisao de bugs -------------

static void renderN(Synth& synth, int n) {
    for (int i = 0; i < n; ++i) { float l, r; synth.renderStereo(l, r); }
}

static float renderRmsN(Synth& synth, int n) {
    double acc = 0.0;
    for (int i = 0; i < n; ++i) { float l, r; synth.renderStereo(l, r); acc += 0.5 * (l * l + r * r); }
    return static_cast<float>(std::sqrt(acc / n));
}

// Voz ATIVA da parte tocando `note` (nota soando, com transpose), ou nullptr.
static Voice* findVoice(Synth& synth, int part, int note) {
    int n = synth.activeVoiceCount(part);
    for (int k = 0; k < n; ++k) {
        Voice& v = synth.debugVoiceOfPart(part, k);
        if (v.note() == note) return &v;
    }
    return nullptr;
}

static void testStolenVoiceNotReleasedByOldNote() {
    Synth synth;
    synth.init(48000.0, 2);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    synth.noteOn(0, 60, 100); renderN(synth, 100);
    synth.noteOn(0, 62, 100); renderN(synth, 100);
    synth.noteOn(0, 64, 100); renderN(synth, 100); // pool de 2: rouba a voz da 60
    synth.noteOff(0, 60);                          // a 60 ja nao soa: nao pode soltar ninguem
    Voice* v64 = findVoice(synth, 0, 64);
    check(v64 && !v64->isReleasing(), "roubo de voz: noteOff da nota roubada NAO solta a nota que roubou o slot");

    // entre partes: parte 1 rouba a voz da parte 0, noteOff da parte 0 nao pode solta-la
    Synth s2;
    s2.init(48000.0, 1);
    for (int p = 0; p < 2; ++p) { s2.part(p).voice = makeSimpleFmPatch(); s2.recompile(p); }
    s2.noteOn(0, 60, 100); renderN(s2, 100);
    s2.noteOn(1, 72, 100); renderN(s2, 100);
    s2.noteOff(0, 60);
    Voice* v72 = findVoice(s2, 1, 72);
    check(v72 && !v72->isReleasing(), "roubo de voz entre partes: noteOff da parte antiga NAO solta a voz da parte nova");
}

static void testSostenutoReleaseFreesHeldNotes() {
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    synth.noteOn(0, 60, 100); renderN(synth, 100);
    synth.sostenutoPedal(0, true);
    synth.noteOff(0, 60); renderN(synth, 4800);
    Voice* v = findVoice(synth, 0, 60);
    check(v && !v->isReleasing(), "sostenuto segura a nota capturada depois do noteOff");
    synth.sostenutoPedal(0, false);
    for (int i = 0; i < 48000 * 3 && synth.activeVoiceCount() > 0; ++i) renderN(synth, 1);
    check(synth.activeVoiceCount() == 0, "soltar o sostenuto libera a nota (antes ela ficava presa)");
}

static void testSustainReleaseKeepsSostenutoNotes() {
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    synth.noteOn(0, 60, 100); renderN(synth, 100);
    synth.sostenutoPedal(0, true);
    synth.sustainPedal(0, true);
    synth.noteOff(0, 60);
    synth.noteOn(0, 64, 100); synth.noteOff(0, 64); // 64 so e segura pelo sustain
    synth.sustainPedal(0, false);
    Voice* v60 = findVoice(synth, 0, 60);
    Voice* v64 = findVoice(synth, 0, 64);
    check(v60 && !v60->isReleasing(), "soltar o sustain NAO solta nota que o sostenuto ainda segura");
    check(v64 && v64->isReleasing(), "soltar o sustain solta a nota que so ele segurava");
}

static void testMonoReleasingNonSoundingNoteDoesNothing() {
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v = makeSimpleFmPatch();
    v.op[0].velocitySens = 7;
    synth.part(0).voice = v;
    synth.part(0).voiceMode = VoiceMode::Mono;
    synth.recompile(0);
    synth.noteOn(0, 60, 20);
    synth.noteOn(0, 64, 20);
    float before = renderRmsN(synth, 4800);
    synth.noteOff(0, 60); // a 60 esta "embaixo", quem soa e a 64
    float after = renderRmsN(synth, 4800);
    std::printf("  (RMS antes=%.4f depois=%.4f)\n", before, after);
    check(nearlyEqualRel(after, before, 0.05f), "mono: soltar a nota de baixo nao reataca a de cima (nem com velocity 100)");

    // velocity original e restaurada no reataque ao voltar pra nota anterior
    Synth s2;
    s2.init(48000.0, 8);
    s2.part(0).voice = v;
    s2.part(0).voiceMode = VoiceMode::Mono;
    s2.recompile(0);
    s2.noteOn(0, 60, 20);
    float quiet = renderRmsN(s2, 4800);
    s2.noteOn(0, 60 + 12, 127); renderN(s2, 4800);
    s2.noteOff(0, 72);    // volta pra 60
    float back = renderRmsN(s2, 4800);
    std::printf("  (RMS nota 60 vel20=%.4f, de volta a 60=%.4f)\n", quiet, back);
    check(nearlyEqualRel(back, quiet, 0.1f), "mono: ao voltar pra nota anterior, usa a velocity original dela (nao 100)");
}

static void testNoteOffOutsideKeyRangeIgnored() {
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v = makeSimpleFmPatch();
    v.op[0].velocitySens = 7;
    synth.part(0).voice = v;
    synth.part(0).voiceMode = VoiceMode::Mono;
    synth.part(0).hiKey = 59;
    synth.recompile(0);
    synth.noteOn(0, 48, 20);
    synth.noteOn(0, 72, 100); // fora do range: ignorada
    float before = renderRmsN(synth, 4800);
    synth.noteOff(0, 72);
    float after = renderRmsN(synth, 4800);
    check(nearlyEqualRel(after, before, 0.05f), "noteOff de nota fora do key range nao reataca a nota mono");
}

static void testCrushHighBitsStaysFinite() {
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v = makeSimpleFmPatch();
    v.op[0].fxType = FxType::Crush;
    v.op[0].fxParam1 = 8000.0f;
    v.op[0].fxParam2 = 16.0f; // maximo do slider: 256 "bits" -> 2^256 estourava pra inf
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.globalFx().reverb.enabled = true;
    synth.globalFx().delay.enabled = true;
    synth.noteOn(0, 60, 100);
    bool finite = true;
    float rms = 0.0f;
    std::vector<float> mono;
    for (int i = 0; i < 9600; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        if (!std::isfinite(l) || !std::isfinite(r)) finite = false;
        mono.push_back(l);
    }
    rms = computeRms(mono, 0, mono.size());
    check(finite && rms > 0.01f, "Crush com FX Param2 no maximo gera audio finito (antes: NaN que matava o master bus)");
}

static void testGlideAlwaysOnDetachedNotes() {
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.part(0).voiceMode = VoiceMode::Mono;
    synth.part(0).glideEnabled = true;
    synth.part(0).glideLegatoOnly = false;
    synth.part(0).glideTimeSec = 0.3f;
    synth.recompile(0);
    synth.noteOn(0, 48, 100); renderN(synth, 4800);
    synth.noteOff(0, 48); renderN(synth, 10);
    synth.noteOn(0, 72, 100); renderN(synth, 2400);
    Voice* v = findVoice(synth, 0, 72);
    float hz = v ? v->debugCurrentHz() : 0.0f;
    std::printf("  (50ms depois da nota destacada: %.1f Hz; 130.8 -> 523.3)\n", hz);
    check(hz > 140.0f && hz < 500.0f, "glide 'sempre' (legatoOnly=false) desliza tambem em nota destacada");

    Synth s2;
    s2.init(48000.0, 8);
    s2.part(0).voice = makeSimpleFmPatch();
    s2.part(0).voiceMode = VoiceMode::Mono;
    s2.part(0).glideEnabled = true;
    s2.part(0).glideLegatoOnly = true;
    s2.part(0).glideTimeSec = 0.3f;
    s2.recompile(0);
    s2.noteOn(0, 48, 100); renderN(s2, 4800);
    s2.noteOff(0, 48); renderN(s2, 10);
    s2.noteOn(0, 72, 100); renderN(s2, 10);
    Voice* v2 = findVoice(s2, 0, 72);
    check(v2 && std::abs(v2->debugCurrentHz() - 523.25f) < 1.0f, "glide 'so em legato': nota destacada comeca direto no alvo");
}

static void testPegStartsAtReleaseLevel() {
    SegmentEG peg;
    uint8_t rate[4] = {99, 99, 99, 99}, level[4] = {50, 50, 50, 50};
    peg.configure(rate, level);
    peg.noteOn(0.0f, 50.0f);
    check(std::abs(peg.process(48000.0) - 50.0f) < 1e-3f, "PEG parte do level[3] (centro), nao de 0");

    // pela voz: PEG centrado com pegDepth=1 tem que soar IDENTICO a pegDepth=0 desde a 1a amostra
    auto firstSamples = [](float pegDepth) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v = makeSimpleFmPatch();
        v.op[0].pegDepth = pegDepth;
        v.op[1].pegDepth = pegDepth;
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);
        std::vector<float> out;
        for (int i = 0; i < 200; ++i) { float l, r; synth.renderStereo(l, r); out.push_back(l); }
        return out;
    };
    auto a = firstSamples(0.0f), b = firstSamples(1.0f);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max(maxDiff, std::abs(a[i] - b[i]));
    check(maxDiff < 1e-4f, "PEG centrado nao faz 'blip' de pitch no inicio da nota");
}

static void testVoiceModeSwitchDoesNotStickNotes() {
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    synth.noteOn(0, 60, 100); renderN(synth, 100);
    synth.part(0).voiceMode = VoiceMode::Mono; // UI troca o modo com a nota segura
    synth.noteOff(0, 60);
    for (int i = 0; i < 48000 * 3 && synth.activeVoiceCount() > 0; ++i) renderN(synth, 1);
    check(synth.activeVoiceCount() == 0, "trocar Poly->Mono com nota segura nao deixa nota presa");

    synth.noteOn(0, 60, 100); synth.noteOn(0, 64, 100);
    synth.part(0).voiceMode = VoiceMode::Poly;
    synth.noteOn(0, 67, 100);                 // 1a nota no modo novo solta as do modo antigo
    synth.noteOff(0, 60); synth.noteOff(0, 64); synth.noteOff(0, 67);
    for (int i = 0; i < 48000 * 3 && synth.activeVoiceCount() > 0; ++i) renderN(synth, 1);
    check(synth.activeVoiceCount() == 0, "trocar Mono->Poly com notas seguras nao deixa nota presa");
}

static void testLevelScalingNeverExceedsMaxLevel() {
    auto peakAt = [](int note, int rightDepth) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].levelScaling.rightDepth = static_cast<int8_t>(rightDepth);
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, note, 127);
        float pk = 0.0f;
        for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); pk = std::max(pk, std::abs(l)); }
        return pk;
    };
    float plain = peakAt(108, 0), boosted = peakAt(108, 12);
    std::printf("  (pico nota 108: sem scaling=%.3f, +12dB/oit=%.3f)\n", plain, boosted);
    check(boosted <= plain * 1.01f, "key level scaling positivo nao passa do nivel maximo (antes: ate +60dB)");
}

static void testFeedbackStaysBoundedAtMax() {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.matrix.mod[0][0] = 1.0f;
    v.op[0].feedback = 1.0f; // diagonal + feedback somados = 2.0: o pior caso
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 60, 100);
    bool finite = true;
    for (int i = 0; i < 48000; ++i) {
        float l, r;
        synth.renderStereo(l, r);
        if (!std::isfinite(l) || std::abs(l) > 1.0001f) finite = false;
    }
    check(finite, "feedback maximo (diagonal + op.feedback) continua finito e dentro de +-1");
}

static void testLfo2DepthsHaveEffect() {
    auto render = [](float lfo2Pitch) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v = makeSimpleFmPatch();
        v.op[0].lfoPitchModSens = 7;
        v.lfo2.rate = 6.0f;
        v.lfo2.pitchDepth = lfo2Pitch;
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);
        std::vector<float> out;
        for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); out.push_back(l); }
        return out;
    };
    auto a = render(0.0f), b = render(1.0f);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max(maxDiff, std::abs(a[i] - b[i]));
    check(maxDiff > 0.05f, "LFO2 pitch depth tem efeito (antes so o LFO1 fazia vibrato)");
}

static void testPolyphonyGainIgnoresSilentPedalVoices() {
    VoiceParams v = makeSimpleFmPatch();
    v.op[0].ampEG.rate[1] = 70;
    v.op[0].ampEG.level[1] = 0; // percussivo: decai a 0 e fica no sustain 0
    v.op[0].ampEG.level[2] = 0;

    Synth ref;
    ref.init(48000.0, 16);
    ref.part(0).voice = v;
    ref.recompile(0);
    ref.noteOn(0, 72, 100);
    float alone = renderRmsN(ref, 2400);

    Synth synth;
    synth.init(48000.0, 16);
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.sustainPedal(0, true);
    for (int n = 0; n < 6; ++n) { synth.noteOn(0, 48 + n, 100); synth.noteOff(0, 48 + n); }
    renderN(synth, 48000); // todas decaem a 0 mas continuam "ativas" pelo pedal
    synth.noteOn(0, 72, 100);
    float withPedal = renderRmsN(synth, 2400);
    std::printf("  (RMS nota nova: sozinha=%.4f, com 6 vozes mudas no pedal=%.4f)\n", alone, withPedal);
    check(nearlyEqualRel(withPedal, alone, 0.1f), "vozes mudas presas no pedal nao abaixam o volume da nota nova");
}

static void testNonZeroReleaseLevelKeepsSounding() {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v = makeSimpleFmPatch();
    v.op[0].ampEG.level[3] = 50; // release termina em 50, nao em 0 (como no DX7: continua soando)
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 60, 100); renderN(synth, 4800);
    synth.noteOff(0, 60); renderN(synth, 48000);
    float rms = renderRmsN(synth, 4800);
    check(synth.activeVoiceCount() == 1 && rms > 0.001f,
          "release com level[3]>0 segue soando (antes a voz era cortada seco, com clique)");
}

static void testJsonLoadClampsAndRejectsGarbage() {
    std::filesystem::create_directories("output");
    {
        std::ofstream f("output/garbage.json");
        f << "isto nao e json";
    }
    PartParams p;
    p.name = "intacto";
    check(!loadPartFile("output/garbage.json", p) && p.name == "intacto",
          "loadPartFile rejeita arquivo que nao e JSON (antes carregava o Init em silencio)");

    JsonValue j = JsonValue::parse(
        R"({"voice":{"op":[{"outputLevel":300,"fxParam2":1e9,"coarseRatio":-5,"ampEG":{"rate":[500,-3,99,99]}}]},)"
        R"("unisonCount":1000,"transpose":-128,"level":1e40})");
    PartParams q = partParamsFromJson(j);
    check(q.voice.op[0].outputLevel == 99 && q.voice.op[0].fxParam2 == 16.0f && q.voice.op[0].coarseRatio > 0.0f &&
          q.voice.op[0].ampEG.rate[0] == 99 && q.voice.op[0].ampEG.rate[1] == 0,
          "JSON com valores fora da faixa e saturado nos limites (sem UB no cast)");
    check(q.unisonCount == 8 && q.transpose == -48 && q.level == 2.0f, "campos da parte tambem sao saturados no load");
}

static void testMidiNoteOffReachesPartAfterDisable() {
    using namespace dx7fm_app;
    Synth synth;
    synth.init(48000.0, 8);
    synth.part(0).voice = makeSimpleFmPatch();
    synth.recompile(0);
    std::atomic<bool> enabled[kNumParts];
    std::atomic<int> channel[kNumParts];
    for (int p = 0; p < kNumParts; ++p) { enabled[p] = (p == 0); channel[p] = 1; }
    NoteRouting routing;

    AppEvent on;
    on.kind = AppEvent::Kind::NoteOn; on.channel1to16 = 1; on.note = 60; on.velocity = 100;
    applyAppEvent(synth, on, enabled, channel, routing);
    renderN(synth, 100);

    enabled[0] = false; // usuario desabilita a parte com a nota segura
    channel[0] = 5;     // ...e muda o canal
    AppEvent off = on;
    off.kind = AppEvent::Kind::NoteOff;
    applyAppEvent(synth, off, enabled, channel, routing);
    for (int i = 0; i < 48000 * 3 && synth.activeVoiceCount() > 0; ++i) renderN(synth, 1);
    check(synth.activeVoiceCount() == 0, "noteOff MIDI chega na parte que recebeu o noteOn mesmo apos desabilitar/mudar canal");
}

static void testRecompileCacheFollowsMatrixChanges() {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    applyAlgorithm(v, 1); // cadeia 0<-1<-...<-7
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.part(0).voice.op[0].outputLevel = 90; // muda so o timbre: cache reaproveitado
    synth.recompile(0);
    applyAlgorithm(synth.part(0).voice, 22);      // 8 portadoras em paralelo: topologia nova
    synth.recompile(0);
    // o compilado publicado tem que bater com o que compileAlgorithm daria do zero
    CompiledAlgorithm fresh = compileAlgorithm(synth.part(0).voice.matrix);
    synth.noteOn(0, 60, 100);
    renderN(synth, 10);
    bool same = true;
    VoiceParams probe = synth.part(0).voice;
    CompiledAlgorithm again = makeSnapshot(probe).compiled;
    for (int i = 0; i < kNumOps; ++i)
        for (int j = 0; j < kNumOps; ++j) same &= fresh.feedbackEdge[i][j] == again.feedbackEdge[i][j];
    std::vector<float> a;
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); a.push_back(l); }
    check(same && computeRms(a, 0, a.size()) > 0.01f, "recompile com cache de algoritmo publica a topologia nova");
}

static void testMasterVolume() {
    // portadora a outputLevel 80 (pico ~0.2): bem abaixo do limiar do
    // limitador, entao o volume geral tem que escalar a saida EXATAMENTE
    auto render = [](float volume, bool changeMidway) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].outputLevel = 80;
        synth.part(0).voice = v;
        synth.recompile(0);
        if (!changeMidway) synth.setMasterVolume(volume);
        synth.noteOn(0, 60, 100);
        std::vector<float> out;
        for (int i = 0; i < 48000; ++i) { // 1s: a rampa (~20ms, exponencial) termina muito antes
            if (changeMidway && i == 4800) synth.setMasterVolume(volume);
            float l, r;
            synth.renderStereo(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto full = render(1.0f, false), half = render(0.5f, false);
    float rFull = computeRms(full, 4800, 4800), rHalf = computeRms(half, 4800, 4800);
    std::printf("  (RMS volume 1.0=%.5f, 0.5=%.5f)\n", rFull, rHalf);
    check(std::abs(rHalf / rFull - 0.5f) < 0.01f, "volume geral 0.5 reduz a saida pela metade");

    auto muted = render(0.0f, false);
    float peakMuted = 0.0f;
    for (size_t i = 24000; i < muted.size(); ++i) peakMuted = std::max(peakMuted, std::abs(muted[i]));
    check(peakMuted == 0.0f, "volume geral 0 e silencio exato (depois da rampa)");

    auto change = render(0.0f, true); // baixa de 1.0 pra 0 no meio de uma nota
    float maxJump = 0.0f;
    for (size_t i = 4801; i < 4900; ++i) maxJump = std::max(maxJump, std::abs(change[i] - change[i - 1]));
    float tailPeak = 0.0f;
    for (size_t i = 24000; i < change.size(); ++i) tailPeak = std::max(tailPeak, std::abs(change[i]));
    check(maxJump < 0.05f && tailPeak < 1e-4f, "mudanca de volume geral e suavizada (sem degrau) e chega ao alvo");

    Synth s;
    s.setMasterVolume(99.0f);
    bool clamped = s.masterVolume() == 2.0f;
    s.setMasterVolume(std::nanf(""));
    check(clamped && s.masterVolume() == 2.0f, "setMasterVolume satura em 0..2 e ignora NaN");
}

// --- [ADICIONADO em 2026-09-23] wavefolder (FxType::Fold) ---------------

static void testTriangleFoldShape() {
    bool identity = true;
    for (float v = -1.0f; v <= 1.0f; v += 0.125f) identity &= std::abs(triangleFold(v) - v) < 1e-6f;
    check(identity, "triangleFold e identidade exata em [-1, 1]");
    check(std::abs(triangleFold(1.5f) - 0.5f) < 1e-6f && std::abs(triangleFold(3.0f) + 1.0f) < 1e-6f &&
          std::abs(triangleFold(-1.5f) + 0.5f) < 1e-6f,
          "triangleFold rebate nas paredes: 1.5 -> 0.5, 3 -> -1, -1.5 -> -0.5");
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> d(-100.0f, 100.0f);
    bool bounded = true;
    for (int i = 0; i < 100000; ++i) { float y = triangleFold(d(rng)); bounded &= (y >= -1.0f && y <= 1.0f); }
    check(bounded, "triangleFold fica em [-1, 1] pra qualquer entrada (100k valores aleatorios em +-100)");
}

// Portadora senoidal isolada (op0) em 440Hz, nivel 80 (pico ~0.2, abaixo do
// limitador, que senao adicionaria harmonicos proprios e sujaria a medicao).
static std::vector<float> renderFoldedSine(FxType fx, float drive, float bias, int samples,
                                           const uint8_t* fxEgLevels = nullptr, const uint8_t* fxEgRates = nullptr) {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 80;
    v.op[0].fxType = fx;
    v.op[0].fxParam1 = drive;
    v.op[0].fxParam2 = bias;
    if (fxEgLevels) for (int k = 0; k < 4; ++k) { v.op[0].fxEG.level[k] = fxEgLevels[k]; v.op[0].fxEG.rate[k] = fxEgRates[k]; }
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 69, 100);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) { float l, r; synth.renderStereo(l, r); out.push_back(l); }
    return out;
}

static float harmonicRatio(const std::vector<float>& x, int harmonic) {
    return goertzelMagnitude(x, 440.0 * harmonic, 48000.0) / goertzelMagnitude(x, 440.0, 48000.0);
}

// "Brilho": centroide harmonico (media dos numeros de harmonico 1..40
// ponderada pela potencia). Razoes isoladas tipo h3/h1 NAO servem pra
// wavefolder: nao sao monotonicas no drive (com drive ~2.4 a fundamental
// quase some e h3/h1 passa de 9; com drive 4 cai pra 0.1). O centroide sobe
// de forma monotonica com o drive (~drive+1).
static float harmonicCentroid(const std::vector<float>& x) {
    double sum = 0.0, weighted = 0.0;
    for (int k = 1; k <= 40; ++k) {
        double m = goertzelMagnitude(x, 440.0 * k, 48000.0);
        sum += m * m;
        weighted += k * m * m;
    }
    return static_cast<float>(weighted / sum);
}

static void testFoldDriveOneIsTransparent() {
    auto bypass = renderFoldedSine(FxType::Bypass, 1.0f, 0.0f, 4800);
    auto fold = renderFoldedSine(FxType::Fold, 1.0f, 0.0f, 4800);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < bypass.size(); ++i) maxDiff = std::max(maxDiff, std::abs(bypass[i] - fold[i]));
    check(maxDiff == 0.0f, "Fold com drive 1 e simetria 0 e IDENTICO ao Bypass (nada dobra)");
}

static void testFoldAddsOddHarmonics() {
    auto bypass = renderFoldedSine(FxType::Bypass, 1.0f, 0.0f, 48000);
    auto fold = renderFoldedSine(FxType::Fold, 4.0f, 0.0f, 48000);
    std::vector<float> b(bypass.begin() + 24000, bypass.end()), f(fold.begin() + 24000, fold.end());
    float cBypass = harmonicCentroid(b), cFold = harmonicCentroid(f), h2Fold = harmonicRatio(f, 2);
    std::printf("  (centroide harmonico: bypass=%.2f, fold drive 4=%.2f; 2o harmonico no fold=%.4f)\n",
                cBypass, cFold, h2Fold);
    check(cBypass < 1.05f && cFold > 3.0f, "Fold com drive 4 desloca a energia pros harmonicos (seno puro fica so na fundamental)");
    check(h2Fold < 0.01f, "Fold simetrico (bias 0) NAO gera harmonicos pares");
    float peak = 0.0f;
    for (float x : f) peak = std::max(peak, std::abs(x));
    float peakBypass = 0.0f;
    for (float x : b) peakBypass = std::max(peakBypass, std::abs(x));
    check(peak <= peakBypass * 1.001f, "Fold nunca aumenta o pico alem do nivel do operador");
}

static void testFoldBiasAddsEvenHarmonicsWithoutDc() {
    auto fold = renderFoldedSine(FxType::Fold, 4.0f, 0.5f, 48000);
    std::vector<float> f(fold.begin() + 24000, fold.end()); // depois do DC blocker assentar
    double mean = 0.0;
    for (float x : f) mean += x;
    mean /= static_cast<double>(f.size());
    float h2 = harmonicRatio(f, 2);
    std::printf("  (simetria 0.5: 2o harmonico/fundamental=%.4f, DC medio=%.6f)\n", h2, mean);
    check(h2 > 0.05f, "Fold com simetria > 0 gera harmonicos pares");
    check(std::abs(mean) < 0.002, "o DC introduzido pela simetria e removido (media ~0)");
}

static void testFoldDriveFollowsFxEnvelope() {
    // fxEG: ataque instantaneo ao maximo (drive 8), depois decai devagar
    // (~0.6s) ate 0 (drive 1 + 7*20% = 2.4)
    const uint8_t levels[4] = {99, 0, 0, 0};
    const uint8_t rates[4] = {99, 40, 99, 99};
    auto x = renderFoldedSine(FxType::Fold, 8.0f, 0.0f, 48000 * 3, levels, rates);
    // janelas de 4800 amostras (440 ciclos inteiros): logo apos o ataque vs no fim
    std::vector<float> early(x.begin() + 480, x.begin() + 5280), late(x.end() - 4800, x.end());
    float cEarly = harmonicCentroid(early), cLate = harmonicCentroid(late);
    std::printf("  (centroide harmonico: logo apos ataque=%.2f, apos o fxEG cair=%.2f)\n", cEarly, cLate);
    check(cEarly > cLate * 1.5f, "fxEG modula o drive do Fold (mais brilho no ataque, menos depois)");
}

static void testFoldJsonRoundTrip() {
    VoiceParams v;
    v.op[2].fxType = FxType::Fold;
    v.op[2].fxParam1 = 5.5f;
    v.op[2].fxParam2 = 0.25f;
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(v).dump()));
    check(back.op[2].fxType == FxType::Fold && back.op[2].fxParam1 == 5.5f && back.op[2].fxParam2 == 0.25f,
          "Fold sobrevive ao round-trip de JSON (nome \"Fold\", drive e simetria)");
}

// --- [ADICIONADO em 2026-09-23] navegador de patches (app/patch_browser.h) ---

static void testSafeFileStem() {
    using dx7fm_app::safeFileStem;
    check(safeFileStem("Piano Eletrico") == "Piano Eletrico", "safeFileStem mantem nome comum (com espaco)");
    check(safeFileStem("Órgão Ácido") == "Órgão Ácido", "safeFileStem mantem acentos (UTF-8)");
    check(safeFileStem("../../etc/passwd") == "_.._etc_passwd", "safeFileStem nao deixa escapar da pasta (../ e /)");
    check(safeFileStem("a:b*c?d\"e<f>g|h\\i") == "a_b_c_d_e_f_g_h_i", "safeFileStem troca caracteres invalidos por _");
    check(safeFileStem("   ") == "sem_nome" && safeFileStem("") == "sem_nome" && safeFileStem("..") == "sem_nome",
          "safeFileStem de nome vazio/so espacos/.. vira sem_nome");
}

static void testListPatchesSortedJsonOnly() {
    namespace fs = std::filesystem;
    fs::path dir = "output/patch_list_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "subpasta.json");
    for (const char* n : {"beta.json", "Alfa.json", "gama.json", "leia-me.txt"}) std::ofstream(dir / n) << "{}";
    auto list = dx7fm_app::listPatches(dir);
    bool ok = list.size() == 3 && list[0].stem == "Alfa" && list[1].stem == "beta" && list[2].stem == "gama";
    check(ok, "listPatches lista so arquivos .json, em ordem alfabetica sem diferenciar maiusculas");
    check(dx7fm_app::listPatches("output/pasta_que_nao_existe").empty(), "listPatches de pasta inexistente e lista vazia (nao erro)");
    fs::remove_all(dir);
}

static void testLoadPatchKeepsPartRouting() {
    PartParams partB;
    partB.enabled = false;
    partB.midiChannel = 3;
    partB.loKey = 60;
    partB.level = 0.4f;
    partB.pan = -0.5f;

    PartParams saved; // patch salvo de OUTRA parte, com roteamento diferente
    saved.name = "Baixo";
    saved.enabled = true;
    saved.midiChannel = 1;
    saved.loKey = 0;
    saved.voiceMode = VoiceMode::Mono;
    saved.transpose = -12;
    saved.voice.op[0].waveform = Waveform::Saw;

    dx7fm_app::copyPatchKeepingRouting(partB, saved);
    check(partB.name == "Baixo" && partB.voiceMode == VoiceMode::Mono && partB.transpose == -12 &&
          partB.voice.op[0].waveform == Waveform::Saw,
          "carregar patch traz o SOM (timbre, nome, modo de voz, transpose)");
    check(!partB.enabled && partB.midiChannel == 3 && partB.loKey == 60 && partB.level == 0.4f && partB.pan == -0.5f,
          "carregar patch NAO muda o roteamento/mix da parte (habilitada, canal, key range, nivel, pan)");
}

static void testPatchFingerprintIgnoresRouting() {
    PartParams a, b;
    b.enabled = false;
    b.midiChannel = 7;
    b.pan = 1.0f;
    check(dx7fm_app::patchFingerprint(a) == dx7fm_app::patchFingerprint(b),
          "fingerprint do patch ignora roteamento (mudar canal/pan nao conta como alteracao do som)");
    b.voice.op[3].outputLevel = 42;
    check(dx7fm_app::patchFingerprint(a) != dx7fm_app::patchFingerprint(b), "fingerprint muda quando o som muda");
    b.voice.op[3].outputLevel = a.voice.op[3].outputLevel;
    check(dx7fm_app::patchFingerprint(a) == dx7fm_app::patchFingerprint(b), "desfazer a edicao volta ao mesmo fingerprint");
}

// --- [ADICIONADO em 2026-10-18] forma de onda Additive (osciladores livres; 64 desde 2026-10-20) ---

// Portadora isolada (op0) em 440Hz (nota 69), nivel 80 (pico ~0.2, abaixo do
// limitador). `setup` ajusta o patch antes de publicar.
template <typename Setup>
static std::vector<float> renderAdditive(Setup setup, int samples = 48000, float modWheel = -1.0f) {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 80;
    v.op[0].waveform = Waveform::Additive;
    setup(v);
    synth.part(0).voice = v;
    synth.recompile(0);
    if (modWheel >= 0.0f) synth.modWheel(0, modWheel);
    synth.noteOn(0, 69, 100);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) { float l, r; synth.renderStereo(l, r); out.push_back(l); }
    return out;
}

static std::vector<float> lastHalf(const std::vector<float>& x) {
    return std::vector<float>(x.begin() + static_cast<long>(x.size() / 2), x.end());
}

static float mag(const std::vector<float>& x, double hz) { return goertzelMagnitude(x, hz, 48000.0); }

static float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0.0f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

static void testAdditiveSinglePartialMatchesSine() {
    auto add = renderAdditive([](VoiceParams&) {}, 9600);
    auto sine = renderAdditive([](VoiceParams& v) { v.op[0].waveform = Waveform::Sine; }, 9600);
    float d = maxAbsDiff(add, sine);
    std::printf("  (diferenca maxima Additive(so parcial 1) x Sine: %.2e)\n", d);
    check(d < 1e-3f, "Additive com so o parcial 1 (razao 1) soa igual ao operador Sine");

    // com PM: o operador 1 modulando o 0 (patch default) -- mesma resposta
    auto pmAdd = renderAdditive([](VoiceParams& v) { v.matrix.mod[0][1] = 1.0f; v.op[1].outputLevel = 70; }, 9600);
    auto pmSine = renderAdditive([](VoiceParams& v) {
        v.matrix.mod[0][1] = 1.0f; v.op[1].outputLevel = 70; v.op[0].waveform = Waveform::Sine; }, 9600);
    float dpm = maxAbsDiff(pmAdd, pmSine);
    std::printf("  (com modulacao de fase: diferenca maxima %.2e)\n", dpm);
    check(dpm < 2e-3f, "Additive recebe a modulacao de fase (FM) igual a uma onda comum");
}

static void testAdditiveFreeRatios() {
    auto x = lastHalf(renderAdditive([](VoiceParams& v) {
        auto& a = v.op[0].additiveA;
        a.amp[0] = 1.0f; a.ratio[0] = 1.0f;
        a.amp[1] = 0.5f; a.ratio[1] = 2.75f; // inarmonico: 1210Hz
    }));
    float m1 = mag(x, 440.0), mInh = mag(x, 1210.0), mH2 = mag(x, 880.0);
    std::printf("  (magnitudes: 440Hz=%.4f, 1210Hz=%.4f, 880Hz=%.5f)\n", m1, mInh, mH2);
    check(std::abs(mInh / m1 - 0.5f) < 0.025f, "parcial com razao livre 2.75 soa em 1210Hz com a amplitude pedida (0.5)");
    check(mH2 < m1 * 0.01f, "sem energia no harmonico 2 (so os parciais definidos soam)");
}

static void testAdditiveSkipsPartialsAboveNyquist() {
    auto render = [](float hz) {
        return renderAdditive([hz](VoiceParams& v) {
            v.op[0].fixedFreq = true;
            v.op[0].fixedHz = hz;
            auto& a = v.op[0].additiveA;
            a.amp[0] = 0.0f;
            a.amp[1] = 1.0f; a.ratio[1] = 20.0f; // so um parcial, em 20x
        }, 9600);
    };
    auto high = render(1500.0f), low = render(300.0f);
    float peakHigh = 0.0f;
    for (float s : high) peakHigh = std::max(peakHigh, std::abs(s));
    std::printf("  (parcial 20x: em 1500Hz (30kHz) pico=%.2e; em 300Hz (6kHz) RMS=%.4f)\n", peakHigh, computeRms(low, 0, low.size()));
    check(peakHigh == 0.0f, "parcial acima de Nyquist (20 x 1500Hz = 30kHz) e desligado, sem alias");
    check(computeRms(low, 0, low.size()) > 0.01f, "o mesmo parcial abaixo de Nyquist (20 x 300Hz = 6kHz) soa normal");
}

static void testAdditiveMorph() {
    auto withDepth = [](float depth) {
        return lastHalf(renderAdditive([depth](VoiceParams& v) {
            auto& o = v.op[0];
            o.additiveB.amp[0] = 0.0f;
            o.additiveB.amp[1] = 1.0f; // B = so o parcial 2 (880Hz)
            o.morphDepth = depth;
        }));
    };
    auto a = withDepth(0.0f), b = withDepth(1.0f), mid = withDepth(0.5f);
    check(mag(a, 880.0) < mag(a, 440.0) * 0.01f, "morph 0: so o espectro A (440Hz)");
    check(mag(b, 440.0) < mag(b, 880.0) * 0.01f, "morph 1 (FX EG no maximo): so o espectro B (880Hz)");
    float r = mag(mid, 880.0) / mag(mid, 440.0);
    check(r > 0.8f && r < 1.25f, "morph 0.5: metade de cada espectro");

    // o morph tambem interpola a RAZAO: parcial 1 indo de 1x (440) a 1.5x (660)
    auto glide = lastHalf(renderAdditive([](VoiceParams& v) {
        v.op[0].additiveB.ratio[0] = 1.5f;
        v.op[0].morphDepth = 1.0f;
    }));
    check(mag(glide, 660.0) > 0.05f && mag(glide, 440.0) < mag(glide, 660.0) * 0.01f,
          "morph interpola a razao de frequencia (parcial 1 vai de 440Hz pra 660Hz)");
}

static void testAdditiveMorphFollowsFxEgAndModMatrix() {
    auto x = renderAdditive([](VoiceParams& v) {
        auto& o = v.op[0];
        o.additiveB.amp[0] = 0.0f;
        o.additiveB.amp[1] = 1.0f;
        o.morphDepth = 1.0f;
        const uint8_t r[4] = {99, 40, 99, 99}, l[4] = {99, 0, 0, 0}; // ataque em B, decai pra A em ~0.6s
        for (int k = 0; k < 4; ++k) { o.fxEG.rate[k] = r[k]; o.fxEG.level[k] = l[k]; }
    }, 48000 * 2);
    std::vector<float> early(x.begin() + 480, x.begin() + 5280), late(x.end() - 4800, x.end());
    std::printf("  (880/440: logo apos o ataque=%.2f, no fim=%.3f)\n", mag(early, 880.0) / mag(early, 440.0),
                mag(late, 880.0) / mag(late, 440.0));
    check(mag(early, 880.0) > mag(early, 440.0) && mag(late, 440.0) > mag(late, 880.0) * 20.0f,
          "FX EG move o morph: comeca em B (880Hz) e termina em A (440Hz)");

    auto wheel = lastHalf(renderAdditive([](VoiceParams& v) {
        auto& o = v.op[0];
        o.additiveB.amp[0] = 0.0f;
        o.additiveB.amp[1] = 1.0f;
        o.morphDepth = 0.0f; // so a mod matrix move o morph
        v.modMatrix[0] = ModRoute{ModSource::ModWheel, ModDest::OperatorMorph, 0, 1.0f};
    }, 48000, 1.0f));
    check(mag(wheel, 440.0) < mag(wheel, 880.0) * 0.01f, "mod wheel -> OperatorMorph leva o espectro ate B");
}

// [2026-10-20] Renderizacao em blocos + threads (Synth::renderBlock,
// dx7_threads.h): a saida tem que ser IDENTICA bit a bit a renderizar
// amostra por amostra, pra qualquer tamanho de bloco e numero de threads.
// Cena: 3 partes (uma aditiva), notas entrando e saindo entre blocos, vozes
// morrendo no meio de um pedaco (release curto), chorus + delay ligados.
static std::vector<float> renderScene(int blockSize, int threads) {
    Synth synth;
    synth.init(48000.0, 24);
    synth.setWorkerThreads(threads);
    for (int p = 0; p < 3; ++p) {
        VoiceParams v;
        applyAlgorithm(v, 1 + p * 7);
        for (auto& op : v.op) {
            op.outputLevel = 85;
            op.ampEG.rate[3] = p == 1 ? 99 : 70; // parte 1: release quase instantaneo
        }
        if (p == 2) {
            v.op[0].waveform = Waveform::Additive;
            for (int k = 0; k < kNumPartials; ++k) v.op[0].additiveA.amp[k] = 1.0f / (k + 1);
        }
        synth.part(p).voice = v;
        synth.part(p).enabled = true;
        synth.part(p).pan = -0.5f + 0.5f * p;
        synth.recompile(p);
    }
    synth.globalFx().chorus.enabled = true;
    synth.globalFx().delay.enabled = true;

    // eventos em amostras "tortas" (nao alinhadas com nenhum bloco)
    struct Ev { int at, part, note; bool on; };
    std::vector<Ev> evs;
    for (int k = 0; k < 30; ++k) {
        int at = 97 + k * 1531, part = k % 3, note = 40 + (k * 7) % 36;
        evs.push_back({at, part, note, true});
        evs.push_back({at + 2311 + (k % 5) * 401, part, note, false});
    }
    std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });

    const int total = 60000;
    std::vector<float> out(2 * static_cast<size_t>(total));
    std::vector<float> l(static_cast<size_t>(total)), r(static_cast<size_t>(total));
    size_t e = 0;
    for (int pos = 0; pos < total;) {
        while (e < evs.size() && evs[e].at <= pos) {
            if (evs[e].on) synth.noteOn(evs[e].part, evs[e].note, 100);
            else synth.noteOff(evs[e].part, evs[e].note);
            ++e;
        }
        int end = std::min(total, pos + blockSize);
        if (e < evs.size()) end = std::min(end, evs[e].at);
        if (blockSize == 1) synth.renderStereo(l[static_cast<size_t>(pos)], r[static_cast<size_t>(pos)]);
        else synth.renderBlock(&l[static_cast<size_t>(pos)], &r[static_cast<size_t>(pos)], end - pos);
        pos = blockSize == 1 ? pos + 1 : end;
    }
    for (int i = 0; i < total; ++i) { out[2 * static_cast<size_t>(i)] = l[static_cast<size_t>(i)]; out[2 * static_cast<size_t>(i) + 1] = r[static_cast<size_t>(i)]; }
    return out;
}

static void testRenderBlockMatchesPerSample() {
    auto ref = renderScene(1, 0);
    double energy = 0.0;
    for (float x : ref) energy += static_cast<double>(x) * x;
    check(energy > 1.0, "cena de teste do renderBlock produz audio");
    bool same = true;
    for (int bs : {7, 64, 256, 1000}) same &= renderScene(bs, 0) == ref;
    check(same, "renderBlock (blocos de 7, 64, 256 e 1000) e bit a bit igual a renderStereo amostra por amostra");
}

static void testRenderBlockThreadsAreDeterministic() {
    auto ref = renderScene(256, 0);
    bool same = true;
    for (int t : {1, 3, 7}) same &= renderScene(256, t) == ref;
    check(same, "renderBlock com 1, 3 e 7 threads extras e bit a bit igual ao serial");
    // estresse: muitos blocos curtos + longos com threads (pega corrida entre blocos)
    bool stress = true;
    for (int rep = 0; rep < 3; ++rep) stress &= renderScene(37, 5) == renderScene(37, 0);
    check(stress, "renderBlock com threads e blocos curtos repetido: sempre igual ao serial");
}

static void testWorkerPoolRunsEveryIndexOnce() {
    struct Ctx { std::atomic<int> hits[200]; } ctx{};
    WorkerPool pool;
    pool.start(4, [](void* c, int i) { static_cast<Ctx*>(c)->hits[i].fetch_add(1, std::memory_order_relaxed); }, &ctx);
    for (int round = 0; round < 2000; ++round) pool.run(1 + round % 200);
    pool.stop();
    long expected[200] = {};
    for (int round = 0; round < 2000; ++round) for (int i = 0; i < 1 + round % 200; ++i) ++expected[i];
    bool ok = true;
    for (int i = 0; i < 200; ++i) ok &= ctx.hits[i].load() == expected[i];
    check(ok, "WorkerPool: 2000 rodadas, cada indice executado exatamente uma vez por rodada");
}

// [2026-10-20] Editor de 64 parciais (app/additive_presets.h): eixos,
// pintura com o mouse e presets.
static void testAdditiveEditorAxes() {
    using namespace dx7fm_app;
    bool ok = true;
    for (float r : {0.25f, 0.5f, 1.0f, 2.756f, 17.0f, 128.0f}) ok &= std::fabs(axisToRatio(ratioToAxis(r), false) - r) < 1e-3f * r;
    check(ok, "editor aditivo: eixo de razao (log) ida e volta preserva a razao");
    check(axisToRatio(ratioToAxis(6.7f), true) == 7.0f && axisToRatio(ratioToAxis(0.34f), true) == 1.0f / 3.0f &&
              axisToRatio(1.0f, true) == kMaxPartialRatio,
          "editor aditivo: Shift encaixa em harmonicos inteiros e subharmonicos 1/n");
    ok = ampToAxis(0.0f, true) == 0.0f && axisToAmp(0.0f, true) == 0.0f && ampToAxis(1.0f, true) == 1.0f;
    for (float a : {0.002f, 0.01f, 0.5f}) ok &= std::fabs(axisToAmp(ampToAxis(a, true), true) - a) < 1e-4f;
    ok &= ampToAxis(0.0005f, true) == 0.0f; // abaixo de -60 dB: no pe do grafico
    ok &= std::fabs(ampToAxis(0.1f, true) - (1.0f - 20.0f / 60.0f)) < 1e-5f; // -20 dB
    check(ok, "editor aditivo: eixo em dB (-60..0) ida e volta, e o pe do grafico e amplitude 0");
}

static void testAdditivePaintStroke() {
    using namespace dx7fm_app;
    float v[kNumPartials] = {};
    paintStroke(10, 0.0f, 20, 1.0f, [&](int k, float t) { v[k] = t; });
    bool ok = v[9] == 0.0f && v[10] == 0.0f && v[20] == 1.0f && v[21] == 0.0f && std::fabs(v[15] - 0.5f) < 1e-6f;
    float w[kNumPartials] = {};
    paintStroke(20, 1.0f, 10, 0.0f, [&](int k, float t) { w[k] = t; }); // da direita pra esquerda
    for (int k = 0; k < kNumPartials; ++k) ok &= std::fabs(v[k] - w[k]) < 1e-6f;
    int calls = 0;
    paintStroke(-5, 0.3f, 200, 0.3f, [&](int, float) { ++calls; }); // fora do grafico: so as 64 barras
    check(ok && calls == kNumPartials, "editor aditivo: arrastar rapido pinta todas as barras do caminho, interpolando");
}

static void testAdditivePresetsAreValid() {
    using namespace dx7fm_app;
    bool ok = true;
    for (const auto& p : kAdditivePresets) {
        OperatorParams op;
        p.make(op.additiveA);
        OperatorParams san = op;
        sanitize(san);
        ok &= std::memcmp(&san.additiveA, &op.additiveA, sizeof op.additiveA) == 0; // nada fora da faixa
        ok &= activePartials(op.additiveA) >= 1;
        if (!ok) { std::printf("  preset invalido: %s\n", p.name); break; }
    }
    check(ok, "presets da aditiva: todos dentro das faixas do sanitize e com pelo menos 1 parcial");

    PartialSet s;
    presets::odd(s, 1.0f);
    check(s.ratio[kNumPartials - 1] == 127.0f && std::fabs(s.amp[1] - 1.0f / 3.0f) < 1e-6f,
          "preset Quadrada: 64 harmonicos impares (ate 127) com amplitude 1/k");
    auto bar = presets::freeBarRatios();
    auto mem = presets::membraneRatios();
    check(bar.size() == 16 && std::fabs(bar[1] - 2.7565f) < 2e-3f && std::fabs(bar[2] - 5.4039f) < 2e-3f &&
              std::fabs(bar[3] - 8.9330f) < 2e-3f && bar.back() <= kMaxPartialRatio,
          "preset Sino: modos da barra livre 1, 2.757, 5.404, 8.933 (16 cabem ate 128x)");
    check(mem.size() == static_cast<size_t>(kNumPartials) && std::fabs(mem[1] - 1.5933f) < 2e-3f &&
              std::fabs(mem[2] - 2.1355f) < 2e-3f && std::fabs(mem[3] - 2.2954f) < 2e-3f && std::is_sorted(mem.begin(), mem.end()),
          "preset Tambor: 64 modos da membrana circular em ordem (1, 1.593, 2.136, 2.295 ..)");
}

static void testAdditiveSquarePresetSounds() {
    auto x = lastHalf(renderAdditive([](VoiceParams& v) { dx7fm_app::presets::odd(v.op[0].additiveA, 1.0f); }));
    float h1 = mag(x, 440.0), h2 = mag(x, 880.0), h3 = mag(x, 1320.0), h5 = mag(x, 2200.0);
    std::printf("  (Quadrada em 440Hz: h1=%.4f h2=%.5f h3=%.4f h5=%.4f)\n", h1, h2, h3, h5);
    check(h2 < h1 * 0.01f && std::fabs(h3 / h1 - 1.0f / 3.0f) < 0.03f && std::fabs(h5 / h1 - 0.2f) < 0.03f,
          "preset Quadrada soa so nos harmonicos impares, com amplitude 1/k");
}

// --- [ADICIONADO em 2026-10-20] ressintese (wav_reader.h + dx7_resynth.h) ---

// WAV de teste em qualquer formato: code 1 = PCM, 3 = float; extensible
// embrulha no WAVE_FORMAT_EXTENSIBLE; junk = um chunk LIST de tamanho impar
// antes do data (o leitor tem que pular, com o byte de preenchimento).
static void writeTestWav(const std::string& path, int code, int bits, int channels, const std::vector<double>& interleaved,
                         bool extensible = false, bool junk = false) {
    std::vector<uint8_t> d;
    auto put = [&](const void* p, size_t n) { d.insert(d.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    auto u16 = [&](uint16_t v) { put(&v, 2); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    const int bytesPer = bits / 8;
    std::vector<uint8_t> data;
    for (double v : interleaved) {
        uint8_t b[8] = {};
        if (code == 3 && bits == 32) { float f = static_cast<float>(v); std::memcpy(b, &f, 4); }
        else if (code == 3) std::memcpy(b, &v, 8);
        else if (bits == 8) b[0] = static_cast<uint8_t>(std::lround(v * 127.0 + 128.0));
        else {
            int64_t q = std::llround(v * (std::pow(2.0, bits - 1) - 1.0));
            std::memcpy(b, &q, static_cast<size_t>(bytesPer)); // little-endian: os bytes baixos
        }
        data.insert(data.end(), b, b + bytesPer);
    }
    put("RIFF", 4); u32(0); put("WAVE", 4);
    put("fmt ", 4); u32(extensible ? 40 : 16);
    u16(static_cast<uint16_t>(extensible ? 0xFFFE : code)); u16(static_cast<uint16_t>(channels)); u32(44100);
    u32(static_cast<uint32_t>(44100 * channels * bytesPer)); u16(static_cast<uint16_t>(channels * bytesPer)); u16(static_cast<uint16_t>(bits));
    if (extensible) {
        u16(22); u16(static_cast<uint16_t>(bits)); u32(0);
        u16(static_cast<uint16_t>(code)); put("\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
    }
    if (junk) { put("LIST", 4); u32(3); put("abc", 3); d.push_back(0); }
    put("data", 4); u32(static_cast<uint32_t>(data.size())); put(data.data(), data.size());
    uint32_t riff = static_cast<uint32_t>(d.size() - 8);
    std::memcpy(&d[4], &riff, 4);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

static void testWavReaderFormats() {
    const std::vector<double> stereo = {0.5, -0.5, 0.25, 0.75, -1.0, -1.0, 0.0, 0.5};
    const float expected[4] = {0.0f, 0.5f, -1.0f, 0.25f}; // media L/R
    struct Fmt { int code, bits; bool ext, junk; float tol; const char* name; };
    const Fmt fmts[] = {{1, 8, false, false, 0.01f, "PCM 8"}, {1, 16, false, true, 1e-4f, "PCM 16"}, {1, 24, false, false, 1e-6f, "PCM 24"},
                        {1, 32, true, false, 1e-6f, "PCM 32 extensible"}, {3, 32, false, true, 1e-7f, "float 32"},
                        {3, 64, true, false, 1e-7f, "float 64 extensible"}};
    bool ok = true;
    for (const auto& f : fmts) {
        writeTestWav("output/test_read.wav", f.code, f.bits, 2, stereo, f.ext, f.junk);
        auto w = readWavMono("output/test_read.wav");
        bool good = w.error.empty() && w.sampleRate == 44100 && w.channels == 2 && w.mono.size() == 4;
        for (size_t i = 0; good && i < 4; ++i) good &= std::fabs(w.mono[i] - expected[i]) <= f.tol + 1.0f / 127.0f * (f.bits == 8);
        if (!good) { std::printf("  falhou: %s (%s)\n", f.name, w.error.c_str()); ok = false; }
    }
    check(ok, "leitor de WAV: PCM 8/16/24/32, float 32/64, extensible e chunks extras (mixa em mono)");
    std::ofstream("output/test_read.wav") << "isto nao e um wav";
    auto bad = readWavMono("output/test_read.wav");
    auto missing = readWavMono("output/nao_existe.wav");
    check(!bad.error.empty() && bad.mono.empty() && !missing.error.empty(), "leitor de WAV: arquivo invalido ou inexistente da erro, sem amostras");
    std::filesystem::remove("output/test_read.wav");
}

// som de teste: soma de senoides (hz, amp) a partir de `startSec`, com decaimento opcional em dB/s
static std::vector<float> sines(const std::vector<std::pair<double, double>>& parts, double seconds, double startSec = 0.1,
                                double decayDbPerSec = 0.0) {
    std::vector<float> x(static_cast<size_t>(seconds * 48000.0));
    for (size_t i = 0; i < x.size(); ++i) {
        double t = i / 48000.0;
        if (t < startSec) continue;
        double g = std::pow(10.0, -decayDbPerSec * (t - startSec) / 20.0), v = 0.0;
        for (const auto& [hz, a] : parts) v += a * std::sin(2.0 * M_PI * hz * (t - startSec));
        x[i] = static_cast<float>(g * v);
    }
    return x;
}

static resynth::Result resynthDefault(const std::vector<float>& x, const resynth::Options& o = {}) {
    auto env = resynth::analyzeEnvelope(x.data(), x.size(), 48000.0);
    double a, b;
    resynth::defaultTimes(env, x.size() / 48000.0, o, 48000.0, a, b);
    return resynth::analyze(x.data(), x.size(), 48000.0, a, b, o);
}

static void testResynthFindsPartials() {
    auto r = resynthDefault(sines({{220.0, 0.5}, {606.4, 0.25}, {1188.9, 0.125}, {3333.3, 0.05}}, 1.5));
    bool ok = r.ok && r.peaksA.size() == 4;
    const double hz[4] = {220.0, 606.4, 1188.9, 3333.3}, amp[4] = {0.5, 0.25, 0.125, 0.05};
    for (size_t i = 0; ok && i < 4; ++i)
        ok &= std::fabs(r.peaksA[i].hz - hz[i]) < 0.001 * hz[i] && std::fabs(r.peaksA[i].amp - amp[i]) < 0.03 * amp[i];
    check(ok, "ressintese: acha os 4 parciais inarmonicos com frequencia (0.1%) e amplitude (3%) certas, e nada mais");
    check(std::fabs(r.refHz - 220.0f) < 0.5f && std::fabs(r.a.ratio[1] - 606.4f / 220.0f) < 2e-3f && r.a.amp[0] == 1.0f &&
              std::fabs(r.a.amp[1] - 0.5f) < 0.02f,
          "ressintese inarmonica: referencia = parcial forte mais grave (220Hz), razoes e amplitudes normalizadas");
}

static void testResynthReferenceHarmonic() {
    std::vector<std::pair<double, double>> saw;
    for (int k = 2; k <= 40; ++k) saw.push_back({196.0 * k, 0.5 / k}); // sem a fundamental
    auto r = resynthDefault(sines(saw, 1.0));
    std::printf("  (serra sem fundamental: referencia %.3f Hz, %d parciais)\n", r.refHz, r.partialsA);
    bool integer = true;
    for (int k = 0; k < r.partialsA; ++k) integer &= std::fabs(r.a.ratio[k] - std::round(r.a.ratio[k])) < 0.002f;
    check(std::fabs(r.refHz - 196.0f) < 0.2f && integer && r.partialsA == 39,
          "ressintese: fundamental ausente (serra so com harmonicos 2..40) detectada em 196Hz, razoes inteiras");
    auto r2 = resynthDefault(sines(saw, 1.0), resynth::Options{8192, -60.0f, 10, 20.0f});
    check(r2.partialsA == 10 && r2.a.amp[0] == 1.0f, "ressintese: maxPartials limita aos parciais mais fortes");
}

static void testResynthPairsAB() {
    // ataque com harmonicos 1,2,3 que viram 1,3,5 no sustain
    auto x = sines({{200, 0.5}, {400, 0.3}, {600, 0.2}}, 1.2);
    auto late = sines({{200, 0.5}, {600, 0.3}, {1000, 0.2}}, 1.2);
    for (size_t i = 0; i < x.size(); ++i) {
        double t = i / 48000.0, u = std::clamp((t - 0.35) / 0.1, 0.0, 1.0);
        x[i] = static_cast<float>(x[i] * (1.0 - u) + late[i] * u);
    }
    auto r = resynth::analyze(x.data(), x.size(), 48000.0, 0.2, 0.8, resynth::Options{});
    bool noGlide = true;
    int onlyA = 0, onlyB = 0;
    for (int k = 0; k < kNumPartials; ++k) {
        bool a = r.a.amp[k] > 0.0f, b = r.b.amp[k] > 0.0f;
        if (a && b) noGlide &= std::fabs(r.a.ratio[k] / r.b.ratio[k] - 1.0f) < 0.03f;
        if (!a || !b) noGlide &= r.a.ratio[k] == r.b.ratio[k]; // slot de um lado so: mesma razao nos dois
        if (a && !b) { ++onlyA; noGlide &= std::fabs(r.a.ratio[k] - 2.0f) < 0.01f; }
        if (b && !a) { ++onlyB; noGlide &= std::fabs(r.b.ratio[k] - 5.0f) < 0.01f; }
    }
    std::printf("  (pareamento: %d em A e B, %d so em A, %d so em B)\n", r.paired, onlyA, onlyB);
    check(noGlide && r.paired == 2 && onlyA == 1 && onlyB == 1,
          "ressintese A->B: parciais pareados por frequencia; o que some (2x) e o que aparece (5x) fazem fade, nao deslizam");

    // fxEG: morph ~0 ate A e chega em 99 (B) no instante B
    auto t = dx7fm_app::egTimes(r.fxEG);
    double tA = 0.2 - r.onsetSec, tB = 0.8 - r.onsetSec;
    std::printf("  (fxEG: estagio 1 %.3fs (A em %.3fs), chega em B em %.3fs (B em %.3fs))\n", t.stage[0], tA, t.stage[0] + t.stage[1], tB);
    check(std::fabs(t.stage[0] - tA) < 0.25 * tA && std::fabs(t.stage[0] + t.stage[1] - tB) < 0.1 * tB && r.fxEG.level[2] == 99,
          "ressintese: fxEG leva o morph de A (no instante A) ate B (no instante B)");
}

static void testResynthEndToEnd() {
    // sino sintetico: 220Hz (= nota 57) + parciais inarmonicos, decaindo 20 dB/s
    auto x = sines({{220.0, 0.5}, {606.4, 0.25}, {1188.9, 0.15}}, 2.0, 0.1, 20.0);
    auto r = resynthDefault(x);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 99;
    resynth::apply(v.op[0], r, resynth::ApplyOptions{});
    Synth synth;
    synth.init(48000.0, 4);
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 57, 127);
    std::vector<float> y;
    for (int i = 0; i < 96000; ++i) { float l, rr; synth.renderStereo(l, rr); y.push_back(l); }
    auto win = [](const std::vector<float>& s, double from) {
        return std::vector<float>(s.begin() + static_cast<long>(from * 48000), s.begin() + static_cast<long>((from + 0.2) * 48000));
    };
    auto xs = win(x, 0.4), ys = win(y, 0.3); // mesmo tempo desde o inicio do som (x comeca em 0.1s)
    float rx = mag(xs, 606.4) / mag(xs, 220.0), ry = mag(ys, 606.4) / mag(ys, 220.0);
    float rx3 = mag(xs, 1188.9) / mag(xs, 220.0), ry3 = mag(ys, 1188.9) / mag(ys, 220.0);
    std::printf("  (original: p2/p1=%.3f p3/p1=%.3f; ressintetizado: %.3f %.3f)\n", rx, rx3, ry, ry3);
    check(std::fabs(ry / rx - 1.0f) < 0.1f && std::fabs(ry3 / rx3 - 1.0f) < 0.1f,
          "ressintese ponta a ponta: tocando a nota da referencia, o espectro bate com o original (10%)");
    auto dropDb = [](const std::vector<float>& s, double t0, double t1) {
        return 20.0f * std::log10(computeRms(s, static_cast<size_t>(t1 * 48000), 4800) / computeRms(s, static_cast<size_t>(t0 * 48000), 4800));
    };
    float dx = dropDb(x, 0.4, 1.4), dy = dropDb(y, 0.3, 1.3);
    std::printf("  (queda em 1s: original %.1f dB, ressintetizado %.1f dB; ampEG r=%d,%d,%d l=%d,%d,%d)\n", dx, dy, r.ampEG.rate[0],
                r.ampEG.rate[1], r.ampEG.rate[2], r.ampEG.level[0], r.ampEG.level[1], r.ampEG.level[2]);
    check(std::fabs(dx - dy) < 3.0f, "ressintese ponta a ponta: o Amp EG ajustado reproduz o decaimento do som (3 dB)");
}

static void testAdditiveSilentWhenAllAmpsZero() {
    auto x = renderAdditive([](VoiceParams& v) { v.op[0].additiveA.amp[0] = 0.0f; }, 4800);
    bool ok = true;
    for (float s : x) ok &= (s == 0.0f);
    check(ok, "Additive com todas as amplitudes em 0 e silencio exato (sem divisao por zero)");
}

static void testAdditiveJson() {
    VoiceParams v;
    v.op[3].waveform = Waveform::Additive;
    v.op[3].additiveA.amp[4] = 0.25f;
    v.op[3].additiveA.ratio[4] = 2.756f;
    v.op[3].additiveB.ratio[7] = 31.5f;
    v.op[3].morphDepth = 0.6f;
    v.modMatrix[2] = ModRoute{ModSource::ModWheel, ModDest::OperatorMorph, 3, 0.5f};
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(v).dump()));
    const auto& o = back.op[3];
    check(o.waveform == Waveform::Additive && o.additiveA.amp[4] == 0.25f && o.additiveA.ratio[4] == 2.756f &&
          o.additiveB.ratio[7] == 31.5f && o.morphDepth == 0.6f && back.modMatrix[2].dest == ModDest::OperatorMorph,
          "Additive (amplitudes, razoes, morph) e OperatorMorph sobrevivem ao round-trip de JSON");

    VoiceParams old = voiceParamsFromJson(JsonValue::parse(R"({"op":[{"waveform":"Saw"}]})"));
    check(old.op[0].additiveA.amp[0] == 1.0f && old.op[0].additiveA.ratio[2] == 3.0f && old.op[0].morphDepth == 0.0f,
          "patch antigo sem a chave \"additive\" carrega com os defaults (seno, razoes harmonicas)");

    VoiceParams bad = voiceParamsFromJson(JsonValue::parse(
        R"({"op":[{"additive":{"a":{"amp":[5,-1],"ratio":[0,1e9]},"morph":7}}]})"));
    check(bad.op[0].additiveA.amp[0] == 1.0f && bad.op[0].additiveA.amp[1] == 0.0f &&
          bad.op[0].additiveA.ratio[0] == 0.01f && bad.op[0].additiveA.ratio[1] == kMaxPartialRatio && bad.op[0].morphDepth == 1.0f,
          "valores fora da faixa no JSON da Additive sao saturados (amp 0..1, razao 0.01..kMaxPartialRatio, morph 0..1)");
}

static void testAdditiveCost() {
    auto timeIt = [](bool additive) {
        Synth synth;
        synth.init(48000.0, 16);
        VoiceParams v;
        applyAlgorithm(v, 1);
        for (auto& op : v.op) {
            op.outputLevel = 80;
            if (additive) {
                op.waveform = Waveform::Additive;
                for (int k = 0; k < kNumPartials; ++k) { op.additiveA.amp[k] = 1.0f / (k + 1); op.additiveA.ratio[k] = k + 1.0f; }
            }
        }
        synth.part(0).voice = v;
        synth.recompile(0);
        for (int n = 0; n < 16; ++n) synth.noteOn(0, 40 + n, 100);
        auto t0 = std::chrono::steady_clock::now();
        float sink = 0.0f;
        for (int i = 0; i < 48000; ++i) { float l, r; synth.renderStereo(l, r); sink += l; }
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return sink == 12345.0f ? 0.0 : sec;
    };
    double sine = timeIt(false), add = timeIt(true);
    // [2026-10-20] 64 parciais com kernel SIMD (dx7_dsp.h): limite por caminho
    // -- AVX2 processa 8 parciais por instrucao, SSE2 (CPU sem AVX2) 4.
    const bool avx2 = simd::selectedKernel() == &simd::kernelAvx2;
    const double limit = avx2 ? 5.0 : 10.0;
    std::printf("  (16 vozes x 8 ops, 1s de audio: Sine=%.3fs, Additive %d parciais=%.3fs, razao=%.2fx, caminho %s)\n",
                sine, kNumPartials, add, add / sine, avx2 ? "AVX2" : "SSE2");
    char msg[160];
    std::snprintf(msg, sizeof msg, "Additive (%d osciladores em todos os 8 ops, SIMD %s) custa menos que %.0fx um operador Sine",
                  kNumPartials, avx2 ? "AVX2" : "SSE2", limit);
    check(add < sine * limit, msg);
}

// --- [ADICIONADO em 2026-10-18] efeitos globais baseados em open source ---

// Roda `n` amostras de um sinal mono (gen(i)) por um bloco/cadeia e devolve L.
template <typename Fx, typename Gen>
static std::vector<float> runFx(Fx&& fx, Gen gen, int n, std::vector<float>* right = nullptr) {
    std::vector<float> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        float l = gen(i), r = l;
        fx(l, r);
        out.push_back(l);
        if (right) right->push_back(r);
    }
    return out;
}
static float sineAt(int i, float hz, float amp = 0.5f) { return amp * std::sin(2.0f * kPi * hz * static_cast<float>(i) / 48000.0f); }
static float rmsTail(const std::vector<float>& x, size_t from) { return computeRms(x, from, x.size() - from); }
static float dbRatio(float out, float in) { return 20.0f * std::log10(out / in); }

static void testGlobalFxChainDisabledIsIdentity() {
    GlobalFxChain chain;
    chain.setSampleRate(48000.0);
    GlobalFxParams p; // tudo desligado por padrao
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    bool same = true;
    for (int i = 0; i < 4800; ++i) {
        float l = d(rng), r = d(rng), l0 = l, r0 = r;
        chain.process(p, l, r);
        same &= (l == l0 && r == r0);
    }
    check(same, "cadeia de efeitos globais toda desligada e identidade EXATA");
}

static void testPhaserNotch() {
    auto run = [](float hz) {
        Phaser ph;
        ph.setSampleRate(48000.0);
        PhaserParams p;
        p.notches = 1; p.rateHz = 0.0f; p.feedback = 0.0f; p.depth = 1.0f;
        p.notchWidthHz = 100.0f; p.freqMinHz = 1000.0f; p.freqMaxHz = 1000.0f;
        auto out = runFx([&](float& l, float& r) { ph.process(p, l, r); }, [hz](int i) { return sineAt(i, hz); }, 24000);
        return rmsTail(out, 12000) / (0.5f / std::sqrt(2.0f));
    };
    float atNotch = run(1000.0f), away = run(4000.0f);
    std::printf("  (phaser 1 notch em 1kHz: ganho em 1kHz=%.1fdB, em 4kHz=%.1fdB)\n", 20 * std::log10(atNotch), 20 * std::log10(away));
    check(atNotch < 0.05f, "phaser: seno na frequencia do notch e cancelado (< -26dB)");
    check(away > 0.7f, "phaser: seno longe do notch passa quase inteiro");
}

static void testFlangerImpulseResponse() {
    Flanger fl;
    fl.setSampleRate(48000.0);
    FlangerParams p;
    p.rateHz = 0.0f; p.delayMs = 1.0f; p.sweepMs = 0.0f; p.depth = 1.0f; p.feedback = 0.5f;
    auto out = runFx([&](float& l, float& r) { fl.process(p, l, r); }, [](int i) { return i == 0 ? 1.0f : 0.0f; }, 400);
    std::printf("  (flanger impulso: y[0]=%.3f y[48]=%.3f y[96]=%.3f y[20]=%.3f)\n", out[0], out[48], out[96], out[20]);
    check(std::abs(out[0] - 0.5f) < 1e-4f && std::abs(out[48] - 0.5f) < 1e-3f && std::abs(out[96] - 0.25f) < 1e-3f &&
          std::abs(out[20]) < 1e-6f,
          "flanger: resposta ao impulso = 0.5 em 0, 0.5 no atraso (1ms), 0.25 no dobro (feedback 0.5)");
}

static void testTapeDelayFiltersRepeats() {
    auto peaks = [](float highCut) {
        TapeDelay dl;
        dl.setSampleRate(48000.0);
        DelayParams p;
        p.enabled = true; p.timeLeftSec = p.timeRightSec = 0.1f; p.feedback = 0.8f; p.mix = 1.0f;
        p.lowCutHz = 10.0f; p.highCutHz = highCut;
        auto out = runFx([&](float& l, float& r) { dl.process(p, l, r); }, [](int i) { return i == 0 ? 1.0f : 0.0f; }, 15000);
        float p1 = 0, p2 = 0;
        for (int i = 4790; i < 4810; ++i) p1 = std::max(p1, std::abs(out[i]));
        for (int i = 9590; i < 9700; ++i) p2 = std::max(p2, std::abs(out[i]));
        return std::make_pair(p1, p2);
    };
    auto [open1, open2] = peaks(20000.0f);
    auto [dark1, dark2] = peaks(1000.0f);
    std::printf("  (pico eco1/eco2: sem filtro %.3f/%.3f, passa-baixas 1kHz %.3f/%.3f)\n", open1, open2, dark1, dark2);
    check(std::abs(open1 - 1.0f) < 1e-3f, "delay: primeiro eco chega exatamente no tempo configurado (100ms)");
    check(open2 > 0.8f * 0.8f && dark2 < 0.5f * 0.8f * dark1, "delay: passa-baixas no loop escurece cada repeticao");

    TapeDelay dl;
    dl.setSampleRate(48000.0);
    DelayParams p;
    p.enabled = true; p.feedback = 0.95f; p.drive = 1.0f; p.wow = 1.0f; p.mix = 1.0f;
    float peak = 0.0f;
    for (int i = 0; i < 48000 * 3; ++i) {
        float l = sineAt(i, 220.0f, 1.0f), r = l;
        dl.process(p, l, r);
        peak = std::max(peak, std::abs(l));
    }
    std::printf("  (delay feedback 0.95 + drive + wow com seno cheio, 3s: pico=%.2f)\n", peak);
    check(std::isfinite(peak) && peak < 20.0f, "delay: feedback maximo com drive e wow continua estavel");
}

static void testReverbTypes() {
    const char* names[] = {"Room (Freeverb)", "Hall (Clouds)", "Plate (MVerb)"};
    for (int t = 0; t < 3; ++t) {
        ReverbBlock rv;
        rv.setSampleRate(48000.0);
        ReverbParams p;
        p.enabled = true; p.type = static_cast<ReverbType>(t); p.mix = 1.0f; p.roomSize = 0.6f;
        std::vector<float> right;
        auto out = runFx([&](float& l, float& r) { rv.process(p, l, r); },
                         [](int i) { return i < 480 ? sineAt(i, 440.0f) : 0.0f; }, 48000 * 3, &right);
        bool finite = true;
        for (float x : out) finite &= std::isfinite(x);
        float early = computeRms(out, 4800, 19200), late = computeRms(out, 96000, 24000);
        double lr = 0;
        for (size_t i = 4800; i < 24000; ++i) lr += std::abs(out[i] - right[i]);
        std::printf("  (%s: RMS 0.1-0.5s=%.4f, 2-2.5s=%.5f, diferenca L/R=%.2f)\n", names[t], early, late, lr);
        char msg[160];
        std::snprintf(msg, sizeof msg, "reverb %s: cauda audivel, finita, decaindo e estereo", names[t]);
        check(finite && early > 0.002f && late < early * 0.5f && lr > 1.0, msg);
    }

    // freeze do Room segura a cauda e ignora entrada nova
    ReverbBlock rv;
    rv.setSampleRate(48000.0);
    ReverbParams p;
    p.enabled = true; p.mix = 1.0f; p.roomSize = 0.6f;
    std::vector<float> out;
    for (int i = 0; i < 48000 * 3; ++i) {
        p.freeze = i >= 9600;
        float l = i < 480 ? sineAt(i, 440.0f) : (i > 48000 ? sineAt(i, 3000.0f) : 0.0f), r = l;
        rv.process(p, l, r);
        out.push_back(l);
    }
    float frozen1 = computeRms(out, 14400, 24000), frozen2 = computeRms(out, 120000, 24000);
    std::printf("  (freeze: RMS 0.3-0.8s=%.4f, 2.5-3s=%.4f -- entrada nova a partir de 1s ignorada)\n", frozen1, frozen2);
    check(frozen1 > 0.002f && frozen2 > frozen1 * 0.8f && frozen2 < frozen1 * 1.25f, "reverb Room com freeze: cauda sustenta sem decair nem somar entrada nova");

    // predelay: nada molhado antes de 100ms, nos 3 algoritmos. Limiar 1e-9
    // e nao 0: o MVerb soma 1e-25 no filtro interno (antidenormal), entao a
    // saida dele nunca e zero exato (~2e-24, ~470dB abaixo do sinal)
    bool predelayOk = true;
    for (int t = 0; t < 3; ++t) {
        ReverbBlock pd;
        pd.setSampleRate(48000.0);
        ReverbParams q;
        q.enabled = true; q.mix = 1.0f; q.predelayMs = 100.0f; q.type = static_cast<ReverbType>(t);
        auto o = runFx([&](float& l, float& r) { pd.process(q, l, r); }, [](int i) { return i == 0 ? 1.0f : 0.0f; }, 9600);
        float before = 0.0f, after = 0.0f;
        for (int i = 0; i < 4800; ++i) before = std::max(before, std::abs(o[i]));
        for (int i = 4800; i < 9600; ++i) after = std::max(after, std::abs(o[i]));
        predelayOk &= before < 1e-9f && after > 0.01f;
    }
    check(predelayOk, "reverb: predelay de 100ms nos 3 algoritmos (nada molhado antes de 4800 amostras)");

    // trocar de algoritmo nao toca a cauda velha do outro
    ReverbBlock sw;
    sw.setSampleRate(48000.0);
    ReverbParams s;
    s.enabled = true; s.mix = 1.0f; s.type = ReverbType::Plate; s.roomSize = 1.0f;
    runFx([&](float& l, float& r) { sw.process(s, l, r); }, [](int i) { return sineAt(i, 440.0f); }, 4800);
    s.type = ReverbType::Room;
    runFx([&](float& l, float& r) { sw.process(s, l, r); }, [](int) { return 0.0f; }, 10);
    s.type = ReverbType::Plate;
    auto back = runFx([&](float& l, float& r) { sw.process(s, l, r); }, [](int) { return 0.0f; }, 4800);
    float stale = 0.0f;
    for (float x : back) stale = std::max(stale, std::abs(x));
    check(stale < 1e-6f, "reverb: voltar pra um algoritmo nao toca a cauda velha congelada dele");
}

static void testEqResponse() {
    auto gainDb = [](EqParams p, float hz) {
        Eq3 eq;
        eq.setSampleRate(48000.0);
        p.enabled = true;
        auto out = runFx([&](float& l, float& r) { eq.process(p, l, r); }, [hz](int i) { return sineAt(i, hz); }, 48000);
        return dbRatio(rmsTail(out, 24000), 0.5f / std::sqrt(2.0f));
    };
    EqParams flat;
    EqParams low; low.lowGainDb = 12.0f;
    EqParams mid; mid.midGainDb = 6.0f;
    EqParams high; high.highGainDb = -12.0f;
    float f40 = gainDb(flat, 40.0f), l40 = gainDb(low, 40.0f), l5k = gainDb(low, 5000.0f);
    float m1k = gainDb(mid, 1000.0f), m100 = gainDb(mid, 100.0f), h16k = gainDb(high, 16000.0f), h200 = gainDb(high, 200.0f);
    std::printf("  (EQ: plano 40Hz=%.2f | low +12: 40Hz=%.2f 5kHz=%.2f | mid +6@1k: 1k=%.2f 100Hz=%.2f | high -12: 16k=%.2f 200Hz=%.2f dB)\n",
                f40, l40, l5k, m1k, m100, h16k, h200);
    check(std::abs(f40) < 0.05f, "EQ com ganhos 0 e transparente");
    check(std::abs(l40 - 12.0f) < 0.7f && std::abs(l5k) < 0.5f, "EQ low shelf +12dB: +12 no grave, ~0 no agudo");
    check(std::abs(m1k - 6.0f) < 0.3f && std::abs(m100) < 0.7f, "EQ peaking +6dB em 1kHz: +6 no centro, ~0 longe");
    check(std::abs(h16k + 12.0f) < 1.0f && std::abs(h200) < 0.5f, "EQ high shelf -12dB: -12 no agudo, ~0 no grave");
}

static void testCompressor() {
    auto run = [](float amp, float kneeDb, float* grOut = nullptr) {
        ZamCompressor c;
        c.setSampleRate(48000.0);
        CompressorParams p;
        p.enabled = true; p.thresholdDb = -20.0f; p.ratio = 4.0f; p.kneeDb = kneeDb; p.makeupDb = 0.0f;
        // onda quadrada: |x| constante, entao o detector de pico ve um nivel fixo
        auto out = runFx([&](float& l, float& r) { c.process(p, l, r); },
                         [amp](int i) { return ((i / 100) % 2) ? amp : -amp; }, 48000);
        if (grOut) *grOut = c.gainReductionDb();
        return std::abs(out.back());
    };
    float gr = 0.0f;
    float y = run(0.5f, 0.0f, &gr); // -6dB -> -20 + 14/4 = -16.5dB
    float expected = std::pow(10.0f, -16.5f / 20.0f);
    std::printf("  (compressor -20dB 4:1, entrada -6dB: saida=%.4f (esperado %.4f), reducao=%.2fdB)\n", y, expected, gr);
    check(std::abs(y / expected - 1.0f) < 0.02f, "compressor: nivel de saida segue a curva estatica (thres -20, 4:1)");
    check(std::abs(gr - 10.49f) < 0.2f, "compressor: medidor de reducao de ganho bate com a curva (~10.5dB)");
    float hardBelow = run(std::pow(10.0f, -30.0f / 20.0f), 0.0f), softBelow = run(std::pow(10.0f, -30.0f / 20.0f), 8.0f);
    float in = std::pow(10.0f, -30.0f / 20.0f);
    std::printf("  (10dB abaixo do threshold: knee 0 -> %.2fdB, knee 8 -> %.2fdB)\n", dbRatio(hardBelow, in), dbRatio(softBelow, in));
    check(std::abs(hardBelow / in - 1.0f) < 1e-3f && softBelow < in * 0.97f,
          "compressor: knee suave comprime antes do threshold (corrigido em relacao ao ZamComp original)");
}

static void testGlobalFxOrder() {
    auto render = [](const GlobalFxType* order) {
        GlobalFxChain chain;
        chain.setSampleRate(48000.0);
        GlobalFxParams p;
        p.compressor.enabled = true; p.compressor.thresholdDb = -30.0f; p.compressor.ratio = 10.0f;
        p.reverb.enabled = true; p.reverb.mix = 0.5f;
        if (order) std::copy(order, order + kNumGlobalFx, p.order);
        return runFx([&](float& l, float& r) { chain.process(p, l, r); },
                     [](int i) { return i < 4800 ? sineAt(i, 440.0f) : 0.0f; }, 24000);
    };
    GlobalFxType compFirst[kNumGlobalFx] = {GlobalFxType::Compressor, GlobalFxType::Phaser, GlobalFxType::Flanger,
                                            GlobalFxType::Chorus, GlobalFxType::Delay, GlobalFxType::Reverb, GlobalFxType::Eq};
    GlobalFxType broken[kNumGlobalFx] = {GlobalFxType::Reverb, GlobalFxType::Reverb, GlobalFxType::Flanger,
                                         GlobalFxType::Chorus, GlobalFxType::Delay, GlobalFxType::Eq, GlobalFxType::Compressor};
    auto def = render(nullptr), cf = render(compFirst), br = render(broken);
    float diff = 0.0f, diffBroken = 0.0f;
    for (size_t i = 0; i < def.size(); ++i) {
        diff = std::max(diff, std::abs(def[i] - cf[i]));
        diffBroken = std::max(diffBroken, std::abs(def[i] - br[i]));
    }
    std::printf("  (diferenca maxima: compressor antes x depois do reverb=%.3f; ordem invalida x padrao=%.1e)\n", diff, diffBroken);
    check(diff > 0.01f, "ordem da cadeia muda o som (compressor antes vs depois do reverb)");
    check(diffBroken == 0.0f, "ordem invalida (bloco repetido) cai na ordem padrao, sem rodar bloco 2x");
}

static void testGlobalFxCost() {
    GlobalFxChain chain;
    chain.setSampleRate(48000.0);
    GlobalFxParams p;
    p.chorus.enabled = p.phaser.enabled = p.flanger.enabled = p.delay.enabled = true;
    p.reverb.enabled = p.eq.enabled = p.compressor.enabled = true;
    p.phaser.notches = 8;
    float worst = 0.0f;
    for (int t = 0; t < 3; ++t) {
        p.reverb.type = static_cast<ReverbType>(t);
        auto t0 = std::chrono::steady_clock::now();
        float sink = 0.0f;
        for (int i = 0; i < 48000; ++i) { float l = sineAt(i, 220.0f), r = l; chain.process(p, l, r); sink += l; }
        float sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, sink == 12345.0f ? 0.0f : sec);
    }
    std::printf("  (os 7 efeitos ligados, pior modo de reverb: %.1f%% de um nucleo)\n", worst * 100.0f);
    check(worst < 0.25f, "cadeia completa de efeitos globais roda bem abaixo do tempo real (< 25% de um nucleo)");
}

// --- [ADICIONADO em 2026-10-18] otimizacao: operadores mortos ---------------

static void testDeadOperatorRevivedByLiveEdit() {
    // op 2 comeca morto (nao e portadora nem modula ninguem). No meio da nota
    // uma edicao ao vivo o torna portadora: ele tem que entrar soando, com o
    // envelope no ponto certo (o motor nao calcula o oscilador de op morto,
    // mas continua andando o envelope dele).
    auto patchWithOp2 = [](bool op2Carrier) {
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].outputLevel = 75;
        v.op[2].outputLevel = 75;
        v.op[2].coarseRatio = 3.0f;          // em 3x a fundamental: da pra medir separado
        v.op[2].ampEG.rate[0] = 45;          // ataque de ~0.8s: aos 0.5s ainda subindo, o ponto do envelope importa
        if (op2Carrier) v.matrix.outputMix[2] = 1.0f;
        return v;
    };
    Synth live;
    live.init(48000.0, 4);
    live.part(0).voice = patchWithOp2(false);
    live.recompile(0);
    live.noteOn(0, 69, 100);
    std::vector<float> before;
    for (int i = 0; i < 24000; ++i) { float l, r; live.renderStereo(l, r); if (i >= 19200) before.push_back(l); }
    live.part(0).voice = patchWithOp2(true); // edicao ao vivo: op 2 vira portadora
    live.recompile(0);
    std::vector<float> a;
    for (int i = 0; i < 4800; ++i) { float l, r; live.renderStereo(l, r); a.push_back(l); }

    Synth fresh; // referencia: o mesmo patch desde o inicio da nota
    fresh.init(48000.0, 4);
    fresh.part(0).voice = patchWithOp2(true);
    fresh.recompile(0);
    fresh.noteOn(0, 69, 100);
    for (int i = 0; i < 24000; ++i) { float l, r; fresh.renderStereo(l, r); }
    std::vector<float> b;
    for (int i = 0; i < 4800; ++i) { float l, r; fresh.renderStereo(l, r); b.push_back(l); }

    float liveH3 = goertzelMagnitude(a, 1320.0, 48000.0), freshH3 = goertzelMagnitude(b, 1320.0, 48000.0);
    float beforeH3 = goertzelMagnitude(before, 1320.0, 48000.0);
    std::printf("  (1320Hz: antes da edicao %.6f; op revivido ao vivo %.4f; mesmo patch desde o inicio %.4f)\n",
                beforeH3, liveH3, freshH3);
    check(beforeH3 < liveH3 * 0.01f && std::abs(liveH3 / freshH3 - 1.0f) < 0.05f,
          "operador morto ligado ao vivo entra soando no ponto certo do envelope");
}

// --- [ADICIONADO em 2026-10-18] grafico de envelopes da UI ------------------

static void testEnvelopeGraphMathMatchesEngine() {
    // as duracoes que o grafico desenha tem que bater com o SegmentEG real
    EGParams eg;
    const uint8_t rate[4] = {70, 45, 30, 55}, level[4] = {99, 60, 40, 0};
    for (int k = 0; k < 4; ++k) { eg.rate[k] = rate[k]; eg.level[k] = level[k]; }
    dx7fm_app::EgTimes t = dx7fm_app::egTimes(eg);

    SegmentEG seg;
    seg.configure(eg.rate, eg.level);
    seg.noteOn(0.0f);
    const double sr = 48000.0;
    int n = 0;
    float v = 0.0f;
    float reachedL3 = -1.0f;
    for (; n < 48000 * 30; ++n) {
        v = seg.process(sr);
        if (reachedL3 < 0.0f && v == 40.0f) reachedL3 = (n + 1) / static_cast<float>(sr);
        if (reachedL3 >= 0.0f) break;
    }
    seg.noteOff();
    int r = 0;
    for (; r < 48000 * 30 && !seg.isIdle(); ++r) seg.process(sr);
    float releaseSec = r / static_cast<float>(sr);
    std::printf("  (ate o sustain: grafico %.4fs, motor %.4fs | release: grafico %.4fs, motor %.4fs)\n",
                t.untilSustain(), reachedL3, t.stage[3], releaseSec);
    check(std::abs(t.untilSustain() - reachedL3) < 3.0f / 48000.0f + 1e-3f * t.untilSustain(),
          "grafico de envelopes: tempo ate o sustain bate com o SegmentEG real");
    check(std::abs(t.stage[3] - releaseSec) < 3.0f / 48000.0f + 1e-3f * t.stage[3],
          "grafico de envelopes: tempo de release bate com o SegmentEG real");

    // conta inversa (arrastar o ponto na horizontal): tempo -> rate devolve o rate
    bool roundTrip = true;
    for (int rr = 0; rr <= 99; ++rr) {
        float sec = dx7fm_app::egStageSeconds(10.0f, 80.0f, static_cast<float>(rr));
        float back = dx7fm_app::egRateForSeconds(10.0f, 80.0f, sec);
        roundTrip &= std::abs(back - rr) < 0.01f;
    }
    check(roundTrip, "grafico de envelopes: tempo -> rate e o inverso exato de rate -> tempo (0..99)");
    check(dx7fm_app::egRateForSeconds(50.0f, 50.0f, 1.0f) < 0.0f, "estagio sem variacao de level: tempo nao depende do rate (sinaliza -1)");

    bool axis = true;
    for (float tt : {0.0f, 0.004f, 0.05f, 0.7f, 3.0f, 19.9f})
        axis &= std::abs(dx7fm_app::egXToTime(dx7fm_app::egTimeToX(tt, 20.0f), 20.0f) - tt) < 1e-4f + tt * 1e-4f;
    check(axis, "eixo de tempo comprimido do grafico: x(t) e t(x) sao inversos");
}

// --- [ADICIONADO em 2026-10-18] visualizadores (osciloscopio/espectrograma) ---

static void testSpectrumAnalyzer() {
    const int n = 4096;
    const double sr = 48000.0;
    dx7fm_app::SpectrumAnalyzer an(n);
    std::vector<float> x(n), db;
    // seno de amplitude 0.5 exatamente no bin 100 (~1171.9 Hz): pico em -6.02 dB no bin 100
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * kPi * 100.0f * i / n);
    an.magnitudeDb(x.data(), db);
    int peakBin = static_cast<int>(std::max_element(db.begin(), db.end()) - db.begin());
    std::printf("  (seno 0.5 no bin 100: pico no bin %d, %.2f dB; bin 140: %.1f dB)\n", peakBin, db[100], db[140]);
    check(peakBin == 100 && std::abs(db[100] - 20.0f * std::log10(0.5f)) < 0.05f,
          "FFT: seno de amplitude 0.5 aparece no bin certo com -6.02 dB (normalizacao da janela de Hann)");
    check(db[140] < -80.0f, "FFT: longe do seno o espectro fica no piso (janela de Hann sem vazamento grande)");

    // faixas log: 1kHz cai na faixa certa
    std::vector<float> bands;
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = std::sin(2.0f * kPi * 1000.0f * i / static_cast<float>(sr));
    an.magnitudeDb(x.data(), db);
    dx7fm_app::logBands(db, sr, n, 56, 20.0f, 20000.0f, bands);
    int bestBand = static_cast<int>(std::max_element(bands.begin(), bands.end()) - bands.begin());
    int expected = static_cast<int>(56 * std::log(1000.0 / 20.0) / std::log(1000.0));
    std::printf("  (1 kHz: faixa mais forte %d de 56, esperada %d)\n", bestBand, expected);
    check(std::abs(bestBand - expected) <= 1, "espectrograma: 1 kHz cai na faixa certa da escala log (20 Hz - 20 kHz)");

    char note[16];
    dx7fm_app::noteName(440.0f, note, sizeof note);
    bool ok = std::string(note) == "A4";
    dx7fm_app::noteName(261.63f, note, sizeof note);
    ok &= std::string(note) == "C4";
    check(ok, "nome de nota: 440 Hz = A4, 261.6 Hz = C4");
}

static void testAudioTapWrapAround() {
    dx7fm_app::AudioTap tap;
    // escreve 3x o tamanho do buffer, em blocos de 256, com L = i e R = -i/2... mono = i/4
    const int total = static_cast<int>(dx7fm_app::AudioTap::kSize) * 3 + 77;
    std::vector<float> block(512);
    int written = 0;
    while (written < total) {
        int frames = std::min(256, total - written);
        for (int i = 0; i < frames; ++i) {
            float v = static_cast<float>(written + i);
            block[static_cast<size_t>(2 * i)] = v;
            block[static_cast<size_t>(2 * i + 1)] = v * 0.5f;
        }
        tap.push(block.data(), frames);
        written += frames;
    }
    std::vector<float> out(1000);
    tap.readMono(tap.written(), 1000, out.data());
    bool ok = tap.written() == static_cast<uint64_t>(total);
    for (int i = 0; i < 1000; ++i) ok &= out[static_cast<size_t>(i)] == 0.75f * static_cast<float>(total - 1000 + i);
    check(ok, "tap de audio: depois de dar a volta no buffer, le as ultimas amostras certas (mono = (L+R)/2)");
    dx7fm_app::AudioTap empty;
    std::vector<float> z(10, 1.0f);
    empty.readMono(0, 10, z.data());
    check(std::all_of(z.begin(), z.end(), [](float v) { return v == 0.0f; }), "tap de audio: antes do inicio le silencio");
}

static void testSpectrogramHistory() {
    using H = dx7fm_app::SpectrogramHistory;
    const double sr = 48000.0;
    const float freqs[8] = {130.8f, 196.0f, 261.6f, 329.6f, 392.0f, 523.3f, 659.3f, 784.0f};
    const int segment = 20160; // 0.42s por nota, 8 notas = 3.36s (mais que os 0.68s que o tap guarda)
    auto bandOf = [](float hz) { return static_cast<int>(H::kBands * std::log(hz / H::kFMin) / std::log(H::kFMax / H::kFMin)); };
    auto peakBand = [](const float* col) { return static_cast<int>(std::max_element(col, col + H::kBands) - col); };

    // modo "UI travada": todo o audio entra antes da primeira atualizacao
    dx7fm_app::AudioTap tap;
    H stalled, live;
    std::vector<float> block(512);
    int frame = 0;
    for (int k = 0; k < 8; ++k)
        for (int i = 0; i < segment; i += 256) {
            for (int j = 0; j < 256; ++j, ++frame) block[2 * j] = block[2 * j + 1] = 0.5f * std::sin(2.0f * kPi * freqs[k] * frame / static_cast<float>(sr));
            tap.push(block.data(), 256);
            live.update(tap, sr); // modo normal: atualiza a cada bloco
        }
    stalled.update(tap, sr);

    // cada coluna termina em um quadro conhecido -> nota esperada
    int wrongStalled = 0, staleRead = 0, silentStalled = 0, wrongLive = 0, checked = 0;
    for (int c = 0; c < H::kCols; ++c) {
        int64_t end = static_cast<int64_t>(tap.written()) - static_cast<int64_t>(H::kCols - 1 - c) * H::kHop;
        int64_t start = end - H::kFft;
        if (start < 0 || start / segment != (end - 1) / segment) continue; // bloco atravessa 2 notas: ambiguo
        int expected = bandOf(freqs[start / segment]);
        ++checked;
        const float* sc = stalled.column(c);
        bool silent = *std::max_element(sc, sc + H::kBands) <= H::kSilenceDb;
        if (silent) ++silentStalled;
        else if (std::abs(peakBand(sc) - expected) > 1) { ++wrongStalled; ++staleRead; }
        if (std::abs(peakBand(live.column(c)) - expected) > 1) ++wrongLive;
    }
    std::printf("  (%d colunas checadas | UI travada: %d em silencio (audio ja saiu do tap), %d com frequencia ERRADA | UI normal: %d erradas)\n",
                checked, silentStalled, wrongStalled, wrongLive);
    check(staleRead == 0 && silentStalled > 0, "espectrograma: apos UI travada, colunas cujo audio saiu do tap ficam em silencio (nunca audio recente no lugar do antigo)");
    check(wrongLive == 0 && checked > 100, "espectrograma: atualizando a cada bloco, cada coluna mostra a nota certa daquele momento");
}

int main() {
    std::filesystem::create_directories("output");

    testEnvelopeShape();
    testPegCentered();
    testEgRateToSpeedMatchesDocumentedTiming();
    testEgLevelToAmplitudeBoundaries();
    testEnvelopeStageProgression();
    testEnvelopeSustainNeverDecaysWithoutNoteOff();
    testEnvelopeNoteOffPreservesCurrentLevel();
    testEnvelopeRateScalingSpeedsUpHighNotes();
    testPegPushesPitchUpAndDown();
    testEnvelopeLegatoReuseAffectsRetrigger();
    testSimpleFmRender();
    testFilteredModulatorFx();
    testMonoGlide();
    testUnison();
    testSustainPedal();
    testPolyAftertouch();
    testOperatorFeedbackModulation();
    testFxEGReleaseFollowsAmp();
    testOversamplingReducesAliasing();
    testPatchJsonRoundTrip();
    testPatchFileSaveLoad();
    testLivePatchEditPropagatesToSoundingVoice();
    testConcurrentPatchEditNoCrash();
    testChorusChangesStereoImage();
    testDelayProducesEchoAtExpectedTime();
    testReverbAddsDecayTail();
    testAfmWaveformsAreBoundedAndDistinct();
    testNoteToHzStandardTuning();
    testCoarseRatioScalesFrequencyLinearly();
    testFixedFreqIgnoresNote();
    testFineAndDetuneCentsShiftPitch();
    testClassicWaveformShapesMatchMath();
    testAfmWaveformStructuralProperties();
    testPhaseModulationZeroMeansUnmodifiedCarrier();
    testKeyLevelScalingAffectsOutputBySide();
    testVelocitySensitivityAffectsLevel();
    testOutputLevelCurveMatchesFormula();
    testAlgorithmPresetsUseEachOperatorExactlyOnce();
    testAlgorithmPresetsRenderAudibly();
    testChordDoesNotClip();
    testMasterLimiterIsTransparentForSingleVoice();
    testPolyphonyGainKeepsTotalLoudnessRoughlyConstant();
    testPolyphonyGainIsSmoothedNoClick();
    testMultitimbral4Parts();
    testPolyphonyCap();
    testStolenVoiceNotReleasedByOldNote();
    testSostenutoReleaseFreesHeldNotes();
    testSustainReleaseKeepsSostenutoNotes();
    testMonoReleasingNonSoundingNoteDoesNothing();
    testNoteOffOutsideKeyRangeIgnored();
    testCrushHighBitsStaysFinite();
    testGlideAlwaysOnDetachedNotes();
    testPegStartsAtReleaseLevel();
    testVoiceModeSwitchDoesNotStickNotes();
    testLevelScalingNeverExceedsMaxLevel();
    testFeedbackStaysBoundedAtMax();
    testLfo2DepthsHaveEffect();
    testPolyphonyGainIgnoresSilentPedalVoices();
    testNonZeroReleaseLevelKeepsSounding();
    testJsonLoadClampsAndRejectsGarbage();
    testMidiNoteOffReachesPartAfterDisable();
    testRecompileCacheFollowsMatrixChanges();
    testMasterVolume();
    testTriangleFoldShape();
    testFoldDriveOneIsTransparent();
    testFoldAddsOddHarmonics();
    testFoldBiasAddsEvenHarmonicsWithoutDc();
    testFoldDriveFollowsFxEnvelope();
    testFoldJsonRoundTrip();
    testSafeFileStem();
    testListPatchesSortedJsonOnly();
    testLoadPatchKeepsPartRouting();
    testPatchFingerprintIgnoresRouting();
    testAdditiveSinglePartialMatchesSine();
    testAdditiveFreeRatios();
    testAdditiveSkipsPartialsAboveNyquist();
    testAdditiveMorph();
    testAdditiveMorphFollowsFxEgAndModMatrix();
    testAdditiveSilentWhenAllAmpsZero();
    testAdditiveJson();
    testAdditiveCost();
    testAdditiveEditorAxes();
    testAdditivePaintStroke();
    testAdditivePresetsAreValid();
    testAdditiveSquarePresetSounds();
    testWavReaderFormats();
    testResynthFindsPartials();
    testResynthReferenceHarmonic();
    testResynthPairsAB();
    testResynthEndToEnd();
    testRenderBlockMatchesPerSample();
    testRenderBlockThreadsAreDeterministic();
    testWorkerPoolRunsEveryIndexOnce();
    testGlobalFxChainDisabledIsIdentity();
    testPhaserNotch();
    testFlangerImpulseResponse();
    testTapeDelayFiltersRepeats();
    testReverbTypes();
    testEqResponse();
    testCompressor();
    testGlobalFxOrder();
    testGlobalFxCost();
    testDeadOperatorRevivedByLiveEdit();
    testEnvelopeGraphMathMatchesEngine();
    testSpectrumAnalyzer();
    testAudioTapWrapAround();
    testSpectrogramHistory();

    std::printf("== Resultado: %d passaram, %d falharam ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

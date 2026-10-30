#include "dx7_params.h"
#include "dx7_dsp.h"
#include "dx7_engine.h"
#include "dx7_alloc.h"
#include "dx7_patch_io.h"
#include "dx7_algorithms.h"
#include "wav_writer.h"
#include "wav_reader.h"
#include "dx7_resynth.h"
#include "dx7_wavegen.h"
#include "dx7_sysex.h"
#include "../app/event_queue.h"
#include "../app/patch_browser.h"
#include "../app/envelope_math.h"
#include "../app/audio_tap.h"
#include "../app/spectrum.h"
#include "../app/spectrogram_history.h"
#include "../app/additive_presets.h"
#include "../app/op_fx_math.h"
#include "../app/settings.h"
#include "../app/midi_clock.h"
#include "../app/mpe.h"
#include "../app/patch_mutate.h"
#include "../app/factory_bank.h"
#include "../app/fx_factory.h"
#include "../app/fx_rack_logic.h"
#include "dx7_fxrack_io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

// [ADICIONADO em 2026-10-28] ADAA no Fold: seno fixo em 1010 Hz dobrado com
// drive 8, SEM oversampling. Os harmonicos 25..47 (acima de 24 kHz) voltam
// como alias em frequencias que nao sao multiplas de 1010 (25o: 25250 ->
// 22750 Hz, 29o: 29290 -> 18710 Hz...). Soma o alias que cai abaixo de 20 kHz,
// relativo a fundamental. Sem ADAA dava -11.4 dB; com ADAA -20.8 dB.
static void testFoldAdaaReducesAliasing() {
    Synth synth;
    synth.init(48000.0, 4, 1);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].fixedFreq = true;
    v.op[0].fixedHz = 1010.0f;
    v.op[0].outputLevel = 99;
    v.op[0].fxType = FxType::Fold;
    v.op[0].fxParam1 = 8.0f;
    v.op[0].fxParam2 = 0.0f;
    for (int i = 0; i < 4; ++i) { v.op[0].fxEG.rate[i] = 99; v.op[0].fxEG.level[i] = 99; }
    for (int i = 1; i < kNumOps; ++i) for (int k = 0; k < 4; ++k) v.op[i].ampEG.level[k] = 0;
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 60, 100);
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); }
    std::vector<float> x;
    for (int i = 0; i < 48000; ++i) { float l, r; synth.renderStereo(l, r); x.push_back((l + r) * 0.5f); }
    const float fund = goertzelMagnitude(x, 1010.0, 48000.0);
    double alias = 0.0;
    for (int h = 25; h <= 47; h += 2) { // simetrico: so harmonicos impares
        double f = std::fmod(1010.0 * h, 48000.0);
        if (f > 24000.0) f = 48000.0 - f;
        if (f < 20000.0) { const double m = goertzelMagnitude(x, f, 48000.0); alias += m * m; }
    }
    const double db = 10.0 * std::log10(alias / (static_cast<double>(fund) * fund) + 1e-30);
    std::printf("  (alias audivel do Fold drive 8 em 1010 Hz, sem oversampling: %.1f dB da fundamental)\n", db);
    check(db < -17.0, "Fold com ADAA: alias audivel bem abaixo da fundamental mesmo sem oversampling");
}

// [ADICIONADO em 2026-10-28] Resposta do decimador half-band (63 taps, Kaiser):
// plano na banda audivel e rejeicao forte acima de ~28 kHz (o que voltaria
// como alias abaixo de 20 kHz).
static void testHalfbandDecimatorResponse() {
    auto gainDb = [](double f) {
        HalfbandDecimator2x d;
        double e = 0.0; int n = 0; float o;
        for (int i = 0; i < 96000; ++i)
            if (d.process(static_cast<float>(std::sin(2.0 * 3.14159265358979 * f * i / 96000.0)), o) && i > 2000) { e += o * o; ++n; }
        return 10.0 * std::log10(2.0 * e / n + 1e-30);
    };
    const double g1k = gainDb(1000.0), g18k = gainDb(18000.0), g20k = gainDb(20000.0), g30k = gainDb(30000.0), g40k = gainDb(40000.0);
    std::printf("  (decimador: 1k %.2f dB, 18k %.2f, 20k %.2f, 30k %.1f, 40k %.1f)\n", g1k, g18k, g20k, g30k, g40k);
    check(std::fabs(g1k) < 0.01 && std::fabs(g18k) < 0.05 && std::fabs(g20k) < 0.2, "decimador half-band: plano ate 20 kHz");
    check(g30k < -75.0 && g40k < -75.0, "decimador half-band: rejeita >= 75 dB a partir de 30 kHz");
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

// [2026-10-29] efeitos pos-mix agora sao o rack (dx7_fxrack.h); os testes dos
// efeitos antigos montam o painel antigo e publicam convertido
static void setLegacyFx(Synth& synth, const std::function<void(GlobalFxParams&)>& edit) {
    GlobalFxParams g;
    edit(g);
    synth.setFxRack(fxRackFromLegacy(g));
}

static void testChorusChangesStereoImage() {
    auto render = [](bool chorusOn) {
        Synth synth;
        synth.init(48000.0, 4);
        synth.part(0).voice = makeSimpleFmPatch();
        synth.recompile(0);
        setLegacyFx(synth, [&](GlobalFxParams& g) {
            g.chorus.enabled = chorusOn;
            g.chorus.mix = 0.5f;
            g.chorus.depth = 0.6f;
            g.chorus.rateHz = 1.2f;
        });
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
    const float delaySec = 0.2f;
    setLegacyFx(synth, [&](GlobalFxParams& g) {
        g.delay.enabled = true;
        g.delay.timeLeftSec = delaySec;
        g.delay.timeRightSec = delaySec;
        g.delay.feedback = 0.0f; // 1 eco so, sem repeticoes -- mais facil de medir
        g.delay.mix = 0.9f;
    });

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
        setLegacyFx(synth, [&](GlobalFxParams& g) {
            g.reverb.enabled = reverbOn;
            g.reverb.mix = 0.6f;
            g.reverb.roomSize = 0.8f;
            g.reverb.damping = 0.2f;
        });

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

static void testPolyphonyGainCatchesChordAttack() {
    // [ADICIONADO em 2026-10-28] Acorde atacado de uma vez: o ganho de
    // polifonia cai rapido (~1ms) quando a carga sobe, entao os primeiros
    // ms do acorde nao passam do nivel que ele tem depois de assentado (com
    // 30ms simetricos o ataque saia bem mais alto e batia no limitador).
    // Volume da parte baixo: o acorde assentado fica longe do limitador.
    auto peakOf = [](int from, int to) {
        Synth synth;
        synth.init(48000.0, 16);
        synth.part(0).level = 0.25f;
        synth.recompile(0);
        for (int n : {48, 55, 60, 64}) synth.noteOn(0, n, 70);
        float pk = 0.0f;
        for (int i = 0; i < to; ++i) {
            float l, r;
            synth.renderStereo(l, r);
            if (i >= from) pk = std::max({pk, std::fabs(l), std::fabs(r)});
        }
        return pk;
    };
    const float attack = peakOf(0, 1440), settled = peakOf(9600, 14400); // 0..30ms x 200..300ms
    std::printf("  (pico do acorde de 4 notas: ataque=%.3f, assentado=%.3f)\n", attack, settled);
    check(attack < settled * 1.25f, "ganho de polifonia: o ataque de um acorde de 4 notas nao sai mais alto que o acorde assentado");
}

static void testPolyphonyGainIsPerPart() {
    // [ADICIONADO em 2026-10-28] A compensacao de polifonia e por parte: um
    // acorde de 4 notas na Parte B nao pode baixar a nota da Parte A (com um
    // ganho so pra soma, caia ~7 dB). A em pan total a esquerda, B a direita:
    // o canal esquerdo e so a Parte A.
    auto leftRms = [](bool chordOnB) {
        Synth synth;
        synth.init(48000.0, 32);
        for (int p : {0, 1}) {
            synth.part(p).enabled = true;
            synth.part(p).level = 0.3f;
            synth.part(p).pan = p == 0 ? -1.0f : 1.0f;
            synth.recompile(p);
        }
        synth.noteOn(0, 36, 100);
        if (chordOnB) for (int n : {60, 64, 67, 71}) synth.noteOn(1, n, 100);
        double e = 0.0;
        for (int i = 0; i < 24000; ++i) { float l, r; synth.renderStereo(l, r); if (i >= 4800) e += static_cast<double>(l) * l; }
        return std::sqrt(e / 19200.0);
    };
    const double alone = leftRms(false), withChord = leftRms(true);
    std::printf("  (Parte A: sozinha %.4f, com acorde na Parte B %.4f)\n", alone, withChord);
    check(std::fabs(withChord / alone - 1.0) < 0.01, "ganho de polifonia por parte: acorde em outra parte nao baixa o volume desta");
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
    setLegacyFx(synth, [](GlobalFxParams& g) { g.reverb.enabled = true; g.delay.enabled = true; });
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

// --- [ADICIONADO em 2026-10-28] MPE ---
static void testMpeInputTranslatesMemberChannels() {
    using namespace dx7fm_app;
    MpeInput mpe;
    std::vector<AppEvent> out;
    auto ev = [](MidiEvent::Type t, int ch, int d1, int d2) { MidiEvent e; e.type = t; e.channel = ch; e.data1 = d1; e.data2 = d2; return e; };

    // desligado: nada e consumido
    check(!mpe.handle(ev(MidiEvent::Type::NoteOn, 1, 60, 100), out) && out.empty(), "MPE desligado: canal 2 segue o caminho normal");

    // MCM (RPN 6 no canal 1 = indice 0) com 7 canais-membro liga o MPE
    mpe.handle(ev(MidiEvent::Type::ControlChange, 0, 101, 0), out);
    mpe.handle(ev(MidiEvent::Type::ControlChange, 0, 100, 6), out);
    const bool mcm = mpe.handle(ev(MidiEvent::Type::ControlChange, 0, 6, 7), out);
    check(mcm && mpe.enabled && mpe.memberChannels == 7, "MPE: mensagem de configuracao (RPN 6) liga a zona com N canais-membro");

    // valores iniciais mandados ANTES do note on (bend +1/4, pressao, CC74)
    out.clear();
    mpe.handle(ev(MidiEvent::Type::PitchBend, 2, 0, 2048), out);
    mpe.handle(ev(MidiEvent::Type::ChannelAftertouch, 2, 0, 127), out);
    mpe.handle(ev(MidiEvent::Type::ControlChange, 2, 74, 0), out);
    check(out.empty(), "MPE: controles num canal sem nota so guardam o valor");
    mpe.handle(ev(MidiEvent::Type::NoteOn, 2, 64, 90), out);
    const bool onOk = out.size() == 4 && out[0].kind == AppEvent::Kind::NoteOn && out[0].channel1to16 == 1 && out[0].note == 64 &&
                      out[1].kind == AppEvent::Kind::NotePitchBend && std::fabs(out[1].floatValue - 12.0f) < 1e-4f &&
                      out[2].kind == AppEvent::Kind::PolyAftertouch && out[2].floatValue == 1.0f &&
                      out[3].kind == AppEvent::Kind::NoteSlide && out[3].floatValue == 0.0f;
    check(onOk, "MPE: note on num canal-membro sai no canal mestre + bend (48 st de faixa)/pressao/slide iniciais da nota");

    out.clear();
    mpe.handle(ev(MidiEvent::Type::ControlChange, 2, 74, 127), out);
    mpe.handle(ev(MidiEvent::Type::PitchBend, 2, 0, -8192), out);
    check(out.size() == 2 && out[0].kind == AppEvent::Kind::NoteSlide && out[0].note == 64 && out[0].floatValue == 1.0f &&
              out[1].kind == AppEvent::Kind::NotePitchBend && std::fabs(out[1].floatValue + 48.0f) < 1e-4f,
          "MPE: CC74 e bend do canal vao so pra nota daquele canal");

    // RPN 0 num canal-membro muda a faixa do bend por nota
    mpe.handle(ev(MidiEvent::Type::ControlChange, 2, 101, 0), out);
    mpe.handle(ev(MidiEvent::Type::ControlChange, 2, 100, 0), out);
    mpe.handle(ev(MidiEvent::Type::ControlChange, 2, 6, 24), out);
    check(mpe.noteBendRange == 24.0f, "MPE: RPN 0 num canal-membro muda a faixa do bend por nota");

    // canal mestre e canal fora da zona seguem o caminho normal
    out.clear();
    const bool master = mpe.handle(ev(MidiEvent::Type::PitchBend, 0, 0, 4000), out);
    const bool outside = mpe.handle(ev(MidiEvent::Type::NoteOn, 9, 60, 100), out);
    check(!master && !outside && out.empty(), "MPE: canal mestre e canais fora da zona nao sao consumidos");
}

static void testMpeBendsOnlyItsNote() {
    // duas notas (C4 e G4); bend de +12 so na C4 -> aparece C5 (523 Hz), o
    // G4 (392 Hz) continua, nao sobra C4 (262 Hz)
    using namespace dx7fm_app;
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 90;
    synth.part(0).voice = v;
    synth.recompile(0);
    std::atomic<bool> enabled[kNumParts];
    std::atomic<int> channel[kNumParts];
    for (int p = 0; p < kNumParts; ++p) { enabled[p] = (p == 0); channel[p] = 1; }
    NoteRouting routing;
    dx7fm_app::MpeInput mpe;
    mpe.enabled = true;
    std::vector<AppEvent> evs;
    auto midi = [&](dx7fm_app::MidiEvent::Type t, int ch, int d1, int d2) {
        dx7fm_app::MidiEvent e; e.type = t; e.channel = ch; e.data1 = d1; e.data2 = d2;
        evs.clear();
        mpe.handle(e, evs);
        for (const auto& a : evs) applyAppEvent(synth, a, enabled, channel, routing);
    };
    midi(dx7fm_app::MidiEvent::Type::NoteOn, 1, 60, 100);
    midi(dx7fm_app::MidiEvent::Type::NoteOn, 2, 67, 100);
    midi(dx7fm_app::MidiEvent::Type::PitchBend, 1, 0, 2048); // 2048/8192 x 48 = +12
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); }
    std::vector<float> x;
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); x.push_back(l + r); }
    const float c4 = goertzelMagnitude(x, 261.63, 48000.0), g4 = goertzelMagnitude(x, 392.0, 48000.0), c5 = goertzelMagnitude(x, 523.25, 48000.0);
    std::printf("  (magnitudes: C4 %.4f, G4 %.4f, C5 %.4f)\n", c4, g4, c5);
    check(c5 > 10.0f * c4 && g4 > 10.0f * c4 && std::fabs(c5 / g4 - 1.0f) < 0.3f, "MPE: o bend do canal 2 sobe so a nota dele (C4 -> C5); o G4 fica");

    // nota solta e tocada de novo: voz nova comeca sem o bend nem a pressao antiga
    midi(dx7fm_app::MidiEvent::Type::NoteOff, 1, 60, 0);
    synth.polyAftertouch(0, 67, 0.8f);
    midi(dx7fm_app::MidiEvent::Type::NoteOff, 2, 67, 0);
    for (int i = 0; i < 48000 * 2 && synth.activeVoiceCount() > 0; ++i) { float l, r; synth.renderStereo(l, r); }
    synth.noteOn(0, 67, 100); // caminho normal (sem MPE): nada de expressao herdada
    x.clear();
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); }
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); x.push_back(l + r); }
    const float g4b = goertzelMagnitude(x, 392.0, 48000.0), up = goertzelMagnitude(x, 784.0, 48000.0);
    check(g4b > 10.0f * up, "MPE: voz reaproveitada nao herda o bend da nota anterior");
}

static void testMpeSlideSource() {
    // Slide -> VoiceLevel -1: slide 1 cala SO a nota que recebeu o slide
    Synth synth;
    synth.init(48000.0, 8);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 90;
    v.modMatrix[0] = ModRoute{ModSource::Slide, ModDest::VoiceLevel, 0, -1.0f};
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 60, 100);
    synth.noteOn(0, 67, 100);
    synth.noteSlide(0, 60, 1.0f);
    for (int i = 0; i < 4800; ++i) { float l, r; synth.renderStereo(l, r); }
    std::vector<float> x;
    for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); x.push_back(l + r); }
    const float c4 = goertzelMagnitude(x, 261.63, 48000.0), g4 = goertzelMagnitude(x, 392.0, 48000.0);
    std::printf("  (Slide -> VoiceLevel -1 na C4: C4 %.4f, G4 %.4f)\n", c4, g4);
    check(g4 > 0.1f && c4 < 0.03f * g4, "MPE: fonte Slide (CC74) na matriz age so na nota que recebeu");
    const JsonValue j = voiceParamsToJson(v);
    const VoiceParams back = voiceParamsFromJson(JsonValue::parse(j.dump()));
    check(back.modMatrix[0].src == ModSource::Slide, "fonte Slide salva e carrega no JSON do patch");
}

// [ADICIONADO em 2026-10-29] volume do patch (VoiceParams::volumeDb)
static void testPatchVolume() {
    auto rmsWith = [](float db) {
        Synth synth;
        synth.init(48000.0, 4);
        synth.part(0).voice.volumeDb = db;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);
        double e = 0.0;
        for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); if (i >= 4800) e += static_cast<double>(l) * l; }
        return std::sqrt(e / 4800.0);
    };
    const double full = rmsWith(0.0f), half = rmsWith(-6.0206f);
    check(std::fabs(half / full - 0.5) < 0.005, "volume do patch: -6 dB = metade da amplitude");
    VoiceParams v;
    v.volumeDb = -7.5f;
    check(voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(v).dump())).volumeDb == -7.5f &&
              voiceParamsToJson(VoiceParams{}).find("volumeDb") == nullptr,
          "volume do patch: salva/carrega no JSON (e nao aparece com 0 dB, como nos patches antigos)");
}

// --- [ADICIONADO em 2026-10-28] Mutar ---
static void testMutatePatch() {
    using dx7fm_app::mutatePatch;
    const auto bank = dx7fm_app::factory::factoryBank();
    const PartParams& piano = bank[0].part; // Piano Eletrico FM: ops 0..3 em uso, 4..7 desligados
    auto json = [](const VoiceParams& v) { return voiceParamsToJson(v).dump(); };

    VoiceParams a = piano.voice;
    mutatePatch(a, 0.0f, 1);
    check(json(a) == json(piano.voice), "Mutar com 0% nao muda nada");

    VoiceParams b = piano.voice, c = piano.voice;
    mutatePatch(b, 0.4f, 77);
    mutatePatch(c, 0.4f, 77);
    check(json(b) == json(c) && json(b) != json(piano.voice), "Mutar: mesma semente = mesmo resultado; muda o som");

    bool unusedSame = true, carrierLevelsClose = true;
    for (int i = 4; i < kNumOps; ++i) unusedSame &= operatorParamsToJson(b.op[i]).dump() == operatorParamsToJson(piano.voice.op[i]).dump();
    for (int i : {0, 2}) carrierLevelsClose &= std::abs(b.op[i].outputLevel - piano.voice.op[i].outputLevel) <= 2;
    check(unusedSame, "Mutar: operadores fora de uso ficam intactos");
    const bool sameAlgo = std::memcmp(b.matrix.mod, piano.voice.matrix.mod, sizeof b.matrix.mod) == 0 &&
                          std::memcmp(b.matrix.outputMix, piano.voice.matrix.outputMix, sizeof b.matrix.outputMix) == 0;
    check(carrierLevelsClose && sameAlgo, "Mutar: portadoras mudam pouco de nivel; o algoritmo fica");

    // 20 mutacoes de 50% de cada patch de fabrica: sempre soam, sem NaN, volume
    // na mesma faixa. Volume = pico do RMS em janelas de 100 ms em 3 s (nao
    // depende de quanto o ataque/decaimento mudou)
    int bad = 0;
    double worstDb = 0.0;
    for (const auto& f : bank) {
        auto rmsOf = [&](const PartParams& p) {
            Synth s;
            s.init(48000.0, 16);
            s.part(0) = p;
            s.recompile(0);
            for (int k = 0; k < VoiceParams::kNumMacros; ++k) s.setMacro(0, k, p.voice.macro[k]);
            s.noteOn(0, 60, 100);
            double best = 0.0;
            bool finite = true;
            for (int w = 0; w < 30; ++w) {
                double e = 0.0;
                for (int i = 0; i < 4800; ++i) { float l, r; s.renderStereo(l, r); finite &= std::isfinite(l); e += static_cast<double>(l) * l; }
                best = std::max(best, std::sqrt(e / 4800.0));
            }
            return finite ? best : -1.0;
        };
        const double ref = rmsOf(f.part);
        for (uint32_t seed = 1; seed <= 20; ++seed) {
            PartParams m = f.part;
            mutatePatch(m.voice, 0.5f, seed);
            sanitize(m);
            const double r = rmsOf(m);
            const double db = r > 0.0 ? 20.0 * std::log10(r / ref) : -999.0;
            if (r < 0.0 || std::fabs(db) > 12.0) { ++bad; std::printf("    %s semente %u: %.1f dB\n", f.part.name.c_str(), seed, db); }
            worstDb = std::max(worstDb, std::fabs(db));
        }
    }
    std::printf("  (240 mutacoes de 50%%: %d fora de +-12 dB ou com NaN; maior variacao %.1f dB)\n", bad, worstDb);
    check(bad == 0, "Mutar 50%: todo patch de fabrica continua soando, sem NaN, volume dentro de +-12 dB");
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
    setLegacyFx(synth, [](GlobalFxParams& g) { g.chorus.enabled = true; g.delay.enabled = true; });

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

// --- [ADICIONADO em 2026-10-20] graficos do painel de efeito (app/op_fx_math.h) ---
// O grafico tem que mostrar o que o DSP faz: mede o filtro/comb de verdade com
// senos e compara com a curva desenhada.
static float measureGainDb(const std::function<float(float)>& process, float hz) {
    std::vector<float> out;
    for (int i = 0; i < 48000; ++i) out.push_back(process(std::sin(2.0f * static_cast<float>(M_PI) * hz * static_cast<float>(i) / 48000.0f)));
    std::vector<float> tail(out.begin() + 24000, out.end()); // regime permanente
    return 20.0f * std::log10(computeRms(tail, 0, tail.size()) / std::sqrt(0.5f));
}

static void testOpFxFilterCurveMatchesDsp() {
    using dx7fm_app::opfx::svfMagnitudeDb;
    float worst = 0.0f;
    for (FxType t : {FxType::LowPass, FxType::BandPass, FxType::HighPass})
        for (float q : {0.707f, 4.0f})
            for (float f : {100.0f, 800.0f, 2000.0f, 5000.0f, 15000.0f}) {
                TPTStateVariableFilter flt;
                flt.setSampleRate(48000.0);
                flt.setParams(2000.0f, q);
                float got = measureGainDb([&](float x) { auto o = flt.process(x); return t == FxType::LowPass ? o.lp : t == FxType::BandPass ? o.bp : o.hp; }, f);
                float drawn = svfMagnitudeDb(t, f, 2000.0f, q, 48000.0f);
                if (drawn > -40.0f) worst = std::max(worst, std::fabs(got - drawn));
            }
    std::printf("  (filtros: maior diferenca grafico x DSP medido = %.3f dB)\n", worst);
    check(worst < 0.1f, "painel de efeito: curva dos filtros LP/BP/HP bate com o TPTStateVariableFilter medido (0.1 dB)");
}

static void testOpFxCombCurveMatchesDsp() {
    using dx7fm_app::opfx::combMagnitudeDb;
    float worst = 0.0f;
    for (float f : {480.0f, 720.0f, 1000.0f, 960.0f, 3000.0f}) { // 480 Hz = atraso inteiro de 100 amostras
        CombFilter c;
        c.setSampleRate(48000.0);
        float got = measureGainDb([&](float x) { return c.process(x, 480.0f, 0.7f); }, f);
        worst = std::max(worst, std::fabs(got - combMagnitudeDb(f, 480.0f, 0.7f)));
    }
    std::printf("  (comb: maior diferenca grafico x DSP medido = %.3f dB)\n", worst);
    check(worst < 0.1f, "painel de efeito: dentes do comb no grafico batem com o CombFilter medido (0.1 dB)");
}

static void testOpFxDefaultsAndSummary() {
    using namespace dx7fm_app::opfx;
    float p1 = 1234.0f, p2 = 3.0f;
    applyDefaults(FxType::LowPass, FxType::HighPass, p1, p2);
    bool keep = p1 == 1234.0f && p2 == 3.0f;
    applyDefaults(FxType::LowPass, FxType::Fold, p1, p2);
    bool fold = p1 == 3.0f && p2 == 0.0f;
    applyDefaults(FxType::Fold, FxType::Crush, p1, p2);
    bool crush = p1 == 8000.0f && p2 * 16.0f == 8.0f;
    check(keep && fold && crush, "painel de efeito: trocar entre filtros mantem corte/Q; trocar de familia poe valores sensatos");
    OperatorParams op;
    op.fxType = FxType::Crush;
    op.fxParam1 = 8000.0f;
    op.fxParam2 = 0.5f;
    char a[48], b[48];
    summary(op, a, sizeof a);
    op.fxType = FxType::LowPass;
    op.fxParam1 = 2000.0f;
    op.fxParam2 = 0.707f;
    summary(op, b, sizeof b);
    check(std::string(a) == "8.0 kHz  8 bits" && std::string(b) == "2.0 kHz  Q 0.7", "painel de efeito: resumo na tabela com nome e unidade");
    bool same = true;
    for (float x = -1.0f; x <= 1.0f; x += 0.01f) same &= shape(FxType::Fold, x, 4.0f, 0.3f, 1.0f) == triangleFold(x * 4.0f + 0.3f);
    check(same, "painel de efeito: curva do Wavefolder = triangleFold do motor com o drive maximo");
}

// --- [ADICIONADO em 2026-10-20] key tracking do efeito + fontes Nota/Velocity ---
// Op0 Sine sozinho na saida (nivel 80, ~0.19 de pico: longe do limitador).
static std::vector<float> renderOp0(const std::function<void(VoiceParams&)>& setup, int note, int velocity = 100, int samples = 24000) {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 80;
    setup(v);
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, note, velocity);
    std::vector<float> out;
    for (int i = 0; i < samples; ++i) { float l, r; synth.renderStereo(l, r); out.push_back(l); }
    return std::vector<float>(out.begin() + samples / 2, out.end());
}

static void testFxKeyTrackMovesFilterWithNote() {
    // passa-baixa em 300 Hz (no C4); tocando C6 (1046.5 Hz): com key track 100%
    // o corte vai pra 1200 Hz (2 oitavas acima), sem key track fica em 300 Hz.
    auto lp = [](float kt) {
        return [kt](VoiceParams& v) { v.op[0].fxType = FxType::LowPass; v.op[0].fxParam1 = 300.0f; v.op[0].fxParam2 = 0.707f; v.op[0].fxKeyTrack = kt; };
    };
    const float dry = computeRms(renderOp0([](VoiceParams&) {}, 84), 0, 12000);
    auto gainDb = [&](float kt) { return 20.0f * std::log10(computeRms(renderOp0(lp(kt), 84), 0, 12000) / dry); };
    const float f = 1046.5f;
    float g0 = gainDb(0.0f), g1 = gainDb(1.0f), gHalf = gainDb(0.5f);
    float e0 = dx7fm_app::opfx::svfMagnitudeDb(FxType::LowPass, f, 300.0f, 0.707f, 48000.0f);
    float e1 = dx7fm_app::opfx::svfMagnitudeDb(FxType::LowPass, f, 1200.0f, 0.707f, 48000.0f);
    float eHalf = dx7fm_app::opfx::svfMagnitudeDb(FxType::LowPass, f, 600.0f, 0.707f, 48000.0f);
    std::printf("  (C6 num passa-baixa de 300Hz: key track 0%% %.1f dB (esperado %.1f), 50%% %.1f (%.1f), 100%% %.1f (%.1f))\n", g0, e0, gHalf,
                eHalf, g1, e1);
    check(std::fabs(g0 - e0) < 0.5f && std::fabs(gHalf - eHalf) < 0.5f && std::fabs(g1 - e1) < 0.5f,
          "key track do efeito: o corte do filtro sobe com a nota (100% = 1 oitava por oitava, 50% = meia)");
    // no C4 o key track nao muda nada
    auto c4a = renderOp0(lp(0.0f), 60), c4b = renderOp0(lp(1.0f), 60);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < c4a.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(c4a[i] - c4b[i]));
    check(maxDiff < 1e-4f, "key track do efeito: no C4 (centro) o efeito fica igual a sem key track");
}

static void testModSourcesKeyNoteAndVelocity() {
    auto level = [](ModSource src, float depth, int note, int vel) {
        auto x = renderOp0([&](VoiceParams& v) { v.modMatrix[0] = ModRoute{src, ModDest::OperatorLevel, 0, depth}; }, note, vel);
        return computeRms(x, 0, x.size());
    };
    // Nota -> nivel, profundidade -0.5: C4 intacto; C9 (nota 120, fonte = +1) pela metade
    float ref60 = computeRms(renderOp0([](VoiceParams&) {}, 60), 0, 12000);
    float ref120 = computeRms(renderOp0([](VoiceParams&) {}, 120), 0, 12000);
    float n60 = level(ModSource::KeyNote, -0.5f, 60, 100), n120 = level(ModSource::KeyNote, -0.5f, 120, 100);
    std::printf("  (Nota -> nivel -0.5: C4 x%.3f, C9 x%.3f)\n", n60 / ref60, n120 / ref120);
    check(std::fabs(n60 / ref60 - 1.0f) < 0.01f && std::fabs(n120 / ref120 - 0.5f) < 0.02f,
          "mod matrix: fonte Nota vale 0 no C4 e +1 cinco oitavas acima");
    // Velocity -> nivel, profundidade 1: ganho (1 + vel/127)
    float v127 = level(ModSource::Velocity, 1.0f, 60, 127), v64 = level(ModSource::Velocity, 1.0f, 60, 64);
    std::printf("  (Velocity -> nivel +1: 127/64 = %.3f, esperado %.3f)\n", v127 / v64, 2.0f / (1.0f + 64.0f / 127.0f));
    check(std::fabs(v127 / v64 - 2.0f / (1.0f + 64.0f / 127.0f)) < 0.02f, "mod matrix: fonte Velocity = velocity/127");

    VoiceParams v;
    v.op[2].fxKeyTrack = 0.75f;
    v.modMatrix[1] = ModRoute{ModSource::KeyNote, ModDest::OperatorFx1, 2, 0.3f};
    v.modMatrix[2] = ModRoute{ModSource::Velocity, ModDest::OperatorMorph, 1, -0.4f};
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(v).dump()));
    // patch "antigo": JSON com key track 0.75 e a chave fxKeyTrack apagada do texto
    std::string old = voiceParamsToJson(v).dump();
    int removed = 0;
    for (size_t at; (at = old.find("\"fxKeyTrack\"")) != std::string::npos; ++removed)
        old.erase(at, old.find(',', at) + 1 - at);
    VoiceParams fromOld = voiceParamsFromJson(JsonValue::parse(old));
    check(back.op[2].fxKeyTrack == 0.75f && back.modMatrix[1].src == ModSource::KeyNote && back.modMatrix[2].src == ModSource::Velocity &&
              removed == kNumOps && fromOld.op[2].fxKeyTrack == 0.0f,
          "key track e fontes Nota/Velocity sobrevivem ao JSON; patch sem a chave carrega com key track 0");
}

// --- [ADICIONADO em 2026-10-22] Waveform::Wavetable (dx7_wavetable.h + dx7_wavegen.h) ---
static void setWavetable(VoiceParams& v, wavegen::Wave a, wavegen::Wave b, float position, bool spectral = true) {
    auto& o = v.op[0];
    o.waveform = Waveform::Wavetable;
    o.wavetable.waveA = a.empty() ? nullptr : wavegen::share(std::move(a));
    o.wavetable.waveB = b.empty() ? nullptr : wavegen::share(std::move(b));
    o.wavetable.position = position;
    o.wavetable.spectralMorph = spectral;
}

// magnitude do harmonico h de um ciclo (DFT de 1 bin)
static float cycleHarmonic(const wavegen::Wave& w, int h) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < w.size(); ++i) {
        double ph = 2.0 * M_PI * h * static_cast<double>(i) / static_cast<double>(w.size());
        re += w[i] * std::cos(ph);
        im += w[i] * std::sin(ph);
    }
    return static_cast<float>(2.0 * std::hypot(re, im) / static_cast<double>(w.size()));
}

static void testWavetableSineMatchesSineOperator() {
    auto ref = renderOp0([](VoiceParams&) {}, 69);
    auto wt = renderOp0([](VoiceParams& v) { setWavetable(v, wavegen::fromShape(Waveform::Sine), {}, 0.3f); }, 69);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < ref.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(ref[i] - wt[i]));
    std::printf("  (Wavetable seno x operador Sine: maior diferenca %.2e)\n", maxDiff);
    check(maxDiff < 1e-3f, "Wavetable com A e B = seno soa igual ao operador Sine");
}

static void testWavetableAntiAliasing() {
    // serra em ~2960 Hz (nota 102): harmonicos 9..16 passam de Nyquist e,
    // na serra ingenua, voltam espelhados (alias) em 48000 - h*f
    const float f = 440.0f * std::pow(2.0f, (102 - 69) / 12.0f);
    auto naive = renderOp0([](VoiceParams& v) { v.op[0].waveform = Waveform::Saw; }, 102);
    auto table = renderOp0([](VoiceParams& v) { setWavetable(v, wavegen::fromShape(Waveform::Saw), {}, 0.0f); }, 102);
    auto aliasEnergy = [&](const std::vector<float>& x) {
        double e = 0.0;
        for (int h = 9; h <= 16; ++h) { float a = std::fabs(48000.0f - h * f); e += mag(x, a) * mag(x, a); }
        return std::sqrt(e);
    };
    float an = aliasEnergy(naive), at = aliasEnergy(table);
    float fundN = mag(naive, f), fundT = mag(table, f);
    std::printf("  (serra em %.0f Hz: alias/fundamental ingenua %.4f, wavetable %.6f)\n", f, an / fundN, at / fundT);
    check(at / fundT < 0.01f * (an / fundN), "Wavetable: mipmaps cortam os harmonicos acima de Nyquist (alias 40 dB abaixo da serra ingenua)");
}

static void testWavetableMorphModes() {
    // crossfade e linear no morph: posicao 0.5 = media de 0 e 1
    auto saw = wavegen::fromShape(Waveform::Saw), sq = wavegen::fromShape(Waveform::Square);
    auto at = [&](float pos, bool spectral, wavegen::Wave a, wavegen::Wave b) {
        return renderOp0([&](VoiceParams& v) { setWavetable(v, a, b, pos, spectral); }, 57);
    };
    auto x0 = at(0.0f, false, saw, sq), x1 = at(1.0f, false, saw, sq), xm = at(0.5f, false, saw, sq);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < xm.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(xm[i] - 0.5f * (x0[i] + x1[i])));
    check(maxDiff < 1e-5f, "Wavetable crossfade: posicao 0.5 = (A + B) / 2 amostra a amostra");

    // serra -> serra invertida: crossfade cancela no meio, espectral nao
    auto inv = saw;
    wavegen::invert(inv);
    float rEnd = computeRms(at(0.0f, true, saw, inv), 0, 12000);
    float rCross = computeRms(at(0.5f, false, saw, inv), 0, 12000);
    float rSpec = computeRms(at(0.5f, true, saw, inv), 0, 12000);
    std::printf("  (serra -> serra invertida no meio do morph: crossfade %.3f, espectral %.3f do volume das pontas)\n", rCross / rEnd, rSpec / rEnd);
    check(rCross / rEnd < 0.01f && rSpec / rEnd > 0.9f, "Wavetable espectral: morph entre ondas de fase oposta nao passa pelo silencio (o crossfade passa)");

    // serra -> quadrada: os harmonicos impares tem fase oposta (180 graus, sentido
    // ambiguo). Girar uns pra +90 e outros pra -90 deixava o meio do morph com
    // cara de ruido (variacao total 74.6 contra 4 nas pontas); sentido unico: ~17
    // (o resto e o ripple de Gibbs de 1023 harmonicos)
    WavetableBank bank(&saw, &sq, true);
    float mid[kWaveSize];
    bank.preview(0.5f, mid, kWaveSize);
    double tv = 0.0;
    for (int i = 0; i < kWaveSize; ++i) tv += std::fabs(mid[(i + 1) % kWaveSize] - mid[i]);
    std::printf("  (serra -> quadrada, meio do morph espectral: variacao total %.1f)\n", tv);
    check(tv < 30.0, "Wavetable espectral: fases opostas giram no mesmo sentido (meio do morph sem cara de ruido)");
}

static void testWavetableMorphModulation() {
    // A = so o harmonico 1, B = so o 2: a energia em 2f mostra onde o morph esta
    const float one[2] = {1.0f, 0.0f}, two[2] = {0.0f, 1.0f};
    auto a = wavegen::fromHarmonics(one, 2), b = wavegen::fromHarmonics(two, 2);
    auto share2f = [&](const std::function<void(VoiceParams&)>& extra, int vel = 100) {
        auto x = renderOp0([&](VoiceParams& v) { setWavetable(v, a, b, 0.0f); extra(v); }, 69, vel);
        float m1 = mag(x, 440.0), m2 = mag(x, 880.0);
        return m2 / (m1 + m2);
    };
    float none = share2f([](VoiceParams&) {});
    float knob = share2f([](VoiceParams& v) { v.op[0].wavetable.position = 1.0f; });
    float eg = share2f([](VoiceParams& v) { v.op[0].morphDepth = 1.0f; }); // fxEG padrao fica em 99
    float matrix = share2f([](VoiceParams& v) { v.modMatrix[0] = ModRoute{ModSource::Velocity, ModDest::OperatorMorph, 0, 1.0f}; }, 127);
    float half = share2f([](VoiceParams& v) { v.modMatrix[0] = ModRoute{ModSource::Velocity, ModDest::OperatorMorph, 0, 0.5f}; }, 127);
    std::printf("  (fracao em 2f: sem morph %.3f, knob %.3f, FX EG %.3f, matriz %.3f, matriz 0.5 %.3f)\n", none, knob, eg, matrix, half);
    check(none < 0.01f && knob > 0.99f && eg > 0.99f && matrix > 0.99f && half > 0.3f && half < 0.7f,
          "Wavetable: morph movido pelo knob de posicao, pelo FX EG e pela mod matrix (OperatorMorph)");
}

static void testWavetableJsonAndSanitize() {
    VoiceParams v;
    setWavetable(v, wavegen::fromShape(Waveform::Saw), wavegen::pulse(0.25f), 0.4f, false);
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(v).dump()));
    const auto& o = back.op[0].wavetable;
    float maxDiff = 0.0f;
    for (int i = 0; i < kWaveSize; ++i) {
        maxDiff = std::max(maxDiff, std::fabs((*o.waveA)[static_cast<size_t>(i)] - (*v.op[0].wavetable.waveA)[static_cast<size_t>(i)]));
        maxDiff = std::max(maxDiff, std::fabs((*o.waveB)[static_cast<size_t>(i)] - (*v.op[0].wavetable.waveB)[static_cast<size_t>(i)]));
    }
    check(back.op[0].waveform == Waveform::Wavetable && maxDiff <= 1e-5f && o.position == 0.4f && !o.spectralMorph,
          "Wavetable: ondas, posicao e modo sobrevivem ao JSON (erro <= 1e-5)");
    VoiceParams fresh = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(VoiceParams{}).dump()));
    OperatorParams odd;
    odd.wavetable.waveA = std::make_shared<const std::vector<float>>(512, 0.5f);
    odd.wavetable.waveB = std::make_shared<const std::vector<float>>(kWaveSize, std::nanf(""));
    sanitize(odd);
    check(!fresh.op[0].wavetable.waveA && odd.wavetable.waveA && odd.wavetable.waveA->size() == static_cast<size_t>(kWaveSize) &&
              !odd.wavetable.waveB,
          "Wavetable: patch sem ondas carrega como seno; onda de tamanho errado e reamostrada, com NaN vira seno");
}

static void testWavegenImport() {
    // wavetable de 4 frames (frame f = seno no harmonico f+1)
    std::vector<float> table;
    for (int f = 0; f < 4; ++f)
        for (int i = 0; i < kWaveSize; ++i) table.push_back(std::sin(2.0f * static_cast<float>(M_PI) * (f + 1) * i / kWaveSize));
    auto frame2 = wavegen::fromWavFrame(table, 2);
    bool same = wavegen::wavetableFrames(table) == 4;
    for (int i = 0; i < kWaveSize; ++i) same &= frame2[static_cast<size_t>(i)] == table[static_cast<size_t>(2 * kWaveSize + i)];
    check(same, "Wavetable/WAV: arquivo de wavetable (4 frames de 2048) e o frame escolhido vem identico");

    // som comum: seno de 440 Hz com DC -> um periodo, sem DC, comecando na subida do zero
    std::vector<float> tone(48000);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.1f + 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * 440.0f * static_cast<float>(i) / 48000.0f);
    float f0 = 0.0f;
    auto per = wavegen::fromWavPeriod(tone, 48000.0, 0.5, &f0);
    auto ref = wavegen::fromShape(Waveform::Sine);
    float maxDiff = per.empty() ? 1.0f : 0.0f;
    for (size_t i = 0; i < per.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(per[i] - ref[i]));
    std::printf("  (periodo de um seno de 440 Hz: f0 %.2f Hz, diferenca do seno %.3f)\n", f0, maxDiff);
    check(std::fabs(f0 - 440.0f) < 0.5f && maxDiff < 0.05f, "Wavetable/WAV: um periodo de um som comum vira um ciclo (f0 detectado, DC tirado)");

    float amp[8];
    for (int k = 0; k < 8; ++k) amp[k] = 1.0f / (k + 1);
    auto saw = wavegen::fromHarmonics(amp, 8);
    float h1 = cycleHarmonic(saw, 1);
    check(std::fabs(cycleHarmonic(saw, 2) / h1 - 0.5f) < 1e-3f && std::fabs(cycleHarmonic(saw, 5) / h1 - 0.2f) < 1e-3f &&
              cycleHarmonic(saw, 9) < 1e-4f,
          "Wavetable/harmonicos: onda gerada tem exatamente as amplitudes pedidas (1/k) e nada acima");
}

static void testWavetableBankCache() {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    setWavetable(v, wavegen::fromShape(Waveform::Saw), wavegen::fromShape(Waveform::Square), 0.0f);
    v.op[3].waveform = Waveform::Wavetable;
    synth.part(0).voice = v;
    synth.recompile(0);
    const void* b0 = synth.debugLiveSnapshot(0)->wavetables[0].get();
    const void* b3 = synth.debugLiveSnapshot(0)->wavetables[3].get();
    synth.part(0).voice.op[0].outputLevel = 50;
    synth.part(0).voice.op[0].wavetable.position = 0.7f;
    synth.recompile(0);
    bool kept = synth.debugLiveSnapshot(0)->wavetables[0].get() == b0 && synth.debugLiveSnapshot(0)->wavetables[3].get() == b3;
    synth.part(0).voice.op[0].wavetable.waveA = wavegen::share(wavegen::pulse(0.1f));
    synth.recompile(0);
    bool swapped = synth.debugLiveSnapshot(0)->wavetables[0].get() != b0 && synth.debugLiveSnapshot(0)->wavetables[3].get() == b3;
    check(b0 && b3 && kept && swapped && !synth.debugLiveSnapshot(0)->wavetables[1],
          "Wavetable: tabelas em cache no recompile (outros parametros nao remontam; trocar a onda A remonta so aquele operador)");
}

static void testWavetableCost() {
    auto timeIt = [](bool wavetable) {
        Synth synth;
        synth.init(48000.0, 16);
        VoiceParams v;
        applyAlgorithm(v, 1);
        for (auto& op : v.op) {
            op.outputLevel = 80;
            if (wavetable) {
                op.waveform = Waveform::Wavetable;
                op.wavetable.waveA = wavegen::share(wavegen::fromShape(Waveform::Saw));
                op.wavetable.waveB = wavegen::share(wavegen::fromShape(Waveform::Square));
                op.wavetable.position = 0.37f;
            }
        }
        synth.part(0).voice = v;
        synth.recompile(0);
        for (int n = 0; n < 16; ++n) synth.noteOn(0, 40 + n, 100);
        auto t0 = std::chrono::steady_clock::now();
        float sink = 0.0f, L[256], R[256];
        for (int b = 0; b < 48000 / 256; ++b) { synth.renderBlock(L, R, 256); sink += L[0]; }
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return sink == 12345.0f ? 0.0 : sec;
    };
    double sine = timeIt(false), wt = timeIt(true);
    std::printf("  (16 vozes x 8 ops, 1s: Sine=%.3fs, Wavetable espectral=%.3fs, razao=%.2fx)\n", sine, wt, wt / sine);
    check(wt < sine * 2.0, "Wavetable custa menos que 2x o operador Sine");
}

// --- [ADICIONADO em 2026-10-22] envelopes de modulacao, LFO3/4 com sync e destinos novos ---
// Op0 Sine sozinho (nivel 80), estereo, com BPM escolhido.
struct StereoRender { std::vector<float> l, r; };
static StereoRender renderVoice(const std::function<void(VoiceParams&)>& setup, int note, int velocity, double seconds, float bpm = 120.0f) {
    Synth synth;
    synth.init(48000.0, 4);
    synth.setTempoBpm(bpm);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 80;
    setup(v);
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, note, velocity);
    StereoRender out;
    const int n = static_cast<int>(seconds * 48000.0);
    out.l.resize(static_cast<size_t>(n));
    out.r.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; i += 256) synth.renderBlock(&out.l[static_cast<size_t>(i)], &out.r[static_cast<size_t>(i)], std::min(256, n - i));
    return out;
}
static float rmsAt(const std::vector<float>& x, double centerSec, double widthSec = 0.04) {
    return computeRms(x, static_cast<size_t>((centerSec - widthSec / 2) * 48000.0), static_cast<size_t>(widthSec * 48000.0));
}

static void testModEnvelopeAsSource() {
    // Env1 sobe de 0 a 99 em T segundos (rate 30); Env1 -> nivel do op0 com +1: ganho 1 -> 2
    const uint8_t rate = 30;
    const double T = dx7fm_app::egStageSeconds(0.0f, 99.0f, rate);
    auto x = renderVoice([&](VoiceParams& v) {
        v.modEnv[0] = ModEnvParams{};
        for (int k = 0; k < 4; ++k) { v.modEnv[0].rate[k] = 99; v.modEnv[0].level[k] = 99; } // EG padrao
        v.modEnv[0].rate[0] = rate;
        v.modEnv[0].level[3] = 0;
        v.modMatrix[15] = ModRoute{ModSource::Env1, ModDest::OperatorLevel, 0, 1.0f}; // ultima das 16 rotas
    }, 69, 100, T + 0.3);
    // referencia: o mesmo som sem a rota, na mesma janela (o ataque do Amp EG fica fora da conta)
    auto ref = renderVoice([](VoiceParams&) {}, 69, 100, T + 0.3);
    float mid = rmsAt(x.l, T / 2) / rmsAt(ref.l, T / 2), end = rmsAt(x.l, T + 0.2) / rmsAt(ref.l, T + 0.2);
    std::printf("  (Env1 -> nivel: T=%.2fs, ganho no meio x%.3f, no fim x%.3f)\n", T, mid, end);
    check(std::fabs(mid - 1.5f) < 0.02f && std::fabs(end - 2.0f) < 0.01f,
          "envelope de modulacao: Env1 sobe no tempo do rate e modula o nivel (rota 16 da matriz)");
}

static void testLfoTempoSync() {
    // LFO3 quadrado sincronizado em 1/4 -> VoiceLevel 0.5: ganho 1.5 na 1a metade do ciclo, 0.5 na 2a
    auto at = [](float bpm) {
        auto x = renderVoice([](VoiceParams& v) {
            v.lfo3.wave = LfoWave::Square;
            v.lfo3.keySync = true;
            v.lfo3.tempoSync = true;
            v.lfo3.division = 6; // 1/4
            v.modMatrix[0] = ModRoute{ModSource::LFO3, ModDest::VoiceLevel, 0, 0.5f};
        }, 69, 100, 1.0, bpm);
        const double beat = 60.0 / bpm; // 1/4 = uma batida
        return std::pair<float, float>(rmsAt(x.l, beat * 0.25, beat * 0.3), rmsAt(x.l, beat * 0.75, beat * 0.3));
    };
    auto [hi120, lo120] = at(120.0f);
    auto [hi240, lo240] = at(240.0f);
    std::printf("  (LFO3 1/4 sincronizado: 120 BPM alto/baixo = %.2f, 240 BPM = %.2f)\n", hi120 / lo120, hi240 / lo240);
    check(std::fabs(hi120 / lo120 - 3.0f) < 0.1f && std::fabs(hi240 / lo240 - 3.0f) < 0.1f,
          "LFO sincronizado: 1/4 dura uma batida do BPM global (120 e 240 BPM)");
}

static void testLfoSmoothRandom() {
    LFOParams p;
    p.wave = LfoWave::SmoothRandom;
    p.rate = 10.0f;
    LFO lfo;
    lfo.setSampleRate(48000.0);
    lfo.configure(p);
    float lo = 0.0f, hi = 0.0f, prev = lfo.process(), maxStep = 0.0f;
    for (int i = 0; i < 96000; ++i) {
        float v = lfo.process();
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        maxStep = std::max(maxStep, std::fabs(v - prev));
        prev = v;
    }
    std::printf("  (SmoothRandom 10 Hz: faixa %.2f..%.2f, maior passo %.5f)\n", lo, hi, maxStep);
    check(lo >= -1.0f && hi <= 1.0f && hi - lo > 0.5f && maxStep < 0.01f, "LFO SmoothRandom: aleatorio dentro de -1..1, sem degraus");
}

static void testNewModDestinations() {
    // VoicePan: velocity 127 x +1 = toda pra direita
    auto panned = renderVoice([](VoiceParams& v) { v.modMatrix[0] = ModRoute{ModSource::Velocity, ModDest::VoicePan, 0, 1.0f}; }, 69, 127, 0.3);
    auto center = renderVoice([](VoiceParams&) {}, 69, 127, 0.3);
    float pl = rmsAt(panned.l, 0.2), pr = rmsAt(panned.r, 0.2), cl = rmsAt(center.l, 0.2);
    check(pl < 1e-6f && std::fabs(pr - cl) < 1e-4f, "destino VoicePan: modulacao leva a voz pro lado (velocity -> pan direita)");

    // OperatorPitch: +12 semitons no op0
    auto up = renderVoice([](VoiceParams& v) { v.modMatrix[0] = ModRoute{ModSource::Velocity, ModDest::OperatorPitch, 0, 1.0f}; }, 69, 127, 0.3);
    std::vector<float> w(up.l.begin() + 4800, up.l.end());
    check(mag(w, 880.0) > 50.0f * mag(w, 440.0), "destino OperatorPitch: +1 = uma oitava acima so naquele operador");

    // OperatorFx2: Q do passa-baixa (no corte, ganho = Q): x2 = +6 dB
    auto lpq = [](float depth) {
        auto x = renderVoice([&](VoiceParams& v) {
            v.op[0].fxType = FxType::LowPass;
            v.op[0].fxParam1 = 440.0f;
            v.op[0].fxParam2 = 0.707f;
            v.modMatrix[0] = ModRoute{ModSource::Velocity, ModDest::OperatorFx2, 0, depth};
        }, 69, 127, 0.4);
        return rmsAt(x.l, 0.3, 0.1);
    };
    float dB = 20.0f * std::log10(lpq(1.0f) / lpq(0.0f));
    std::printf("  (OperatorFx2 dobrando o Q no corte: %+.2f dB, esperado %+.2f)\n", dB, 20.0f * std::log10(2.0f));
    check(std::fabs(dB - 6.02f) < 0.2f, "destino OperatorFx2: multiplica o 2o parametro do efeito (Q do filtro)");

    // LfoRate / LfoDepth no LFO2 (quadrado 2 Hz -> VoiceLevel 0.5)
    auto lfo2 = [](const std::function<void(VoiceParams&)>& extra) {
        auto x = renderVoice([&](VoiceParams& v) {
            v.lfo2.wave = LfoWave::Square;
            v.lfo2.keySync = true;
            v.lfo2.rate = 2.0f;
            v.modMatrix[0] = ModRoute{ModSource::LFO2, ModDest::VoiceLevel, 0, 0.5f};
            extra(v);
        }, 69, 127, 0.6);
        return std::array<float, 2>{rmsAt(x.l, 0.0625, 0.08), rmsAt(x.l, 0.1875, 0.08)}; // 1a e 2a metade de um ciclo de 4 Hz
    };
    auto base = lfo2([](VoiceParams&) {});
    auto fast = lfo2([](VoiceParams& v) { v.modMatrix[1] = ModRoute{ModSource::Velocity, ModDest::LfoRate, 1, 1.0f / 3.0f}; });
    auto flat = lfo2([](VoiceParams& v) { v.modMatrix[1] = ModRoute{ModSource::Velocity, ModDest::LfoDepth, 1, -1.0f}; });
    std::printf("  (LFO2 2 Hz: alto/baixo em 4 Hz -- normal %.2f, LfoRate x2 %.2f, LfoDepth -1 %.2f)\n", base[0] / base[1],
                fast[0] / fast[1], flat[0] / flat[1]);
    check(std::fabs(base[0] / base[1] - 1.0f) < 0.05f && std::fabs(fast[0] / fast[1] - 3.0f) < 0.15f && std::fabs(flat[0] / flat[1] - 1.0f) < 0.02f,
          "destinos LfoRate (+1/3 = dobro da velocidade) e LfoDepth (-1 = LFO desligado)");
}

static void testModulationJson() {
    VoiceParams v;
    v.lfo3.tempoSync = true;
    v.lfo3.division = 11; // 1/8T
    v.lfo4.wave = LfoWave::SmoothRandom;
    v.modEnv[2].rate[1] = 12;
    v.modEnv[2].level[2] = 33;
    v.modMatrix[13] = ModRoute{ModSource::Env3, ModDest::LfoRate, 3, -0.25f};
    v.modMatrix[14] = ModRoute{ModSource::LFO4, ModDest::OperatorFx2, 5, 0.5f};
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(v).dump()));
    bool ok = back.lfo3.tempoSync && back.lfo3.division == 11 && back.lfo4.wave == LfoWave::SmoothRandom && back.modEnv[2].rate[1] == 12 &&
              back.modEnv[2].level[2] == 33 && back.modMatrix[13].src == ModSource::Env3 && back.modMatrix[13].dest == ModDest::LfoRate &&
              back.modMatrix[13].opIndex == 3 && back.modMatrix[14].dest == ModDest::OperatorFx2;
    check(ok, "LFO3/4 (sync, divisao, SmoothRandom), envelopes de modulacao e 16 rotas sobrevivem ao JSON");
    // patch antigo: 8 rotas, sem lfo3/lfo4/modEnv
    VoiceParams oldStyle;
    oldStyle.modMatrix[2] = ModRoute{ModSource::ModWheel, ModDest::OperatorLevel, 1, 0.3f};
    JsonValue j = voiceParamsToJson(oldStyle);
    std::string txt = j.dump();
    auto cut = [&](const char* key) { // apaga "key": {...} ou [...] do texto
        size_t at = txt.find(std::string("\"") + key + "\"");
        if (at == std::string::npos) return;
        size_t open = txt.find_first_of("{[", at);
        int depth = 0;
        size_t i = open;
        for (; i < txt.size(); ++i) { if (txt[i] == '{' || txt[i] == '[') ++depth; if (txt[i] == '}' || txt[i] == ']') if (--depth == 0) break; }
        size_t end = txt.find_first_not_of(" \n,", i + 1);
        txt.erase(at, end - at);
    };
    cut("lfo3");
    cut("lfo4");
    cut("modEnv");
    VoiceParams loaded = voiceParamsFromJson(JsonValue::parse(txt));
    const VoiceParams defaults;
    check(txt.find("lfo3") == std::string::npos && loaded.modMatrix[2].src == ModSource::ModWheel && !loaded.lfo3.tempoSync &&
              loaded.modEnv[0].rate[0] == defaults.modEnv[0].rate[0] && loaded.modMatrix[12].src == ModSource::None,
          "patch sem LFO3/4 e envelopes de modulacao carrega com os padroes");
}

static void testLfoSteps() {
    auto values = [](uint8_t steps, LfoWave wave) {
        LFOParams p;
        p.wave = wave;
        p.rate = 3.0f;
        p.steps = steps;
        LFO lfo;
        lfo.setSampleRate(48000.0);
        lfo.configure(p);
        std::vector<float> seen;
        for (int i = 0; i < 48000; ++i) {
            float v = lfo.process();
            bool known = false;
            for (float s : seen) known |= std::fabs(s - v) < 1e-6f;
            if (!known && seen.size() < 100) seen.push_back(v);
        }
        std::sort(seen.begin(), seen.end());
        return seen;
    };
    auto four = values(4, LfoWave::Sine), three = values(3, LfoWave::SmoothRandom), smooth = values(0, LfoWave::Sine);
    bool ok = four.size() == 4 && std::fabs(four.front() + 1.0f) < 1e-6f && std::fabs(four.back() - 1.0f) < 1e-6f &&
              std::fabs(four[1] + 1.0f / 3.0f) < 1e-6f && three.size() <= 3 && smooth.size() == 100;
    std::printf("  (LFO seno com 4 degraus: %zu valores; SmoothRandom com 3: %zu; suave: %zu+)\n", four.size(), three.size(), smooth.size());
    check(ok, "LFO quantizado: 4 degraus = exatamente -1, -1/3, 1/3, 1; 0 = suave como antes");
}

// Env1 em loop -> VoiceLevel +1: ganho (1 + env) medido contra o mesmo som sem a rota.
// Ciclo: L3=0 -> L1=99 -> L2=0 -> L3=0 (picos de ganho 2 no meio do ciclo).
static std::vector<float> loopGain(bool loop, bool sync, float bpm, double noteOffSec, double seconds) {
    auto render = [&](bool withRoute) {
        Synth synth;
        synth.init(48000.0, 4);
        synth.setTempoBpm(bpm);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].outputLevel = 80;
        v.op[0].ampEG.rate[3] = 10; // release longo: ainda soa depois do note off
        auto& e = v.modEnv[0];
        for (int k = 0; k < 4; ++k) e.rate[k] = 50;
        e.level[0] = 99; e.level[1] = 0; e.level[2] = 0; e.level[3] = 0;
        e.loop = loop;
        e.tempoSync = sync;
        e.division = 6; // 1/4
        if (withRoute) v.modMatrix[0] = ModRoute{ModSource::Env1, ModDest::VoiceLevel, 0, 1.0f};
        synth.part(0).voice = v;
        synth.recompile(0);
        synth.noteOn(0, 69, 100);
        std::vector<float> out;
        for (int i = 0; i < static_cast<int>(seconds * 48000); ++i) {
            if (i == static_cast<int>(noteOffSec * 48000)) synth.noteOff(0, 69);
            float l, r;
            synth.renderStereo(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto a = render(true), b = render(false);
    std::vector<float> gain; // um valor a cada 10 ms
    for (size_t at = 0; at + 480 <= a.size(); at += 480) gain.push_back(computeRms(a, at, 480) / std::max(computeRms(b, at, 480), 1e-9f));
    return gain;
}
static float gainAt(const std::vector<float>& g, double sec) { return g[std::min(g.size() - 1, static_cast<size_t>(sec / 0.01))]; }

static void testModEnvelopeLoop() {
    const double half = 99.0 / egRateToSpeed(50.0f); // 0 -> 99 no rate 50
    auto loop = loopGain(true, false, 120.0f, 1.6, 2.6), once = loopGain(false, false, 120.0f, 1.6, 2.6);
    const double p1 = half, p2 = 3 * half, p3 = 5 * half; // picos: meio de cada ciclo (ciclo = 2 x half)
    std::printf("  (loop: picos em %.2f/%.2f/%.2fs = %.2f %.2f %.2f; sem loop no 2o pico %.2f; depois do note off %.2f)\n", p1, p2, p3,
                gainAt(loop, p1), gainAt(loop, p2), gainAt(loop, p3), gainAt(once, p2), gainAt(loop, 2.3));
    check(gainAt(loop, p1) > 1.93f && gainAt(loop, p2) > 1.93f && gainAt(loop, p3) > 1.93f && gainAt(loop, 2 * half) < 1.07f &&
              gainAt(once, p2) < 1.02f && std::fabs(gainAt(loop, 2.3) - 1.0f) < 0.02f,
          "envelope de modulacao em loop: repete L3->L1->L2->L3 enquanto a nota esta presa e para no note off");

    // sync em 1/4: o ciclo dura uma batida
    bool ok = true;
    for (float bpm : {120.0f, 90.0f}) {
        auto g = loopGain(true, true, bpm, 5.0, 2.0);
        const double beat = 60.0 / bpm;
        float peak1 = gainAt(g, beat * 0.5), trough = gainAt(g, beat), peak2 = gainAt(g, beat * 1.5);
        std::printf("  (sync 1/4 a %.0f BPM: pico %.2f, vale %.2f, pico %.2f)\n", bpm, peak1, trough, peak2);
        ok &= peak1 > 1.93f && peak2 > 1.93f && trough < 1.07f;
    }
    check(ok, "envelope de modulacao com sync: o ciclo do loop dura a divisao do BPM (120 e 90 BPM)");
}

// --- [ADICIONADO em 2026-10-24] morph entre algoritmos ---
static std::vector<float> renderAlgo(const std::function<void(VoiceParams&)>& setup, int vel = 100, int samples = 24000) {
    Synth synth;
    synth.init(48000.0, 4);
    VoiceParams v;
    for (auto& op : v.op) op.outputLevel = 85;
    setup(v);
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.noteOn(0, 57, vel);
    std::vector<float> out;
    for (int i = 0; i < samples; ++i) { float l, r; synth.renderStereo(l, r); out.push_back(l); }
    return out;
}

static void testAlgoMorphTwoSlots() {
    // slot 1: op1 modula op0 (FM); slot 2: so op0 (seno puro)
    auto setup = [](float pos, bool step) {
        return [=](VoiceParams& v) {
            v.matrix = AlgorithmMatrix{};
            v.matrix.outputMix[0] = 1.0f;
            v.matrix.mod[0][1] = 1.0f;
            v.algoSlot[0] = AlgorithmMatrix{};
            v.algoSlot[0].outputMix[0] = 1.0f;
            v.numAlgos = 2;
            v.algoMorph = pos;
            v.algoStep = step;
        };
    };
    auto single = [](float w) {
        return [=](VoiceParams& v) { v.matrix = AlgorithmMatrix{}; v.matrix.outputMix[0] = 1.0f; v.matrix.mod[0][1] = w; };
    };
    float d0 = maxAbsDiff(renderAlgo(setup(0.0f, false)), renderAlgo(single(1.0f)));
    float dHalf = maxAbsDiff(renderAlgo(setup(0.5f, false)), renderAlgo(single(0.5f)));
    float d1 = maxAbsDiff(renderAlgo(setup(1.0f, false)), renderAlgo(single(0.0f)));
    float dStep = maxAbsDiff(renderAlgo(setup(0.4f, true)), renderAlgo(single(1.0f)));
    std::printf("  (morph 2 slots: diferenca na posicao 0 %.1e, 0.5 %.1e, 1 %.1e; saltar em 0.4 %.1e)\n", d0, dHalf, d1, dStep);
    check(d0 == 0.0f && dHalf == 0.0f && d1 == 0.0f && dStep == 0.0f,
          "morph de algoritmos: pontas = cada algoritmo, meio = pesos interpolados (indice de FM pela metade), Saltar = slot mais proximo");

    // a mod matrix move o morph (Velocity -> AlgoMorph +1: posicao 1 = seno puro)
    auto viaMatrix = renderAlgo([&](VoiceParams& v) { setup(0.0f, false)(v); v.modMatrix[0] = ModRoute{ModSource::Velocity, ModDest::AlgoMorph, 0, 1.0f}; }, 127);
    std::vector<float> tail(viaMatrix.begin() + 12000, viaMatrix.end());
    check(mag(tail, 440.0) < 0.001f * mag(tail, 220.0), "morph de algoritmos: destino AlgoMorph da matriz move a posicao");
}

static void testAlgoMorphEightSlots() {
    // 8 topologias sem lacos entre operadores (so arestas de indice maior pra menor):
    // a ordem de avaliacao nao muda o resultado, entao cada ponta tem que ser
    // identica ao algoritmo sozinho
    auto slotMatrix = [](int s) {
        AlgorithmMatrix m;
        m.outputMix[0] = 1.0f;
        if (s % 2) m.outputMix[1] = 0.5f;
        for (int j = 1; j < kNumOps; ++j)
            if ((s + j) % 3 == 0) m.mod[0][j] = 0.2f + 0.1f * static_cast<float>((s + j) % 4);
        m.mod[1][2 + s % 6] = 0.4f;
        m.mod[s % kNumOps][s % kNumOps] = 0.2f; // feedback na diagonal
        return m;
    };
    float worst = 0.0f;
    for (int k = 0; k < kMaxAlgoSlots; ++k) {
        auto morph = renderAlgo([&](VoiceParams& v) {
            for (int s = 0; s < kMaxAlgoSlots; ++s) v.algo(s) = slotMatrix(s);
            v.numAlgos = kMaxAlgoSlots;
            v.algoMorph = static_cast<float>(k) / (kMaxAlgoSlots - 1);
        }, 100, 9600);
        auto alone = renderAlgo([&](VoiceParams& v) { v.matrix = slotMatrix(k); }, 100, 9600);
        worst = std::max(worst, maxAbsDiff(morph, alone));
    }
    std::printf("  (8 slots: maior diferenca entre a posicao k/7 e o algoritmo k sozinho %.1e)\n", worst);
    check(worst < 1e-6f, "morph de 8 algoritmos: a posicao de cada slot soa igual ao algoritmo dele sozinho");

    VoiceParams v;
    for (int s = 0; s < kMaxAlgoSlots; ++s) v.algo(s) = slotMatrix(s);
    v.numAlgos = 5;
    v.algoMorph = 0.35f;
    v.algoStep = true;
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(v).dump()));
    bool same = back.numAlgos == 5 && back.algoMorph == 0.35f && back.algoStep;
    for (int s = 0; s < 5; ++s) same &= std::memcmp(&back.algo(s), &v.algo(s), sizeof(AlgorithmMatrix)) == 0;
    VoiceParams old = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(VoiceParams{}).dump()));
    check(same && old.numAlgos == 1 && voiceParamsToJson(VoiceParams{}).dump().find("algorithms") == std::string::npos,
          "morph de algoritmos: slots sobrevivem ao JSON; patch sem morph nao grava nem le slots");
}

static void testAlgoMorphCost() {
    // mesmos 8 slots (a uniao das topologias e a mesma), posicao parada x
    // modulada por LFO: o que muda e so o recalculo dos pesos a cada amostra
    auto timeIt = [](bool moving) {
        Synth synth;
        synth.init(48000.0, 64);
        VoiceParams v;
        applyAlgorithm(v, 1);
        for (auto& op : v.op) op.outputLevel = 80;
        for (int s = 1; s < kMaxAlgoSlots; ++s) applyAlgorithmToMatrix(v.algo(s), 1 + s * 4);
        v.numAlgos = kMaxAlgoSlots;
        v.algoMorph = 0.5f;
        v.lfo1.rate = 3.0f;
        v.modMatrix[0] = ModRoute{ModSource::LFO1, moving ? ModDest::AlgoMorph : ModDest::VoiceLevel, 0, moving ? 0.5f : 0.0f};
        synth.part(0).voice = v;
        synth.recompile(0);
        for (int n = 0; n < 64; ++n) synth.noteOn(0, 30 + n, 100);
        float sink = 0.0f, L[256], R[256];
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < 48000 / 256; ++b) { synth.renderBlock(L, R, 256); sink += L[0]; }
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return sink == 12345.0f ? 0.0 : sec;
    };
    double still = std::min(timeIt(false), timeIt(false)), moving = std::min(timeIt(true), timeIt(true));
    std::printf("  (64 vozes, 8 slots: posicao parada %.3fs, modulada a cada amostra %.3fs, razao %.2fx)\n", still, moving, moving / still);
    check(moving < still * 1.3, "morph de algoritmos: mover a posicao a cada amostra custa menos que 1.3x a posicao parada");
}

// --- [ADICIONADO em 2026-10-24] configuracoes do app (app/settings.h) ---
static void testAppSettings() {
    using namespace dx7fm_app;
    AppSettings s;
    s.oversample = 4;
    s.midiClock = false;
    s.learn(CcTarget::Macro2, 21);
    s.learn(CcTarget::Bpm, 22);
    s.learn(CcTarget::Macro3, 21); // CC 21 passa pra Macro3 (sai da Macro2)
    s.learn(CcTarget::Bpm, 23);    // BPM troca de CC
    s.sclPath[2] = "/tmp/x.scl";
    AppSettings back = settingsFromJson(settingsToJson(s));
    check(back.oversample == 4 && !back.midiClock && back.ccFor(CcTarget::Macro2) == -1 && back.ccFor(CcTarget::Macro3) == 21 &&
              back.ccFor(CcTarget::Bpm) == 23 && back.cc.size() == 2 && back.sclPath[2] == "/tmp/x.scl",
          "configuracoes: oversampling, clock, CCs aprendidos (um por alvo/um alvo por CC) e afinacao sobrevivem ao arquivo");
    AppSettings bad = settingsFromJson("{\"oversample\": 3, \"cc\": [{\"cc\": 200, \"target\": \"Macro1\"}]}");
    check(bad.oversample == 1 && bad.cc.empty(), "configuracoes: valores invalidos (oversample 3, CC 200) caem nos padroes");
}

// --- [ADICIONADO em 2026-10-24] macros ---
static void testMacros() {
    using namespace dx7fm_app;
    // Macro1 -> VoiceLevel +1 com a macro em 0.5: ganho 1.5; valor dado ANTES da nota vale pra ela
    auto render = [](float macro, bool viaEvent) {
        Synth synth;
        synth.init(48000.0, 4);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].outputLevel = 80;
        v.modMatrix[0] = ModRoute{ModSource::Macro1, ModDest::VoiceLevel, 0, 1.0f};
        synth.part(0).voice = v;
        synth.recompile(0);
        if (viaEvent) { // como o app: evento Macro roteado pelo canal (parte 0 em omni)
            std::atomic<bool> enabled[kNumParts];
            std::atomic<int> channel[kNumParts];
            for (int p = 0; p < kNumParts; ++p) { enabled[p] = (p == 0); channel[p] = 0; }
            NoteRouting routing;
            AppEvent e;
            e.kind = AppEvent::Kind::Macro;
            e.channel1to16 = 3;
            e.note = 0;
            e.floatValue = macro;
            applyAppEvent(synth, e, enabled, channel, routing);
        } else {
            synth.setMacro(0, 0, macro);
        }
        synth.noteOn(0, 69, 100);
        std::vector<float> out;
        for (int i = 0; i < 9600; ++i) { float l, r; synth.renderStereo(l, r); out.push_back(l); }
        return std::pair<float, float>(computeRms(out, 4800, 4800), synth.macroValue(0, 0));
    };
    auto [off, v0] = render(0.0f, false);
    auto [half, v1] = render(0.5f, false);
    auto [viaEv, v2] = render(0.5f, true);
    std::printf("  (Macro1 -> volume: 0.5 da ganho x%.3f; via evento x%.3f)\n", half / off, viaEv / off);
    check(std::fabs(half / off - 1.5f) < 0.01f && std::fabs(viaEv / off - 1.5f) < 0.01f && v1 == 0.5f && v2 == 0.5f && v0 == 0.0f,
          "macros: Macro1 modula pela matriz; valor dado antes da nota vale pra ela; evento Macro chega pela fila");

    VoiceParams p;
    p.macro[2] = 0.7f;
    p.macroName[2] = "Brilho";
    VoiceParams back = voiceParamsFromJson(JsonValue::parse(voiceParamsToJson(p).dump()));
    check(back.macro[2] == 0.7f && back.macroName[2] == "Brilho" && back.macroName[0] == "Macro 1",
          "macros: valor e nome sobrevivem ao JSON do patch");
}

// --- [ADICIONADO em 2026-10-24] clock MIDI, posicao de batida e LFO global ---
static void testMidiClockEstimator() {
    using dx7fm_app::ClockEstimator;
    auto run = [](double bpm, double jitter) {
        ClockEstimator c;
        const double dt = 60.0 / (bpm * 24.0);
        for (int i = 0; i < 200; ++i) c.tick(i * dt + ((i * 7919) % 13 - 6) / 6.0 * jitter); // jitter +-
        return c.bpm();
    };
    double a = run(120.0, 0.0), b = run(97.5, 0.0), j = run(120.0, 0.0005);
    ClockEstimator stopped;
    stopped.tick(0.0);
    stopped.tick(0.02);
    std::printf("  (clock MIDI: 120 -> %.3f, 97.5 -> %.3f, 120 com jitter de 0.5ms -> %.2f)\n", a, b, j);
    check(std::fabs(a - 120.0) < 1e-6 && std::fabs(b - 97.5) < 1e-6 && std::fabs(j - 120.0) < 0.5 && stopped.bpm() == 0.0,
          "clock MIDI: media de uma batida de pulsos da o BPM (tambem com jitter); poucos pulsos = sem estimativa");
}

// Mede o ganho (1 + LFO x 0.5) de cada nota ao longo do tempo: duas notas de alturas
// diferentes, a 2a entrando 0.3s depois; Goertzel em janelas de 40ms.
static std::pair<std::vector<float>, std::vector<float>> twoNoteGains(bool global, bool sync, bool resetMid) {
    auto render = [&](bool route) {
        Synth synth;
        synth.init(48000.0, 4);
        synth.setTempoBpm(120.0f);
        VoiceParams v;
        v.matrix = AlgorithmMatrix{};
        v.matrix.outputMix[0] = 1.0f;
        v.op[0].outputLevel = 80;
        v.lfo1.wave = LfoWave::Square;
        v.lfo1.rate = 2.0f;
        v.lfo1.keySync = true;
        v.lfo1.global = global;
        v.lfo1.tempoSync = sync;
        v.lfo1.division = 6; // 1/4 = 0.5s a 120 BPM
        if (route) v.modMatrix[0] = ModRoute{ModSource::LFO1, ModDest::VoiceLevel, 0, 0.5f};
        synth.part(0).voice = v;
        synth.recompile(0);
        std::vector<float> out(48000 * 2);
        for (int i = 0; i < static_cast<int>(out.size()); i += 240) {
            if (i == 4800) synth.noteOn(0, 69, 100);  // 0.1s: 440Hz
            if (i == 19200) synth.noteOn(0, 76, 100); // 0.4s: 659Hz
            if (resetMid && i == 28800) synth.resetTransport(); // 0.6s
            float r[240];
            synth.renderBlock(&out[static_cast<size_t>(i)], r, 240);
        }
        return out;
    };
    auto a = render(true), b = render(false);
    std::vector<float> ga, gb;
    for (int w = 0; w < 48; ++w) { // janelas de 40ms a partir de 0.42s
        size_t at = static_cast<size_t>((0.42 + w * 0.025) * 48000);
        std::vector<float> wa(a.begin() + static_cast<long>(at), a.begin() + static_cast<long>(at + 1920));
        std::vector<float> wb(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + 1920));
        ga.push_back(mag(wa, 440.0) / mag(wb, 440.0));
        gb.push_back(mag(wa, 659.26) / mag(wb, 659.26));
    }
    return {ga, gb};
}

static void testGlobalLfo() {
    auto [ga, gb] = twoNoteGains(true, false, false);
    auto [la, lb] = twoNoteGains(false, false, false);
    float diffGlobal = 0.0f, diffLocal = 0.0f;
    for (size_t i = 0; i < ga.size(); ++i) { diffGlobal = std::max(diffGlobal, std::fabs(ga[i] - gb[i])); diffLocal = std::max(diffLocal, std::fabs(la[i] - lb[i])); }
    std::printf("  (2 notas entrando em tempos diferentes: maior diferenca de ganho -- LFO global %.3f, LFO por voz %.3f)\n", diffGlobal, diffLocal);
    check(diffGlobal < 0.02f && diffLocal > 0.5f, "LFO global: notas que entram em tempos diferentes ondulam juntas (por voz, nao)");

    // global + sync 1/4: alinhado a batida (0.5s), nao ao inicio da nota (0.1s);
    // resetTransport em 0.6s realinha o compasso ali
    auto [sa, sb] = twoNoteGains(true, true, false);
    auto [ra, rb] = twoNoteGains(true, true, true);
    auto at = [](const std::vector<float>& g, double sec) { return g[static_cast<size_t>(std::lround((sec - 0.42) / 0.025))]; };
    // 0.80s: na grade original (meias batidas a partir de 0) cai no trecho baixo
    // (0.75..1.0); na grade realinhada em 0.6s, no alto (0.6..0.85)
    std::printf("  (sync 1/4: ganho em 0.52/0.77/0.80s = %.2f/%.2f/%.2f; com Start em 0.6s, em 0.80/0.92s = %.2f/%.2f)\n", at(sa, 0.52),
                at(sa, 0.77), at(sa, 0.80), at(ra, 0.80), at(ra, 0.92));
    check(at(sa, 0.52) > 1.45f && at(sa, 0.77) < 0.55f && at(sa, 0.80) < 0.55f && at(sa, 1.02) > 1.45f && at(ra, 0.80) > 1.45f &&
              at(ra, 0.92) < 0.55f,
          "LFO global com sync: fase na grade das batidas; Start do transporte (clock MIDI) realinha");
}

// [ADICIONADO em 2026-10-28] LfoRate num LFO global: LFO1 global (quadrada,
// 2 Hz, ou sync 1/4 = 2 Hz a 120 BPM) -> VoiceLevel; Macro1 (ou Velocity, que
// e por voz e nao vale no LFO global) -> LfoRate do LFO1 com 1/3 (x2 no
// maximo). Conta os ciclos da ondulacao em 2 s.
static int globalLfoCycles(bool sync, ModSource src, float macro) {
    Synth synth;
    synth.init(48000.0, 4);
    synth.setTempoBpm(120.0f);
    VoiceParams v;
    v.matrix = AlgorithmMatrix{};
    v.matrix.outputMix[0] = 1.0f;
    v.op[0].outputLevel = 80;
    v.lfo1.wave = LfoWave::Square;
    v.lfo1.rate = 2.0f;
    v.lfo1.global = true;
    v.lfo1.tempoSync = sync;
    v.lfo1.division = 6;
    v.modMatrix[0] = ModRoute{ModSource::LFO1, ModDest::VoiceLevel, 0, 0.5f};
    v.modMatrix[1] = ModRoute{src, ModDest::LfoRate, 0, 1.0f / 3.0f};
    synth.part(0).voice = v;
    synth.recompile(0);
    synth.setMacro(0, 0, macro);
    synth.noteOn(0, 69, 127);
    std::vector<float> peaks; // pico por bloco de 5 ms; o nivel alterna entre 0.5x e 1.5x
    for (int b = 0; b < 400; ++b) {
        float L[240], R[240];
        synth.renderBlock(L, R, 240);
        float pk = 0.0f;
        for (float x : L) pk = std::max(pk, std::fabs(x));
        if (b >= 4) peaks.push_back(pk);
    }
    const float lo = *std::min_element(peaks.begin(), peaks.end()), hi = *std::max_element(peaks.begin(), peaks.end());
    const float up = lo + 0.7f * (hi - lo), down = lo + 0.3f * (hi - lo);
    int cycles = 0;
    bool high = false;
    for (float pk : peaks) {
        if (!high && pk > up) { high = true; ++cycles; }
        if (high && pk < down) high = false;
    }
    return cycles;
}

static void testGlobalLfoRateModulation() {
    const int base = globalLfoCycles(false, ModSource::Macro1, 0.0f), fast = globalLfoCycles(false, ModSource::Macro1, 1.0f);
    const int perVoice = globalLfoCycles(false, ModSource::Velocity, 0.0f);
    const int syncBase = globalLfoCycles(true, ModSource::Macro1, 0.0f), syncFast = globalLfoCycles(true, ModSource::Macro1, 1.0f);
    std::printf("  (ciclos em 2 s: livre %d -> %d com a macro; velocity (por voz) %d; sync %d -> %d)\n", base, fast, perVoice, syncBase, syncFast);
    check(std::abs(base - 4) <= 1 && std::abs(fast - 8) <= 1, "LFO global: Macro -> LfoRate dobra a velocidade");
    check(perVoice == base, "LFO global: fonte por voz (velocity) nao mexe na velocidade dele");
    check(std::abs(syncBase - 4) <= 1 && std::abs(syncFast - 8) <= 1, "LFO global com sync: LfoRate tambem vale (vira deslocamento de fase)");
}

// --- [ADICIONADO em 2026-10-24] arpejador ---
struct ArpEvent { int note; bool on; double sec; };
static std::vector<ArpEvent> runArp(const std::function<void(ArpParams&)> setup, const std::vector<int>& keys, double seconds,
                                    const std::function<void(Synth&, double)>& atTime = nullptr) {
    Synth synth;
    synth.init(48000.0, 16);
    synth.setTempoBpm(120.0f);
    VoiceParams v;
    v.arp.on = true;
    setup(v.arp);
    synth.part(0).voice = v;
    synth.recompile(0);
    std::vector<ArpEvent> log;
    synth.setArpObserver([](void* ctx, int, int note, bool on, double beat) {
        static_cast<std::vector<ArpEvent>*>(ctx)->push_back({note, on, beat * 0.5}); // 120 BPM: 0.5 s por batida
    }, &log);
    for (int k : keys) synth.noteOn(0, k, 100);
    float L[100], R[100];
    for (int i = 0; i < static_cast<int>(seconds * 480); ++i) {
        if (atTime) atTime(synth, i / 480.0);
        synth.renderBlock(L, R, 100);
    }
    return log;
}
static std::vector<int> onsets(const std::vector<ArpEvent>& log, std::vector<double>* times = nullptr) {
    std::vector<int> n;
    for (const auto& e : log) if (e.on) { n.push_back(e.note); if (times) times->push_back(e.sec); }
    return n;
}

static void testArpeggiator() {
    // Up, 1/16 a 120 BPM (passo 125 ms), gate 50%: C E G C E G, liga a cada 125 ms e solta 62.5 ms depois
    std::vector<double> t;
    auto up = runArp([](ArpParams& a) { a.mode = ArpMode::Up; }, {64, 60, 67}, 0.74);
    auto upNotes = onsets(up, &t);
    bool timing = upNotes.size() == 6;
    for (size_t i = 0; timing && i < t.size(); ++i) timing &= std::fabs(t[i] - 0.125 * i) < 1.0 / 48000.0;
    double firstOff = -1.0;
    for (const auto& e : up) if (!e.on) { firstOff = e.sec; break; }
    std::printf("  (arp Up: %d %d %d %d %d %d; ligou em 0/%.4f/%.4f s; 1o note off em %.4f s)\n", upNotes[0], upNotes[1], upNotes[2], upNotes[3],
                upNotes[4], upNotes[5], t[1], t[2], firstOff);
    check(upNotes == std::vector<int>({60, 64, 67, 60, 64, 67}) && timing && std::fabs(firstOff - 0.0625) < 1.0 / 48000.0,
          "arpejador Up 1/16: ordem crescente, um passo a cada 125 ms (exato na amostra), gate 50% solta na metade");

    auto down = onsets(runArp([](ArpParams& a) { a.mode = ArpMode::Down; }, {64, 60, 67}, 0.37));
    auto updown = onsets(runArp([](ArpParams& a) { a.mode = ArpMode::UpDown; }, {64, 60, 67}, 0.74));
    auto oct = onsets(runArp([](ArpParams& a) { a.octaves = 2; }, {60, 64}, 0.49));
    auto played = onsets(runArp([](ArpParams& a) { a.mode = ArpMode::AsPlayed; }, {67, 60, 64}, 0.37));
    check(down == std::vector<int>({67, 64, 60}) && updown == std::vector<int>({60, 64, 67, 64, 60, 64}) &&
              oct == std::vector<int>({60, 64, 72, 76}) && played == std::vector<int>({67, 60, 64}),
          "arpejador: Down, UpDown (sem repetir as pontas), 2 oitavas e como tocado");

    // swing 1/3: passos impares atrasados 1/3 de passo
    std::vector<double> ts;
    onsets(runArp([](ArpParams& a) { a.swing = 1.0f / 3.0f; }, {60, 64}, 0.5), &ts);
    std::printf("  (swing 1/3: passos em");
    for (double x : ts) std::printf(" %.5f", x);
    std::printf(" s)\n");
    // o evento sai na 1a amostra no instante exato ou depois dele (ate 1 amostra de arredondamento)
    const double swungExact = 0.125 + 0.125 * static_cast<double>(1.0f / 3.0f);
    check(ts.size() >= 4 && ts[1] - swungExact >= -1e-9 && ts[1] - swungExact < 1.0 / 48000.0 && std::fabs(ts[2] - 0.25) < 1.0 / 48000.0,
          "arpejador: swing atrasa os passos impares");

    // latch: solta tudo em 0.2s -> continua; tecla nova em 0.45s -> padrao novo so com ela
    auto latched = runArp([](ArpParams& a) { a.latch = true; }, {60, 64}, 0.8, [](Synth& s, double sec) {
        if (std::fabs(sec - 0.2) < 1e-9) { s.noteOff(0, 60); s.noteOff(0, 64); }
        if (std::fabs(sec - 0.45) < 1e-9) s.noteOn(0, 70, 100);
    });
    auto ln = onsets(latched);
    // sem latch: soltar tudo para o arpejo (e solta a nota que soava)
    auto stopped = runArp([](ArpParams&) {}, {60, 64}, 0.6, [](Synth& s, double sec) {
        if (std::fabs(sec - 0.2) < 1e-9) { s.noteOff(0, 60); s.noteOff(0, 64); }
    });
    bool lastOff = !stopped.empty() && !stopped.back().on;
    std::printf("  (latch: %zu notas, ultimas %d %d; sem latch: %zu notas)\n", ln.size(), ln[ln.size() - 2], ln.back(), onsets(stopped).size());
    check(ln.size() == 7 && ln[3] == 64 && ln.back() == 70 && ln[ln.size() - 2] == 70 && onsets(stopped).size() == 2 && lastOff,
          "arpejador: latch mantem o padrao depois de soltar e troca na tecla nova; sem latch, soltar para");
}

// --- [ADICIONADO em 2026-10-24] microafinacao (dx7_tuning.h) ---
static const char* kJustScl =
    "! justa.scl\n!\nEscala justa maior de 7 notas\n 7\n!\n 9/8\n 5/4\n 4/3\n 3/2\n 5/3\n 15/8\n 2/1\n";

static void testTuningParsers() {
    TuningTable et = equalTemperament();
    bool same = true;
    for (int n = 0; n < 128; ++n) same &= et.hz[n] == Voice::noteToHz(n) && et.mapped[n];
    check(same, "afinacao: tabela 12-TET padrao e bit a bit igual a conta do motor (sem afinacao, nada muda)");

    std::string err;
    Scale et12;
    std::string sc = "doze\n12\n";
    for (int i = 1; i <= 12; ++i) sc += std::to_string(i * 100) + ".0\n";
    bool ok12 = parseScl(sc, et12, err);
    TuningTable t12 = buildTuning(et12, nullptr, 440.0f);
    float worst = 0.0f;
    for (int n = 0; n < 128; ++n) worst = std::max(worst, std::fabs(t12.hz[n] / et.hz[n] - 1.0f));
    check(ok12 && worst < 1e-5f, "afinacao: .scl de 12 graus em cents (100.0, 200.0 ...) da o 12-TET");

    Scale just;
    bool okJ = parseScl(kJustScl, just, err);
    TuningTable tj = buildTuning(just, nullptr, 440.0f);
    // sem .kbm: nota 60 = grau 0; nota 69 (A4) = 440 Hz -> nota 60 = 440 / (5/3) ... mas 69 e o 6o grau? 60,61..: graus consecutivos
    // (mapa linear: cada tecla e um grau) -> 60 = C, 61 = 9/8, 62 = 5/4, ..., 67 = 2/1 (oitava), 69 = 2 x 5/4
    const double c4 = 440.0 / 2.5;
    bool ratios = okJ && std::fabs(tj.hz[61] / c4 - 9.0 / 8) < 1e-5 && std::fabs(tj.hz[64] / c4 - 3.0 / 2) < 1e-5 &&
                  std::fabs(tj.hz[67] / c4 - 2.0) < 1e-5 && std::fabs(tj.hz[69] - 440.0f) < 1e-3f;
    check(ratios, "afinacao: escala justa de 7 notas -- razoes exatas e a nota 69 no diapasao");

    // .kbm: mapa de 12 teclas -> 7 graus (teclas brancas), pretas sem som; referencia: nota 69 = 432 Hz no grau 5 (5/3)
    const char* kbm = "! brancas.kbm\n12\n0\n127\n60\n69\n432.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n";
    KeyMap km;
    bool okK = parseKbm(kbm, km, err);
    TuningTable tk = buildTuning(just, &km, 440.0f);
    bool kbmOk = okK && tk.mapped[60] && !tk.mapped[61] && tk.mapped[62] && !tk.mapped[70] && std::fabs(tk.hz[69] - 432.0f) < 1e-3f &&
                 std::fabs(tk.hz[60] / (432.0 / (5.0 / 3)) - 1.0) < 1e-6 && std::fabs(tk.hz[72] / tk.hz[60] - 2.0) < 1e-5 &&
                 std::fabs(tk.hz[67] / tk.hz[60] - 1.5) < 1e-5;
    TuningTable t432 = equalTemperament(432.0f);
    Scale bad;
    check(kbmOk && std::fabs(t432.hz[69] - 432.0f) < 1e-4f && !parseScl("x\nabc\n", bad, err) && !parseScl("x\n3\n9/8\n", bad, err),
          "afinacao: .kbm com teclas sem som, referencia propria e oitava formal; A4 = 432; .scl invalido e recusado");
}

static void testTuningInEngine() {
    // parte com a escala justa: a nota 64 (E4, grau 4 = 3/2 de C) soa em C x 1.5; tecla sem som nao toca
    Synth synth;
    synth.init(48000.0, 4);
    synth.part(0).voice = VoiceParams{};
    synth.part(0).voice.op[1].outputLevel = 0; // so o seno da portadora
    synth.part(0).tuning.sclText = kJustScl;
    synth.part(0).tuning.kbmText = "12\n0\n127\n60\n69\n440.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n";
    synth.recompile(0);
    synth.noteOn(0, 67, 100); // G4 = grau 4 (3/2)
    std::vector<float> x;
    for (int i = 0; i < 24000; ++i) { float l, r; synth.renderStereo(l, r); x.push_back(l); }
    const double c4 = 440.0 / (5.0 / 3.0);
    std::vector<float> tail(x.begin() + 4800, x.end());
    const double hz = estimateFrequencyByZeroCrossings(tail, 48000.0); // 396 (justa) x 392 (12-TET): 1% de diferenca
    synth.noteOn(0, 61, 100); // tecla preta: sem som no .kbm
    const int voices = synth.activeVoiceCount();
    std::printf("  (G4 na escala justa: esperado %.2f Hz, medido %.2f Hz (12-TET seria %.2f); vozes depois da tecla sem som: %d)\n", c4 * 1.5, hz,
                Voice::noteToHz(67), voices);
    check(std::fabs(hz / (c4 * 1.5) - 1.0) < 0.002 && voices == 1 && synth.tuningError(0).empty(),
          "afinacao na parte: a nota soa na altura da escala; tecla sem som no .kbm nao toca");
    PartParams p;
    p.tuning.sclText = kJustScl;
    p.tuning.a4Hz = 432.0f;
    PartParams back = partParamsFromJson(JsonValue::parse(partParamsToJson(p).dump()));
    PartParams kept;
    dx7fm_app::copyPatchKeepingRouting(kept, PartParams{});
    PartParams dst = p;
    dx7fm_app::copyPatchKeepingRouting(dst, PartParams{});
    check(back.tuning.sclText == kJustScl && back.tuning.a4Hz == 432.0f && dst.tuning.sclText == kJustScl,
          "afinacao: salva com a parte e fica na parte ao carregar outro patch");
}

// --- [ADICIONADO em 2026-10-24] SysEx do DX7 (dx7_sysex.h) ---
// empacota uma voz VCED (155) no formato VMEM (128), como o DX7 guarda nos bancos
static void packVmem(const sysex::Dx7Voice& v, uint8_t* b) {
    const uint8_t* p = v.p;
    for (int k = 0; k < 6; ++k) {
        const uint8_t* s = p + k * 21;
        uint8_t* d = b + k * 17;
        for (int i = 0; i < 11; ++i) d[i] = s[i];
        d[11] = static_cast<uint8_t>(s[11] | (s[12] << 2));
        d[12] = static_cast<uint8_t>(s[13] | (s[20] << 3));
        d[13] = static_cast<uint8_t>(s[14] | (s[15] << 2));
        d[14] = s[16];
        d[15] = static_cast<uint8_t>(s[17] | (s[18] << 1));
        d[16] = s[19];
    }
    for (int i = 0; i < 8; ++i) b[102 + i] = p[126 + i];
    b[110] = p[134];
    b[111] = static_cast<uint8_t>(p[135] | (p[136] << 3));
    for (int i = 0; i < 4; ++i) b[112 + i] = p[137 + i];
    b[116] = static_cast<uint8_t>(p[141] | (p[142] << 1) | (p[143] << 4));
    b[117] = p[144];
    for (int i = 0; i < 10; ++i) b[118 + i] = p[145 + i];
}

static sysex::Dx7Voice testVoice(int k) {
    sysex::Dx7Voice v;
    for (int op = 0; op < 6; ++op) {
        uint8_t* o = v.p + op * 21;
        for (int i = 0; i < 4; ++i) { o[i] = static_cast<uint8_t>((k * 7 + op * 13 + i * 17) % 100); o[4 + i] = static_cast<uint8_t>((99 - k - i * 5) % 100); }
        o[8] = 39; o[9] = static_cast<uint8_t>(k % 100); o[10] = 50; o[11] = static_cast<uint8_t>(k % 4); o[12] = 3;
        o[13] = static_cast<uint8_t>(op % 8); o[14] = static_cast<uint8_t>(op % 4); o[15] = static_cast<uint8_t>(k % 8);
        o[16] = static_cast<uint8_t>(80 + op); o[17] = op == 5 && k % 2; o[18] = static_cast<uint8_t>((k + op) % 32);
        o[19] = static_cast<uint8_t>(k % 100); o[20] = static_cast<uint8_t>((k + op) % 15);
    }
    for (int i = 0; i < 8; ++i) v.p[126 + i] = static_cast<uint8_t>(50 + i);
    v.p[134] = static_cast<uint8_t>(k % 32); v.p[135] = static_cast<uint8_t>(k % 8); v.p[136] = 1;
    v.p[137] = 35; v.p[138] = 10; v.p[139] = 5; v.p[140] = 0; v.p[141] = 1; v.p[142] = static_cast<uint8_t>(k % 6); v.p[143] = 3;
    v.p[144] = 24;
    std::string name = "VOZ " + std::to_string(k);
    for (int i = 0; i < 10; ++i) v.p[145 + i] = static_cast<uint8_t>(i < static_cast<int>(name.size()) ? name[static_cast<size_t>(i)] : ' ');
    return v;
}

static void testDx7Sysex() {
    // banco sintetico de 32 vozes -> VMEM -> parser -> as mesmas vozes
    std::vector<uint8_t> syx = {0xF0, 0x43, 0x00, 0x09, 0x20, 0x00};
    std::vector<uint8_t> body(4096);
    for (int k = 0; k < 32; ++k) packVmem(testVoice(k), &body[static_cast<size_t>(k) * 128]);
    syx.insert(syx.end(), body.begin(), body.end());
    syx.push_back(sysex::checksum(body.data(), body.size()));
    syx.push_back(0xF7);
    std::string err;
    auto voices = sysex::parse(syx, err);
    bool same = voices.size() == 32 && err.empty();
    for (int k = 0; same && k < 32; ++k) same &= std::memcmp(voices[static_cast<size_t>(k)].p, testVoice(k).p, 155) == 0;
    check(same && voices[7].name() == "VOZ 7", "SysEx DX7: banco de 32 vozes (VMEM) ida e volta pelo parser, nomes inclusive");
    auto broken = syx;
    broken[100] ^= 1;
    auto none = sysex::parse(broken, err);
    std::vector<uint8_t> single = {0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B};
    sysex::Dx7Voice one = testVoice(5);
    single.insert(single.end(), one.p, one.p + 155);
    single.push_back(sysex::checksum(one.p, 155));
    single.push_back(0xF7);
    std::string err2;
    auto singles = sysex::parse(single, err2);
    check(none.empty() && !err.empty() && singles.size() == 1 && std::memcmp(singles[0].p, one.p, 155) == 0,
          "SysEx DX7: checksum errado e recusado; voz unica (VCED) tambem le");

    // topologias: portadoras de cada um dos 32 algoritmos do DX7
    const int carriers[32] = {2, 2, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 3, 3, 4, 4, 4, 5, 5, 3, 3, 3, 4, 4, 5, 6};
    bool topo = true;
    for (int a = 1; a <= 32; ++a) {
        AlgorithmMatrix m = sysex::algorithmMatrix(a, 0.5f);
        int c = 0, fb = 0;
        for (int i = 0; i < kNumOps; ++i) { c += m.outputMix[i] != 0.0f; for (int j = 0; j < kNumOps; ++j) fb += m.mod[i][j] == 0.5f; }
        topo &= c == carriers[a - 1] && fb == 1 && m.outputMix[6] == 0.0f && m.outputMix[7] == 0.0f;
    }
    AlgorithmMatrix a1 = sysex::algorithmMatrix(1, 0.5f), a4 = sysex::algorithmMatrix(4, 0.5f);
    topo &= a1.mod[0][1] == 1.0f && a1.mod[2][3] == 1.0f && a1.mod[3][4] == 1.0f && a1.mod[5][5] == 0.5f; // 2->1, 4->3, 5->4; fb no 6
    topo &= a4.mod[5][3] == 0.5f && a4.outputMix[0] == 1.0f && a4.outputMix[3] == 1.0f;                 // alg 4: laco 4 -> 6
    check(topo, "SysEx DX7: os 32 algoritmos com o numero certo de portadoras e o feedback (proprio ou o laco dos alg. 4 e 6)");

    // conversoes
    bool conv = std::fabs(sysex::ratio(0, 0) - 0.5f) < 1e-6f && std::fabs(sysex::ratio(3, 50) - 4.5f) < 1e-5f &&
                std::fabs(sysex::fixedHz(2, 0) - 100.0f) < 1e-3f && std::fabs(sysex::fixedHz(1, 50) - std::sqrt(10.0f) * 10.0f) < 1e-2f &&
                sysex::detuneCents(7) == 0.0f && sysex::levelToOurs(99) == 99 && sysex::levelToOurs(50) == 50 && sysex::levelToOurs(0) == 0 &&
                sysex::feedbackAmount(7) == 0.5f && sysex::feedbackAmount(0) == 0.0f && sysex::rateToOurs(99) > sysex::rateToOurs(50) &&
                sysex::rateToOurs(50) > sysex::rateToOurs(20) && std::fabs(sysex::lfoHz(35) - 5.81f) < 0.05f;
    std::printf("  (rates DX7 -> nossos: 99->%d 75->%d 50->%d 25->%d; LFO 35 = %.2f Hz, 99 = %.1f Hz)\n", sysex::rateToOurs(99),
                sysex::rateToOurs(75), sysex::rateToOurs(50), sysex::rateToOurs(25), sysex::lfoHz(35), sysex::lfoHz(99));
    check(conv, "SysEx DX7: razao, frequencia fixa, detune, nivel, feedback, rates e LFO convertidos");

    // banco real do Dexed, se estiver instalado (nao falha sem ele)
    const char* home = std::getenv("HOME");
    const std::string dexed = std::string(home ? home : "") + "/.local/share/DigitalSuburban/Dexed/Cartridges/Dexed_01.syx";
    std::ifstream f(dexed, std::ios::binary);
    if (f) {
        std::vector<uint8_t> real((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto bank = sysex::parse(real, err);
        bool sounds = bank.size() == 32;
        Synth synth;
        synth.init(48000.0, 4);
        PartParams pp = sysex::toPart(bank.empty() ? sysex::Dx7Voice{} : bank[0]);
        synth.part(0).voice = pp.voice;
        synth.recompile(0);
        synth.noteOn(0, 60, 100);
        std::vector<float> x;
        for (int i = 0; i < 24000; ++i) { float l, r; synth.renderStereo(l, r); x.push_back(l); }
        float rms = computeRms(x, 4800, 19200);
        std::printf("  (Dexed_01.syx: %zu vozes, 1a = \"%s\", 2a = \"%s\"; tocando a 1a: RMS %.3f)\n", bank.size(),
                    bank.empty() ? "" : bank[0].name().c_str(), bank.size() > 1 ? bank[1].name().c_str() : "", rms);
        check(sounds && rms > 0.01f && std::isfinite(rms), "SysEx DX7: banco real (Dexed_01.syx) le as 32 vozes e a primeira toca");
    } else {
        std::printf("  (Dexed_01.syx nao encontrado: teste com banco real pulado)\n");
    }
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

// roda o rack isolado: entrada mono gen(i) nos envios (como o Synth faz), blocos de 256
static std::vector<float> runRack(FxRack& rack, const std::function<float(int)>& gen, int n, std::vector<float>* right = nullptr) {
    std::vector<float> out;
    float L[256], R[256], a1[256], b1[256], a2[256], b2[256];
    for (int at = 0; at < n; at += 256) {
        const int len = std::min(256, n - at);
        const FxRackParams& p = rack.beginBlock();
        for (int i = 0; i < len; ++i) {
            const float x = gen(at + i);
            L[i] = R[i] = x * p.dry[0];
            a1[i] = b1[i] = x * p.send[0][0];
            a2[i] = b2[i] = x * p.send[0][1];
        }
        FxContext ctx;
        rack.process(L, R, a1, b1, a2, b2, len, ctx);
        for (int i = 0; i < len; ++i) { out.push_back(L[i]); if (right) right->push_back(R[i]); }
    }
    return out;
}

static void testGlobalFxChainDisabledIsIdentity() {
    FxRack rack;
    rack.prepare(48000.0);
    rack.setParams(FxRackParams{}); // padrao: tudo vazio, as partes entram na cadeia 1
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> in(4800);
    for (float& x : in) x = d(rng);
    auto out = runRack(rack, [&](int i) { return in[static_cast<size_t>(i)]; }, 4800);
    check(out == in, "rack de efeitos vazio e identidade EXATA");
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

// --- [ADICIONADO em 2026-10-29] rack: envios, serie/paralelo, nivel/pan, troca de tipo ---
static FxSlotParams eqCut() { // EQ cortando tudo ~-18 dB perto de 1 kHz
    FxSlotParams s;
    s.setKind(FxKind::Eq3);
    s.p[1] = -18.0f; s.p[3] = -18.0f; s.p[4] = 0.3f; s.p[6] = -18.0f;
    return s;
}
static float rackGainAt1k(const FxRackParams& p) {
    FxRack rack;
    rack.prepare(48000.0);
    rack.setParams(p);
    auto out = runRack(rack, [](int i) { return sineAt(i, 1000.0f); }, 24000);
    return rmsTail(out, 12000) / (0.5f / std::sqrt(2.0f));
}

static void testFxRackRouting() {
    FxRackParams one;
    one.chain[0].slot[0] = eqCut();
    const float g = rackGainAt1k(one);

    FxRackParams par = one, ser = one;
    par.chain[1].slot[0] = eqCut();
    par.send[0][1] = 1.0f;
    ser = par;
    ser.serial = true;
    const float gp = rackGainAt1k(par), gs = rackGainAt1k(ser);
    std::printf("  (1 EQ: %.3f; duas cadeias com EQ, paralelo %.3f (esperado %.3f), serie %.3f (esperado %.3f))\n", g, gp, 2 * g, gs,
                g * (1 + g));
    check(g < 0.2f && std::fabs(gp - 2 * g) < 0.01f * g && std::fabs(gs - g * (1 + g)) < 0.01f * g,
          "rack: paralelo soma as cadeias; em serie a cadeia 1 entra na 2 (junto com o envio da parte)");

    FxRackParams dry;
    dry.dry[0] = 1.0f; // seco + cadeia 1 vazia = 2x
    FxRackParams lvl;
    lvl.chain[0].level = 0.5f;
    lvl.chain[0].pan = 1.0f;
    FxRack rack;
    rack.prepare(48000.0);
    rack.setParams(dry);
    auto d = runRack(rack, [](int i) { return sineAt(i, 300.0f); }, 2048);
    rack.setParams(lvl);
    std::vector<float> rgt;
    auto l = runRack(rack, [](int i) { return sineAt(i, 300.0f); }, 2048, &rgt);
    bool dryOk = true, panOk = true;
    for (int i = 0; i < 2048; ++i) {
        dryOk &= d[static_cast<size_t>(i)] == 2.0f * sineAt(i, 300.0f);
        panOk &= l[static_cast<size_t>(i)] == 0.0f && rgt[static_cast<size_t>(i)] == 0.5f * sineAt(i, 300.0f);
    }
    check(dryOk, "rack: nivel seco da parte vai direto pra saida (seco 1 + cadeia vazia = 2x)");
    check(panOk, "rack: nivel e pan (balanco) da cadeia: nivel 0.5, pan todo a direita -> E mudo, D pela metade");

    FxRackParams off = one, mix0 = one;
    off.chain[0].slot[0].on = false;
    mix0.chain[0].slot[0].mix = 0.0f;
    for (auto* q : {&off, &mix0}) {
        FxRack r2;
        r2.prepare(48000.0);
        r2.setParams(*q);
        auto o = runRack(r2, [](int i) { return sineAt(i, 1000.0f); }, 4800);
        bool same = true;
        for (int i = 0; i < 4800; ++i) same &= o[static_cast<size_t>(i)] == sineAt(i, 1000.0f);
        check(same, q == &off ? "rack: slot desligado e identidade exata" : "rack: slot com mix 0 e identidade exata");
    }
}

static void testFxRackSendsPerPart() {
    // Parte A (toda a esquerda) so na cadeia 1 (vazia); Parte B (toda a direita)
    // so na cadeia 2, com o EQ cortando: so a B fica mais baixa
    auto render = [](bool cutOnB) {
        Synth synth;
        synth.init(48000.0, 8);
        for (int p : {0, 1}) {
            synth.part(p).enabled = true;
            synth.part(p).pan = p == 0 ? -1.0f : 1.0f;
            synth.recompile(p);
        }
        FxRackParams r;
        r.send[1][0] = 0.0f;
        r.send[1][1] = 1.0f;
        if (cutOnB) r.chain[1].slot[0] = eqCut();
        synth.setFxRack(r);
        synth.noteOn(0, 69, 100);
        synth.noteOn(1, 69, 100);
        std::vector<float> L, R;
        for (int i = 0; i < 24000; ++i) { float l, rr; synth.renderStereo(l, rr); L.push_back(l); R.push_back(rr); }
        return std::make_pair(computeRms(L, 4800, 19200), computeRms(R, 4800, 19200));
    };
    auto [a0, b0] = render(false);
    auto [a1, b1] = render(true);
    std::printf("  (Parte A: %.4f -> %.4f; Parte B: %.4f -> %.4f com o EQ so na cadeia 2)\n", a0, a1, b0, b1);
    check(a1 == a0 && b1 < 0.3f * b0, "rack: envios por parte -- o efeito da cadeia 2 so pega a parte que manda pra ela");
}

static void testFxRackKindSwapWhilePlaying() {
    // outra thread troca o tipo dos slots sem parar enquanto o audio roda:
    // sem travar, sem NaN (DSP criado fora do audio, apagado so 2 blocos depois)
    FxRack rack;
    rack.prepare(48000.0);
    std::atomic<bool> stop{false};
    std::atomic<int> swaps{0};
    std::thread ui([&] {
        const FxKind kinds[] = {FxKind::ReverbClassic, FxKind::Chorus, FxKind::TapeDelay, FxKind::Off, FxKind::Compressor};
        int k = 0;
        while (!stop.load()) {
            FxRackParams p;
            for (int c = 0; c < kFxChains; ++c)
                for (int sl = 0; sl < kFxSlots; ++sl) p.chain[c].slot[sl].setKind(kinds[(k + c + sl) % 5]);
            p.send[0][1] = 1.0f;
            rack.setParams(p);
            ++k;
            swaps.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    bool finite = true;
    int blocks = 0; // renderiza (mais rapido que tempo real) ate a outra thread ter trocado bastante
    while (swaps.load() < 200 && blocks < 200000) {
        auto out = runRack(rack, [](int i) { return sineAt(i, 330.0f); }, 256);
        for (float x : out) finite &= std::isfinite(x);
        ++blocks;
    }
    stop = true;
    ui.join();
    std::printf("  (%d trocas de tipo durante %d blocos de audio)\n", swaps.load(), blocks);
    check(finite && swaps.load() > 50, "rack: trocar o tipo dos slots durante o audio nao trava nem gera NaN");
}

// --- [ADICIONADO em 2026-10-29] todos os efeitos do rack ---
// roda UM slot com o tipo k e parametros dados sobre gen(i) (estereo: R defasado)
static std::pair<std::vector<float>, std::vector<float>> runSlot(FxKind k, const std::function<void(FxSlotParams&)>& setup,
                                                                   const std::function<float(int)>& gen, int n, float bpm = 120.0f) {
    FxRack rack;
    rack.prepare(48000.0);
    FxRackParams p;
    p.chain[0].slot[0].setKind(k);
    p.chain[0].slot[0].mix = 1.0f;
    if (setup) setup(p.chain[0].slot[0]);
    rack.setParams(p);
    std::vector<float> L, R;
    float a[256], b[256], z1[256], z2[256], d1[256], d2[256];
    double beat = 0.0;
    for (int at = 0; at < n; at += 256) {
        const int len = std::min(256, n - at);
        rack.beginBlock();
        for (int i = 0; i < len; ++i) { a[i] = gen(at + i); b[i] = gen(at + i + 7); d1[i] = d2[i] = 0.0f; z1[i] = z2[i] = 0.0f; }
        FxContext ctx;
        ctx.bpm = bpm;
        ctx.beatPos = beat;
        beat += len * bpm / (60.0 * 48000.0);
        rack.process(d1, d2, a, b, z1, z2, len, ctx);
        for (int i = 0; i < len; ++i) { L.push_back(d1[i]); R.push_back(d2[i]); }
    }
    return {L, R};
}

static void testAllFxKindsStable() {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(0.0f, 1.0f), noise(-1.0f, 1.0f);
    std::vector<float> nz(48000);
    for (float& x : nz) x = noise(rng);
    std::string bad;
    int runs = 0;
    for (int k = 1; k < static_cast<int>(FxKind::Count); ++k) {
        const FxKind kind = static_cast<FxKind>(k);
        const FxInfo& info = fxInfo(kind);
        for (int variant = 0; variant < 5; ++variant) { // padrao, minimos, maximos, 2 aleatorios
            for (float level : {0.01f, 0.5f, 2.0f}) {
                auto set = [&](FxSlotParams& s) {
                    for (int i = 0; i < info.nParams; ++i) {
                        const auto& pi = info.params[i];
                        if (variant == 1) s.p[i] = pi.min;
                        else if (variant == 2) s.p[i] = pi.max;
                        else if (variant >= 3) s.p[i] = pi.min + (pi.max - pi.min) * u(rng);
                    }
                };
                for (int sig = 0; sig < 2; ++sig) {
                    auto [L, R] = runSlot(kind, set, [&](int i) { return level * (sig == 0 ? nz[static_cast<size_t>(i) % nz.size()] : std::sin(0.0571f * i)); }, 24000);
                    float pk = 0.0f;
                    bool fin = true;
                    for (size_t i = 0; i < L.size(); ++i) { fin &= std::isfinite(L[i]) && std::isfinite(R[i]); pk = std::max({pk, std::fabs(L[i]), std::fabs(R[i])}); }
                    ++runs;
                    if (!fin || pk > 40.0f * std::max(level, 0.05f)) {
                        char b[128];
                        std::snprintf(b, sizeof b, " %s(v%d nivel %.2f %s pico %.1f%s)", info.id, variant, level, sig ? "seno" : "ruido", pk, fin ? "" : " NaN");
                        bad += b;
                    }
                }
            }
        }
    }
    // [2026-10-29] e todos SOAM com os valores padrao (um bug deixou o Micro
    // mudo com a forma em exatamente 0.5 e os testes de estabilidade passavam)
    std::string mute;
    for (int k = 1; k < static_cast<int>(FxKind::Count); ++k) {
        auto [L, R] = runSlot(static_cast<FxKind>(k), nullptr, [](int i) { return 0.5f * std::sin(0.0571f * i); }, 48000);
        if (computeRms(L, 24000, 24000) + computeRms(R, 24000, 24000) < 0.002f) { mute += " "; mute += fxInfo(static_cast<FxKind>(k)).id; }
    }
    const std::string muteMsg = "todos os efeitos do rack produzem som com os valores padrao" + (mute.empty() ? std::string() : " -- mudos:" + mute);
    check(mute.empty(), muteMsg.c_str());
    std::printf("  (%d tipos x 5 ajustes x 3 niveis x ruido/seno = %d rodadas)\n", static_cast<int>(FxKind::Count) - 1, runs);
    const std::string msg = "todos os efeitos do rack: estaveis e finitos com parametros nos extremos e sinais de -40 a +6 dB" + (bad.empty() ? std::string() : " --" + bad);
    check(bad.empty(), msg.c_str());
}

static void testFxKindsCost() {
    // custo de 1 s de audio estereo por tipo (o rack roda isso por slot ativo)
    std::string top;
    float worst = 0.0f;
    std::vector<std::pair<float, std::string>> costs;
    for (int k = 1; k < static_cast<int>(FxKind::Count); ++k) {
        const FxKind kind = static_cast<FxKind>(k);
        auto t0 = std::chrono::steady_clock::now();
        auto out = runSlot(kind, nullptr, [](int i) { return 0.3f * std::sin(0.05f * i); }, 48000);
        const float sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        costs.push_back({out.first[100] == 12345.0f ? 0.0f : sec, fxInfo(kind).id});
        worst = std::max(worst, sec);
    }
    std::sort(costs.rbegin(), costs.rend());
    std::printf("  (mais caros por slot: ");
    for (int i = 0; i < 5; ++i) std::printf("%s %.1f%%%s", costs[static_cast<size_t>(i)].second.c_str(), costs[static_cast<size_t>(i)].first * 100.0f, i < 4 ? ", " : ")\n");
    check(worst < 0.10f, "cada efeito do rack custa menos de 10% de um nucleo (inclui criar o DSP)");
}

static float bandRms(const std::vector<float>& x, double f, size_t from, size_t len) {
    std::vector<float> seg(x.begin() + static_cast<long>(from), x.begin() + static_cast<long>(from + len));
    return goertzelMagnitude(seg, f, 48000.0);
}
// energia na regiao +-7% em volta de f (o deslocador granular espalha um seno
// deslocado em bandas laterais na taxa dos graos, com um vale bem em cima de f)
static float regionRms(const std::vector<float>& x, double f, size_t from, size_t len) {
    double e = 0.0;
    for (double h = f * 0.93; h <= f * 1.07; h += 5.0) { const float m = bandRms(x, h, from, len); e += m * m; }
    return static_cast<float>(std::sqrt(e));
}

static void testFxBehaviour() {
    auto sine = [](float hz, float a) { return [hz, a](int i) { return a * std::sin(2.0f * kPi * hz * static_cast<float>(i) / 48000.0f); }; };
    // overdrive: mais drive = mais 3o harmonico
    {
        auto lo = runSlot(FxKind::Overdrive, [](FxSlotParams& s) { s.p[0] = 0.1f; }, sine(220.0f, 0.5f), 24000).first;
        auto hi = runSlot(FxKind::Overdrive, [](FxSlotParams& s) { s.p[0] = 0.9f; }, sine(220.0f, 0.5f), 24000).first;
        const float r0 = bandRms(lo, 660.0, 12000, 9600) / bandRms(lo, 220.0, 12000, 9600), r1 = bandRms(hi, 660.0, 12000, 9600) / bandRms(hi, 220.0, 12000, 9600);
        std::printf("  (overdrive: 3o harmonico/fundamental %.3f -> %.3f)\n", r0, r1);
        check(r1 > 3.0f * r0, "overdrive (Airwindows Drive): mais drive gera mais harmonicos");
    }
    // pitch shifter +12: energia em 2f; -12: em f/2
    {
        auto up = runSlot(FxKind::PitchShift, [](FxSlotParams& s) { s.p[0] = 12.0f; }, sine(440.0f, 0.5f), 24000).first;
        auto dn = runSlot(FxKind::PitchShift, [](FxSlotParams& s) { s.p[0] = -12.0f; }, sine(440.0f, 0.5f), 24000).first;
        const float u2 = bandRms(up, 880.0, 9600, 9600), u1 = bandRms(up, 440.0, 9600, 9600);
        const float d2 = bandRms(dn, 220.0, 9600, 9600), d1 = bandRms(dn, 440.0, 9600, 9600);
        std::printf("  (pitch +12: 880/440 = %.1f; -12: 220/440 = %.1f)\n", u2 / u1, d2 / d1);
        check(u2 > 5.0f * u1 && d2 > 5.0f * d1, "pitch shifter (Clouds): +12 st vai pra 2f, -12 st pra f/2");
    }
    // delay digital sincronizado: 1/4 a 120 BPM = eco em 0.5 s
    {
        auto [L, R] = runSlot(FxKind::DigitalDelay, [](FxSlotParams& s) { s.p[1] = 3.0f; s.p[2] = 0.0f; s.p[4] = 20000.0f; s.p[5] = 20.0f; },
                              [](int i) { return i < 480 ? 0.5f : 0.0f; }, 48000);
        int peak = 0;
        for (int i = 1; i < 48000; ++i) if (std::fabs(L[static_cast<size_t>(i)]) > std::fabs(L[static_cast<size_t>(peak)])) peak = i;
        std::printf("  (delay 1/4 a 120 BPM: eco em %.3f s, amostra %d)\n", peak / 48000.0, peak);
        check(peak >= 24000 && peak < 24000 + 480 + 64, "delay digital: sync 1/4 a 120 BPM poe o eco em 0.5 s");
    }
    // delay reverso: uma rampa que desce volta subindo
    {
        auto [L, R] = runSlot(FxKind::ReverseDelay, [](FxSlotParams& s) { s.p[0] = 500.0f; s.p[2] = 0.0f; },
                              [](int i) { return i < 12000 ? 0.8f * (1.0f - i / 12000.0f) * std::sin(0.2f * i) : 0.0f; }, 72000);
        // depois do pedaco gravado, a energia do eco cresce (ataque invertido)
        const float a = computeRms(L, 24000 + 1000, 4000), b = computeRms(L, 24000 + 14000, 4000);
        std::printf("  (reverso: RMS no comeco %.3f, no fim do trecho %.3f)\n", a, b);
        check(b > a * 1.3f, "delay reverso: o trecho volta de tras pra frente (o que decaia agora cresce)");
    }
    // gate: ruido baixo e cortado, alto passa
    {
        auto quiet = runSlot(FxKind::Gate, [](FxSlotParams& s) { s.p[0] = 0.3f; }, [](int i) { return 0.02f * std::sin(0.03f * i); }, 24000).first;
        auto loud = runSlot(FxKind::Gate, [](FxSlotParams& s) { s.p[0] = 0.3f; }, [](int i) { return 0.8f * std::sin(0.03f * i); }, 24000).first;
        const float q = computeRms(quiet, 12000, 12000) / (0.02f / std::sqrt(2.0f)), l = computeRms(loud, 12000, 12000) / (0.8f / std::sqrt(2.0f));
        std::printf("  (gate: sinal baixo sai x%.3f, alto x%.2f)\n", q, l);
        check(q < 0.1f && l > 0.5f, "noise gate (Airwindows Gatelope): fecha abaixo do threshold e deixa passar acima");
    }
    // tremolo: o volume oscila
    {
        auto t = runSlot(FxKind::Tremolo, [](FxSlotParams& s) { s.p[0] = 0.5f; s.p[1] = 1.0f; }, sine(440.0f, 0.5f), 96000).first;
        float lo = 1.0f, hi = 0.0f;
        for (int w = 4; w < 40; ++w) { const float r = computeRms(t, static_cast<size_t>(w * 2400), 2400); lo = std::min(lo, r); hi = std::max(hi, r); }
        std::printf("  (tremolo: RMS em janelas de 50 ms entre %.3f e %.3f)\n", lo, hi);
        check(hi > 3.0f * lo, "tremolo (Airwindows): o volume oscila");
    }
    // auto-wah: brilho segue o envelope
    {
        auto [L, R] = runSlot(FxKind::Wah, [](FxSlotParams& s) { s.p[0] = 1.0f; s.p[1] = 0.0f; s.p[2] = 8.0f; s.p[4] = 1.0f; },
                              [](int i) { const float env = i < 24000 ? 0.05f : 0.8f; return env * (std::sin(0.02f * i) + std::sin(0.31f * i)); }, 48000);
        const float softBright = bandRms(L, 48000.0 * 0.31 / (2 * kPi), 12000, 9600) / bandRms(L, 48000.0 * 0.02 / (2 * kPi), 12000, 9600);
        const float loudBright = bandRms(L, 48000.0 * 0.31 / (2 * kPi), 36000, 9600) / bandRms(L, 48000.0 * 0.02 / (2 * kPi), 36000, 9600);
        std::printf("  (auto-wah: agudo/grave com sinal fraco %.2f, forte %.2f)\n", softBright, loudBright);
        check(loudBright > 2.0f * softBright, "auto-wah (Faust crybaby): tocar mais forte abre o filtro");
    }
    // filtro com LFO sincronizado: periodo = 1/4 a 120 BPM (0.5 s)
    {
        auto [L, R] = runSlot(FxKind::FilterLfo, [](FxSlotParams& s) { s.p[0] = 0.0f; s.p[1] = 200.0f; s.p[5] = 3.0f; s.p[6] = 5.0f; },
                              [](int i) { return 0.3f * std::sin(0.27f * i); }, 96000);
        std::vector<float> e;
        for (int w = 0; w < 160; ++w) e.push_back(computeRms(L, static_cast<size_t>(w * 600), 600));
        int bestLag = 0;
        double best = -1e9;
        for (int lag = 20; lag < 60; ++lag) { double c = 0; for (size_t i = 0; i + lag < e.size(); ++i) c += e[i] * e[i + static_cast<size_t>(lag)]; if (c > best) { best = c; bestLag = lag; } }
        std::printf("  (filtro com LFO 1/4 a 120 BPM: periodo %.3f s)\n", bestLag * 600 / 48000.0);
        check(std::abs(bestLag * 600 / 48000.0 - 0.5) < 0.03, "filtro com LFO: sync 1/4 a 120 BPM varre a cada 0.5 s");
    }
    // rotary: 1 s em lento, depois a chave vai pra rapido -- a corneta acelera com inercia
    {
        fxextra::Rotary rot;
        rot.prepare(48000.0);
        std::vector<float> L, R;
        for (int b = 0; b < 48000 * 4 / 256; ++b) {
            float l[256], r[256];
            for (int i = 0; i < 256; ++i) l[i] = r[i] = 0.5f * std::sin(2.0f * kPi * 3000.0f * static_cast<float>(b * 256 + i) / 48000.0f);
            rot.process(b * 256 >= 48000, 0.8f, 6.7f, 0.0f, 1.0f, 0.0f, 1.0f, l, r, 256);
            L.insert(L.end(), l, l + 256);
        }
        auto rate = [&](size_t from) { // cruzamentos do envelope (tremolo) pela media em 1 s
            std::vector<float> e;
            for (int w = 0; w < 200; ++w) e.push_back(computeRms(L, from + static_cast<size_t>(w * 240), 240));
            float m = 0; for (float x : e) m += x; m /= e.size();
            int c = 0; for (size_t i = 1; i < e.size(); ++i) c += (e[i - 1] < m) != (e[i] < m);
            return c / 2.0f;
        };
        const float slow = rate(0), ramp = rate(48000), fast = rate(48000 * 3);
        std::printf("  (rotary: lento ~%.1f Hz; 1o segundo depois da chave ~%.1f Hz; depois ~%.1f Hz)\n", slow, ramp, fast);
        check(slow < 1.5f && fast > 5.5f && ramp > slow && ramp < fast - 0.5f, "rotary: lento -> rapido acelera aos poucos (inercia da corneta)");
    }
    // fita: velocidade baixa perde agudo; wow modula a altura
    {
        auto fast = runSlot(FxKind::TapeSim, [](FxSlotParams& s) { s.p[4] = 30.0f; s.p[5] = 2.0f; }, [](int i) { return 0.2f * (std::sin(0.03f * i) + std::sin(1.05f * i)); }, 48000).first;
        auto slow = runSlot(FxKind::TapeSim, [](FxSlotParams& s) { s.p[4] = 3.75f; s.p[5] = 2.0f; }, [](int i) { return 0.2f * (std::sin(0.03f * i) + std::sin(1.05f * i)); }, 48000).first;
        const double fh = 48000.0 * 1.05 / (2 * kPi), fl = 48000.0 * 0.03 / (2 * kPi);
        const float rf = bandRms(fast, fh, 24000, 9600) / bandRms(fast, fl, 24000, 9600), rs = bandRms(slow, fh, 24000, 9600) / bandRms(slow, fl, 24000, 9600);
        std::printf("  (fita: agudo/grave a 30 ips %.2f, a 3.75 ips %.2f)\n", rf, rs);
        check(rs < 0.7f * rf, "simulador de fita (CHOW): fita lenta perde agudos (perdas da cabeca)");
    }
}

// --- [ADICIONADO em 2026-10-29] reverbs estilo BigSky ---
static std::vector<float> envDb(const std::vector<float>& x, int win) { // RMS em janelas, em dB
    std::vector<float> e;
    for (size_t a = 0; a + static_cast<size_t>(win) <= x.size(); a += static_cast<size_t>(win))
        e.push_back(20.0f * std::log10(computeRms(x, a, static_cast<size_t>(win)) + 1e-9f));
    return e;
}

static void testReverbMachines() {
    auto impulse = [](int i) { return i < 48 ? 0.5f : 0.0f; };
    // Hall: o decay medido (inclinacao da cauda) acompanha o knob
    auto t60 = [&](float decay) {
        auto L = runSlot(FxKind::Hall, [decay](FxSlotParams& s) { s.p[0] = decay; s.p[1] = 0.0f; }, impulse, 48000 * 6).first;
        auto e = envDb(L, 2400);
        // inclinacao entre 0.3 s e o ponto em que caiu 40 dB
        const float start = e[6];
        size_t k = 6;
        while (k < e.size() && e[k] > start - 40.0f) ++k;
        return (k - 6) * 0.05f * 60.0f / 40.0f;
    };
    const float t1 = t60(1.0f), t4 = t60(4.0f);
    std::printf("  (Hall: RT60 medido com decay 1 s = %.2f s, com 4 s = %.2f s)\n", t1, t4);
    check(t4 > 2.5f * t1 && t1 > 0.5f && t1 < 2.0f, "Hall (Dragonfly): o decay medido acompanha o knob");
    // pre-delay: nada molhado antes dele
    {
        auto L = runSlot(FxKind::Hall, [](FxSlotParams& s) { s.p[1] = 60.0f; s.p[6] = 0.0f; }, impulse, 48000).first;
        const float before = computeRms(L, 0, 2400), after = computeRms(L, 3360, 4800);
        std::printf("  (Hall com pre-delay 60 ms: RMS antes de 50 ms %.5f, depois de 70 ms %.4f)\n", before, after);
        check(before < 0.05f * after, "Hall: o pre-delay atrasa a cauda");
    }
    // shimmer: aparece energia uma oitava acima de uma nota que ja acabou
    {
        auto L = runSlot(FxKind::Shimmer, [](FxSlotParams& s) { s.p[1] = 12.0f; s.p[2] = 12.0f; s.p[3] = 0.8f; s.p[4] = 16000.0f; },
                         [](int i) { return i < 9600 ? 0.4f * std::sin(2.0f * kPi * 440.0f * i / 48000.0f) : 0.0f; }, 48000 * 3).first;
        const float oct = regionRms(L, 880.0, 48000 * 2, 9600), fund = regionRms(L, 440.0, 48000 * 2, 9600);
        auto plainHall = runSlot(FxKind::Hall, [](FxSlotParams& s) { s.p[0] = 4.0f; s.p[3] = 16000.0f; },
                                 [](int i) { return i < 9600 ? 0.4f * std::sin(2.0f * kPi * 440.0f * i / 48000.0f) : 0.0f; }, 48000 * 3).first;
        const float hOct = regionRms(plainHall, 880.0, 48000 * 2, 9600), hFund = regionRms(plainHall, 440.0, 48000 * 2, 9600);
        std::printf("  (2 s depois: oitava/fundamental no Shimmer %.2f, no Hall puro %.3f)\n", oct / fund, hOct / hFund);
        check(oct / fund > 10.0f * (hOct / hFund), "Shimmer: a cauda sobe uma oitava (pitch shifter do Clouds na realimentacao)");
        // a cauda morre: antes o Hall inteiro ficava dentro do laco e com amount
        // 0.5 a cauda crescia pra sempre depois da nota
        float worst = 0.0f;
        for (float decay : {2.0f, 10.0f})
            for (float amount : {0.5f, 0.9f}) {
                auto T = runSlot(FxKind::Shimmer, [=](FxSlotParams& sl) { sl.p[0] = decay; sl.p[3] = amount; },
                                 [](int i) { return i < 48000 ? 0.3f * std::sin(0.0571f * i) : 0.0f; }, 48000 * 12).first;
                worst = std::max(worst, computeRms(T, 48000 * 11, 48000) / computeRms(T, 0, 48000));
            }
        std::printf("  (Shimmer: cauda 10 s depois da nota, pior caso %.1f dB)\n", 20.0f * std::log10(std::max(worst, 1e-9f)));
        check(worst < 0.01f, "Shimmer: a cauda decai (laco de realimentacao com ganho < 1) mesmo com decay 10 s e amount 0.9");
    }
    // nonlinear: Reverse cresce ate o fim do tempo e corta; Gate fica plano e corta
    {
        auto rev = runSlot(FxKind::Nonlinear, [](FxSlotParams& s) { s.p[0] = 0.0f; s.p[1] = 400.0f; s.p[2] = 0.3f; }, impulse, 48000).first;
        auto gate = runSlot(FxKind::Nonlinear, [](FxSlotParams& s) { s.p[0] = 3.0f; s.p[1] = 400.0f; s.p[2] = 0.3f; }, impulse, 48000).first;
        const float r1 = computeRms(rev, 2400, 4800), r2 = computeRms(rev, 13000, 4800), rAfter = computeRms(rev, 24000, 4800);
        const float g1 = computeRms(gate, 2400, 4800), g2 = computeRms(gate, 13000, 4800), gAfter = computeRms(gate, 24000, 4800);
        std::printf("  (Nonlinear Reverse: %.4f -> %.4f, depois %.5f; Gate: %.4f -> %.4f, depois %.5f)\n", r1, r2, rAfter, g1, g2, gAfter);
        check(r2 > 2.0f * r1 && rAfter < 0.1f * r2, "Nonlinear Reverse: a cauda cresce e corta no fim do tempo");
        check(std::fabs(g2 / g1 - 1.0f) < 0.5f && gAfter < 0.1f * g2, "Nonlinear Gate: a cauda fica plana e corta");
    }
    // swell: o ataque some (as primeiras dezenas de ms ficam baixas e o som cresce)
    {
        auto [L, R] = runSlot(FxKind::Swell, [](FxSlotParams& s) { s.p[0] = 500.0f; s.p[4] = 0.0f; },
                              [](int i) { return i < 48000 ? 0.4f * std::sin(2.0f * kPi * 330.0f * i / 48000.0f) : 0.0f; }, 48000);
        const float early = computeRms(L, 240, 960), late = computeRms(L, 30000, 4800);
        std::printf("  (Swell: RMS nos primeiros 25 ms %.4f, depois de 0.6 s %.4f)\n", early, late);
        check(early < 0.1f * late, "Swell: o ataque some e a nota cresce aos poucos");
    }
    // reflections: resposta ao impulso feita de picos separados (reflexoes discretas)
    {
        auto L = runSlot(FxKind::Reflections, nullptr, [](int i) { return i == 0 ? 1.0f : 0.0f; }, 9600).first;
        float pk = 0.0f;
        for (float x : L) pk = std::max(pk, std::fabs(x));
        int peaks = 0;
        for (size_t i = 1; i + 1 < L.size(); ++i)
            if (std::fabs(L[i]) > 0.2f * pk && std::fabs(L[i]) >= std::fabs(L[i - 1]) && std::fabs(L[i]) >= std::fabs(L[i + 1])) ++peaks;
        std::printf("  (Reflections: %d picos acima de 20%% do maior em 200 ms)\n", peaks);
        check(peaks >= 4 && peaks < 400, "Reflections (Dragonfly): reflexoes discretas, nao uma cauda continua");
    }
    // rebuild: mudar o tamanho troca o DSP (sem alocar no audio) e continua soando
    {
        FxRack rack;
        rack.prepare(48000.0);
        FxRackParams p;
        p.chain[0].slot[0].setKind(FxKind::Hall);
        rack.setParams(p);
        auto a = runRack(rack, [](int i) { return 0.3f * std::sin(0.05f * i); }, 9600);
        p.chain[0].slot[0].p[2] = 50.0f; // tamanho: parametro "rebuild"
        rack.setParams(p);
        auto b = runRack(rack, [](int i) { return 0.3f * std::sin(0.05f * i); }, 24000);
        bool fin = true;
        for (float x : b) fin &= std::isfinite(x);
        check(fin && computeRms(b, 12000, 9600) > 0.01f, "reverbs: mudar o tamanho recria o DSP na thread da UI e segue soando");
    }
}

// --- [ADICIONADO em 2026-10-29] Micro (granular) ---
// [2026-10-29] rack <-> JSON: ida e volta exata; arquivo com efeito/parametro
// desconhecido carrega o resto; lixo nao vira rack.
static void testFxRackJson() {
    FxRackParams r;
    r.serial = true;
    r.dry[2] = 0.7f; r.send[1][1] = 0.4f;
    r.chain[1].level = 1.3f; r.chain[1].pan = -0.25f;
    r.chain[0].slot[0].setKind(FxKind::Hall); r.chain[0].slot[0].p[0] = 3.5f; r.chain[0].slot[0].mix = 0.42f;
    r.chain[0].slot[2].setKind(FxKind::Micro); r.chain[0].slot[2].on = false; r.chain[0].slot[2].p[0] = 6;
    r.chain[1].slot[3].setKind(FxKind::DigitalDelay); r.chain[1].slot[3].p[1] = 4;
    r.expr = {1, 3, 2, 0.1f, 0.8f}; // feedback do delay
    sanitize(r);
    const std::string text = fxRackToString(r);
    FxRackParams b = fxRackFromString(text);
    check(fxRackToString(b) == text, "rack JSON: ida e volta identica");
    check(b.serial && b.chain[0].slot[2].kind == FxKind::Micro && !b.chain[0].slot[2].on && b.chain[0].slot[2].p[0] == 6.0f,
          "rack JSON: tipo, on e parametro de escolha voltam");
    check(b.expr.chain == 1 && b.expr.slot == 3 && b.expr.param == 2 && b.expr.max == 0.8f, "rack JSON: pedal de expressao volta");

    // parametro desconhecido ignorado, ausente fica no padrao; efeito desconhecido vira vazio
    const std::string odd = R"({"chains":[{"slots":[{"kind":"Hall","mix":0.5,"params":{"decay":4,"naoExiste":9}},
                                           {"kind":"EfeitoDoFuturo","params":{"x":1}}]}],
                                "expression":{"chain":0,"slot":0,"param":"inexistente"}})";
    FxRackParams o = fxRackFromString(odd);
    const FxInfo& hall = fxInfo(FxKind::Hall);
    check(o.chain[0].slot[0].kind == FxKind::Hall && o.chain[0].slot[0].p[0] == 4.0f && o.chain[0].slot[0].p[1] == hall.params[1].def,
          "rack JSON: parametro desconhecido ignorado, ausentes no padrao");
    check(o.chain[0].slot[1].kind == FxKind::Off && o.expr.chain == -1, "rack JSON: efeito/parametro de expressao desconhecido nao quebra");
    FxRackParams junk;
    check(!loadFxRackFile("/nao/existe/fxrack.json", junk), "rack JSON: arquivo ausente nao carrega");
}

// pedal de expressao: o parametro alvo vai de min a max; sem valor do pedal, vale o do slot
static void testFxExpressionPedal() {
    auto level = [](float pedal) {
        FxRack rack;
        rack.prepare(48000.0);
        FxRackParams p;
        p.chain[0].slot[0].setKind(FxKind::Eq3);
        p.chain[0].slot[0].p[6] = 0.0f; // highGain
        p.expr = {0, 0, 6, -18.0f, 18.0f};
        sanitize(p);
        rack.setParams(p);
        if (pedal >= 0.0f) rack.setExpression(pedal);
        auto out = runRack(rack, [](int i) { return sineAt(i, 12000.0f, 0.25f); }, 9600);
        return rmsTail(out, 4800);
    };
    const float none = level(-1.0f), lo = level(0.0f), hi = level(1.0f);
    check(std::fabs(none / (0.25f / std::sqrt(2.0f)) - 1.0f) < 0.1f, "expressao: sem pedal o parametro do slot vale (0 dB)");
    check(hi / lo > 30.0f, "expressao: pedal 0..1 leva o agudo de -18 a +18 dB");
    FxRackParams bad;
    bad.chain[0].slot[0].setKind(FxKind::Hall);
    bad.expr = {0, 0, 2, 10, 60}; // size e "rebuild": nao pode ser alvo
    sanitize(bad);
    check(bad.expr.chain == -1, "expressao: parametro rebuild nao pode ser alvo");
}

// presets de fabrica do rack: nomes de parametros validos, estaveis e com som
static void testFxFactoryPresets() {
    using namespace dx7fm_app::fxfactory;
    fxFactoryErrors() = 0;
    auto bank = fxFactoryBank();
    check(fxFactoryErrors() == 0, "presets de efeitos de fabrica: todos os parametros existem");
    check(bank.size() >= 10, "presets de efeitos de fabrica: pelo menos 10");
    bool allOk = true;
    float maxPeak = 0.0f;
    for (const auto& pr : bank) {
        FxRackParams p = pr.rack;
        FxRackParams q = fxRackFromString(fxRackToString(p));
        if (fxRackToString(q) != fxRackToString(p)) { std::printf("  JSON diverge: %s\n", pr.name.c_str()); allOk = false; }
        // seno sustentado (pode cair num formante/modo: so estabilidade) e ruido (nivel)
        std::mt19937 rng(7);
        std::normal_distribution<float> nd(0.0f, 0.15f);
        std::vector<float> noise(48000);
        for (float& x : noise) x = nd(rng);
        float peak = 0.0f, gainDb = 0.0f;
        bool finite = true;
        for (int sig = 0; sig < 2; ++sig) {
            FxRack rack;
            rack.prepare(48000.0);
            rack.setParams(p);
            rack.setExpression(0.7f);
            auto out = sig == 0 ? runRack(rack, [](int i) { return i < 24000 ? 0.4f * std::sin(0.0571f * i) * (1.0f + 0.5f * std::sin(0.0003f * i)) : 0.0f; }, 48000)
                                : runRack(rack, [&](int i) { return noise[static_cast<size_t>(i)]; }, 48000);
            for (float x : out) { finite = finite && std::isfinite(x); if (sig == 0) peak = std::max(peak, std::fabs(x)); }
            if (sig == 1) gainDb = 20.0f * std::log10(std::max(1e-9f, computeRms(out, 12000, 36000) / computeRms(noise, 12000, 36000)));
        }
        maxPeak = std::max(maxPeak, peak);
        if (!finite || peak > 3.0f || gainDb < -24.0f || gainDb > 8.0f) {
            std::printf("  preset %s: finito=%d pico(seno)=%.2f ganho(ruido)=%+.1f dB\n", pr.name.c_str(), finite, peak, gainDb);
            allOk = false;
        }
    }
    std::printf("  %zu presets de efeitos, maior pico %.2f (entrada 0.6)\n", bank.size(), maxPeak);
    check(allOk, "presets de efeitos de fabrica: JSON ida e volta, sem NaN, pico do seno < 3, ganho em ruido -24..+8 dB");
}

// pedaleira (app/fx_rack_logic.h): arrastar pedais, knobs "rebuild" e desfazer
static void testFxRackUiLogic() {
    using namespace dx7fm_app::fxui;
    FxRackParams r;
    r.chain[0].slot[0].setKind(FxKind::Tremolo);
    r.chain[1].slot[2].setKind(FxKind::Hall);
    r.expr = {0, 0, 1, 0.0f, 1.0f};
    moveSlot(r, 0, 0, 1, 2, false);
    check(r.chain[1].slot[2].kind == FxKind::Tremolo && r.chain[0].slot[0].kind == FxKind::Hall && r.expr.chain == 1 && r.expr.slot == 2,
          "pedaleira: arrastar troca os pedais e o pedal de expressao vai junto");
    moveSlot(r, 1, 2, 0, 0, true);
    check(r.chain[0].slot[0].kind == FxKind::Tremolo && r.chain[1].slot[2].kind == FxKind::Tremolo && r.expr.chain == 1,
          "pedaleira: Ctrl+arrastar copia por cima");

    // arrastando o knob de tamanho do Hall: publica o resto, segura o tamanho
    struct FakeSynth { int publishes = 0; FxRackParams last; void setFxRack(const FxRackParams& p) { ++publishes; last = p; } } synth;
    FxRackUi ui;
    FxRackParams h;
    h.chain[0].slot[0].setKind(FxKind::Hall);
    setRack(ui, synth, h, 0.0);
    ui.committed = ui.edit;
    ui.edit.chain[0].slot[0].p[2] = 50.0f; // size (rebuild)
    ui.edit.chain[0].slot[0].mix = 0.6f;
    syncRack(ui, synth, true, true, 0.1);
    check(synth.last.chain[0].slot[0].mix == 0.6f && synth.last.chain[0].slot[0].p[2] == h.chain[0].slot[0].p[2],
          "pedaleira: durante o arraste o parametro rebuild nao e publicado (o resto e)");
    syncRack(ui, synth, false, false, 0.2);
    check(synth.last.chain[0].slot[0].p[2] == 50.0f && ui.undo.size() == 1, "pedaleira: ao soltar publica o tamanho e vira 1 passo de desfazer");
    undoRack(ui, synth, 0.3);
    check(ui.edit.chain[0].slot[0].p[2] == h.chain[0].slot[0].p[2] && synth.last.chain[0].slot[0].mix == h.chain[0].slot[0].mix && ui.redo.size() == 1,
          "pedaleira: desfazer volta e publica");
    redoRack(ui, synth, 0.4);
    check(ui.edit.chain[0].slot[0].p[2] == 50.0f, "pedaleira: refazer");
    check(choiceCount("A|B|C") == 3 && choiceAt("A|B|C", 1) == "B" && choiceAt("A|B|C", 2) == "C", "pedaleira: escolhas 'A|B|C'");
}

static void testMicroGranular() {
    auto tone = [](float hz, int len) { return [hz, len](int i) { return i < len ? 0.4f * std::sin(2.0f * kPi * hz * i / 48000.0f) : 0.0f; }; };
    // Hold: a entrada acaba em 1 s; sem Hold o Mosaic some, com Hold continua
    {
        auto setup = [](float hold) { return [hold](FxSlotParams& s) { s.p[0] = 0.0f; s.p[3] = 0.0f; s.p[4] = 200.0f; s.p[9] = 0.0f; s.p[10] = hold; }; };
        auto off = runSlot(FxKind::Micro, setup(0.0f), tone(330.0f, 48000), 48000 * 3).first;
        // o Hold e ligado so depois que a nota entrou (congela o que foi gravado)
        FxRack rack;
        rack.prepare(48000.0);
        FxRackParams p;
        p.chain[0].slot[0].setKind(FxKind::Micro);
        p.chain[0].slot[0].mix = 1.0f;
        setup(0.0f)(p.chain[0].slot[0]);
        rack.setParams(p);
        auto first = runRack(rack, tone(330.0f, 48000), 40000);
        p.chain[0].slot[0].p[10] = 1.0f;
        rack.setParams(p);
        auto held = runRack(rack, [](int) { return 0.0f; }, 48000 * 2);
        const float offLate = computeRms(off, 48000 * 2, 24000), heldLate = computeRms(held, 48000, 24000);
        std::printf("  (Micro Mosaic 1 s depois da nota: sem Hold RMS %.5f, com Hold %.4f)\n", offLate, heldLate);
        check(offLate < 0.002f && heldLate > 0.02f, "Micro: Hold congela o buffer (continua soando depois que a entrada para)");
    }
    // Seq: os passos caem na grade (atividade 0.5 -> 1/8 = 0.25 s a 120 BPM)
    {
        auto L = runSlot(FxKind::Micro, [](FxSlotParams& s) { s.p[0] = 1.0f; s.p[1] = 0.5f; s.p[3] = 0.0f; s.p[9] = 0.0f; }, tone(330.0f, 48000 * 4), 48000 * 4).first;
        auto e = envDb(L, 480); // janelas de 10 ms
        int best = 0;
        double bestC = -1e30;
        std::vector<float> lin(e.size());
        for (size_t i = 0; i < e.size(); ++i) lin[i] = std::pow(10.0f, e[i] / 20.0f);
        float m = 0; for (float x : lin) m += x; m /= lin.size();
        for (int lag = 10; lag < 60; ++lag) {
            double c = 0; for (size_t i = 100; i + static_cast<size_t>(lag) < lin.size(); ++i) c += (lin[i] - m) * (lin[i + static_cast<size_t>(lag)] - m);
            if (c > bestC) { bestC = c; best = lag; }
        }
        std::printf("  (Micro Seq a 120 BPM: periodo do envelope %.2f s)\n", best * 0.01);
        check(std::abs(best * 0.01 - 0.25) < 0.03 || std::abs(best * 0.01 - 0.5) < 0.03, "Micro Seq: os passos caem na grade do BPM");
    }
    // Glide: a altura desliza (energia sai da nota original)
    {
        auto g = runSlot(FxKind::Micro, [](FxSlotParams& s) { s.p[0] = 2.0f; s.p[11] = 1.0f; s.p[3] = 0.0f; s.p[9] = 0.0f; }, tone(440.0f, 48000 * 6), 48000 * 6).first;
        auto fixed = runSlot(FxKind::Micro, [](FxSlotParams& s) { s.p[0] = 2.0f; s.p[11] = 0.0f; s.p[3] = 0.0f; s.p[9] = 0.0f; }, tone(440.0f, 48000 * 6), 48000 * 6).first;
        const float gOn = bandRms(g, 440.0, 48000 * 3, 48000) / (computeRms(g, 48000 * 3, 48000) + 1e-9f);
        const float fOn = bandRms(fixed, 440.0, 48000 * 3, 48000) / (computeRms(fixed, 48000 * 3, 48000) + 1e-9f);
        std::printf("  (Micro Glide: energia em 440 Hz relativa -- sem deslize %.1f, com deslize %.1f)\n", fOn, gOn);
        check(gOn < 0.6f * fOn, "Micro Glide: a altura dos loops desliza");
    }
    // Tunnel: drone uma oitava abaixo que acompanha a entrada (300 -> 500 Hz)
    {
        auto T = runSlot(FxKind::Micro, [](FxSlotParams& s) { s.p[0] = 4.0f; s.p[11] = 1.0f; s.p[3] = 0.0f; s.p[9] = 0.0f; },
                         [](int i) { const float hz = i < 72000 ? 300.0f : 500.0f; return 0.4f * std::sin(2.0f * kPi * hz * i / 48000.0f); }, 48000 * 3).first;
        const float before = bandRms(T, 150.0, 48000, 9600) / (bandRms(T, 250.0, 48000, 9600) + 1e-9f);
        const float after = bandRms(T, 250.0, 120000, 9600) / (bandRms(T, 150.0, 120000, 9600) + 1e-9f);
        std::printf("  (Micro Tunnel: oitava abaixo de 300 Hz domina antes (%.1fx), de 500 Hz depois (%.1fx))\n", before, after);
        check(before > 3.0f && after > 3.0f, "Micro Tunnel: drone uma oitava abaixo que acompanha a entrada");
    }
}

static void testGlobalFxOrder() {
    // a ordem dos slots muda o som: compressor antes x depois do reverb
    auto render = [](bool compFirst) {
        GlobalFxParams g;
        g.compressor.enabled = true; g.compressor.thresholdDb = -30.0f; g.compressor.ratio = 10.0f;
        g.reverb.enabled = true; g.reverb.mix = 0.5f;
        if (compFirst) {
            GlobalFxType order[kNumGlobalFx] = {GlobalFxType::Compressor, GlobalFxType::Phaser, GlobalFxType::Flanger,
                                                GlobalFxType::Chorus, GlobalFxType::Delay, GlobalFxType::Reverb, GlobalFxType::Eq};
            std::copy(order, order + kNumGlobalFx, g.order);
        }
        FxRack rack;
        rack.prepare(48000.0);
        rack.setParams(fxRackFromLegacy(g));
        return runRack(rack, [](int i) { return i < 4800 ? sineAt(i, 440.0f) : 0.0f; }, 24000);
    };
    auto def = render(false), cf = render(true);
    float diff = 0.0f;
    for (size_t i = 0; i < def.size(); ++i) diff = std::max(diff, std::abs(def[i] - cf[i]));
    std::printf("  (diferenca maxima: compressor antes x depois do reverb=%.3f)\n", diff);
    check(diff > 0.01f, "ordem dos slots muda o som (compressor antes vs depois do reverb)");
}

static void testGlobalFxCost() {
    GlobalFxParams g;
    g.chorus.enabled = g.phaser.enabled = g.flanger.enabled = g.delay.enabled = true;
    g.reverb.enabled = g.eq.enabled = g.compressor.enabled = true;
    g.phaser.notches = 8;
    float worst = 0.0f;
    for (int t = 0; t < 3; ++t) {
        g.reverb.type = static_cast<ReverbType>(t);
        FxRack rack;
        rack.prepare(48000.0);
        rack.setParams(fxRackFromLegacy(g));
        auto t0 = std::chrono::steady_clock::now();
        auto out = runRack(rack, [](int i) { return sineAt(i, 220.0f); }, 48000);
        float sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, out[100] == 12345.0f ? 0.0f : sec);
    }
    std::printf("  (os 7 efeitos antigos ligados no rack, pior modo de reverb: %.1f%% de um nucleo)\n", worst * 100.0f);
    check(worst < 0.25f, "rack com os 7 efeitos ligados roda bem abaixo do tempo real (< 25% de um nucleo)");
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
    testFoldAdaaReducesAliasing();
    testHalfbandDecimatorResponse();
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
    testPolyphonyGainCatchesChordAttack();
    testPolyphonyGainIsPerPart();
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
    testMpeInputTranslatesMemberChannels();
    testMpeBendsOnlyItsNote();
    testMpeSlideSource();
    testMutatePatch();
    testPatchVolume();
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
    testWavetableSineMatchesSineOperator();
    testWavetableAntiAliasing();
    testWavetableMorphModes();
    testWavetableMorphModulation();
    testWavetableJsonAndSanitize();
    testWavegenImport();
    testWavetableBankCache();
    testWavetableCost();
    testModEnvelopeAsSource();
    testLfoTempoSync();
    testLfoSmoothRandom();
    testLfoSteps();
    testModEnvelopeLoop();
    testAlgoMorphTwoSlots();
    testAlgoMorphEightSlots();
    testAlgoMorphCost();
    testAppSettings();
    testMacros();
    testMidiClockEstimator();
    testGlobalLfo();
    testGlobalLfoRateModulation();
    testArpeggiator();
    testTuningParsers();
    testTuningInEngine();
    testDx7Sysex();
    testNewModDestinations();
    testModulationJson();
    testAdditiveEditorAxes();
    testAdditivePaintStroke();
    testAdditivePresetsAreValid();
    testAdditiveSquarePresetSounds();
    testOpFxFilterCurveMatchesDsp();
    testOpFxCombCurveMatchesDsp();
    testOpFxDefaultsAndSummary();
    testFxKeyTrackMovesFilterWithNote();
    testModSourcesKeyNoteAndVelocity();
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
    testFxRackRouting();
    testFxRackSendsPerPart();
    testFxRackKindSwapWhilePlaying();
    testAllFxKindsStable();
    testFxBehaviour();
    testFxKindsCost();
    testReverbMachines();
    testMicroGranular();
    testFxRackJson();
    testFxExpressionPedal();
    testFxFactoryPresets();
    testFxRackUiLogic();
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

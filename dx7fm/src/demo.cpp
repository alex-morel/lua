#include "dx7_params.h"
#include "dx7_dsp.h"
#include "dx7_engine.h"
#include "dx7_alloc.h"
#include "dx7_patch_io.h"
#include "dx7_algorithms.h"
#include "wav_writer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
        synth.globalFx().chorus.depthMs = 4.0f;
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
    testMultitimbral4Parts();
    testPolyphonyCap();

    std::printf("== Resultado: %d passaram, %d falharam ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

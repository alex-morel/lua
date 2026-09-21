#include "dx7_params.h"
#include "dx7_dsp.h"
#include "dx7_engine.h"
#include "dx7_alloc.h"
#include "dx7_patch_io.h"
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
    testMultitimbral4Parts();
    testPolyphonyCap();

    std::printf("== Resultado: %d passaram, %d falharam ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#pragma once
#include "factory_preset.h"
#include "factory_sy.h"

// [ADICIONADO em 2026-10-29] Patches originais: cada um mostra um recurso do
// Mare Tranquillitatis que os FM classicos nao tinham (MPE, LFO sincronizado no filtro,
// gate global, ring mod, frequency shifter, bitcrusher, matriz com lacos,
// aditiva inarmonica com morph, arpejador aleatorio com S&H, ruido num comb).
namespace dx7fm_app::factory {

inline void addOriginalPresets(std::vector<FactoryPreset>& out) {
    auto start = [&](const char* name, const char* category, const char* description) -> FactoryPreset& {
        FactoryPreset p;
        silenceAll(p.part.voice);
        p.part.name = name;
        p.family = "Originais";
        p.category = category;
        p.description = description;
        out.push_back(p);
        return out.back();
    };
    { // MPE: slide (CC74) abre o filtro, pressao por nota abre o modulador e o volume; sem MPE, aftertouch e mod wheel fazem o mesmo
        auto& p = start("Expressivo MPE", "Lead", "slide (CC74) abre o filtro, pressao abre o brilho; sem MPE, aftertouch e mod wheel fazem o mesmo");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.mod[0][1] = 1.0f;
        auto& c = v.op[0]; sawOp(c); c.outputLevel = 90; eg(c.ampEG, {80, 30, 30, 50}, {99, 92, 90, 0});
        c.fxType = FxType::LowPass; c.fxParam1 = 450.0f; c.fxParam2 = 2.5f; c.fxKeyTrack = 0.5f;
        eg(c.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
        auto& m = v.op[1]; m.outputLevel = 70; eg(m.ampEG, {80, 30, 30, 50}, {99, 92, 90, 0});
        // slide: corte x3 (duas rotas somam); pressao: corte x2, modulador x2 e +40% de volume
        v.modMatrix[0] = ModRoute{ModSource::Slide, ModDest::OperatorFx1, 0, 1.0f};
        v.modMatrix[1] = ModRoute{ModSource::Slide, ModDest::OperatorFx1, 0, 1.0f};
        v.modMatrix[2] = ModRoute{ModSource::PolyAT, ModDest::OperatorFx1, 0, 1.0f};
        v.modMatrix[3] = ModRoute{ModSource::PolyAT, ModDest::OperatorLevel, 1, 1.0f};
        v.modMatrix[4] = ModRoute{ModSource::PolyAT, ModDest::VoiceLevel, 0, 0.4f};
        v.modMatrix[5] = ModRoute{ModSource::ChannelAT, ModDest::OperatorFx1, 0, 1.0f};
        v.modMatrix[6] = ModRoute{ModSource::ChannelAT, ModDest::OperatorLevel, 1, 1.0f};
        v.modMatrix[7] = ModRoute{ModSource::ModWheel, ModDest::OperatorFx1, 0, 2.0f / 3.0f};
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 5.5f; v.lfo1.delaySec = 0.5f; v.lfo1.pitchDepth = 0.1f;
        c.lfoPitchModSens = 3;
        p.notes = {60}; p.modWheel = 0.5f; p.seconds = 2.5; p.holdSec = 2.0;
        v.volumeDb = -3.0f;
    }
    { // wobble: LFO sincronizado em 1/8 tercina no corte do filtro; mono
        auto& p = start("Baixo Wobble", "Baixo", "LFO sincronizado (1/8 tercina) varre o passa-baixa ressonante; mono");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[1] = 0.6f; v.matrix.mod[0][2] = 1.0f;
        auto& c = v.op[0]; sawOp(c); c.outputLevel = 92; eg(c.ampEG, {99, 30, 30, 70}, {99, 95, 95, 0});
        c.fxType = FxType::LowPass; c.fxParam1 = 700.0f; c.fxParam2 = 5.0f; c.fxKeyTrack = 0.3f;
        eg(c.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
        auto& s = v.op[1]; s.coarseRatio = 0.5f; s.outputLevel = 80; eg(s.ampEG, {99, 30, 30, 70}, {99, 95, 95, 0});
        auto& m = v.op[2]; m.outputLevel = 60; m.feedback = 0.3f; eg(m.ampEG, {99, 30, 30, 70}, {99, 95, 95, 0});
        v.lfo1.wave = LfoWave::Sine; v.lfo1.tempoSync = true; v.lfo1.division = 11; v.lfo1.keySync = true;
        v.modMatrix[0] = ModRoute{ModSource::LFO1, ModDest::OperatorFx1, 0, 0.9f};
        p.part.voiceMode = VoiceMode::Mono;
        p.notes = {33}; p.seconds = 3.0; p.holdSec = 2.5;
        v.volumeDb = -8.0f;
    }
    { // gate: LFO quadrado GLOBAL sincronizado em 1/16 no volume -- o acorde inteiro pulsa junto, na grade do compasso
        auto& p = start("Pad Gate Ritmico", "Movimento", "LFO quadrado global em 1/16 no volume: o acorde pulsa junto, na grade do compasso");
        auto& v = p.part.voice;
        const float det[3] = {-7.0f, 0.0f, 7.0f};
        for (int i = 0; i < 3; ++i) {
            v.matrix.outputMix[i] = 1.0f;
            auto& o = v.op[i]; sawOp(o); o.outputLevel = 84; o.detuneCents = det[i];
            eg(o.ampEG, {70, 30, 30, 45}, {99, 95, 95, 0});
            o.fxType = FxType::LowPass; o.fxParam1 = 2800.0f; o.fxParam2 = 1.0f; o.fxKeyTrack = 0.4f;
            eg(o.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
        }
        v.lfo2.wave = LfoWave::Square; v.lfo2.tempoSync = true; v.lfo2.division = 12; v.lfo2.global = true;
        v.modMatrix[0] = ModRoute{ModSource::LFO2, ModDest::VoiceLevel, 0, 0.95f};
        p.notes = {48, 55, 60, 63}; p.seconds = 4.0; p.holdSec = 3.5; p.bpm = 124.0f;
        v.volumeDb = -10.0f;
    }
    { // ring mod: portadora x um seno de razao 2.76 (fora da saida) = soma e diferenca, inarmonico como sino
        auto& p = start("Sinos em Anel", "Sinos", "ring mod (efeito do operador) com um seno de razao 2.76: soma e diferenca inarmonicas");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.6f; v.matrix.mod[2][3] = 1.0f;
        auto& c = v.op[0]; c.outputLevel = 96; eg(c.ampEG, {99, 22, 20, 40}, {99, 0, 0, 0}); c.velocitySens = 3;
        c.fxType = FxType::RingMod; c.ringModSource = 1;
        auto& r = v.op[1]; r.coarseRatio = 2.76f; r.outputLevel = 99; eg(r.ampEG, {99, 99, 99, 99}, {99, 99, 99, 0});
        auto& c2 = v.op[2]; c2.outputLevel = 84; eg(c2.ampEG, {99, 28, 20, 40}, {99, 0, 0, 0});
        auto& m2 = v.op[3]; m2.coarseRatio = 5.4f; m2.outputLevel = 66; eg(m2.ampEG, {99, 60, 40, 40}, {99, 0, 0, 0});
        p.notes = {72, 79}; p.seconds = 4.0; p.holdSec = 3.5;
        v.volumeDb = -5.0f;
    }
    { // frequency shifter: a mesma serra deslocada 1.3 Hz bate com a original -- phaser/chorus sem LFO
        auto& p = start("Pad Desvio de Frequencia", "Pad", "duas serras, uma deslocada 1.3 Hz por frequency shifter: batimento tipo phaser, sem LFO");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[1] = 1.0f;
        for (int i = 0; i < 2; ++i) {
            auto& o = v.op[i]; sawOp(o); o.outputLevel = 86; eg(o.ampEG, {45, 30, 30, 38}, {99, 95, 95, 0});
        }
        v.op[1].fxType = FxType::FreqShift; v.op[1].fxParam1 = 1.3f;
        auto& f = v.op[0]; f.fxType = FxType::LowPass; f.fxParam1 = 3000.0f; f.fxParam2 = 0.7f; f.fxKeyTrack = 0.5f;
        eg(f.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
        p.notes = {48, 55, 60, 64}; p.seconds = 5.0; p.holdSec = 4.0;
        v.volumeDb = -9.0f;
    }
    { // lo-fi: piano eletrico de dois pares passado num bitcrusher (8 bits, ~11 kHz)
        auto& p = start("Teclas Lo-Fi", "Teclas", "piano eletrico FM num bitcrusher (8 bits, ~11 kHz) nas portadoras");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.8f; v.matrix.mod[0][1] = 1.0f; v.matrix.mod[2][3] = 1.0f;
        for (int i : {0, 2}) {
            auto& c = v.op[i]; c.outputLevel = 88; c.detuneCents = i ? 4.0f : -2.0f; eg(c.ampEG, {99, 26, 18, 55}, {99, 80, 0, 0}); c.velocitySens = 4;
            c.fxType = FxType::Crush; c.fxParam1 = 11025.0f; c.fxParam2 = 0.5f;
        }
        auto& m1 = v.op[1]; m1.outputLevel = 74; eg(m1.ampEG, {99, 40, 26, 55}, {99, 60, 0, 0}); m1.velocitySens = 6;
        auto& m2 = v.op[3]; m2.coarseRatio = 14.0f; m2.outputLevel = 66; eg(m2.ampEG, {99, 65, 50, 70}, {99, 0, 0, 0}); m2.velocitySens = 7;
        p.notes = {48, 60, 64, 67};
        v.volumeDb = -4.0f;
    }
    { // laco na matriz: 0 e 1 se modulam mutuamente; a Macro 1 ("Caos") sobe o modulador e leva o laco ao caos
        auto& p = start("Feedback Cruzado", "Movimento", "operadores 1 e 2 se modulam em laco (a matriz permite); Macro 1 'Caos' leva do tom ao ruido");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.mod[0][1] = 1.0f; v.matrix.mod[1][0] = 0.6f;
        auto& c = v.op[0]; c.outputLevel = 90; eg(c.ampEG, {50, 30, 30, 40}, {99, 95, 95, 0});
        c.fxType = FxType::LowPass; c.fxParam1 = 4000.0f; c.fxParam2 = 0.8f; c.fxKeyTrack = 0.5f;
        eg(c.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
        auto& m = v.op[1]; m.coarseRatio = 1.5f; m.outputLevel = 70; eg(m.ampEG, {50, 30, 30, 40}, {99, 95, 95, 0});
        v.macroName[0] = "Caos";
        v.macro[0] = 0.3f;
        v.modMatrix[0] = ModRoute{ModSource::Macro1, ModDest::OperatorLevel, 1, 1.0f};
        v.modMatrix[1] = ModRoute{ModSource::Macro1, ModDest::OperatorFeedback, 0, 0.4f};
        p.notes = {40, 47}; p.seconds = 4.0; p.holdSec = 3.5;
        v.volumeDb = -4.0f;
    }
    { // kalimba: aditiva com os modos de uma lingueta (1, 5.9, 12.1) que morpham pra so a fundamental
        auto& p = start("Kalimba Aditiva", "Sinos", "aditiva com modos de lingueta (1, 5.9, 12.1): os agudos somem em ~0.3 s pelo morph, a fundamental fica");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f;
        auto& o = v.op[0]; o.outputLevel = 96; o.waveform = Waveform::Additive;
        o.additiveA = OperatorParams::PartialSet{};
        const float ratio[5] = {1.0f, 5.9f, 12.1f, 3.0f, 20.3f}, amp[5] = {1.0f, 0.55f, 0.3f, 0.12f, 0.12f};
        for (int k = 0; k < 5; ++k) { o.additiveA.ratio[k] = ratio[k]; o.additiveA.amp[k] = amp[k]; }
        o.additiveB = o.additiveA;
        for (int k = 1; k < 5; ++k) o.additiveB.amp[k] = 0.0f;
        o.morphDepth = 1.0f; eg(o.fxEG, {58, 99, 99, 99}, {99, 99, 99, 99});
        eg(o.ampEG, {99, 26, 22, 45}, {99, 0, 0, 0}); o.velocitySens = 3;
        p.notes = {67, 71, 74}; p.seconds = 3.0; p.holdSec = 2.5;
        v.volumeDb = -2.0f;
    }
    { // arpejador aleatorio + S&H sincronizado no filtro: cada passo com nota e brilho diferentes
        auto& p = start("Arpejo Aleatorio S&H", "Movimento", "arpejador aleatorio (2 oitavas) + LFO S&H sincronizado no filtro: cada passo com outro brilho");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.mod[0][1] = 1.0f;
        auto& c = v.op[0]; sawOp(c); c.outputLevel = 90; eg(c.ampEG, {99, 55, 40, 60}, {99, 0, 0, 0});
        c.fxType = FxType::LowPass; c.fxParam1 = 1800.0f; c.fxParam2 = 4.0f; c.fxKeyTrack = 0.5f;
        eg(c.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
        auto& m = v.op[1]; m.coarseRatio = 2.0f; m.outputLevel = 60; eg(m.ampEG, {99, 60, 40, 60}, {99, 0, 0, 0});
        v.lfo3.wave = LfoWave::SampleHold; v.lfo3.tempoSync = true; v.lfo3.division = 12; v.lfo3.global = true;
        v.modMatrix[0] = ModRoute{ModSource::LFO3, ModDest::OperatorFx1, 0, 0.7f};
        v.arp.on = true; v.arp.mode = ArpMode::Random; v.arp.octaves = 2; v.arp.division = 12; v.arp.gate = 0.6f;
        p.notes = {57, 60, 64, 67}; p.seconds = 4.0; p.holdSec = 3.6;
        v.volumeDb = -3.0f;
    }
    { // ruido num comb afinado na nota (key track 100%): sopro num tubo ressonante
        auto& p = start("Sopro no Tubo", "Sopros", "ruido passando num comb filter afinado na nota (key track 100%): ar num tubo ressonante");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[1] = 0.25f;
        auto& o = v.op[0]; o.waveform = Waveform::Noise; o.outputLevel = 84; eg(o.ampEG, {55, 30, 30, 50}, {99, 92, 90, 0});
        o.fxType = FxType::Comb; o.fxParam1 = 261.63f; o.fxParam2 = 0.93f; o.fxKeyTrack = 1.0f;
        auto& t = v.op[1]; t.outputLevel = 70; eg(t.ampEG, {50, 30, 30, 50}, {99, 92, 90, 0}); // um fio de tom puro por baixo
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 0.3f; v.lfo1.global = true;
        v.modMatrix[0] = ModRoute{ModSource::LFO1, ModDest::OperatorFx2, 0, 0.04f}; // o "fole" respira
        p.notes = {60}; p.seconds = 3.0; p.holdSec = 2.5;
        v.volumeDb = -2.0f;
    }
}

} // namespace dx7fm_app::factory

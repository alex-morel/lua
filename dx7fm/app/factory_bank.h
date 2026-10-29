#pragma once
#include "factory_preset.h"
#include "factory_dx.h"
#include "factory_sy.h"
#include "factory_originals.h"

// [ADICIONADO em 2026-10-28] Banco de fabrica: os patches de demonstracao,
// montados em codigo (sem arquivos). Usado pelo app (banco "Fabrica" no
// navegador de patches), por tools/presets/gen_presets (grava .json + previas
// .wav), tools/presets/patch_tests (testes de comportamento) e tools/golden.
// Mudar um patch daqui muda o golden dos cenarios de fabrica.
// Familias: os 12 de demonstracao (aqui), Estilo DX7 (factory_dx.h),
// Estilo SY77 (factory_sy.h) e Originais (factory_originals.h).
namespace dx7fm_app::factory {
using namespace dx7fm;

inline std::vector<FactoryPreset> factoryBank() {
    std::vector<FactoryPreset> out;

    { // 1. Piano eletrico classico: dois pares portadora/modulador; o "tine" e um modulador de razao 14 que some rapido
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Piano Eletrico FM"; p.category = "Teclas"; p.description = "dois pares portadora/modulador + tine de razao 14 que some rapido";
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.8f;
        v.matrix.mod[0][1] = 1.0f; v.matrix.mod[2][3] = 1.0f;
        auto& c1 = v.op[0]; c1.outputLevel = 85; eg(c1.ampEG, {99, 28, 18, 55}, {99, 84, 0, 0}); c1.velocitySens = 5;
        auto& m1 = v.op[1]; m1.outputLevel = 80; eg(m1.ampEG, {99, 38, 26, 55}, {99, 66, 0, 0}); m1.velocitySens = 6;
        m1.levelScaling.breakpoint = 60; m1.levelScaling.rightDepth = -5;
        auto& c2 = v.op[2]; c2.outputLevel = 79; c2.detuneCents = 4.0f; eg(c2.ampEG, {99, 32, 20, 55}, {99, 78, 0, 0}); c2.velocitySens = 5;
        auto& m2 = v.op[3]; m2.coarseRatio = 14.0f; m2.outputLevel = 74; eg(m2.ampEG, {99, 62, 50, 70}, {99, 0, 0, 0}); m2.velocitySens = 7;
        p.notes = {48, 60, 64, 67};
        out.push_back(p);
    }
    { // 2. Baixo: feedback num modulador (serra), passa-baixa ressonante com envelope e key track, mono com glide
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Baixo Filtrado"; p.category = "Baixo"; p.description = "mono com glide legato, passa-baixa por operador que fecha no ataque, sub-oitava";
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.6f; v.matrix.mod[0][1] = 1.0f;
        auto& c = v.op[0]; c.outputLevel = 90; eg(c.ampEG, {99, 45, 35, 70}, {99, 88, 80, 0});
        c.fxType = FxType::LowPass; c.fxParam1 = 2600.0f; c.fxParam2 = 3.5f; c.fxKeyTrack = 0.5f;
        eg(c.fxEG, {99, 50, 40, 60}, {99, 12, 8, 0});
        auto& m = v.op[1]; m.outputLevel = 84; m.feedback = 0.55f; eg(m.ampEG, {99, 40, 30, 60}, {99, 80, 70, 0});
        auto& sub = v.op[2]; sub.coarseRatio = 0.5f; sub.outputLevel = 82; eg(sub.ampEG, {99, 40, 30, 70}, {99, 90, 85, 0});
        p.part.voiceMode = VoiceMode::Mono; p.part.glideEnabled = true; p.part.glideTimeSec = 0.07f; p.part.glideLegatoOnly = true;
        p.notes = {36};
        out.push_back(p);
    }
    { // 3. Sino aditivo: modos de barra livre (inarmonicos) que fazem morph pra um espectro mais escuro com o FX EG
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Sino de Vidro"; p.category = "Sinos"; p.description = "aditiva com modos de barra livre; morph pros modos graves com o FX EG";
        v.matrix.outputMix[0] = 1.0f;
        auto& o = v.op[0]; o.outputLevel = 97; o.waveform = Waveform::Additive;
        dx7fm_app::presets::bell(o.additiveA);
        o.additiveB = o.additiveA;
        for (int k = 3; k < kNumPartials; ++k) o.additiveB.amp[k] = 0.0f; // B: so os 3 modos graves
        o.morphDepth = 1.0f; eg(o.fxEG, {32, 99, 99, 99}, {99, 99, 99, 99});
        eg(o.ampEG, {99, 24, 13, 30}, {99, 86, 0, 0}); o.velocitySens = 2;
        p.notes = {72}; p.seconds = 4.0; p.holdSec = 3.5;
        out.push_back(p);
    }
    { // 4. Pad: duas wavetables (serra -> pulso, morph espectral) movidas por um LFO global lento; unison
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Pad Wavetable"; p.category = "Pad"; p.description = "2 wavetables serra/seno com morph espectral num LFO global sincronizado; unison"; v.volumeDb = -5.0f; // o acorde com unison soma alto
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[1] = 1.0f;
        auto saw = wavegen::share(wavegen::fromShape(Waveform::Saw)), pulse = wavegen::share(wavegen::fromShape(Waveform::Sine));
        for (int i = 0; i < 2; ++i) {
            auto& o = v.op[i]; o.outputLevel = 88; o.waveform = Waveform::Wavetable;
            o.wavetable.waveA = saw; o.wavetable.waveB = pulse; o.wavetable.position = 0.5f; o.wavetable.spectralMorph = true;
            o.detuneCents = i ? 7.0f : -7.0f;
            eg(o.ampEG, {30, 22, 20, 28}, {99, 88, 88, 0});
            o.fxType = FxType::LowPass; o.fxParam1 = 3500.0f; o.fxParam2 = 0.8f; o.fxKeyTrack = 0.6f;
            eg(o.fxEG, {35, 99, 99, 30}, {99, 99, 99, 60});
        }
        v.lfo3.global = true; v.lfo3.tempoSync = true; v.lfo3.division = 1; v.lfo3.wave = LfoWave::Sine; // 2/1
        v.modMatrix[0] = ModRoute{ModSource::LFO3, ModDest::OperatorMorph, 0, 0.5f};
        v.modMatrix[1] = ModRoute{ModSource::LFO3, ModDest::OperatorMorph, 1, 0.5f};
        p.part.voiceMode = VoiceMode::Unison; p.part.unisonCount = 2; p.part.unisonDetuneCents = 9.0f; p.part.unisonSpread = 0.7f;
        p.notes = {48, 55, 60, 64}; p.seconds = 5.0; p.holdSec = 4.0;
        out.push_back(p);
    }
    { // 5. Arpejo pluck: comb filter afinado na nota (key track 100%) + arpejador sobe/desce 2 oitavas com swing
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Arpejo Pluck Comb"; p.category = "Movimento"; p.description = "comb filter afinado na nota + arpejador sobe/desce 2 oitavas com swing";
        v.matrix.outputMix[0] = 1.0f; v.matrix.mod[0][1] = 1.0f;
        auto& c = v.op[0]; c.outputLevel = 76; eg(c.ampEG, {99, 55, 40, 65}, {99, 0, 0, 0});
        c.fxType = FxType::Comb; c.fxParam1 = 261.63f; c.fxParam2 = 0.75f; c.fxKeyTrack = 1.0f;
        auto& m = v.op[1]; m.coarseRatio = 2.0f; m.outputLevel = 78; eg(m.ampEG, {99, 70, 50, 70}, {99, 0, 0, 0}); m.velocitySens = 5;
        v.arp.on = true; v.arp.mode = ArpMode::UpDown; v.arp.octaves = 2; v.arp.division = 12; v.arp.gate = 0.45f; v.arp.swing = 0.15f;
        p.notes = {57, 60, 64}; p.seconds = 4.0; p.holdSec = 3.6;
        out.push_back(p);
    }
    { // 6. Morph de algoritmos: 4 pares -> 2 cadeias de 4 -> 8 em serie, varrido por um envelope em loop no tempo
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Algoritmos em Movimento"; p.category = "Movimento"; p.description = "morph entre 3 algoritmos varrido por envelope em loop no tempo"; v.volumeDb = -3.1f; // o slot com 4 portadoras soma alto (antes part.level 0.7, que nao vai junto ao carregar)
        applyAlgorithmToMatrix(v.algo(0), 15);
        applyAlgorithmToMatrix(v.algo(1), 5);
        applyAlgorithmToMatrix(v.algo(2), 1);
        v.numAlgos = 3;
        const float ratios[kNumOps] = {1, 2, 1, 3, 1, 0.5f, 2, 1};
        for (int i = 0; i < kNumOps; ++i) {
            auto& o = v.op[i]; o.coarseRatio = ratios[i]; o.outputLevel = i == 0 ? 75 : 84;
            eg(o.ampEG, {80, 40, 40, 50}, {99, 90, 88, 0});
        }
        auto& e = v.modEnv[0]; eg(e, {50, 50, 99, 5}, {99, 0, 0, 0}); e.loop = true; e.tempoSync = true; e.division = 3; // 1/2
        v.modMatrix[0] = ModRoute{ModSource::Env1, ModDest::AlgoMorph, 0, 1.0f};
        p.notes = {45, 57}; p.seconds = 4.5; p.holdSec = 4.0;
        out.push_back(p);
    }
    { // 7. Lead com wavefolder: o drive abre no ataque (FX EG), vibrato atrasado, mod wheel dobra mais
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Lead Dobrado"; p.category = "Lead"; p.description = "wavefolder que abre no ataque, vibrato com atraso, mod wheel dobra mais";
        v.matrix.outputMix[0] = 1.0f; v.matrix.mod[0][1] = 1.0f;
        auto& c = v.op[0]; c.outputLevel = 90; eg(c.ampEG, {90, 40, 35, 60}, {99, 92, 90, 0}); c.lfoPitchModSens = 5;
        c.fxType = FxType::Fold; c.fxParam1 = 4.0f; c.fxParam2 = 0.15f; eg(c.fxEG, {99, 38, 40, 50}, {99, 12, 8, 0});
        auto& m = v.op[1]; m.outputLevel = 66; eg(m.ampEG, {99, 50, 40, 60}, {99, 80, 75, 0}); m.lfoPitchModSens = 5;
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 5.5f; v.lfo1.delaySec = 0.4f; v.lfo1.pitchDepth = 0.35f;
        v.modMatrix[0] = ModRoute{ModSource::ModWheel, ModDest::OperatorFx1, 0, 0.8f};
        p.part.voiceMode = VoiceMode::Mono; p.part.glideEnabled = true; p.part.glideTimeSec = 0.09f; p.part.glideLegatoOnly = true;
        p.notes = {64}; p.modWheel = 0.4f;
        out.push_back(p);
    }
    { // 8. Metais: modulador com ataque lento (o brilho cresce), velocity abre o modulador, pitch EG sobe um pouco no ataque
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Metais FM"; p.category = "Metais"; p.description = "moduladores com ataque lento, velocity abre o brilho, pitch EG no ataque";
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.8f; v.matrix.mod[0][1] = 1.0f; v.matrix.mod[2][3] = 1.0f;
        for (int i : {0, 2}) { auto& c = v.op[i]; c.outputLevel = 82; eg(c.ampEG, {62, 45, 40, 58}, {99, 92, 90, 0}); c.pegDepth = 1.0f; }
        v.op[2].detuneCents = -5.0f;
        for (int i : {1, 3}) { auto& m = v.op[i]; m.outputLevel = 93; m.feedback = i == 1 ? 0.15f : 0.0f;
                               eg(m.ampEG, {52, 40, 35, 55}, {99, 86, 80, 0}); m.pegDepth = 1.0f; m.velocitySens = 3; }
        v.peg.rate[0] = 70; v.peg.level[3] = 47; v.peg.level[0] = 50; v.peg.level[1] = 50; v.peg.level[2] = 50; // sobe ~1/4 de tom no ataque
        v.modMatrix[0] = ModRoute{ModSource::Velocity, ModDest::OperatorLevel, 1, 1.0f};
        v.modMatrix[1] = ModRoute{ModSource::Velocity, ModDest::OperatorLevel, 3, 1.0f};
        p.notes = {55, 60, 64};
        out.push_back(p);
    }
    { // 9. Coro de vogais: aditiva "a" -> "o"; a Macro 1 ("Vogal") faz o morph e um LFO lento balanca
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Coro de Vogais"; p.category = "Pad"; p.description = "aditiva de formantes 'a' -> 'o' na Macro 1 (Vogal)";
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[1] = 1.0f;
        for (int i = 0; i < 2; ++i) {
            auto& o = v.op[i]; o.outputLevel = 90; o.waveform = Waveform::Additive; o.detuneCents = i ? 6.0f : -6.0f;
            dx7fm_app::presets::vowel(o.additiveA);
            for (int k = 0; k < kNumPartials; ++k) { // "o": formantes ~450 e ~800 Hz numa voz em ~150 Hz
                float h = static_cast<float>(k + 1);
                auto g = [](float x, float c, float w) { return std::exp(-0.5f * (x - c) * (x - c) / (w * w)); };
                float a = 0.4f * g(h, 1, 1.2f) + g(h, 3, 1.0f) + 0.6f * g(h, 5.3f, 1.4f) + 0.15f * g(h, 16, 3);
                o.additiveB.amp[k] = a < 0.01f ? 0.0f : std::min(a, 1.0f); o.additiveB.ratio[k] = h;
            }
            eg(o.ampEG, {40, 30, 30, 35}, {99, 90, 90, 0});
            o.lfoPitchModSens = 3;
        }
        v.macroName[0] = "Vogal";
        v.modMatrix[0] = ModRoute{ModSource::Macro1, ModDest::OperatorMorph, 0, 1.0f};
        v.modMatrix[1] = ModRoute{ModSource::Macro1, ModDest::OperatorMorph, 1, 1.0f};
        v.lfo2.wave = LfoWave::Sine; v.lfo2.rate = 0.25f;
        v.modMatrix[2] = ModRoute{ModSource::LFO2, ModDest::OperatorMorph, 0, 0.25f};
        v.modMatrix[3] = ModRoute{ModSource::LFO2, ModDest::OperatorMorph, 1, 0.25f};
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 5.0f; v.lfo1.pitchDepth = 0.15f; v.lfo1.delaySec = 0.6f;
        p.part.voiceMode = VoiceMode::Unison; p.part.unisonCount = 3; p.part.unisonDetuneCents = 12.0f; p.part.unisonSpread = 0.8f;
        p.notes = {50, 57, 62}; p.seconds = 4.5; p.holdSec = 4.0;
        out.push_back(p);
    }
    { // 10. Bleeps: S&H global em 1/16, quantizado em semitons (13 degraus = +-6), e um envelope em loop que recorta o som
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Bleeps Sample-Hold"; p.category = "Movimento"; p.description = "LFO sample & hold quantizado em semitons, sincronizado";
        v.matrix.outputMix[0] = 1.0f; v.matrix.mod[0][1] = 1.0f;
        auto& c = v.op[0]; c.outputLevel = 88; eg(c.ampEG, {99, 99, 99, 60}, {99, 99, 99, 0});
        auto& m = v.op[1]; m.coarseRatio = 3.0f; m.outputLevel = 64; eg(m.ampEG, {99, 99, 99, 60}, {99, 99, 99, 0});
        v.lfo2.wave = LfoWave::SampleHold; v.lfo2.global = true; v.lfo2.tempoSync = true; v.lfo2.division = 12; v.lfo2.steps = 13;
        v.modMatrix[0] = ModRoute{ModSource::LFO2, ModDest::VoicePitch, 0, 0.5f};
        auto& e = v.modEnv[0]; eg(e, {99, 60, 99, 99}, {0, 99, 99, 99}); e.loop = true; e.tempoSync = true; e.division = 12; // portao em 1/16
        v.modMatrix[1] = ModRoute{ModSource::Env1, ModDest::VoiceLevel, 0, -1.0f};
        v.modMatrix[2] = ModRoute{ModSource::LFO2, ModDest::VoicePan, 0, 0.5f};
        p.notes = {72}; p.seconds = 3.0; p.holdSec = 2.8;
        out.push_back(p);
    }
    { // 11. Orgao: drawbars na aditiva, tremolo/pan de um LFO global (cara de Leslie)
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Orgao Drawbar"; p.category = "Orgao"; p.description = "aditiva com drawbars + tremolo";
        v.matrix.outputMix[0] = 1.0f;
        auto& o = v.op[0]; o.outputLevel = 85; o.waveform = Waveform::Additive; dx7fm_app::presets::organ(o.additiveA);
        o.additiveB = o.additiveA; eg(o.ampEG, {99, 99, 99, 80}, {99, 99, 99, 0}); o.lfoAmpModSens = 2;
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 6.2f; v.lfo1.ampDepth = 0.3f; v.lfo1.global = true;
        v.modMatrix[0] = ModRoute{ModSource::LFO1, ModDest::VoicePan, 0, 0.35f};
        p.notes = {48, 55, 60, 64, 67};
        out.push_back(p);
    }
    { // 12. Tambor: modos da membrana na aditiva + queda de afinacao do pitch EG
        FactoryPreset p; auto& v = p.part.voice; silenceAll(v); p.part.name = "Tambor de Membrana"; p.category = "Percussao"; p.description = "modos da membrana circular na aditiva + queda de afinacao no pitch EG";
        v.matrix.outputMix[0] = 1.0f;
        auto& o = v.op[0]; o.outputLevel = 97; o.waveform = Waveform::Additive; dx7fm_app::presets::drum(o.additiveA);
        for (int k = 20; k < kNumPartials; ++k) o.additiveA.amp[k] = 0.0f;
        o.additiveB = o.additiveA; eg(o.ampEG, {99, 48, 40, 50}, {99, 0, 0, 0}); o.velocitySens = 2; o.pegDepth = 1.0f;
        v.peg.level[3] = 62; v.peg.rate[0] = 72; v.peg.level[0] = 50; v.peg.level[1] = 50; v.peg.level[2] = 50; v.peg.rangeOctaves = 2.0f;
        p.notes = {45}; p.seconds = 1.5; p.holdSec = 0.3;
        out.push_back(p);
    }
    addDxPresets(out);
    addSyPresets(out);
    addOriginalPresets(out);
    return out;
}

// Toca a previa do preset (notas, mod wheel, macros, BPM) em L/R.
inline void renderPreview(const FactoryPreset& p, const PartParams& part, std::vector<float>& L, std::vector<float>& R) {
    Synth synth;
    synth.init(48000.0, 64);
    synth.setTempoBpm(p.bpm);
    synth.part(0) = part;
    synth.recompile(0);
    for (int k = 0; k < VoiceParams::kNumMacros; ++k) synth.setMacro(0, k, part.voice.macro[k]);
    if (p.modWheel >= 0.0f) synth.modWheel(0, p.modWheel);
    for (int n : p.notes) synth.noteOn(0, n, 100);
    const int total = static_cast<int>(p.seconds * 48000), hold = static_cast<int>(p.holdSec * 48000);
    L.assign(static_cast<size_t>(total), 0.0f);
    R.assign(static_cast<size_t>(total), 0.0f);
    for (int i = 0; i < total; i += 256) {
        if (i <= hold && hold < i + 256) for (int n : p.notes) synth.noteOff(0, n);
        synth.renderBlock(&L[static_cast<size_t>(i)], &R[static_cast<size_t>(i)], std::min(256, total - i));
    }
}

} // namespace dx7fm_app::factory

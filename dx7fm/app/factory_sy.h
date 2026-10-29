#pragma once
#include "factory_preset.h"

// [ADICIONADO em 2026-10-29] Patches de fabrica no estilo do Yamaha SY77:
// programacoes proprias que usam o que o SY77 tinha alem do DX7 -- formas de
// onda por operador (AFM: meio seno, seno retificado...), filtros com
// envelope, camada de ataque + camada de sustain, pan em movimento --
// com os recursos nativos do dx7fm (8 operadores, wavetable de serra sem
// aliasing, filtro por operador, LFO global).
namespace dx7fm_app::factory {

// serra band-limited (wavetable com mipmaps): sem o aliasing da Saw crua nas agudas
inline std::shared_ptr<const std::vector<float>> sawTable() {
    static const auto w = wavegen::share(wavegen::fromShape(Waveform::Saw));
    return w;
}
inline void sawOp(OperatorParams& o) {
    o.waveform = Waveform::Wavetable;
    o.wavetable.waveA = sawTable();
    o.wavetable.waveB = sawTable();
    o.wavetable.position = 0.0f;
}

inline void addSyPresets(std::vector<FactoryPreset>& out) {
    auto start = [&](const char* name, const char* category, const char* description) -> FactoryPreset& {
        FactoryPreset p;
        silenceAll(p.part.voice);
        p.part.name = name;
        p.family = "Estilo SY77";
        p.category = category;
        p.description = description;
        out.push_back(p);
        return out.back();
    };
    { // camada de sino (0/1, some em ~1 s) sobre um pad de meio-seno filtrado que abre devagar (2..5)
        auto& p = start("SY Pad Cristal", "Pad", "ataque de sino FM sobre um pad de onda meio-seno (AFM) com filtro que abre devagar");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.7f; v.matrix.outputMix[3] = 0.7f;
        v.matrix.mod[0][1] = 1.0f; v.matrix.mod[2][4] = 1.0f; v.matrix.mod[3][5] = 1.0f;
        auto& b = v.op[0]; b.outputLevel = 99; eg(b.ampEG, {99, 40, 30, 40}, {99, 0, 0, 0}); b.velocitySens = 3;
        auto& bm = v.op[1]; bm.coarseRatio = 3.5f; bm.outputLevel = 84; eg(bm.ampEG, {99, 55, 35, 40}, {99, 0, 0, 0});
        for (int i : {2, 3}) {
            auto& c = v.op[i]; c.outputLevel = 88; c.waveform = Waveform::HalfSine; c.detuneCents = i == 2 ? -8.0f : 8.0f;
            eg(c.ampEG, {50, 30, 30, 32}, {99, 92, 92, 0});
            c.fxType = FxType::LowPass; c.fxParam1 = 5000.0f; c.fxParam2 = 1.2f; c.fxKeyTrack = 0.5f;
            eg(c.fxEG, {30, 99, 99, 35}, {99, 99, 99, 20});
            auto& m = v.op[i + 2]; m.outputLevel = 66; m.coarseRatio = i == 2 ? 1.0f : 2.0f; eg(m.ampEG, {50, 30, 30, 32}, {99, 90, 90, 0});
        }
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 4.8f; v.lfo1.delaySec = 0.8f; v.lfo1.pitchDepth = 0.08f;
        for (int i : {2, 3}) v.op[i].lfoPitchModSens = 3;
        p.notes = {48, 55, 60, 64}; p.seconds = 5.0; p.holdSec = 4.0;
        v.volumeDb = -6.0f;
    }
    { // 4 serras sem aliasing desafinadas, passa-baixa com envelope lento, vibrato com atraso
        auto& p = start("SY Cordas Analogicas", "Cordas", "4 serras (wavetable sem aliasing) desafinadas, filtro com envelope lento e vibrato atrasado");
        auto& v = p.part.voice;
        const float det[4] = {-9.0f, -3.0f, 3.0f, 9.0f};
        for (int i = 0; i < 4; ++i) {
            v.matrix.outputMix[i] = 1.0f;
            auto& o = v.op[i]; sawOp(o); o.outputLevel = 84; o.detuneCents = det[i];
            eg(o.ampEG, {48, 30, 30, 38}, {99, 94, 94, 0});
            o.fxType = FxType::LowPass; o.fxParam1 = 4200.0f; o.fxParam2 = 0.8f; o.fxKeyTrack = 0.5f;
            eg(o.fxEG, {40, 40, 99, 40}, {99, 80, 80, 30});
            o.lfoPitchModSens = 3;
        }
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 5.2f; v.lfo1.delaySec = 0.7f; v.lfo1.pitchDepth = 0.1f;
        p.notes = {48, 55, 60, 64}; p.seconds = 4.5; p.holdSec = 3.5;
        v.volumeDb = -9.0f;
    }
    { // piano hibrido: corpo FM com portadoras de onda AFM (seno retificado / meio seno) + martelo curto
        auto& p = start("SY Piano AFM", "Teclas", "portadoras com ondas AFM (seno retificado e meio seno) + martelo de razao 7; velocity abre o brilho");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.7f;
        v.matrix.mod[0][1] = 1.0f; v.matrix.mod[2][3] = 1.0f; v.matrix.mod[0][4] = 1.0f;
        auto& c1 = v.op[0]; c1.outputLevel = 92; c1.waveform = Waveform::AbsSine; eg(c1.ampEG, {99, 36, 18, 58}, {99, 78, 0, 0}); c1.velocitySens = 4;
        auto& m1 = v.op[1]; m1.outputLevel = 72; eg(m1.ampEG, {99, 45, 24, 58}, {99, 60, 0, 0}); m1.velocitySens = 6;
        auto& c2 = v.op[2]; c2.outputLevel = 86; c2.waveform = Waveform::HalfSine; c2.coarseRatio = 2.0f; c2.detuneCents = 3.0f;
        eg(c2.ampEG, {99, 40, 20, 58}, {99, 70, 0, 0}); c2.velocitySens = 4;
        auto& m2 = v.op[3]; m2.outputLevel = 60; m2.coarseRatio = 1.0f; eg(m2.ampEG, {99, 50, 30, 58}, {99, 50, 0, 0});
        auto& h = v.op[4]; h.outputLevel = 70; h.coarseRatio = 7.0f; eg(h.ampEG, {99, 80, 50, 60}, {99, 0, 0, 0}); h.velocitySens = 7;
        for (int i : {1, 3, 4}) { v.op[i].levelScaling.breakpoint = 60; v.op[i].levelScaling.rightDepth = -6; }
        p.notes = {48, 60, 64, 67}; p.seconds = 3.0; p.holdSec = 2.2;
        v.volumeDb = -7.0f;
    }
    { // baixo de filtro ressonante: serra + sub quadrado uma oitava abaixo, mono com glide
        auto& p = start("SY Baixo Ressonante", "Baixo", "serra com passa-baixa ressonante (Q 6) que fecha rapido + sub quadrado; mono com glide");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[1] = 0.5f;
        auto& c = v.op[0]; sawOp(c); c.outputLevel = 92; eg(c.ampEG, {99, 40, 30, 70}, {99, 90, 85, 0}); c.velocitySens = 2;
        c.fxType = FxType::LowPass; c.fxParam1 = 3200.0f; c.fxParam2 = 6.0f; c.fxKeyTrack = 0.6f;
        eg(c.fxEG, {99, 55, 40, 70}, {99, 15, 10, 0});
        auto& s = v.op[1]; s.waveform = Waveform::Square; s.coarseRatio = 0.5f; s.outputLevel = 74; eg(s.ampEG, {99, 40, 30, 70}, {99, 90, 85, 0});
        s.fxType = FxType::LowPass; s.fxParam1 = 600.0f; s.fxParam2 = 0.7f; s.fxKeyTrack = 1.0f;
        p.part.voiceMode = VoiceMode::Mono; p.part.glideEnabled = true; p.part.glideTimeSec = 0.05f; p.part.glideLegatoOnly = true;
        p.notes = {36}; p.seconds = 2.0; p.holdSec = 1.3;
        v.volumeDb = -4.0f;
    }
    { // coro: serras em dois filtros passa-banda fixos (formantes de "a"), vibrato lento, pan que gira (LFO global)
        auto& p = start("SY Coro Etereo", "Pad", "serras em dois passa-bandas fixos (formantes ~750 e ~1200 Hz), unison e pan girando num LFO global");
        auto& v = p.part.voice;
        const float freq[4] = {750.0f, 750.0f, 1200.0f, 1200.0f}, q[4] = {4.0f, 4.0f, 5.0f, 5.0f}, det[4] = {-6.0f, 6.0f, -4.0f, 4.0f};
        for (int i = 0; i < 4; ++i) {
            v.matrix.outputMix[i] = i < 2 ? 1.0f : 0.7f;
            auto& o = v.op[i]; sawOp(o); o.outputLevel = 92; o.detuneCents = det[i];
            eg(o.ampEG, {42, 30, 30, 34}, {99, 95, 95, 0});
            o.fxType = FxType::BandPass; o.fxParam1 = freq[i]; o.fxParam2 = q[i];
            eg(o.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
            o.lfoPitchModSens = 4;
        }
        v.lfo1.wave = LfoWave::Sine; v.lfo1.rate = 4.5f; v.lfo1.delaySec = 0.5f; v.lfo1.pitchDepth = 0.1f;
        v.lfo2.wave = LfoWave::Sine; v.lfo2.rate = 0.2f; v.lfo2.global = true;
        v.modMatrix[0] = ModRoute{ModSource::LFO2, ModDest::VoicePan, 0, 0.6f};
        p.part.voiceMode = VoiceMode::Unison; p.part.unisonCount = 2; p.part.unisonDetuneCents = 8.0f; p.part.unisonSpread = 0.6f;
        p.notes = {50, 57, 62}; p.seconds = 5.0; p.holdSec = 4.0;
        v.volumeDb = -10.0f;
    }
    { // varredura metalica: par inarmonico (2.01 / 7) + passa-baixa ressonante varrido por um LFO lento; pan em movimento
        auto& p = start("SY Varredura Metalica", "Movimento", "FM inarmonico (razoes 2.01 e 7) num passa-baixa ressonante varrido por LFO lento; pan em movimento");
        auto& v = p.part.voice;
        v.matrix.outputMix[0] = 1.0f; v.matrix.outputMix[2] = 0.8f;
        v.matrix.mod[0][1] = 1.0f; v.matrix.mod[2][3] = 1.0f;
        auto& c1 = v.op[0]; c1.outputLevel = 90; eg(c1.ampEG, {60, 30, 30, 40}, {99, 95, 95, 0});
        auto& m1 = v.op[1]; m1.outputLevel = 80; m1.coarseRatio = 2.01f; eg(m1.ampEG, {60, 30, 30, 40}, {99, 95, 95, 0});
        auto& c2 = v.op[2]; c2.outputLevel = 86; c2.coarseRatio = 1.0f; c2.detuneCents = 5.0f; eg(c2.ampEG, {60, 30, 30, 40}, {99, 95, 95, 0});
        auto& m2 = v.op[3]; m2.outputLevel = 76; m2.coarseRatio = 7.0f; eg(m2.ampEG, {60, 30, 30, 40}, {99, 90, 90, 0});
        for (int i : {0, 2}) {
            auto& c = v.op[i]; c.fxType = FxType::LowPass; c.fxParam1 = 2500.0f; c.fxParam2 = 5.0f; c.fxKeyTrack = 0.5f;
            eg(c.fxEG, {99, 99, 99, 99}, {99, 99, 99, 99});
        }
        v.lfo3.wave = LfoWave::Triangle; v.lfo3.rate = 0.25f; v.lfo3.global = true;
        v.modMatrix[0] = ModRoute{ModSource::LFO3, ModDest::OperatorFx1, 0, 0.8f};
        v.modMatrix[1] = ModRoute{ModSource::LFO3, ModDest::OperatorFx1, 2, 0.8f};
        v.lfo2.wave = LfoWave::Sine; v.lfo2.rate = 0.13f; v.lfo2.global = true;
        v.modMatrix[2] = ModRoute{ModSource::LFO2, ModDest::VoicePan, 0, 0.7f};
        p.notes = {45, 52, 57}; p.seconds = 6.0; p.holdSec = 5.0;
        v.volumeDb = -9.0f;
    }
}

} // namespace dx7fm_app::factory

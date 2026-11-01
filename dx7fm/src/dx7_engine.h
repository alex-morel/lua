#pragma once
#include "dx7_params.h"
#include "dx7_dsp.h"
#include "dx7_filters.h"
#include "dx7_wavetable.h"
#include "dx7_tuning.h"
#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <random>

namespace dx7fm {

struct CompiledAlgorithm {
    int order[kNumOps] = {0, 1, 2, 3, 4, 5, 6, 7};
    // feedbackEdge[i][j] = true se o operador i precisa usar o sinal do
    // operador j da AMOSTRA ANTERIOR (prevOut) em vez do valor fresco desta
    // amostra (signal), porque j e avaliado depois (ou e) i na ordem escolhida.
    bool feedbackEdge[kNumOps][kNumOps] = {};
};

// Busca por forca bruta (8! = 40320 permutacoes) a ordem de avaliacao dos
// operadores que MINIMIZA o numero de conexoes que precisam de atraso de 1
// amostra. So deve ser chamada ao trocar de patch/algoritmo (Synth::recompile),
// nunca no caminho quente de audio.
inline CompiledAlgorithm compileAlgorithm(const AlgorithmMatrix& m) {
    int perm[kNumOps];
    for (int i = 0; i < kNumOps; ++i) perm[i] = i;

    int bestCount = kNumOps * kNumOps + 1;
    int bestPerm[kNumOps];
    for (int i = 0; i < kNumOps; ++i) bestPerm[i] = i;

    do {
        int pos[kNumOps];
        for (int p = 0; p < kNumOps; ++p) pos[perm[p]] = p;

        int count = 0;
        for (int i = 0; i < kNumOps; ++i) {
            for (int j = 0; j < kNumOps; ++j) {
                if (i == j) continue; // auto-loop precisa de atraso em QUALQUER ordem: nao conta
                bool hasEdge = (m.mod[i][j] != 0.0f) || m.ring[i][j];
                if (hasEdge && pos[j] >= pos[i]) ++count;
            }
        }
        if (count < bestCount) {
            bestCount = count;
            for (int i = 0; i < kNumOps; ++i) bestPerm[i] = perm[i];
            if (bestCount == 0) break;
        }
    } while (std::next_permutation(perm, perm + kNumOps));

    CompiledAlgorithm result;
    for (int i = 0; i < kNumOps; ++i) result.order[i] = bestPerm[i];

    int pos[kNumOps];
    for (int p = 0; p < kNumOps; ++p) pos[bestPerm[p]] = p;
    for (int i = 0; i < kNumOps; ++i) {
        for (int j = 0; j < kNumOps; ++j) {
            bool hasEdge = (m.mod[i][j] != 0.0f) || m.ring[i][j];
            result.feedbackEdge[i][j] = hasEdge && (pos[j] >= pos[i]);
        }
    }
    return result;
}

// [ADICIONADO em 2026-09-21] Par casado (VoiceParams, CompiledAlgorithm):
// o compilado SEMPRE deve corresponder a matrix do voice que o acompanha
// (compileAlgorithm deriva um do outro). Publicar os dois juntos, de uma
// vez, e a base do mecanismo de troca atomica de patch em dx7_alloc.h
// (thread safety) -- ver PartRuntime::live()/recompile().
using WavetableBankPtr = std::shared_ptr<const WavetableBank>;
using WavetableBanks = std::array<WavetableBankPtr, kNumOps>;

// Tabelas da Waveform::Wavetable de um patch (nullptr nos operadores que nao
// sao Wavetable). Custa ms por operador: o Synth guarda cache (recompile).
inline WavetableBankPtr makeWavetableBank(const OperatorParams& o) {
    if (o.waveform != Waveform::Wavetable) return nullptr;
    return std::make_shared<const WavetableBank>(o.wavetable.waveA.get(), o.wavetable.waveB.get(), o.wavetable.spectralMorph);
}

struct PatchSnapshot {
    VoiceParams voice;
    CompiledAlgorithm compiled;
    // [2026-10-18, otimizacao] incrementado a cada Synth::recompile(): a Voice
    // guarda dados derivados do patch em cache e usa (ponteiro, geracao) pra
    // saber quando refaze-los -- so o ponteiro nao basta, porque o triple
    // buffer reusa os mesmos 3 slots de memoria.
    uint64_t generation = 0;
    WavetableBanks wavetables{}; // [2026-10-22] montadas a partir de voice (ver makeWavetableBank)
    TuningTable tuning = equalTemperament(); // [2026-10-24] afinacao da parte (Synth::recompile)
};
inline PatchSnapshot makeSnapshot(const VoiceParams& v) {
    PatchSnapshot s{v, compileAlgorithm(algoUnion(v)), 0, {}, equalTemperament()};
    for (int i = 0; i < kNumOps; ++i) s.wavetables[static_cast<size_t>(i)] = makeWavetableBank(v.op[i]);
    return s;
}

struct OperatorState {
    double phase = 0.0;
    SegmentEG ampEG;
    SegmentEG fxEG;
    TPTStateVariableFilter filter;
    ModelFilter modelFilter; // [2026-10-30] OperatorParams::fxModel != Svf
    Crusher crusher;
    CombFilter comb;
    FreqShifter shifter;
    DcBlocker dcBlock; // so usado pelo FxType::Fold com simetria != 0
    AdaaState adaa;    // antialiasing de Fold/Saturate (dx7_dsp.h)
    PartialOscillators partials; // so usado pela Waveform::Additive
    float signal = 0.0f;
    float prevOut = 0.0f;
    float prevPrevOut = 0.0f; // auto-feedback usa a media das 2 ultimas amostras (estilo DX7)
    float lastAmp = 0.0f;     // amplitude do ampEG na ultima amostra (0..1), ver Voice::loudness()
    float lastAmpRaw = -1.0f; // nivel do EG (0..99) que gerou lastAmp (cache da exp2)
    // cache do key tracking do efeito (OperatorParams::fxKeyTrack)
    float ktLog2 = -1.0f, ktAmount = -1.0f, ktFactor = 1.0f;
};

// log2 da frequencia do C4 (nota 60, A4 = 440 Hz): centro do key tracking
constexpr float kLog2C4 = 8.7813609f - 0.75f; // log2(440) - 9/12

struct VoiceControllers {
    float pitchBendSemis = 0.0f;
    float modWheel01 = 0.0f;
    float channelAT01 = 0.0f;
    float polyAT01 = 0.0f;
    float macro01[4] = {}; // [2026-10-24] macros da parte (ModSource::Macro1..4)
    // [2026-10-28] expressao POR NOTA (MPE): bend da nota (somado ao bend do
    // canal) e slide/CC74 (ModSource::Slide); a pressao por nota e polyAT01
    float noteBendSemis = 0.0f;
    float slide01 = 0.0f;
    // [2026-10-30] CC2, CC4, CC11, pedal de sustain; os dois ultimos fixos por nota (Synth)
    float cc01[4] = {};
    float randomNote = 0.0f;
    float alternate = 1.0f;
};

// A voz fisica completa: 8 operadores + PEG + 2 LFOs. NAO possui o patch,
// so aponta pra ele -- permite edicao "ao vivo" do timbre (ver ARQUITETURA.txt
// secao 10), ao custo de nao ter thread-safety.
class Voice {
public:
    // factor 1 = sem oversampling (default); 2 ou 4 = processa internamente
    // a essa taxa e decima com HalfbandDecimator2x (cascateado 2x pra 4x)
    // antes de devolver a amostra na taxa base. Ver ARQUITETURA.txt secao 11.
    void setOversample(int factor) {
        oversampleFactor_ = (factor >= 4) ? 4 : (factor >= 2) ? 2 : 1;
        if (baseSampleRate_ > 0.0) setSampleRate(baseSampleRate_);
    }

    void setSampleRate(double sr) {
        baseSampleRate_ = sr;
        sampleRate_ = sr * oversampleFactor_;
        for (auto& l : lfos_) l.setSampleRate(sampleRate_);
        for (auto& op : ops_) {
            op.filter.setSampleRate(sampleRate_);
            op.modelFilter.setSampleRate(sampleRate_);
            op.crusher.setSampleRate(sampleRate_);
            op.comb.setSampleRate(sampleRate_);
            op.shifter.setSampleRate(sampleRate_);
            op.dcBlock.setSampleRate(sampleRate_);
        }
    }

    // glideFromHz > 0: com glide ligado e fora de legato, a nota desliza a
    // partir dessa frequencia (a da nota anterior da parte, ver
    // Synth::triggerSlot) em vez de comecar ja no alvo.
    void noteOn(const VoiceParams* patch, const CompiledAlgorithm* compiled,
                int note, int velocity, float extraDetuneCents = 0.0f, bool legato = false,
                float glideFromHz = 0.0f, uint64_t generation = 0) {
        patch_ = patch;
        compiled_ = compiled;
        generation_ = generation;
        cacheValid_ = false; // nota/velocity/detune mudaram: refaz o cache derivado do patch
        note_ = note;
        velocity_ = velocity;
        extraDetuneCents_ = extraDetuneCents;
        active_ = true;
        releasing_ = false;

        targetHz_ = tuning_ ? tuning_->hz[std::clamp(note, 0, 127)] : noteToHz(note);
        targetLog2_ = std::log2(targetHz_);
        if (legato && glideEnabled_) {
            // mantem currentHz_: desliza de onde a voz estiver agora
        } else if (glideEnabled_ && glideFromHz > 0.0f) {
            currentHz_ = glideFromHz;
        } else {
            currentHz_ = targetHz_;
        }
        currentLog2_ = std::log2(currentHz_);

        bool reuse = legato && reuseEnvOnLegato_;
        if (!reuse) {
            for (int i = 0; i < kNumOps; ++i) {
                auto& op = ops_[i];
                const auto& opParams = patch_->op[i];
                if (opParams.waveform != Waveform::Noise) op.phase = 0.0;
                float rateScaleAdd = opParams.rateScaling * std::max(0, note - 60) / 12.0f * 2.0f;
                op.ampEG.configure(opParams.ampEG.rate, opParams.ampEG.level);
                op.ampEG.noteOn(rateScaleAdd);
                op.fxEG.configure(opParams.fxEG.rate, opParams.fxEG.level);
                op.fxEG.noteOn(rateScaleAdd);
                op.prevOut = 0.0f;
                op.prevPrevOut = 0.0f;
                op.signal = 0.0f;
                // estado dos FX do slot NAO pode vazar da nota anterior (voz roubada)
                op.filter.reset();
                op.modelFilter.reset();
                op.crusher.reset();
                op.comb.reset();
                op.shifter.reset();
                op.dcBlock.reset();
                op.adaa.reset();
                op.partials.reset();
            }
            peg_.configure(patch_->peg.rate, patch_->peg.level);
            peg_.noteOn(0.0f, static_cast<float>(patch_->peg.level[3]));
            noteAge_ = 0.0f;
            for (float& m : modEnvRateMul_) m = 1.0f;
            for (int k = 0; k < kNumLfos; ++k) {
                lfos_[k].configure(patch_->lfo(k));
                lfos_[k].noteOn();
                lfoRateMul_[k] = lfoDepthMul_[k] = 1.0f;
            }
            for (int k = 0; k < kNumModEnvs; ++k) { // comecam no level[3], como o PEG
                modEnv_[k].configure(patch_->modEnv[k].rate, patch_->modEnv[k].level);
                modEnv_[k].setLoop(patch_->modEnv[k].loop);
                modEnv_[k].noteOn(0.0f, static_cast<float>(patch_->modEnv[k].level[3]));
            }
        }
    }

    void noteOff() {
        if (!patch_) return;
        releasing_ = true;
        for (int i = 0; i < kNumOps; ++i) {
            ops_[i].ampEG.noteOff();
            if (patch_->op[i].fxEGReleaseWithAmp) ops_[i].fxEG.noteOff();
        }
        peg_.noteOff();
        for (auto& e : modEnv_) e.noteOff();
    }

    void setGlide(bool enabled, float timeSec) { glideEnabled_ = enabled; glideTimeSec_ = timeSec; }
    void setLegatoEnvReuse(bool reuse) { reuseEnvOnLegato_ = reuse; }

    // Ativa enquanto alguma PORTADORA (outputMix != 0) ainda soa. Moduladores
    // nao contam: sem portadora audivel a voz e silencio, mesmo que um
    // modulador ainda esteja no release.
    bool isActive() const { return active_ && anyCarrierSounding(); }

    // [2026-10-22] pan vindo da matriz (ModDest::VoicePan) na ultima amostra;
    // o Synth soma no pan da parte quando o patch usa esse destino
    float panMod() const { return panMod_; }
    // BPM global pros LFOs sincronizados (o Synth atualiza a cada bloco)
    void setTempo(float bpm) {
        for (auto& l : lfos_) l.setTempo(bpm);
        tempoBpm_ = bpm;
    }

    // Estimativa de "quanto essa voz pesa" no mix (0..1): maior amplitude de
    // envelope entre as portadoras. Usada pela compensacao de polifonia, pra
    // vozes em release ou com sustain 0 nao abaixarem o volume das outras.
    float loudness() const {
        if (!active_ || !patch_) return 0.0f;
        float m = 0.0f;
        for (int i = 0; i < kNumOps; ++i)
            if (isCarrier(i) && patch_->op[i].enabled) m = std::max(m, ops_[i].lastAmp);
        return m;
    }
    bool isReleasing() const { return releasing_; }
    int note() const { return note_; }
    float debugCurrentHz() const { return currentHz_; }
    bool debugFxEGIdle(int opIndex) const { return ops_[opIndex].fxEG.isIdle(); }
    VoiceControllers& controllers() { return controllers_; }

    // [ADICIONADO em 2026-09-21] Atualiza os ponteiros de patch/compiled
    // SEM disparar noteOn() (nao reseta EGs/fase/LFOs). Chamado pelo Synth
    // a CADA amostra, pra vozes ja soando acompanharem "ao vivo" edicoes
    // de timbre publicadas via recompile() -- ver PartRuntime::live() em
    // dx7_alloc.h. Custo desprezivel (2 escritas de ponteiro).
    void rebind(const VoiceParams* patch, const CompiledAlgorithm* compiled, uint64_t generation = 0) {
        patch_ = patch;
        compiled_ = compiled;
        generation_ = generation;
    }

    // [2026-10-31] solo (so na edicao, nao vai no patch): sai so este operador,
    // com os moduladores dele; os outros nao sao calculados. -1 = desligado.
    void setSoloOperator(int op) {
        if (op == soloOp_) return;
        soloOp_ = op;
        cacheValid_ = false;
    }

    // [2026-10-22] tabelas da Waveform::Wavetable do MESMO snapshot de patch_
    // (o Synth chama junto com noteOn/rebind). nullptr = operadores Wavetable soam seno.
    void bindWavetables(const WavetableBanks* banks) { wavetables_ = banks; }
    // [2026-10-24] tabela de afinacao (nota -> Hz) usada no noteOn; nullptr = 12-TET
    void bindTuning(const TuningTable* t) { tuning_ = t; }

    // [2026-10-24] LFOs globais da parte: buffer do pedaco (uma amostra por
    // process()) pra cada LFO global, nullptr nos que sao da voz. O Synth chama
    // antes de cada pedaco; o indice volta a 0.
    void bindGlobalLfos(const float* const bufs[kNumLfos]) {
        for (int k = 0; k < kNumLfos; ++k) globalLfo_[k] = bufs[k];
        globalIdx_ = 0;
    }

    // Uma amostra na taxa BASE (nao superamostrada). Se oversampleFactor_>1,
    // roda processOneSample() internamente a taxa superamostrada e decima
    // com HalfbandDecimator2x (cascateado pra 4x) antes de devolver.
    float process() {
        if (oversampleFactor_ <= 1) {
            float out = processOneSample();
            ++globalIdx_;
            return out;
        }
        ++globalIdx_; // (as sub-amostras leem a mesma amostra do LFO global: idx - 1)

        float finalOut = 0.0f;
        for (int k = 0; k < oversampleFactor_; ++k) {
            float sub = processOneSample();
            float stage1Out;
            if (decim1_.process(sub, stage1Out)) {
                if (oversampleFactor_ == 2) {
                    finalOut = stage1Out;
                } else {
                    float stage2Out;
                    if (decim2_.process(stage1Out, stage2Out)) finalOut = stage2Out;
                }
            }
        }
        return finalOut;
    }

private:
    // [2026-10-18, otimizacao] Dados de cada operador que so mudam quando a
    // nota comeca ou o patch publicado muda -- antes eram recalculados a
    // cada amostra (pow de dB, pow de cents, varredura da matriz 8x8).
    struct OpCache {
        bool used = true;         // contribui pra saida (portadora ou modulador de alguem usado)
        int partialGroups = 0;    // Additive: grupos de 8 parciais ate o ultimo com amplitude (ver PartialOscillators)
        bool fixed = false;
        float baseHz = 0.0f;      // fixo: fixedHz; senao: multiplicador (ratio * 2^(cents/1200))
        bool pegApplies = false;
        float pegDepth = 0.0f;
        float pitchSens = 0.0f;   // lfoPitchModSens/7
        float ampSens = 0.0f;     // lfoAmpModSens/3
        float staticGain = 1.0f;  // outputLevel + key scaling + velocity (teto 0dB), linear
        float selfMod = 0.0f;     // diagonal da matriz
        int nMod = 0;
        uint8_t modSrc[kNumOps];
        float modAmt[kNumOps];
        bool modDelayed[kNumOps];
        int nRing = 0;
        uint8_t ringSrc[kNumOps];
        bool ringDelayed[kNumOps];
        bool ringOn[kNumOps];     // [2026-10-24] morph de algoritmos: ring do slot mais proximo
        // [2026-10-24] morph de algoritmos (numAlgos > 1): peso de cada aresta em
        // cada slot; modAmt/selfMod/ringOn sao refeitos quando a posicao muda
        float modAmtSlot[kNumOps][kMaxAlgoSlots];
        float selfModSlot[kMaxAlgoSlots];
        bool ringSlot[kNumOps][kMaxAlgoSlots];
    };

    void rebuildCache() {
        const VoiceParams& v = *patch_;
        // operadores usados: portadoras + tudo que alimenta (PM, ring, fxRingMod) algo usado
        // [2026-10-24] topologia = uniao dos slots do morph (com 1 algoritmo, a matriz)
        const int nAlgo = v.activeAlgos();
        const AlgorithmMatrix topo = algoUnion(v);
        bool used[kNumOps] = {};
        for (int i = 0; i < kNumOps; ++i) used[i] = soloOp_ >= 0 ? i == soloOp_ : (topo.outputMix[i] != 0.0f && v.op[i].enabled);
        for (bool grew = true; grew;) {
            grew = false;
            for (int i = 0; i < kNumOps; ++i) {
                if (!used[i]) continue;
                for (int j = 0; j < kNumOps; ++j) {
                    bool feeds = j != i && (topo.mod[i][j] != 0.0f || topo.ring[i][j]);
                    if (v.op[i].fxType == FxType::RingMod && v.op[i].ringModSource == j) feeds = true;
                    if (feeds && !used[j] && v.op[j].enabled) { used[j] = true; grew = true; }
                }
            }
        }
        for (int i = 0; i < kNumOps; ++i) {
            const OperatorParams& o = v.op[i];
            OpCache& c = cache_[i];
            c.used = used[i];
            c.partialGroups = o.waveform == Waveform::Additive ? PartialOscillators::groupsFor(o.additiveA, o.additiveB) : 0;
            c.fixed = o.fixedFreq;
            c.baseHz = o.fixedFreq ? o.fixedHz
                                   : o.coarseRatio * std::exp2((o.fineCents + o.detuneCents + extraDetuneCents_) / 1200.0f);
            c.pegApplies = !(o.fixedFreq && !o.pegAffectsFixed) && o.pegDepth != 0.0f;
            c.pegDepth = o.pegDepth;
            c.pitchSens = o.lfoPitchModSens / 7.0f;
            c.ampSens = o.lfoAmpModSens / 3.0f;
            float levelDb = (static_cast<float>(o.outputLevel) - 99.0f) * 0.75f;
            levelDb += keyLevelScaleDb(o.levelScaling, note_);
            levelDb += (static_cast<float>(velocity_) - 127.0f) / 127.0f * (o.velocitySens / 7.0f) * 12.0f;
            c.staticGain = dbToLinear(std::min(levelDb, 0.0f)); // teto de 0dB (= outputLevel 99), como no DX7
            c.selfMod = v.matrix.mod[i][i];
            for (int s = 0; s < nAlgo; ++s) c.selfModSlot[s] = v.algo(s).mod[i][i];
            c.nMod = c.nRing = 0;
            for (int j = 0; j < kNumOps; ++j) {
                if (j == i) continue;
                if (!v.op[j].enabled) continue; // desligado: como se a conexao nao existisse
                bool delayed = compiled_->feedbackEdge[i][j];
                if (topo.mod[i][j] != 0.0f) {
                    c.modSrc[c.nMod] = static_cast<uint8_t>(j);
                    c.modAmt[c.nMod] = v.matrix.mod[i][j];
                    c.modDelayed[c.nMod] = delayed;
                    for (int s = 0; s < nAlgo; ++s) c.modAmtSlot[c.nMod][s] = v.algo(s).mod[i][j];
                    ++c.nMod;
                }
                if (topo.ring[i][j]) {
                    c.ringSrc[c.nRing] = static_cast<uint8_t>(j);
                    c.ringDelayed[c.nRing] = delayed;
                    c.ringOn[c.nRing] = v.matrix.ring[i][j];
                    for (int s = 0; s < nAlgo; ++s) c.ringSlot[c.nRing][s] = v.algo(s).ring[i][j];
                    ++c.nRing;
                }
            }
        }
        for (int i = 0; i < kNumOps; ++i) outMix_[i] = v.matrix.outputMix[i];
        numAlgos_ = nAlgo;
        morphPos_ = -1.0f; // forca o calculo dos pesos na 1a amostra
        cachedPatch_ = patch_;
        cachedCompiled_ = compiled_;
        cachedGeneration_ = generation_;
        // [2026-10-22] modulacao: so as rotas ativas e so os LFOs/envelopes que
        // alguem usa rodam por amostra (a matriz tem 16 rotas, quase sempre vazias)
        patchGain_ = v.volumeDb != 0.0f ? dbToLinear(v.volumeDb) : 1.0f;
        numRoutes_ = 0;
        for (int k = 0; k < kNumLfos; ++k) lfoRateMul_[k] = lfoDepthMul_[k] = 1.0f; // rota removida: nao fica preso no valor antigo
        for (float& m : modEnvRateMul_) m = 1.0f;
        for (int k = 0; k < kNumLfos; ++k) lfoUsed_[k] = v.lfo(k).pitchDepth != 0.0f || v.lfo(k).ampDepth != 0.0f;
        for (bool& e : envUsed_) e = false;
        for (int r = 0; r < kNumModRoutes; ++r) {
            const auto& route = v.modMatrix[r];
            if (route.src == ModSource::None || route.dest == ModDest::None) continue;
            routes_[numRoutes_++] = static_cast<uint8_t>(r);
            if (route.src == ModSource::LFO1) lfoUsed_[0] = true;
            if (route.src == ModSource::LFO2) lfoUsed_[1] = true;
            if (route.src == ModSource::LFO3) lfoUsed_[2] = true;
            if (route.src == ModSource::LFO4) lfoUsed_[3] = true;
            if (route.src == ModSource::Env1) envUsed_[0] = true;
            if (route.src == ModSource::Env2) envUsed_[1] = true;
            if (route.src == ModSource::Env3) envUsed_[2] = true;
        }
        // duracao natural do ciclo dos envelopes com sync (0 = sem sync)
        for (int k = 0; k < kNumModEnvs; ++k) {
            const auto& e = v.modEnv[k];
            envCycleSec_[k] = 0.0f;
            if (!e.tempoSync) continue;
            const float l1 = e.level[0], l2 = e.level[1], l3 = e.level[2];
            envCycleSec_[k] = std::fabs(l1 - l3) / egRateToSpeed(e.rate[0]) + std::fabs(l2 - l1) / egRateToSpeed(e.rate[1]) +
                              std::fabs(l3 - l2) / egRateToSpeed(e.rate[2]);
        }
        cacheValid_ = true;
    }

    float processOneSample() {
        if (!cacheValid_ || patch_ != cachedPatch_ || compiled_ != cachedCompiled_ || generation_ != cachedGeneration_)
            rebuildCache();

        // 1. glide (portamento continuo em log2 de frequencia)
        if (glideEnabled_ && currentHz_ != targetHz_) {
            if (glideTimeSec_ != glideCoefTime_ || sampleRate_ != glideCoefRate_) {
                glideCoef_ = 1.0f - std::exp(-1.0f / (std::max(0.001f, glideTimeSec_) * static_cast<float>(sampleRate_)));
                glideCoefTime_ = glideTimeSec_;
                glideCoefRate_ = sampleRate_;
            }
            if (std::fabs(targetLog2_ - currentLog2_) < 1e-5f) { // encaixa (float pode nunca ficar ==)
                currentHz_ = targetHz_;
                currentLog2_ = targetLog2_;
            } else {
                currentLog2_ += (targetLog2_ - currentLog2_) * glideCoef_;
                currentHz_ = std::exp2(currentLog2_);
            }
        } else {
            currentHz_ = targetHz_;
            currentLog2_ = targetLog2_;
        }

        // 2. LFOs (rate/profundidade modulados pela matriz na amostra ANTERIOR)
        // (LFO/envelope que ninguem usa nao roda -- ver rebuildCache)
        float lfoV[kNumLfos] = {};
        for (int k = 0; k < kNumLfos; ++k) {
            if (!lfoUsed_[k]) continue;
            if (globalLfo_[k]) { // LFO da parte (o rate vindo da matriz nao se aplica)
                lfoV[k] = globalLfo_[k][oversampleFactor_ <= 1 ? globalIdx_ : globalIdx_ - 1];
                if (lfoDepthMul_[k] != 1.0f) lfoV[k] *= lfoDepthMul_[k];
                continue;
            }
            lfoV[k] = lfos_[k].process(lfoRateMul_[k]);
            if (lfoDepthMul_[k] != 1.0f) lfoV[k] *= lfoDepthMul_[k];
        }
        const float lfo1v = lfoV[0], lfo2v = lfoV[1];
        // envelopes de modulacao (fontes Env1..Env3, 0..1)
        float envV[kNumModEnvs] = {};
        for (int k = 0; k < kNumModEnvs; ++k)
            if (envUsed_[k]) {
                // sync: o EG anda `mul` vezes mais rapido (passo = velocidade / taxa,
                // entao divide a taxa) pro ciclo L3->L1->L2->L3 durar a divisao do BPM
                double rate = sampleRate_;
                if (envCycleSec_[k] > 0.0f)
                    rate /= envCycleSec_[k] * tempoBpm_ / (60.0f * lfoDivisionBeats(patch_->modEnv[k].division));
                if (modEnvRateMul_[k] != 1.0f) rate /= modEnvRateMul_[k]; // ModDest::ModEnvSpeed (amostra anterior)
                envV[k] = modEnv_[k].process(rate) * (1.0f / 99.0f);
            }

        // 3. PEG
        float pegRaw = peg_.process(sampleRate_);
        float pegNorm = (pegRaw - 50.0f) / 50.0f; // -1..1
        float pegSemis = pegNorm * patch_->peg.rangeOctaves * 12.0f;
        if (numRoutes_ > 0) noteAge_ = std::min(1.0f, noteAge_ + static_cast<float>(0.2 / sampleRate_));

        // 4. fontes da matriz de modulacao -> trims
        float levelTrim[kNumOps] = {};
        float fx1Trim[kNumOps] = {};
        float morphTrim[kNumOps] = {};
        float fbTrim[kNumOps] = {};
        float opPitchTrimSemis[kNumOps] = {};
        float fx2Trim[kNumOps] = {};
        float voicePitchTrimSemis = 0.0f, voicePanTrim = 0.0f, voiceLevelTrim = 0.0f, algoMorphTrim = 0.0f;
        float lfoRateTrim[kNumLfos] = {}, lfoDepthTrim[kNumLfos] = {};
        float fmInTrim[kNumOps] = {}, phaseDistTrim[kNumOps] = {}, egSpeedTrim[kNumOps] = {};
        float voiceFmIn = 0.0f, voiceFx1 = 0.0f, voiceFx2 = 0.0f, pegDepthTrim = 0.0f;
        float modEnvSpeedTrim[kNumModEnvs] = {};

        for (int ri = 0; ri < numRoutes_; ++ri) {
            const auto& route = patch_->modMatrix[routes_[ri]];
            float srcVal = 0.0f;
            switch (route.src) {
                case ModSource::LFO1:      srcVal = lfo1v; break;
                case ModSource::LFO2:      srcVal = lfo2v; break;
                case ModSource::ModWheel:  srcVal = controllers_.modWheel01; break;
                case ModSource::ChannelAT: srcVal = controllers_.channelAT01; break;
                case ModSource::PolyAT:    srcVal = controllers_.polyAT01; break;
                case ModSource::PitchBend: srcVal = controllers_.pitchBendSemis / std::max(0.01f, bendRangeSemis_); break;
                case ModSource::KeyNote:   srcVal = std::clamp((static_cast<float>(note_) - 60.0f) / 60.0f, -1.0f, 1.0f); break;
                case ModSource::Velocity:  srcVal = static_cast<float>(velocity_) / 127.0f; break;
                case ModSource::LFO3:      srcVal = lfoV[2]; break;
                case ModSource::LFO4:      srcVal = lfoV[3]; break;
                case ModSource::Env1:      srcVal = envV[0]; break;
                case ModSource::Env2:      srcVal = envV[1]; break;
                case ModSource::Env3:      srcVal = envV[2]; break;
                case ModSource::Macro1:    srcVal = controllers_.macro01[0]; break;
                case ModSource::Macro2:    srcVal = controllers_.macro01[1]; break;
                case ModSource::Macro3:    srcVal = controllers_.macro01[2]; break;
                case ModSource::Macro4:    srcVal = controllers_.macro01[3]; break;
                case ModSource::Slide:     srcVal = controllers_.slide01; break;
                case ModSource::Breath:       srcVal = controllers_.cc01[0]; break;
                case ModSource::Foot:         srcVal = controllers_.cc01[1]; break;
                case ModSource::Expression:   srcVal = controllers_.cc01[2]; break;
                case ModSource::SustainPedal: srcVal = controllers_.cc01[3]; break;
                case ModSource::RandomNote:   srcVal = controllers_.randomNote; break;
                case ModSource::Alternate:    srcVal = controllers_.alternate; break;
                case ModSource::NoteAge:      srcVal = noteAge_; break;
                case ModSource::AmpEnv:
                    for (int k = 0; k < kNumOps; ++k) if (isCarrier(k) && patch_->op[k].enabled) srcVal = std::max(srcVal, ops_[k].lastAmpRaw / 99.0f);
                    break;
                case ModSource::PegEnv:       srcVal = pegNorm; break;
                case ModSource::Constant:     srcVal = 1.0f; break;
                default: break;
            }
            float amount = srcVal * route.depth;
            int oi = route.opIndex;
            switch (route.dest) {
                case ModDest::OperatorLevel:    if (oi >= 0 && oi < kNumOps) levelTrim[oi] += amount; break;
                case ModDest::OperatorFx1:      if (oi >= 0 && oi < kNumOps) fx1Trim[oi] += amount; break;
                case ModDest::OperatorMorph:    if (oi >= 0 && oi < kNumOps) morphTrim[oi] += amount; break;
                case ModDest::OperatorFeedback: if (oi >= 0 && oi < kNumOps) fbTrim[oi] += amount; break;
                case ModDest::VoicePitch:       voicePitchTrimSemis += amount * 12.0f; break;
                case ModDest::VoicePan:         voicePanTrim += amount; break;
                case ModDest::VoiceLevel:       voiceLevelTrim += amount; break;
                case ModDest::OperatorPitch:    if (oi >= 0 && oi < kNumOps) opPitchTrimSemis[oi] += amount * 12.0f; break;
                case ModDest::OperatorFx2:      if (oi >= 0 && oi < kNumOps) fx2Trim[oi] += amount; break;
                case ModDest::LfoRate:          if (oi >= 0 && oi < kNumLfos) lfoRateTrim[oi] += amount; break;
                case ModDest::LfoDepth:         if (oi >= 0 && oi < kNumLfos) lfoDepthTrim[oi] += amount; break;
                case ModDest::AlgoMorph:        algoMorphTrim += amount; break;
                case ModDest::OperatorFmIn:     if (oi >= 0 && oi < kNumOps) fmInTrim[oi] += amount; break;
                case ModDest::OperatorPhaseDist:if (oi >= 0 && oi < kNumOps) phaseDistTrim[oi] += amount; break;
                case ModDest::OperatorEgSpeed:  if (oi >= 0 && oi < kNumOps) egSpeedTrim[oi] += amount; break;
                case ModDest::VoiceFmIn:        voiceFmIn += amount; break;
                case ModDest::VoiceFx1:         voiceFx1 += amount; break;
                case ModDest::VoiceFx2:         voiceFx2 += amount; break;
                case ModDest::PegDepth:         pegDepthTrim += amount; break;
                case ModDest::ModEnvSpeed:      if (oi >= 0 && oi < kNumModEnvs) modEnvSpeedTrim[oi] += amount; break;
                default: break;
            }
        }

        if (voiceFmIn != 0.0f) for (float& t : fmInTrim) t += voiceFmIn;
        if (voiceFx1 != 0.0f) for (float& t : fx1Trim) t += voiceFx1;
        if (voiceFx2 != 0.0f) for (float& t : fx2Trim) t += voiceFx2;
        if (pegDepthTrim != 0.0f) pegSemis *= std::max(0.0f, 1.0f + pegDepthTrim);
        // pitch comum a todos os operadores da voz (bend + mod matrix) e
        // vibrato antes da sensibilidade de cada operador
        const float voiceSemis = controllers_.pitchBendSemis + controllers_.noteBendSemis + voicePitchTrimSemis;
        const float vibratoRaw = lfo1v * patch_->lfo1.pitchDepth + lfo2v * patch_->lfo2.pitchDepth +
                                 lfoV[2] * patch_->lfo3.pitchDepth + lfoV[3] * patch_->lfo4.pitchDepth;
        const float tremoloRaw = lfo1v * patch_->lfo1.ampDepth + lfo2v * patch_->lfo2.ampDepth +
                                 lfoV[2] * patch_->lfo3.ampDepth + lfoV[3] * patch_->lfo4.ampDepth;
        // rate/profundidade dos LFOs: valem a partir da PROXIMA amostra
        if (numRoutes_ > 0)
            for (int k = 0; k < kNumLfos; ++k) {
                lfoRateMul_[k] = lfoRateTrim[k] != 0.0f ? std::exp2(3.0f * lfoRateTrim[k]) : 1.0f;
                lfoDepthMul_[k] = std::max(0.0f, 1.0f + lfoDepthTrim[k]);
            }
        if (numRoutes_ > 0)
            for (int k = 0; k < kNumModEnvs; ++k)
                modEnvRateMul_[k] = modEnvSpeedTrim[k] != 0.0f ? std::exp2(3.0f * modEnvSpeedTrim[k]) : 1.0f;
        panMod_ = voicePanTrim;
        if (numAlgos_ > 1) applyAlgoMorph(std::clamp(patch_->algoMorph + algoMorphTrim, 0.0f, 1.0f));
        const double invSr = 1.0 / sampleRate_;

        // 5. operadores, na ordem compilada
        for (int idx = 0; idx < kNumOps; ++idx) {
            int i = compiled_->order[idx];
            const auto& opParams = patch_->op[i];
            const OpCache& c = cache_[i];
            auto& op = ops_[i];
            // ModDest::OperatorEgSpeed: passo do EG = velocidade / taxa, entao divide a taxa
            const double egRate = egSpeedTrim[i] != 0.0f ? sampleRate_ / std::exp2(3.0f * egSpeedTrim[i]) : sampleRate_;

            if (!c.used) {
                // operador morto (nao chega na saida por nenhum caminho): nao
                // calcula oscilador/FX, so deixa os envelopes andarem -- se uma
                // edicao ao vivo o ligar no meio da nota, ele entra no ponto
                // certo do envelope. Saida 0 (ninguem le, mas fica coerente).
                op.fxEG.process(egRate);
                op.ampEG.process(egRate);
                op.signal = 0.0f;
                continue;
            }

            // a. frequencia: 1 exp2 por operador (antes: 4 pow), e nenhuma
            // quando nao ha PEG/vibrato/bend
            float semis = voiceSemis + vibratoRaw * c.pitchSens + (c.pegApplies ? pegSemis * c.pegDepth : 0.0f);
            if (opPitchTrimSemis[i] != 0.0f) semis += opPitchTrimSemis[i];
            float freqHz = c.fixed ? c.baseHz : currentHz_ * c.baseHz;
            if (semis != 0.0f) freqHz *= std::exp2(semis * (1.0f / 12.0f));

            // b. modulacao de fase recebida (PM aditiva) + feedback proprio
            float modIn = 0.0f;
            for (int k = 0; k < c.nMod; ++k) {
                const auto& src = ops_[c.modSrc[k]];
                modIn += c.modAmt[k] * (c.modDelayed[k] ? src.prevOut : src.signal);
            }
            if (fmInTrim[i] != 0.0f) modIn *= std::max(0.0f, 1.0f + fmInTrim[i]);
            // Auto-feedback = diagonal da matriz + op.feedback (somados). Usa a
            // media das 2 ultimas saidas, como o DX7: com 1 amostra so, feedback
            // alto entra em oscilacao caotica (ruido "chiado" em vez de serra).
            float effectiveFeedback = std::clamp(opParams.feedback + fbTrim[i], 0.0f, 1.0f);
            float selfAmount = c.selfMod + effectiveFeedback;
            if (selfAmount != 0.0f) modIn += selfAmount * 0.5f * (op.prevOut + op.prevPrevOut);

            // c. avanca fase, aplica phase distortion, le forma de onda
            op.phase += static_cast<double>(freqHz) * invSr;
            if (op.phase >= 1.0) op.phase -= std::floor(op.phase);
            double phaseWithMod = op.phase + static_cast<double>(modIn);
            if (phaseWithMod >= 1.0 || phaseWithMod < 0.0) phaseWithMod -= std::floor(phaseWithMod);
            const float phaseDist = phaseDistTrim[i] != 0.0f ? std::clamp(opParams.phaseDistortion + phaseDistTrim[i], 0.0f, 1.0f) : opParams.phaseDistortion;
            float ph = phaseDistort(static_cast<float>(phaseWithMod), phaseDist);
            // fxEG processado ANTES da onda: alem do FX (passo e), ele move o
            // morph A->B da onda Additive
            float fxEnv = op.fxEG.process(egRate);
            float raw;
            if (opParams.waveform == Waveform::Additive) {
                // kNumPartials (64) osciladores reais (dx7_dsp.h::PartialOscillators), cada um
                // na sua frequencia; recebem a MESMA PM (modIn) que o operador
                // e sampleRate_ e a taxa INTERNA (com oversampling)
                float morph = std::clamp(opParams.morphDepth * (fxEnv / 99.0f) + morphTrim[i], 0.0f, 1.0f);
                raw = op.partials.process(opParams.additiveA, opParams.additiveB, morph, freqHz, modIn,
                                          phaseDist, static_cast<float>(sampleRate_), c.partialGroups);
            } else if (opParams.waveform == Waveform::Wavetable && wavetables_ && (*wavetables_)[static_cast<size_t>(i)]) {
                // duas ondas com morph (dx7_wavetable.h): posicao do knob +
                // fxEG x profundidade + mod matrix; mipmap pela frequencia
                float morph = std::clamp(opParams.wavetable.position + opParams.morphDepth * (fxEnv / 99.0f) + morphTrim[i], 0.0f, 1.0f);
                raw = (*wavetables_)[static_cast<size_t>(i)]->read(ph, morph, freqHz, static_cast<float>(sampleRate_));
            } else {
                raw = oscWave(ph, opParams.waveform, rng_, dist_);
            }

            // d. ring modulation entre operadores (conexoes da matriz marcadas ring=true)
            for (int k = 0; k < c.nRing; ++k) {
                if (!c.ringOn[k]) continue; // morph de algoritmos: conexao ausente no slot atual
                const auto& src = ops_[c.ringSrc[k]];
                raw *= c.ringDelayed[k] ? src.prevOut : src.signal;
            }

            // e. fx slot do operador, com fxEG modulando fxParam1
            float fx1 = opParams.fxParam1 * (1.0f + fx1Trim[i]);
            const float fx2 = fx2Trim[i] != 0.0f ? opParams.fxParam2 * (1.0f + fx2Trim[i]) : opParams.fxParam2;
            // key tracking do efeito: segue a altura ATUAL da voz (com glide;
            // sem bend/vibrato), relativa ao C4. exp2 so quando a altura muda.
            if (opParams.fxKeyTrack > 0.0f) {
                if (currentLog2_ != op.ktLog2 || opParams.fxKeyTrack != op.ktAmount) {
                    op.ktLog2 = currentLog2_;
                    op.ktAmount = opParams.fxKeyTrack;
                    op.ktFactor = std::exp2(opParams.fxKeyTrack * (currentLog2_ - kLog2C4));
                }
                fx1 *= op.ktFactor;
            }
            float processed = raw;
            switch (opParams.fxType) {
                case FxType::Bypass: break;
                case FxType::LowPass: {
                    float cutoff = std::max(20.0f, fx1 * (0.2f + 0.8f * (fxEnv / 99.0f)));
                    if (opParams.fxModel != FilterModel::Svf) { processed = op.modelFilter.process(raw, cutoff, fx2, opParams.fxModel, kFilterLP); break; }
                    op.filter.setParams(cutoff, std::max(0.1f, fx2));
                    processed = op.filter.process(raw).lp;
                    break;
                }
                case FxType::BandPass: {
                    float cutoff = std::max(20.0f, fx1 * (0.2f + 0.8f * (fxEnv / 99.0f)));
                    if (opParams.fxModel != FilterModel::Svf) { processed = op.modelFilter.process(raw, cutoff, fx2, opParams.fxModel, kFilterBP); break; }
                    op.filter.setParams(cutoff, std::max(0.1f, fx2));
                    processed = op.filter.process(raw).bp;
                    break;
                }
                case FxType::HighPass: {
                    float cutoff = std::max(20.0f, fx1 * (0.2f + 0.8f * (fxEnv / 99.0f)));
                    if (opParams.fxModel != FilterModel::Svf) { processed = op.modelFilter.process(raw, cutoff, fx2, opParams.fxModel, kFilterHP); break; }
                    op.filter.setParams(cutoff, std::max(0.1f, fx2));
                    processed = op.filter.process(raw).hp;
                    break;
                }
                case FxType::Notch: {
                    float cutoff = std::max(20.0f, fx1 * (0.2f + 0.8f * (fxEnv / 99.0f)));
                    op.filter.setParams(cutoff, std::max(0.1f, fx2));
                    const auto o = op.filter.process(raw);
                    processed = o.lp + o.hp;
                    break;
                }
                case FxType::Waveshape: {
                    // FX1 = drive (1..16; o fxEG varre como no Fold), FX2 = bias (0..1)
                    const float maxDrive = std::clamp(fx1, 1.0f, 16.0f);
                    const float drive = 1.0f + (maxDrive - 1.0f) * (0.2f + 0.8f * (fxEnv / 99.0f));
                    const float bias = std::clamp(fx2, 0.0f, 1.0f);
                    processed = op.adaa.waveshape(raw * drive + bias, opParams.fxShape);
                    // bias e as curvas assimetricas geram DC
                    if (bias > 0.0f || opParams.fxShape == WaveshapeCurve::Tube || opParams.fxShape == WaveshapeCurve::Rectify)
                        processed = op.dcBlock.process(processed);
                    break;
                }
                case FxType::RingMod: {
                    int src = opParams.ringModSource;
                    if (src >= 0 && src < kNumOps && !patch_->op[src].enabled) break; // fonte desligada: passa direto
                    float srcSig = (src >= 0 && src < kNumOps) ? ops_[src].prevOut : 0.0f;
                    processed = raw * srcSig;
                    break;
                }
                case FxType::Saturate: processed = op.adaa.saturate(raw, 1.0f + fx2 * 8.0f); break; // = softSaturate, com ADAA
                case FxType::Crush:    processed = op.crusher.process(raw, fx1, fx2 * 16.0f); break;
                case FxType::FreqShift:processed = op.shifter.process(raw, fx1); break;
                case FxType::Comb:     processed = op.comb.process(raw, fx1, fx2); break;
                case FxType::Fold: {
                    // FX1 = drive (1 = sem dobra, ate 16), modulado pelo fxEG do
                    // mesmo jeito que o corte do filtro: 20% no nivel 0 do
                    // envelope, 100% no nivel 99 -- a dobra "abre" no ataque e
                    // "fecha" no release. FX Param2 = simetria/bias (0..1):
                    // 0 = dobra simetrica, so harmonicos impares; >0 = pares
                    // tambem (e DC, removido pelo dcBlock).
                    float maxDrive = std::clamp(fx1, 1.0f, 16.0f);
                    float drive = 1.0f + (maxDrive - 1.0f) * (0.2f + 0.8f * (fxEnv / 99.0f));
                    float bias = std::clamp(fx2, 0.0f, 1.0f);
                    processed = op.adaa.fold(raw * drive + bias); // triangleFold com ADAA
                    // drive 1 sem simetria nao dobra nada: sai o sinal exato (o
                    // ADAA daria a media com a amostra anterior); o estado do
                    // ADAA segue atualizado, sem degrau se o drive subir depois
                    if (maxDrive <= 1.0f && bias == 0.0f) processed = raw;
                    if (bias > 0.0f) processed = op.dcBlock.process(processed);
                    break;
                }
            }

            // f. envelope de amplitude x ganho estatico (cache) x tremolo x levelTrim.
            // A amplitude do EG (uma exp2) so e recalculada quando o nivel muda --
            // no sustain, nunca.
            float ampRaw = op.ampEG.process(egRate);
            if (ampRaw != op.lastAmpRaw) {
                op.lastAmpRaw = ampRaw;
                op.lastAmp = egLevelToAmplitude(ampRaw);
            }
            float tremolo = 1.0f + tremoloRaw * c.ampSens * 0.5f;
            float levelLinear = c.staticGain * op.lastAmp * std::max(0.0f, tremolo) * std::max(0.0f, 1.0f + levelTrim[i]);

            op.signal = processed * levelLinear;
            if (!std::isfinite(op.signal)) {
                // rede de seguranca: um NaN/Inf aqui se propagaria pelo feedback
                // da voz e pelos buffers dos efeitos globais, silenciando tudo
                op.signal = 0.0f;
                op.filter.reset();
                op.modelFilter.reset();
                op.comb.reset();
                op.shifter.reset();
                op.crusher.reset();
                op.dcBlock.reset();
                op.adaa.reset();
            }
        }

        // 6. soma outputMix, depois propaga signal -> prevOut para TODOS de uma vez
        float sum = 0.0f;
        if (soloOp_ >= 0 && soloOp_ < kNumOps) sum = ops_[soloOp_].signal;
        else for (int i = 0; i < kNumOps; ++i) sum += ops_[i].signal * outMix_[i];
        for (auto& op : ops_) { op.prevPrevOut = op.prevOut; op.prevOut = op.signal; }
        if (voiceLevelTrim != 0.0f) sum *= std::max(0.0f, 1.0f + voiceLevelTrim); // ModDest::VoiceLevel
        if (patchGain_ != 1.0f) sum *= patchGain_;                                  // VoiceParams::volumeDb

        // 7. idle check
        if (!anyCarrierSounding()) active_ = false;

        return sum;
    }

public:
    // Membros publicos "soltos", de proposito, pro alocador settar direto:
    float bendRangeSemis_ = 2.0f;
    bool glideEnabled_ = false;
    float glideTimeSec_ = 0.05f;
    bool reuseEnvOnLegato_ = false;

    static float noteToHz(int note) {
        return 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
    }

private:
    // Portadora em algum slot ativo do morph. Le do patch (nao do cache): o
    // Synth pergunta isActive() de uma voz recem-disparada antes do cache dela
    // ser refeito.
    bool isCarrier(int i) const {
        const int n = patch_->activeAlgos();
        for (int s = 0; s < n; ++s)
            if (patch_->algo(s).outputMix[i] != 0.0f) return true;
        return false;
    }

    // [2026-10-24] pesos do ponto `pos` (0..1) do morph entre algoritmos:
    // interpola PM, diagonal e outputMix entre os 2 slots vizinhos; ring
    // (liga/desliga) vem do mais proximo. So refaz quando a posicao muda.
    void applyAlgoMorph(float pos) {
        const int n = numAlgos_;
        float x = pos * static_cast<float>(n - 1);
        const int s = std::min(static_cast<int>(x), n - 2);
        float t = x - static_cast<float>(s);
        if (patch_->algoStep) t = t < 0.5f ? 0.0f : 1.0f;
        const float key = static_cast<float>(s) + t;
        if (key == morphPos_) return;
        morphPos_ = key;
        const bool nearB = t >= 0.5f;
        for (int i = 0; i < kNumOps; ++i) {
            OpCache& c = cache_[i];
            for (int k = 0; k < c.nMod; ++k) c.modAmt[k] = c.modAmtSlot[k][s] + (c.modAmtSlot[k][s + 1] - c.modAmtSlot[k][s]) * t;
            c.selfMod = c.selfModSlot[s] + (c.selfModSlot[s + 1] - c.selfModSlot[s]) * t;
            for (int k = 0; k < c.nRing; ++k) c.ringOn[k] = c.ringSlot[k][nearB ? s + 1 : s];
            const float a = patch_->algo(s).outputMix[i], b = patch_->algo(s + 1).outputMix[i];
            outMix_[i] = a + (b - a) * t;
        }
    }

    bool anyCarrierSounding() const {
        if (!patch_) return false;
        for (int i = 0; i < kNumOps; ++i)
            if (isCarrier(i) && !ops_[i].ampEG.isSilent()) return true;
        return false;
    }

    static float keyLevelScaleDb(const KeyLevelScaling& kls, int note) {
        int dist = note - static_cast<int>(kls.breakpoint);
        if (dist == 0) return 0.0f;
        float octaves = std::abs(dist) / 12.0f;
        bool right = dist > 0;
        int8_t depth = right ? kls.rightDepth : kls.leftDepth;
        bool exp = right ? kls.rightExp : kls.leftExp;
        float sign = depth >= 0 ? 1.0f : -1.0f;
        return exp ? (sign * octaves * octaves * 0.05f * std::abs(depth)) : (octaves * depth);
    }

    const VoiceParams* patch_ = nullptr;
    const WavetableBanks* wavetables_ = nullptr;
    const TuningTable* tuning_ = nullptr;
    const float* globalLfo_[kNumLfos] = {};
    int globalIdx_ = 0;
    const CompiledAlgorithm* compiled_ = nullptr;
    double baseSampleRate_ = 0.0; // taxa "externa"; 0 ate o primeiro setSampleRate()
    double sampleRate_ = 48000.0; // taxa INTERNA (= baseSampleRate_ * oversampleFactor_)
    int oversampleFactor_ = 1;
    HalfbandDecimator2x decim1_, decim2_; // decim2_ so usado quando oversampleFactor_==4
    int note_ = 60;
    int velocity_ = 100;
    float extraDetuneCents_ = 0.0f;
    bool active_ = false;
    bool releasing_ = false;
    float currentHz_ = 440.0f;
    float targetHz_ = 440.0f;
    float currentLog2_ = std::log2(440.0f), targetLog2_ = std::log2(440.0f); // glide em log2
    float glideCoef_ = 1.0f, glideCoefTime_ = -1.0f;
    double glideCoefRate_ = 0.0;
    OpCache cache_[kNumOps];
    const VoiceParams* cachedPatch_ = nullptr;
    const CompiledAlgorithm* cachedCompiled_ = nullptr;
    uint64_t generation_ = 0, cachedGeneration_ = 0;
    bool cacheValid_ = false;
    // rotas ativas da matriz e LFOs/envelopes usados (rebuildCache)
    uint8_t routes_[kNumModRoutes] = {};
    int numRoutes_ = 0;
    bool lfoUsed_[kNumLfos] = {}, envUsed_[kNumModEnvs] = {};
    float envCycleSec_[kNumModEnvs] = {}; // ciclo natural dos envelopes com sync (rebuildCache)
    // morph de algoritmos: outputMix atual (com 1 algoritmo = o da matriz), slots e ultima posicao
    float outMix_[kNumOps] = {};
    int numAlgos_ = 1;
    float morphPos_ = -1.0f;
    float tempoBpm_ = 120.0f;

    OperatorState ops_[kNumOps];
    SegmentEG peg_;
    SegmentEG modEnv_[kNumModEnvs];
    LFO lfos_[kNumLfos];
    float lfoRateMul_[kNumLfos] = {1.0f, 1.0f, 1.0f, 1.0f}, lfoDepthMul_[kNumLfos] = {1.0f, 1.0f, 1.0f, 1.0f};
    float modEnvRateMul_[kNumModEnvs] = {1.0f, 1.0f, 1.0f};
    float noteAge_ = 0.0f;
    int soloOp_ = -1;
    float panMod_ = 0.0f;
    float patchGain_ = 1.0f; // [2026-10-29] dbToLinear(VoiceParams::volumeDb), no rebuildCache
    VoiceControllers controllers_;
    std::mt19937 rng_{newRandomSeed()};
    std::uniform_real_distribution<float> dist_{-1.0f, 1.0f};
};

} // namespace dx7fm

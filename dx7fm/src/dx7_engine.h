#pragma once
#include "dx7_params.h"
#include "dx7_dsp.h"
#include <algorithm>
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

struct OperatorState {
    double phase = 0.0;
    SegmentEG ampEG;
    SegmentEG fxEG;
    TPTStateVariableFilter filter;
    Crusher crusher;
    CombFilter comb;
    FreqShifter shifter;
    float signal = 0.0f;
    float prevOut = 0.0f;
};

struct VoiceControllers {
    float pitchBendSemis = 0.0f;
    float modWheel01 = 0.0f;
    float channelAT01 = 0.0f;
    float polyAT01 = 0.0f;
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
        lfo1_.setSampleRate(sampleRate_);
        lfo2_.setSampleRate(sampleRate_);
        for (auto& op : ops_) {
            op.filter.setSampleRate(sampleRate_);
            op.crusher.setSampleRate(sampleRate_);
            op.comb.setSampleRate(sampleRate_);
            op.shifter.setSampleRate(sampleRate_);
        }
    }

    void noteOn(const VoiceParams* patch, const CompiledAlgorithm* compiled,
                int note, int velocity, float extraDetuneCents = 0.0f, bool legato = false) {
        patch_ = patch;
        compiled_ = compiled;
        note_ = note;
        velocity_ = velocity;
        extraDetuneCents_ = extraDetuneCents;
        active_ = true;
        releasing_ = false;

        targetHz_ = noteToHz(note);
        if (!(legato && glideEnabled_)) currentHz_ = targetHz_;

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
                op.signal = 0.0f;
            }
            peg_.configure(patch_->peg.rate, patch_->peg.level);
            peg_.noteOn(0.0f);
            lfo1_.configure(patch_->lfo1);
            lfo2_.configure(patch_->lfo2);
            lfo1_.noteOn();
            lfo2_.noteOn();
        }
    }

    void noteOff() {
        releasing_ = true;
        for (int i = 0; i < kNumOps; ++i) {
            ops_[i].ampEG.noteOff();
            if (patch_->op[i].fxEGReleaseWithAmp) ops_[i].fxEG.noteOff();
        }
        peg_.noteOff();
    }

    void setGlide(bool enabled, float timeSec) { glideEnabled_ = enabled; glideTimeSec_ = timeSec; }
    void setLegatoEnvReuse(bool reuse) { reuseEnvOnLegato_ = reuse; }

    bool isActive() const {
        if (!active_) return false;
        for (auto& op : ops_) if (!op.ampEG.isIdle()) return true;
        return false;
    }
    bool isReleasing() const { return releasing_; }
    int note() const { return note_; }
    float debugCurrentHz() const { return currentHz_; }
    bool debugFxEGIdle(int opIndex) const { return ops_[opIndex].fxEG.isIdle(); }
    VoiceControllers& controllers() { return controllers_; }

    // Uma amostra na taxa BASE (nao superamostrada). Se oversampleFactor_>1,
    // roda processOneSample() internamente a taxa superamostrada e decima
    // com HalfbandDecimator2x (cascateado pra 4x) antes de devolver.
    float process() {
        if (oversampleFactor_ <= 1) return processOneSample();

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
    float processOneSample() {
        // 1. glide (portamento continuo em log2 de frequencia)
        if (glideEnabled_ && currentHz_ != targetHz_) {
            float coef = 1.0f - std::exp(-1.0f / (std::max(0.001f, glideTimeSec_) * static_cast<float>(sampleRate_)));
            float lg0 = std::log2(currentHz_);
            float lg1 = std::log2(targetHz_);
            currentHz_ = std::pow(2.0f, lg0 + (lg1 - lg0) * coef);
        } else {
            currentHz_ = targetHz_;
        }

        // 2. LFOs
        float lfo1v = lfo1_.process();
        float lfo2v = lfo2_.process();

        // 3. PEG
        float pegRaw = peg_.process(sampleRate_);
        float pegNorm = (pegRaw - 50.0f) / 50.0f; // -1..1
        float pegSemis = pegNorm * patch_->peg.rangeOctaves * 12.0f;

        // 4. fontes da matriz de modulacao -> trims
        float levelTrim[kNumOps] = {};
        float fx1Trim[kNumOps] = {};
        float fbTrim[kNumOps] = {};
        float voicePitchTrimSemis = 0.0f;

        for (int r = 0; r < kNumModRoutes; ++r) {
            const auto& route = patch_->modMatrix[r];
            if (route.src == ModSource::None || route.dest == ModDest::None) continue;
            float srcVal = 0.0f;
            switch (route.src) {
                case ModSource::LFO1:      srcVal = lfo1v; break;
                case ModSource::LFO2:      srcVal = lfo2v; break;
                case ModSource::ModWheel:  srcVal = controllers_.modWheel01; break;
                case ModSource::ChannelAT: srcVal = controllers_.channelAT01; break;
                case ModSource::PolyAT:    srcVal = controllers_.polyAT01; break;
                case ModSource::PitchBend: srcVal = controllers_.pitchBendSemis / std::max(0.01f, bendRangeSemis_); break;
                default: break;
            }
            float amount = srcVal * route.depth;
            int oi = route.opIndex;
            switch (route.dest) {
                case ModDest::OperatorLevel:    if (oi >= 0 && oi < kNumOps) levelTrim[oi] += amount; break;
                case ModDest::OperatorFx1:      if (oi >= 0 && oi < kNumOps) fx1Trim[oi] += amount; break;
                case ModDest::OperatorFeedback: if (oi >= 0 && oi < kNumOps) fbTrim[oi] += amount; break;
                case ModDest::VoicePitch:       voicePitchTrimSemis += amount * 12.0f; break;
                default: break;
            }
        }

        // 5. operadores, na ordem compilada
        for (int idx = 0; idx < kNumOps; ++idx) {
            int i = compiled_->order[idx];
            const auto& opParams = patch_->op[i];
            auto& op = ops_[i];

            // a. frequencia
            float freqHz;
            if (opParams.fixedFreq) {
                freqHz = opParams.fixedHz;
            } else {
                float centsTotal = opParams.fineCents + opParams.detuneCents + extraDetuneCents_;
                freqHz = currentHz_ * opParams.coarseRatio * std::pow(2.0f, centsTotal / 1200.0f);
            }
            if (!(opParams.fixedFreq && !opParams.pegAffectsFixed)) {
                freqHz *= std::pow(2.0f, pegSemis * opParams.pegDepth / 12.0f);
            }
            float lfoPitchSens = opParams.lfoPitchModSens / 7.0f;
            float vibratoSemis = lfo1v * patch_->lfo1.pitchDepth * lfoPitchSens;
            freqHz *= std::pow(2.0f, vibratoSemis / 12.0f);
            freqHz *= std::pow(2.0f, (controllers_.pitchBendSemis + voicePitchTrimSemis) / 12.0f);

            // b. modulacao de fase recebida (PM aditiva) + feedback proprio
            float modIn = 0.0f;
            for (int j = 0; j < kNumOps; ++j) {
                bool hasEdge = (patch_->matrix.mod[i][j] != 0.0f) || patch_->matrix.ring[i][j];
                if (!hasEdge) continue;
                float srcSig = compiled_->feedbackEdge[i][j] ? ops_[j].prevOut : ops_[j].signal;
                modIn += patch_->matrix.mod[i][j] * srcSig;
            }
            float effectiveFeedback = std::clamp(opParams.feedback + fbTrim[i], 0.0f, 1.0f);
            if (effectiveFeedback > 0.0f) modIn += effectiveFeedback * op.prevOut;

            // c. avanca fase, aplica phase distortion, le forma de onda
            op.phase += static_cast<double>(freqHz) / sampleRate_;
            op.phase -= std::floor(op.phase);
            double phaseWithMod = op.phase + static_cast<double>(modIn);
            phaseWithMod -= std::floor(phaseWithMod);
            float ph = phaseDistort(static_cast<float>(phaseWithMod), opParams.phaseDistortion);
            float raw = oscWave(ph, opParams.waveform, rng_, dist_);

            // d. ring modulation entre operadores (conexoes da matriz marcadas ring=true)
            for (int j = 0; j < kNumOps; ++j) {
                if (!patch_->matrix.ring[i][j]) continue;
                float srcSig = compiled_->feedbackEdge[i][j] ? ops_[j].prevOut : ops_[j].signal;
                raw *= srcSig;
            }

            // e. fx slot do operador, com fxEG modulando fxParam1
            float fxEnv = op.fxEG.process(sampleRate_);
            float fx1 = opParams.fxParam1 * (1.0f + fx1Trim[i]);
            float processed = raw;
            switch (opParams.fxType) {
                case FxType::Bypass: break;
                case FxType::LowPass: {
                    float cutoff = std::max(20.0f, fx1 * (0.2f + 0.8f * (fxEnv / 99.0f)));
                    op.filter.setParams(cutoff, std::max(0.1f, opParams.fxParam2));
                    processed = op.filter.process(raw).lp;
                    break;
                }
                case FxType::BandPass: {
                    float cutoff = std::max(20.0f, fx1 * (0.2f + 0.8f * (fxEnv / 99.0f)));
                    op.filter.setParams(cutoff, std::max(0.1f, opParams.fxParam2));
                    processed = op.filter.process(raw).bp;
                    break;
                }
                case FxType::HighPass: {
                    float cutoff = std::max(20.0f, fx1 * (0.2f + 0.8f * (fxEnv / 99.0f)));
                    op.filter.setParams(cutoff, std::max(0.1f, opParams.fxParam2));
                    processed = op.filter.process(raw).hp;
                    break;
                }
                case FxType::RingMod: {
                    int src = opParams.ringModSource;
                    float srcSig = (src >= 0 && src < kNumOps) ? ops_[src].prevOut : 0.0f;
                    processed = raw * srcSig;
                    break;
                }
                case FxType::Saturate: processed = softSaturate(raw, opParams.fxParam2); break;
                case FxType::Crush:    processed = op.crusher.process(raw, fx1, opParams.fxParam2 * 16.0f); break;
                case FxType::FreqShift:processed = op.shifter.process(raw, fx1); break;
                case FxType::Comb:     processed = op.comb.process(raw, fx1, opParams.fxParam2); break;
            }

            // f. envelope de amplitude + key/velocity scaling + tremolo + levelTrim
            float ampRaw = op.ampEG.process(sampleRate_);
            float amp = egLevelToAmplitude(ampRaw);
            float levelDb = (static_cast<float>(opParams.outputLevel) - 99.0f) * 0.75f;
            levelDb += keyLevelScaleDb(opParams.levelScaling, note_);
            levelDb += (static_cast<float>(velocity_) - 127.0f) / 127.0f * (opParams.velocitySens / 7.0f) * 12.0f;
            float tremolo = 1.0f + lfo1v * patch_->lfo1.ampDepth * (opParams.lfoAmpModSens / 3.0f) * 0.5f;
            float levelLinear = dbToLinear(levelDb) * amp * std::max(0.0f, tremolo) * (1.0f + levelTrim[i]);

            op.signal = processed * levelLinear;
        }

        // 6. soma outputMix, depois propaga signal -> prevOut para TODOS de uma vez
        float sum = 0.0f;
        for (int i = 0; i < kNumOps; ++i) sum += ops_[i].signal * patch_->matrix.outputMix[i];
        for (auto& op : ops_) op.prevOut = op.signal;

        // 7. idle check
        bool anyActive = false;
        for (auto& op : ops_) if (!op.ampEG.isIdle()) { anyActive = true; break; }
        if (!anyActive) active_ = false;

        return sum;
    }

public:
    // Membros publicos "soltos", de proposito, pro alocador settar direto:
    float bendRangeSemis_ = 2.0f;
    bool glideEnabled_ = false;
    float glideTimeSec_ = 0.05f;
    bool reuseEnvOnLegato_ = false;

private:
    static float noteToHz(int note) {
        return 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
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

    OperatorState ops_[kNumOps];
    SegmentEG peg_;
    LFO lfo1_, lfo2_;
    VoiceControllers controllers_;
    std::mt19937 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> dist_{-1.0f, 1.0f};
};

} // namespace dx7fm

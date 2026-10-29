#pragma once
// Porta do Gatelope do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/Gatelope). Copyright (c) Chris Johnson / airwindows.
// Licenca MIT -- texto completo em THIRD_PARTY_NOTICES.md:
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software [...] THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//   WARRANTY OF ANY KIND.
// GERADO por tools/ports/airwindows_port.py: estado, construtor e
// processReplacing originais, sem a camada VST. Unica mudanca: a semente do
// dither vem de dx7fm::newRandomSeed() em vez de rand().
#include "../dx7_dsp.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <math.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

namespace dx7fm::airwindows {

class Gatelope {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=Thresh, B=TrebSus, C=BassSus, D=AttackS, E=Dry/Wet)
    float A = 0.0f;
    float B = 1.0f;
    float C = 0.5f;
    float D = 0.0f;
    float E = 1.0f;
    Gatelope() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        iirLowpassAL = 0.0;
        iirLowpassBL = 0.0;
        iirHighpassAL = 0.0;
        iirHighpassBL = 0.0;
        iirLowpassAR = 0.0;
        iirLowpassBR = 0.0;
        iirHighpassAR = 0.0;
        iirHighpassBR = 0.0;
        treblefreq = 1.0;
        bassfreq = 0.0;
        flip = false;
        fpdL = dx7fm::newRandomSeed() | 16386u;
        fpdR = dx7fm::newRandomSeed() | 16386u;
        //this is reset: values being initialized only once. Startup values, whatever they are.
    }

    void processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames)
    {
        float* in1  =  inputs[0];
        float* in2  =  inputs[1];
        float* out1 = outputs[0];
        float* out2 = outputs[1];

    	double overallscale = 1.0;
    	overallscale /= 44100.0;
    	overallscale *= getSampleRate();
    	//speed settings around release
    	double threshold = pow(A,2);
    	//gain settings around threshold
    	double trebledecay = pow(1.0-B,2)/4196.0;
    	double bassdecay =  pow(1.0-C,2)/8192.0;
    	double slowAttack = (pow(D,3)*3)+0.003;
    	double wet = E;
    	slowAttack /= overallscale;
    	trebledecay /= overallscale;
    	bassdecay /= overallscale;
    	trebledecay += 1.0;
    	bassdecay += 1.0;
    	double attackSpeed;
    	double highestSample;
    	//this VST version comes from the AU, Gatelinked, because it's stereo.
    	//if used on a mono track it'll act like the mono N to N

        while (--sampleFrames >= 0)
        {
    		double inputSampleL = *in1;
    		double inputSampleR = *in2;

    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;
    		double drySampleL = inputSampleL;
    		double drySampleR = inputSampleR;

    		if (fabs(inputSampleL) > fabs(inputSampleR)) {
    			attackSpeed = slowAttack - (fabs(inputSampleL)*slowAttack*0.5);
    			highestSample = fabs(inputSampleL);
    		} else {
    			attackSpeed = slowAttack - (fabs(inputSampleR)*slowAttack*0.5); //we're triggering off the highest amplitude
    			highestSample = fabs(inputSampleR); //and making highestSample the abs() of that amplitude
    		}

    		if (attackSpeed < 0.0) attackSpeed = 0.0;
    		//softening onset click depending on how hard we're getting it

    		if (flip)
    		{
    			if (highestSample > threshold)
    			{
    				treblefreq += attackSpeed;
    				if (treblefreq > 2.0) treblefreq = 2.0;
    				bassfreq -= attackSpeed;
    				bassfreq -= attackSpeed;
    				if (bassfreq < 0.0) bassfreq = 0.0;
    				iirLowpassAL = iirLowpassBL = inputSampleL;
    				iirHighpassAL = iirHighpassBL = 0.0;
    				iirLowpassAR = iirLowpassBR = inputSampleR;
    				iirHighpassAR = iirHighpassBR = 0.0;
    			}
    			else
    			{
    				treblefreq -= bassfreq;
    				treblefreq /= trebledecay;
    				treblefreq += bassfreq;
    				bassfreq -= treblefreq;
    				bassfreq /= bassdecay;
    				bassfreq += treblefreq;
    			}

    			if (treblefreq >= 1.0) {
    				iirLowpassAL = inputSampleL;
    				iirLowpassAR = inputSampleR;
    			} else {
    				iirLowpassAL = (iirLowpassAL * (1.0 - treblefreq)) + (inputSampleL * treblefreq);
    				iirLowpassAR = (iirLowpassAR * (1.0 - treblefreq)) + (inputSampleR * treblefreq);
    			}

    			if (bassfreq > 1.0) bassfreq = 1.0;

    			if (bassfreq > 0.0) {
    				iirHighpassAL = (iirHighpassAL * (1.0 - bassfreq)) + (inputSampleL * bassfreq);
    				iirHighpassAR = (iirHighpassAR * (1.0 - bassfreq)) + (inputSampleR * bassfreq);
    			} else {
    				iirHighpassAL = 0.0;
    				iirHighpassAR = 0.0;
    			}

    			if (treblefreq > bassfreq) {
    				inputSampleL = (iirLowpassAL - iirHighpassAL);
    				inputSampleR = (iirLowpassAR - iirHighpassAR);
    			} else {
    				inputSampleL = 0.0;
    				inputSampleR = 0.0;
    			}
    		}
    		else
    		{
    			if (highestSample > threshold)
    			{
    				treblefreq += attackSpeed;
    				if (treblefreq > 2.0) treblefreq = 2.0;
    				bassfreq -= attackSpeed;
    				bassfreq -= attackSpeed;
    				if (bassfreq < 0.0) bassfreq = 0.0;
    				iirLowpassAL = iirLowpassBL = inputSampleL;
    				iirHighpassAL = iirHighpassBL = 0.0;
    				iirLowpassAR = iirLowpassBR = inputSampleR;
    				iirHighpassAR = iirHighpassBR = 0.0;
    			}
    			else
    			{
    				treblefreq -= bassfreq;
    				treblefreq /= trebledecay;
    				treblefreq += bassfreq;
    				bassfreq -= treblefreq;
    				bassfreq /= bassdecay;
    				bassfreq += treblefreq;
    			}

    			if (treblefreq >= 1.0) {
    				iirLowpassBL = inputSampleL;
    				iirLowpassBR = inputSampleR;
    			} else {
    				iirLowpassBL = (iirLowpassBL * (1.0 - treblefreq)) + (inputSampleL * treblefreq);
    				iirLowpassBR = (iirLowpassBR * (1.0 - treblefreq)) + (inputSampleR * treblefreq);
    			}

    			if (bassfreq > 1.0) bassfreq = 1.0;

    			if (bassfreq > 0.0) {
    				iirHighpassBL = (iirHighpassBL * (1.0 - bassfreq)) + (inputSampleL * bassfreq);
    				iirHighpassBR = (iirHighpassBR * (1.0 - bassfreq)) + (inputSampleR * bassfreq);
    			} else {
    				iirHighpassBL = 0.0;
    				iirHighpassBR = 0.0;
    			}

    			if (treblefreq > bassfreq) {
    				inputSampleL = (iirLowpassBL - iirHighpassBL);
    				inputSampleR = (iirLowpassBR - iirHighpassBR);
    			} else {
    				inputSampleL = 0.0;
    				inputSampleR = 0.0;
    			}			
    		}
    		//done full gated envelope filtered effect
    		inputSampleL  = ((1-wet)*drySampleL)+(wet*inputSampleL);
    		inputSampleR  = ((1-wet)*drySampleR)+(wet*inputSampleR);
    		//we're going to set up a dry/wet control instead of a min. threshold

    		flip = !flip;

    		//begin 32 bit stereo floating point dither
    		int expon; frexpf((float)inputSampleL, &expon);
    		fpdL ^= fpdL << 13; fpdL ^= fpdL >> 17; fpdL ^= fpdL << 5;
    		inputSampleL += ((double(fpdL)-uint32_t(0x7fffffff)) * 5.5e-36l * pow(2,expon+62));
    		frexpf((float)inputSampleR, &expon);
    		fpdR ^= fpdR << 13; fpdR ^= fpdR >> 17; fpdR ^= fpdR << 5;
    		inputSampleR += ((double(fpdR)-uint32_t(0x7fffffff)) * 5.5e-36l * pow(2,expon+62));
    		//end 32 bit stereo floating point dither

    		*out1 = inputSampleL;
    		*out2 = inputSampleR;

    		*in1++;
    		*in2++;
    		*out1++;
    		*out2++;
        }
    }

private:
    double sampleRate_ = 44100.0;
    double iirLowpassAR;
    double iirLowpassBR;
    double iirHighpassAR;
    double iirHighpassBR;
    double iirLowpassAL;
    double iirLowpassBL;
    double iirHighpassAL;
    double iirHighpassBL;
    double treblefreq;
    double bassfreq;
    bool flip;
    uint32_t fpdL;
    uint32_t fpdR;
    //default stuff
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

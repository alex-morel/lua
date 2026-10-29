#pragma once
// Porta do RingModulator do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/RingModulator). Copyright (c) Chris Johnson / airwindows.
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

class RingModulator {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=Freq, B=Freq, C=Soar, D=Dry/Wet)
    float A = 0.5f;
    float B = 0.5f;
    float C = 0.0f;
    float D = 0.0f;
    RingModulator() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        sinePosL = 0.0;
        sinePosR = 0.0;
        incLA = 0.0; incLB = 0.0;
        incRA = 0.0; incRB = 0.0;
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

    	VstInt32 inFramesToProcess = sampleFrames; //vst doesn't give us this as a separate variable so we'll make it
    	double overallscale = 1.0;
    	overallscale /= 44100.0;
    	overallscale *= getSampleRate();

    	incLA = incLB; incLB = pow(A,5)/overallscale;
    	incRA = incRB; incRB = pow(B,5)/overallscale;
    	double soar = 0.3-(C*0.3);
    	double wet = pow(D,2);

        while (--sampleFrames >= 0)
        {
    		double inputSampleL = *in1;
    		double inputSampleR = *in2;
    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;
    		double drySampleL = inputSampleL;
    		double drySampleR = inputSampleR;

    		double temp = (double)sampleFrames/inFramesToProcess;
    		double incL = (incLA*temp)+(incLB*(1.0-temp));
    		double incR = (incRA*temp)+(incRB*(1.0-temp));

    		sinePosL += incL;
    		if (sinePosL > 6.283185307179586) sinePosL -= 6.283185307179586;
    		double sinResultL = sin(sinePosL);
    		sinePosR += incR;
    		if (sinePosR > 6.283185307179586) sinePosR -= 6.283185307179586;
    		double sinResultR = sin(sinePosR);

    		double out = 0.0;
    		double snM = fabs(sinResultL)+(soar*soar);
    		double inM = fabs(inputSampleL);
    		if (inM < snM) {
    			inM = fabs(sinResultL);
    			snM = fabs(inputSampleL)+(soar*soar);
    		}

    		if (inputSampleL > 0.0 && sinResultL > 0.0) out = fmax((sqrt(inM/snM)*snM)-soar,0.0);
    		if (inputSampleL < 0.0 && sinResultL > 0.0) out = fmin((-sqrt(inM/snM)*snM)+soar,0.0);
    		if (inputSampleL > 0.0 && sinResultL < 0.0) out = fmin((-sqrt(inM/snM)*snM)+soar,0.0);
    		if (inputSampleL < 0.0 && sinResultL < 0.0) out = fmax((sqrt(inM/snM)*snM)-soar,0.0);
    		inputSampleL = out;

    		out = 0.0;
    		snM = fabs(sinResultR)+(soar*soar);
    		inM = fabs(inputSampleR);
    		if (inM < snM) {
    			inM = fabs(sinResultR);
    			snM = fabs(inputSampleR)+(soar*soar);
    		}

    		if (inputSampleR > 0.0 && sinResultR > 0.0) out = fmax((sqrt(inM/snM)*snM)-soar,0.0);
    		if (inputSampleR < 0.0 && sinResultR > 0.0) out = fmin((-sqrt(inM/snM)*snM)+soar,0.0);
    		if (inputSampleR > 0.0 && sinResultR < 0.0) out = fmin((-sqrt(inM/snM)*snM)+soar,0.0);
    		if (inputSampleR < 0.0 && sinResultR < 0.0) out = fmax((sqrt(inM/snM)*snM)-soar,0.0);
    		inputSampleR = out;

    		if (wet != 1.0) {
    			inputSampleL = (inputSampleL * wet) + (drySampleL * (1.0-wet));
    			inputSampleR = (inputSampleR * wet) + (drySampleR * (1.0-wet));
    		}
    		//Dry/Wet control, defaults to the last slider

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

    		in1++;
    		in2++;
    		out1++;
    		out2++;
        }
    }

private:
    double sampleRate_ = 44100.0;
    double sinePosL;
    double sinePosR;
    double incLA;
    double incLB;
    double incRA;
    double incRB;
    uint32_t fpdL;
    uint32_t fpdR;
    //default stuff
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#pragma once
// Porta do AutoPan do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/AutoPan). Copyright (c) Chris Johnson / airwindows.
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

class AutoPan {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=Rate, B=Phase, C=Wide, D=Dry/Wet)
    float A = 0.1f;
    float B = 0.5f;
    float C = 0.0f;
    float D = 1.0f;
    AutoPan() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        rate = 3.0;
        oldfpd = 429496.7295;
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

    	double drift = (pow(A,4)*0.01)/overallscale;
    	double offset = 3.141592653589793 + (B*6.283185307179586);
    	double panlaw = 1.0-pow(C,2);
    	double wet = D;

        while (--sampleFrames >= 0)
        {
    		double inputSampleL = *in1;
    		double inputSampleR = *in2;
    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;
    		double drySampleL = inputSampleL;
    		double drySampleR = inputSampleR;

    		rate += (oldfpd*drift);
    		if (rate > 6.283185307179586) {
    			rate = 0.0;
    			oldfpd = 0.4294967295+(fpdL*0.0000000000618);
    		} else if (rate < 0.0) {
    			rate = 6.283185307179586;
    			oldfpd = 0.4294967295+(fpdL*0.0000000000618);
    		}

    		inputSampleL *= (sin(rate)+1.0);
    		inputSampleR *= (sin(rate+offset)+1.0);

    		double mid = (inputSampleL + inputSampleR)*panlaw;
    		double side = inputSampleL - inputSampleR;
    		//assign mid and side.Between these sections, you can do mid/side processing

    		inputSampleL = (mid+side)/4.0;
    		inputSampleR = (mid-side)/4.0;
    		//unassign mid and side, and compensate for the sin()+1 volume boost

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

    		*in1++;
    		*in2++;
    		*out1++;
    		*out2++;
        }
    }

private:
    double sampleRate_ = 44100.0;
    double rate, oldfpd;
    uint32_t fpdL;
    uint32_t fpdR;
    //default stuff
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

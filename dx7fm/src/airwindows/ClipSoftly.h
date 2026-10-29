#pragma once
// Porta do ClipSoftly do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/ClipSoftly). Copyright (c) Chris Johnson / airwindows.
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

class ClipSoftly {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: )

    ClipSoftly() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        lastSampleL = 0.0;
        lastSampleR = 0.0;
        for (int x = 0; x < 16; x++) {intermediateL[x] = 0.0; intermediateR[x] = 0.0;}
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
    	int spacing = floor(overallscale); //should give us working basic scaling, usually 2 or 4
    	if (spacing < 1) spacing = 1; if (spacing > 16) spacing = 16;

        while (--sampleFrames >= 0)
        {
    		double inputSampleL = *in1;
    		double inputSampleR = *in2;
    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;

    		double softSpeed = fabs(inputSampleL);
    		if (softSpeed < 1.0) softSpeed = 1.0; else softSpeed = 1.0/softSpeed;
    		if (inputSampleL > 1.57079633) inputSampleL = 1.57079633;
    		if (inputSampleL < -1.57079633) inputSampleL = -1.57079633;
    		inputSampleL = sin(inputSampleL)*0.9549925859; //scale to what cliponly uses
    		inputSampleL = (inputSampleL*softSpeed)+(lastSampleL*(1.0-softSpeed));

    		softSpeed = fabs(inputSampleR);
    		if (softSpeed < 1.0) softSpeed = 1.0; else softSpeed = 1.0/softSpeed;
    		if (inputSampleR > 1.57079633) inputSampleR = 1.57079633;
    		if (inputSampleR < -1.57079633) inputSampleR = -1.57079633;
    		inputSampleR = sin(inputSampleR)*0.9549925859; //scale to what cliponly uses
    		inputSampleR = (inputSampleR*softSpeed)+(lastSampleR*(1.0-softSpeed));

    		intermediateL[spacing] = inputSampleL;
            inputSampleL = lastSampleL; //Latency is however many samples equals one 44.1k sample
    		for (int x = spacing; x > 0; x--) intermediateL[x-1] = intermediateL[x];
    		lastSampleL = intermediateL[0]; //run a little buffer to handle this

    		intermediateR[spacing] = inputSampleR;
            inputSampleR = lastSampleR; //Latency is however many samples equals one 44.1k sample
    		for (int x = spacing; x > 0; x--) intermediateR[x-1] = intermediateR[x];
    		lastSampleR = intermediateR[0]; //run a little buffer to handle this

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
    double lastSampleL;
    double intermediateL[16];
    double lastSampleR;
    double intermediateR[16];
    uint32_t fpdL;
    uint32_t fpdR;
    //default stuff
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

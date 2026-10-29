#pragma once
// Porta do Pockey2 do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/Pockey2). Copyright (c) Chris Johnson / airwindows.
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

class Pockey2 {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=DeFreq, B=DeRez, C=Dry/Wet)
    float A = 0.0f;
    float B = 0.66f;
    float C = 1.0f;
    Pockey2() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        lastSampleL = 0.0;
        heldSampleL = 0.0;
        previousHeldL = 0.0;
        lastSampleR = 0.0;
        heldSampleR = 0.0;
        previousHeldR = 0.0;
        position = 0.0;
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

    	int freq = floor(pow(A,3)*32.0*overallscale);
    	//dividing of derez must always be integer values now: no freq grinding

    	double rez = 4+(B*12.0);
    	//4 to 16, with 12 being the default.
    	int rezFactor = (int)pow(2,rez); //256, 4096, 65536 or anything in between

    	double wet = C;

        while (--sampleFrames >= 0)
        {
    		double inputSampleL = *in1;
    		double inputSampleR = *in2;
    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;
    		double drySampleL = inputSampleL;
    		double drySampleR = inputSampleR;

    		if (inputSampleL > 1.0) inputSampleL = 1.0; if (inputSampleL < -1.0) inputSampleL = -1.0;
    		if (inputSampleL > 0) inputSampleL = log(1.0+(255*fabs(inputSampleL)))/log(255);
    		if (inputSampleL < 0) inputSampleL = -log(1.0+(255*fabs(inputSampleL)))/log(255);

    		if (inputSampleR > 1.0) inputSampleR = 1.0; if (inputSampleR < -1.0) inputSampleR = -1.0;
    		if (inputSampleR > 0) inputSampleR = log(1.0+(255*fabs(inputSampleR)))/log(255);
    		if (inputSampleR < 0) inputSampleR = -log(1.0+(255*fabs(inputSampleR)))/log(255);
    		//end uLaw encode		

    		inputSampleL *= rezFactor;
    		inputSampleR *= rezFactor;
    		if (inputSampleL > 0) inputSampleL = floor(inputSampleL);
    		if (inputSampleL < 0) inputSampleL = -floor(-inputSampleL);
    		if (inputSampleR > 0) inputSampleR = floor(inputSampleR);
    		if (inputSampleR < 0) inputSampleR = -floor(-inputSampleR);
    		inputSampleL /= rezFactor;
    		inputSampleR /= rezFactor;

    		if (inputSampleL > 1.0) inputSampleL = 1.0; if (inputSampleL < -1.0) inputSampleL = -1.0;
    		if (inputSampleL > 0) inputSampleL = (pow(256,fabs(inputSampleL))-1.0) / 255;
    		if (inputSampleL < 0) inputSampleL = -(pow(256,fabs(inputSampleL))-1.0) / 255;

    		if (inputSampleR > 1.0) inputSampleR = 1.0; if (inputSampleR < -1.0) inputSampleR = -1.0;
    		if (inputSampleR > 0) inputSampleR = (pow(256,fabs(inputSampleR))-1.0) / 255;
    		if (inputSampleR < 0) inputSampleR = -(pow(256,fabs(inputSampleR))-1.0) / 255;
    		//end uLaw decode

    		double blurL = 0.618033988749894848204586-(fabs(inputSampleL - lastSampleL)*overallscale);
    		if (blurL < 0.0) blurL = 0.0;
    		double blurR = 0.618033988749894848204586-(fabs(inputSampleR - lastSampleR)*overallscale);
    		if (blurR < 0.0) blurR = 0.0; //reverse it. Mellow stuff gets blur, bright gets edge

    		if (position < 1)
    		{
    			position = freq; //one to ? scaled by overallscale
    			heldSampleL = inputSampleL;			
    			heldSampleR = inputSampleR;			
    		}
    		inputSampleL = heldSampleL;
    		inputSampleR = heldSampleR;
    		lastSampleL = drySampleL;
    		lastSampleR = drySampleR;
    		position--;

    		inputSampleL = (inputSampleL * blurL) + (previousHeldL * (1.0-blurL));
    		inputSampleR = (inputSampleR * blurR) + (previousHeldR * (1.0-blurR));
    		//conditional average: only if we actually have brightness
    		previousHeldL = heldSampleL;
    		previousHeldR = heldSampleR;
    		//end Frequency Derez

    		if (wet !=1.0) {
    			inputSampleL = (inputSampleL * wet) + (drySampleL * (1.0-wet));
    			inputSampleR = (inputSampleR * wet) + (drySampleR * (1.0-wet));
    		}

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
    double heldSampleL;
    double previousHeldL;
    double lastSampleR;
    double heldSampleR;
    double previousHeldR;
    double position;
    uint32_t fpdL;
    uint32_t fpdR;
    //default stuff
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

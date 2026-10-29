#pragma once
// Porta do Drive do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/Drive). Copyright (c) Chris Johnson / airwindows.
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

class Drive {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=Drive, B=Highpass, C=Out Level, D=Dry/Wet)
    float A = 0.0f;
    float B = 0.0f;
    float C = 1.0f;
    float D = 1.0f;
    Drive() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        iirSampleAL = 0.0;
        iirSampleBL = 0.0;
        iirSampleAR = 0.0;
        iirSampleBR = 0.0;
        fpFlip = true;
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

    	double driveone = pow(A*2.0,2);
    	double iirAmount = pow(B,3)/overallscale;
    	double output = C;
    	double wet = D;
    	double dry = 1.0-wet;
    	double glitch = 0.60;
    	double out;


    	double inputSampleL;
    	double inputSampleR;
    	double drySampleL;
    	double drySampleR;

        while (--sampleFrames >= 0)
        {
    		inputSampleL = *in1;
    		inputSampleR = *in2;
    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;
    		drySampleL = inputSampleL;
    		drySampleR = inputSampleR;

    		if (fpFlip)
    		{
    			iirSampleAL = (iirSampleAL * (1.0 - iirAmount)) + (inputSampleL * iirAmount);
    			inputSampleL -= iirSampleAL;
    			iirSampleAR = (iirSampleAR * (1.0 - iirAmount)) + (inputSampleR * iirAmount);
    			inputSampleR -= iirSampleAR;
    		}
    		else
    		{
    			iirSampleBL = (iirSampleBL * (1.0 - iirAmount)) + (inputSampleL * iirAmount);
    			inputSampleL -= iirSampleBL;
    			iirSampleBR = (iirSampleBR * (1.0 - iirAmount)) + (inputSampleR * iirAmount);
    			inputSampleR -= iirSampleBR;
    		}
    		//highpass section
    		fpFlip = !fpFlip;


    		if (inputSampleL > 1.0) inputSampleL = 1.0;
    		if (inputSampleL < -1.0) inputSampleL = -1.0;
    		if (inputSampleR > 1.0) inputSampleR = 1.0;
    		if (inputSampleR < -1.0) inputSampleR = -1.0;

    		out = driveone;
    		while (out > glitch)
    		{
    			out -= glitch;
    			inputSampleL -= (inputSampleL * (fabs(inputSampleL) * glitch) * (fabs(inputSampleL) * glitch) );
    			inputSampleL *= (1.0+glitch);
    			inputSampleR -= (inputSampleR * (fabs(inputSampleR) * glitch) * (fabs(inputSampleR) * glitch) );
    			inputSampleR *= (1.0+glitch);
    		}
    		//that's taken care of the really high gain stuff

    		inputSampleL -= (inputSampleL * (fabs(inputSampleL) * out) * (fabs(inputSampleL) * out) );
    		inputSampleL *= (1.0+out);
    		inputSampleR -= (inputSampleR * (fabs(inputSampleR) * out) * (fabs(inputSampleR) * out) );
    		inputSampleR *= (1.0+out);

    		if (output < 1.0) {
    			inputSampleL *= output;
    			inputSampleR *= output;
    		}
    		if (wet < 1.0) {
    			inputSampleL = (drySampleL * dry)+(inputSampleL * wet);
    			inputSampleR = (drySampleR * dry)+(inputSampleR * wet);
    		}
    		//nice little output stage template: if we have another scale of floating point
    		//number, we really don't want to meaninglessly multiply that by 1.0.

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
    uint32_t fpdL;
    uint32_t fpdR;
    //default stuff
    double iirSampleAL;
    double iirSampleBL;
    double iirSampleAR;
    double iirSampleBR;
    bool fpFlip;
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#pragma once
// Porta do Density2 do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/Density2). Copyright (c) Chris Johnson / airwindows.
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

class Density2 {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=Density, B=Highpass, C=Output, D=Dry/Wet)
    float A = 0.2f;
    float B = 0.0f;
    float C = 1.0f;
    float D = 1.0f;
    Density2() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        ataAL = ataBL = ataCL = lastDiffSampleL = 0.0;
        iirSampleAL = iirSampleBL = last3SampleL = last2SampleL = last1SampleL = 0.0;
        ataAR = ataBR = ataCR = lastDiffSampleR = 0.0;
        iirSampleAR = iirSampleBR = last3SampleR = last2SampleR = last1SampleR = 0.0;
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
    	double density = (A*5.0)-1.0;
    	double out = fabs(density);
    	while (out > 1.0) out = out - 1.0;
    	density = density * fabs(density);
    	double iirAmount = pow(B,3)/overallscale;
    	double output = C;
    	double wet = D;

        while (--sampleFrames >= 0)
        {
    		double inputSampleL = *in1;
    		double inputSampleR = *in2;
    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;
    		double drySampleL = inputSampleL;
    		double drySampleR = inputSampleR;

    		double halfwaySampleL = (inputSampleL + last1SampleL + ((-last2SampleL + last3SampleL) * 0.0414213562373095048801688)) / 2.0;
    		double halfDrySampleL = halfwaySampleL;

    		double halfwaySampleR = (inputSampleR + last1SampleR + ((-last2SampleR + last3SampleR) * 0.0414213562373095048801688)) / 2.0;
    		double halfDrySampleR = halfwaySampleR;

    		last3SampleL = last2SampleL; last2SampleL = last1SampleL; last1SampleL = inputSampleL;
    		last3SampleR = last2SampleR; last2SampleR = last1SampleR; last1SampleR = inputSampleR;

    		iirSampleBL = (iirSampleBL * (1.0 - iirAmount)) + (halfwaySampleL * iirAmount); halfwaySampleL -= iirSampleBL; //highpass section
    		iirSampleBR = (iirSampleBR * (1.0 - iirAmount)) + (halfwaySampleR * iirAmount); halfwaySampleR -= iirSampleBR; //highpass section

    		double bridgerectifier;

    		double count = density;
    		while (count > 1.0) {
    			bridgerectifier = fabs(halfwaySampleL)*1.57079633;
    			if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    			bridgerectifier = sin(bridgerectifier);
    			if (halfwaySampleL > 0.0) halfwaySampleL = bridgerectifier;
    			else halfwaySampleL = -bridgerectifier;
    			count = count - 1.0;
    		}
    		bridgerectifier = fabs(halfwaySampleL)*1.57079633;
    		if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    		if (density > 0) bridgerectifier = sin(bridgerectifier);
    		else bridgerectifier = 1-cos(bridgerectifier); //produce either boosted or starved version
    		if (halfwaySampleL > 0) halfwaySampleL = (halfwaySampleL*(1.0-out))+(bridgerectifier*out);
    		else halfwaySampleL = (halfwaySampleL*(1.0-out))-(bridgerectifier*out); //blend according to density control

    		count = density;
    		while (count > 1.0) {
    			bridgerectifier = fabs(halfwaySampleR)*1.57079633;
    			if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    			bridgerectifier = sin(bridgerectifier);
    			if (halfwaySampleR > 0.0) halfwaySampleR = bridgerectifier;
    			else halfwaySampleR = -bridgerectifier;
    			count = count - 1.0;
    		}
    		bridgerectifier = fabs(halfwaySampleR)*1.57079633;
    		if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    		if (density > 0) bridgerectifier = sin(bridgerectifier);
    		else bridgerectifier = 1-cos(bridgerectifier); //produce either boosted or starved version
    		if (halfwaySampleR > 0) halfwaySampleR = (halfwaySampleR*(1.0-out))+(bridgerectifier*out);
    		else halfwaySampleR = (halfwaySampleR*(1.0-out))-(bridgerectifier*out); //blend according to density control

    		ataCL = halfwaySampleL - halfDrySampleL;
    		ataAL *= 0.915965594177219015; ataBL *= 0.915965594177219015;
    		ataBL += ataCL; ataAL -= ataCL; ataCL = ataBL;
    		double halfDiffSampleL = ataCL * 0.915965594177219015;

    		ataCR = halfwaySampleR - halfDrySampleR;
    		ataAR *= 0.915965594177219015; ataBR *= 0.915965594177219015;
    		ataBR += ataCR; ataAR -= ataCR; ataCR = ataBR;
    		double halfDiffSampleR = ataCR * 0.915965594177219015;

    		iirSampleAL = (iirSampleAL * (1.0 - iirAmount)) + (inputSampleL * iirAmount); inputSampleL -= iirSampleAL; //highpass section
    		iirSampleAR = (iirSampleAR * (1.0 - iirAmount)) + (inputSampleR * iirAmount); inputSampleR -= iirSampleAR; //highpass section

    		count = density;
    		while (count > 1.0) {
    			bridgerectifier = fabs(inputSampleL)*1.57079633;
    			if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    			bridgerectifier = sin(bridgerectifier);
    			if (inputSampleL > 0.0) inputSampleL = bridgerectifier;
    			else inputSampleL = -bridgerectifier;
    			count = count - 1.0;
    		}
    		bridgerectifier = fabs(inputSampleL)*1.57079633;
    		if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    		if (density > 0) bridgerectifier = sin(bridgerectifier);
    		else bridgerectifier = 1-cos(bridgerectifier); //produce either boosted or starved version
    		if (inputSampleL > 0) inputSampleL = (inputSampleL*(1-out))+(bridgerectifier*out);
    		else inputSampleL = (inputSampleL*(1-out))-(bridgerectifier*out); //blend according to density control

    		count = density;
    		while (count > 1.0) {
    			bridgerectifier = fabs(inputSampleR)*1.57079633;
    			if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    			bridgerectifier = sin(bridgerectifier);
    			if (inputSampleR > 0.0) inputSampleR = bridgerectifier;
    			else inputSampleR = -bridgerectifier;
    			count = count - 1.0;
    		}
    		bridgerectifier = fabs(inputSampleR)*1.57079633;
    		if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    		if (density > 0) bridgerectifier = sin(bridgerectifier);
    		else bridgerectifier = 1-cos(bridgerectifier); //produce either boosted or starved version
    		if (inputSampleR > 0) inputSampleR = (inputSampleR*(1-out))+(bridgerectifier*out);
    		else inputSampleR = (inputSampleR*(1-out))-(bridgerectifier*out); //blend according to density control

    		ataCL = inputSampleL - drySampleL;
    		ataAL *= 0.915965594177219015; ataBL *= 0.915965594177219015;
    		ataAL += ataCL; ataBL -= ataCL; ataCL = ataAL;
    		double diffSampleL = ataCL * 0.915965594177219015; 

    		ataCR = inputSampleR - drySampleR;
    		ataAR *= 0.915965594177219015; ataBR *= 0.915965594177219015;
    		ataAR += ataCR; ataBR -= ataCR; ataCR = ataAR;
    		double diffSampleR = ataCR * 0.915965594177219015; 

    		inputSampleL = drySampleL + ((diffSampleL + halfDiffSampleL + lastDiffSampleL) / 1.187);
    		lastDiffSampleL = diffSampleL / 2.0;
    		inputSampleL *= output;
    		inputSampleL = (drySampleL*(1.0-wet))+(inputSampleL*wet);

    		inputSampleR = drySampleR + ((diffSampleR + halfDiffSampleR + lastDiffSampleR) / 1.187);
    		lastDiffSampleR = diffSampleR / 2.0;
    		inputSampleR *= output;
    		inputSampleR = (drySampleR*(1.0-wet))+(inputSampleR*wet);

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
    double last3SampleL;
    double last2SampleL;
    double last1SampleL;
    double ataAL;
    double ataBL;
    double ataCL;
    double lastDiffSampleL;
    double iirSampleAL;
    double iirSampleBL;
    double last3SampleR;
    double last2SampleR;
    double last1SampleR;
    double ataAR;
    double ataBR;
    double ataCR;
    double lastDiffSampleR;
    double iirSampleAR;
    double iirSampleBR;
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#pragma once
// Porta do Distortion do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/Distortion). Copyright (c) Chris Johnson / airwindows.
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

class Distortion {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=Input, B=Mode, C=Output, D=Dry/Wet)
    float A = 0.5f;
    float B = 0.5f;
    float C = 0.5f;
    float D = 1.0f;
    Distortion() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        for (int x = 0; x < 9; x++) {
        previousInL[x] = 0.0; previousOutL[x] = 0.0;
        previousInR[x] = 0.0; previousOutR[x] = 0.0;
        }
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

    	int stages = (int)floor(getSampleRate()/25000.0);
    	if (stages > 8) stages = 8;
    	double input = pow(10.0,((A-0.5)*24.0)/20.0);
    	int mode = (int) ( B * 4.999 );
    	double output = pow(10.0,((C-0.5)*24.0)/20.0);
    	double wet = D;

        while (--sampleFrames >= 0)
        {
    		double inputSampleL = *in1;
    		double inputSampleR = *in2;
    		if (fabs(inputSampleL)<1.18e-23) inputSampleL = fpdL * 1.18e-17;
    		if (fabs(inputSampleR)<1.18e-23) inputSampleR = fpdR * 1.18e-17;
    		double drySampleL = inputSampleL;
    		double drySampleR = inputSampleR;

    		inputSampleL *= input;
    		inputSampleR *= input;

    		for (int x = 0; x < stages; x++) {
    			double temp;
    			temp = (inputSampleL+previousInL[x])*0.5;
    			previousInL[x] = inputSampleL;
    			inputSampleL = temp;
    			temp = (inputSampleR+previousInR[x])*0.5;
    			previousInR[x] = inputSampleR;
    			inputSampleR = temp;
    		}

    		switch (mode)
    		{
    			case 0: //Density
    				if (inputSampleL > 1.570796326794897) inputSampleL = 1.570796326794897;
    				if (inputSampleL < -1.570796326794897) inputSampleL = -1.570796326794897;
    				if (inputSampleR > 1.570796326794897) inputSampleR = 1.570796326794897;
    				if (inputSampleR < -1.570796326794897) inputSampleR = -1.570796326794897;
    				//clip to 1.570796326794897 to reach maximum output
    				inputSampleL = sin(inputSampleL);
    				inputSampleR = sin(inputSampleR);
    				break;
    			case 1: //Drive				
    				if (inputSampleL > 1.0) inputSampleL = 1.0;
    				if (inputSampleL < -1.0) inputSampleL = -1.0;
    				if (inputSampleR > 1.0) inputSampleR = 1.0;
    				if (inputSampleR < -1.0) inputSampleR = -1.0;
    				inputSampleL -= (inputSampleL * (fabs(inputSampleL) * 0.6) * (fabs(inputSampleL) * 0.6));
    				inputSampleR -= (inputSampleR * (fabs(inputSampleR) * 0.6) * (fabs(inputSampleR) * 0.6));
    				inputSampleL *= 1.5;
    				inputSampleR *= 1.5;
    				break;
    			case 2: //Spiral
    				if (inputSampleL > 1.2533141373155) inputSampleL = 1.2533141373155;
    				if (inputSampleL < -1.2533141373155) inputSampleL = -1.2533141373155;
    				if (inputSampleR > 1.2533141373155) inputSampleR = 1.2533141373155;
    				if (inputSampleR < -1.2533141373155) inputSampleR = -1.2533141373155;
    				//clip to 1.2533141373155 to reach maximum output
    				inputSampleL = sin(inputSampleL * fabs(inputSampleL)) / ((fabs(inputSampleL) == 0.0) ?1:fabs(inputSampleL));
    				inputSampleR = sin(inputSampleR * fabs(inputSampleR)) / ((fabs(inputSampleR) == 0.0) ?1:fabs(inputSampleR));
    				break;
    			case 3: //Mojo
    				double mojo; mojo = pow(fabs(inputSampleL),0.25);
    				if (mojo > 0.0) inputSampleL = (sin(inputSampleL * mojo * M_PI * 0.5) / mojo) * 0.987654321;
    				mojo = pow(fabs(inputSampleR),0.25);
    				if (mojo > 0.0) inputSampleR = (sin(inputSampleR * mojo * M_PI * 0.5) / mojo) * 0.987654321;
    				//mojo is the one that flattens WAAAAY out very softly before wavefolding				
    				break;
    			case 4: //Dyno
    				double dyno; dyno = pow(fabs(inputSampleL),4);
    				if (dyno > 0.0) inputSampleL = (sin(inputSampleL * dyno) / dyno) * 1.1654321;
    				dyno = pow(fabs(inputSampleR),4);
    				if (dyno > 0.0) inputSampleR = (sin(inputSampleR * dyno) / dyno) * 1.1654321;
    				//dyno is the one that tries to raise peak energy				
    				break;
    		}				

    		for (int x = 1; x < (stages/2); x++) {
    			double temp;
    			temp = (inputSampleL+previousOutL[x])*0.5;
    			previousOutL[x] = inputSampleL;
    			inputSampleL = temp;
    			temp = (inputSampleR+previousOutR[x])*0.5;
    			previousOutR[x] = inputSampleR;
    			inputSampleR = temp;
    		}		

    		if (output != 1.0) {
    			inputSampleL *= output;
    			inputSampleR *= output;
    		}

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

    		*in1++;
    		*in2++;
    		*out1++;
    		*out2++;
        }
    }

private:
    double sampleRate_ = 44100.0;
    double previousInL[9];
    double previousOutL[9];
    double previousInR[9];
    double previousOutR[9];
    uint32_t fpdL;
    uint32_t fpdR;
    //default stuff
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

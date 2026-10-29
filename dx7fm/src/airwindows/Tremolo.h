#pragma once
// Porta do Tremolo do Airwindows (https://github.com/airwindows/airwindows,
// plugins/LinuxVST/src/Tremolo). Copyright (c) Chris Johnson / airwindows.
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

class Tremolo {
public:
    using VstInt32 = int32_t;
    // parametros 0..1 (nomes no plugin: A=Speed, B=Depth)
    float A = 0.5f;
    float B = 1.0f;
    Tremolo() { reset(); }
    void setSampleRate(double sr) { sampleRate_ = sr; }
    double getSampleRate() const { return sampleRate_; }
    // estado inicial (construtor original, sem os parametros)
    void reset() {
        sweep = 3.141592653589793238 / 2.0;
        speedChase = 0.0;
        depthChase = 0.0;
        speedAmount = 1.0;
        depthAmount = 0.0;
        lastSpeed = 1000.0;
        lastDepth = 1000.0;
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

    	speedChase = pow(A,4);
    	depthChase = B;
    	double speedSpeed = 300 / (fabs( lastSpeed - speedChase)+1.0);
    	double depthSpeed = 300 / (fabs( lastDepth - depthChase)+1.0);
    	lastSpeed = speedChase;
    	lastDepth = depthChase;

    	double speed;
    	double depth;
    	double skew;
    	double density;

    	double tupi = 3.141592653589793238;
    	double control;
    	double tempcontrol;
    	double thickness;
    	double out;
    	double bridgerectifier;
    	double offset;

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

    		speedAmount = (((speedAmount*speedSpeed)+speedChase)/(speedSpeed + 1.0));
    		depthAmount = (((depthAmount*depthSpeed)+depthChase)/(depthSpeed + 1.0));
    		speed = 0.0001+(speedAmount/1000.0);
    		speed /= overallscale;
    		depth = 1.0 - pow(1.0-depthAmount,5);
    		skew = 1.0+pow(depthAmount,9);
    		density = ((1.0-depthAmount)*2.0) - 1.0;		

    		offset = sin(sweep);
    		sweep += speed;
    		if (sweep > tupi){sweep -= tupi;}
    		control = fabs(offset);
    		if (density > 0)
    		{
    			tempcontrol = sin(control);
    			control = (control * (1.0-density))+(tempcontrol * density);
    		}
    		else
    		{
    			tempcontrol = 1-cos(control);
    			control = (control * (1.0+density))+(tempcontrol * -density);
    		}
    		//produce either boosted or starved version of control signal
    		//will go from 0 to 1

    		thickness = ((control * 2.0) - 1.0)*skew;
    		out = fabs(thickness);

    		//do L
    		bridgerectifier = fabs(inputSampleL);
    		if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    		//max value for sine function
    		if (thickness > 0) bridgerectifier = sin(bridgerectifier);
    		else bridgerectifier = 1-cos(bridgerectifier);
    		//produce either boosted or starved version
    		if (inputSampleL > 0) inputSampleL = (inputSampleL*(1-out))+(bridgerectifier*out);
    		else inputSampleL = (inputSampleL*(1-out))-(bridgerectifier*out);
    		//blend according to density control
    		inputSampleL *= (1.0 - control);
    		inputSampleL *= 2.0;
    		//apply tremolo, apply gain boost to compensate for volume loss
    		inputSampleL = (drySampleL * (1-depth)) + (inputSampleL*depth);
    		//end L

    		//do R
    		bridgerectifier = fabs(inputSampleR);
    		if (bridgerectifier > 1.57079633) bridgerectifier = 1.57079633;
    		//max value for sine function
    		if (thickness > 0) bridgerectifier = sin(bridgerectifier);
    		else bridgerectifier = 1-cos(bridgerectifier);
    		//produce either boosted or starved version
    		if (inputSampleR > 0) inputSampleR = (inputSampleR*(1-out))+(bridgerectifier*out);
    		else inputSampleR = (inputSampleR*(1-out))-(bridgerectifier*out);
    		//blend according to density control
    		inputSampleR *= (1.0 - control);
    		inputSampleR *= 2.0;
    		//apply tremolo, apply gain boost to compensate for volume loss
    		inputSampleR = (drySampleR * (1-depth)) + (inputSampleR*depth);
    		//end R

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
    double sweep;
    double speedChase;
    double depthChase;
    double speedAmount;
    double depthAmount;
    double lastSpeed;
    double lastDepth;
};

} // namespace dx7fm::airwindows

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

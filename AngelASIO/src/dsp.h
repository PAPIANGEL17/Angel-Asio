#pragma once
// "Chip DSP" virtual: cadena de monitoreo de fase minima, sin lookahead, sin oversampling
// (latencia algoritmica = 0 muestras). HPF -> EQ 3 bandas -> compresor -> (reverb solo monitoreo).
#include <cmath>
#include <vector>
#include <algorithm>
#include "config.h"

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void set(double B0, double B1, double B2, double A0, double A1, double A2) {
        b0 = (float)(B0 / A0); b1 = (float)(B1 / A0); b2 = (float)(B2 / A0);
        a1 = (float)(A1 / A0); a2 = (float)(A2 / A0);
    }
    void highpass(double fs, double f, double q) {
        double w = 2.0 * 3.14159265358979323846 * f / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void peak(double fs, double f, double q, double db) {
        double A = std::pow(10.0, db / 40.0), w = 2.0 * 3.14159265358979323846 * f / fs;
        double c = std::cos(w), al = std::sin(w) / (2.0 * q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowShelf(double fs, double f, double db) {
        double A = std::pow(10.0, db / 40.0), w = 2.0 * 3.14159265358979323846 * f / fs;
        double c = std::cos(w), al = std::sin(w) / 2.0 * std::sqrt(2.0), t = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * c + t), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - t),
            (A + 1) + (A - 1) * c + t, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - t);
    }
    void highShelf(double fs, double f, double db) {
        double A = std::pow(10.0, db / 40.0), w = 2.0 * 3.14159265358979323846 * f / fs;
        double c = std::cos(w), al = std::sin(w) / 2.0 * std::sqrt(2.0), t = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) + (A - 1) * c + t), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - t),
            (A + 1) - (A - 1) * c + t, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - t);
    }
};

struct Comb {
    std::vector<float> buf; size_t idx = 0; float store = 0;
    inline float process(float in, float fb, float damp) {
        float out = buf[idx];
        store = out * (1.f - damp) + store * damp;
        buf[idx] = in + store * fb;
        if (++idx >= buf.size()) idx = 0;
        return out;
    }
};
struct Allpass {
    std::vector<float> buf; size_t idx = 0;
    inline float process(float in) {
        float b = buf[idx];
        float out = -in + b;
        buf[idx] = in + b * 0.5f;
        if (++idx >= buf.size()) idx = 0;
        return out;
    }
};

class Reverb {
public:
    void init(double fs) {
        static const int cd[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int ad[4] = { 556, 441, 341, 225 };
        double k = fs / 44100.0;
        for (int ch = 0; ch < 2; ch++) {
            int spread = ch ? 23 : 0;
            for (int i = 0; i < 8; i++) { comb[ch][i].buf.assign((size_t)((cd[i] + spread) * k) + 1, 0.f); comb[ch][i].idx = 0; comb[ch][i].store = 0; }
            for (int i = 0; i < 4; i++) { ap[ch][i].buf.assign((size_t)((ad[i] + spread) * k) + 1, 0.f); ap[ch][i].idx = 0; }
        }
    }
    inline void process(float in, float& l, float& r) {
        float x = in * 0.015f, o[2];
        for (int ch = 0; ch < 2; ch++) {
            float s = 0;
            for (int i = 0; i < 8; i++) s += comb[ch][i].process(x, 0.84f, 0.2f);
            for (int i = 0; i < 4; i++) s = ap[ch][i].process(s);
            o[ch] = s * 3.f;
        }
        l = o[0]; r = o[1];
    }
private:
    Comb comb[2][8]; Allpass ap[2][4];
};

class MonitorDsp {
public:
    void configure(double fs, const Config& c) {
        if (fs != fs_) { fs_ = fs; reverb.init(fs); }
        useHpf = c.hpf;
        for (int ch = 0; ch < 2; ch++) {
            hpf[ch].highpass(fs, std::max(20.f, c.hpfHz), 0.7071);
            useLow = std::fabs(c.eqLowDb) > 0.05f;  lo[ch].lowShelf(fs, 120.0, c.eqLowDb);
            useMid = std::fabs(c.eqMidDb) > 0.05f;  mid[ch].peak(fs, 3500.0, 1.0, c.eqMidDb);
            useHigh = std::fabs(c.eqHighDb) > 0.05f; hi[ch].highShelf(fs, 10000.0, c.eqHighDb);
        }
        useComp = c.comp;
        thr = c.compThreshDb; ratio = std::max(1.f, c.compRatio);
        atk = coef(c.compAttackMs); rel = coef(c.compReleaseMs); makeup = c.compMakeupDb;
        monGain = std::pow(10.f, c.monitorGainDb * 0.05f);
        revMix = std::max(0.f, std::min(1.f, c.reverbMix));
    }
    // in: estereo intercalado. fx: senal procesada (sin reverb ni ganancia). mon: mezcla de monitoreo.
    void process(const float* in, float* fx, float* mon, int n) {
        for (int i = 0; i < n; i++) {
            float l = in[2 * i], r = in[2 * i + 1];
            if (useHpf) { l = hpf[0].process(l); r = hpf[1].process(r); }
            if (useLow)  { l = lo[0].process(l);  r = lo[1].process(r); }
            if (useMid)  { l = mid[0].process(l); r = mid[1].process(r); }
            if (useHigh) { l = hi[0].process(l);  r = hi[1].process(r); }
            if (useComp) {
                float pk = std::max(std::fabs(l), std::fabs(r));
                float db = 20.f * std::log10(pk + 1e-9f);
                float over = db - thr;
                float target = over > 0.f ? -over * (1.f - 1.f / ratio) : 0.f;
                float cf = target < gr ? atk : rel;
                gr = cf * gr + (1.f - cf) * target;
                float g = std::pow(10.f, (gr + makeup) * 0.05f);
                l *= g; r *= g;
            }
            fx[2 * i] = l; fx[2 * i + 1] = r;
            float wl = 0.f, wr = 0.f;
            if (revMix > 0.001f) reverb.process((l + r) * 0.5f, wl, wr);
            mon[2 * i]     = (l + wl * revMix) * monGain;
            mon[2 * i + 1] = (r + wr * revMix) * monGain;
        }
    }
private:
    float coef(float ms) const { return std::exp(-1.f / (float)(fs_ * std::max(0.1f, ms) * 0.001)); }
    double fs_ = 0;
    Biquad hpf[2], lo[2], mid[2], hi[2];
    Reverb reverb;
    bool useHpf = false, useLow = false, useMid = false, useHigh = false, useComp = false;
    float thr = -18, ratio = 3, atk = 0.99f, rel = 0.99f, makeup = 0, gr = 0, monGain = 1, revMix = 0;
};

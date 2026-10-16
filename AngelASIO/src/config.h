#pragma once
#include <string>

struct Config {
    std::wstring inputId, outputId;   // vacio = predeterminado del sistema
    int   bufferSize = 64;            // tamano de bloque ASIO preferido (frames)
    bool  exclusive = true;           // WASAPI exclusivo (menor latencia)
    float devicePeriodMs = 0.f;       // 0 = minimo del dispositivo

    bool  monitor = true;             // monitoreo directo con efectos (el "chip DSP")
    float monitorGainDb = 0.f;
    bool  recordFx = false;           // true = el DAW graba la senal con EQ/compresor
    bool  hpf = true;  float hpfHz = 80.f;
    bool  comp = true;
    float compThreshDb = -18.f, compRatio = 3.f, compAttackMs = 5.f, compReleaseMs = 80.f, compMakeupDb = 0.f;
    float eqLowDb = 0.f, eqMidDb = 0.f, eqHighDb = 0.f;
    float reverbMix = 0.f;            // solo monitoreo, nunca se graba

    void load();
    void save() const;
};
std::wstring angelConfigPath();

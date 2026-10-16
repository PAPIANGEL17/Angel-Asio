#include "config.h"
#include "log.h"
#include <cwchar>
#include <cstdlib>
#include <windows.h>

std::wstring angelConfigPath() { return angelDataDir() + L"\\config.ini"; }

static const wchar_t* S = L"AngelASIO";

static std::wstring getS(const std::wstring& path, const wchar_t* key) {
    wchar_t buf[512]; buf[0] = 0;
    GetPrivateProfileStringW(S, key, L"", buf, 512, path.c_str());
    return buf;
}
static float getF(const std::wstring& path, const wchar_t* key, float def) {
    std::wstring v = getS(path, key);
    if (v.empty()) return def;
    return (float)wcstod(v.c_str(), nullptr);
}
static bool getB(const std::wstring& path, const wchar_t* key, bool def) {
    std::wstring v = getS(path, key);
    if (v.empty()) return def;
    return v[0] == L'1' || v[0] == L't' || v[0] == L'T';
}
static void putS(const std::wstring& path, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(S, key, v.c_str(), path.c_str());
}
static void putF(const std::wstring& path, const wchar_t* key, float v) {
    wchar_t b[64]; swprintf(b, 64, L"%.3f", v); putS(path, key, b);
}
static void putB(const std::wstring& path, const wchar_t* key, bool v) { putS(path, key, v ? L"1" : L"0"); }

void Config::load() {
    std::wstring p = angelConfigPath();
    inputId = getS(p, L"input_id");
    outputId = getS(p, L"output_id");
    bufferSize = (int)getF(p, L"buffer_size", 64);
    if (bufferSize < 32) bufferSize = 32;
    if (bufferSize > 2048) bufferSize = 2048;
    { int b = 32; while (b < bufferSize) b <<= 1; bufferSize = b; }
    exclusive = getB(p, L"exclusive", true);
    devicePeriodMs = getF(p, L"device_period_ms", 0.f);
    monitor = getB(p, L"monitor", true);
    monitorGainDb = getF(p, L"monitor_gain_db", 0.f);
    recordFx = getB(p, L"record_fx", false);
    hpf = getB(p, L"hpf", true);
    hpfHz = getF(p, L"hpf_hz", 80.f);
    comp = getB(p, L"comp", true);
    compThreshDb = getF(p, L"comp_threshold_db", -18.f);
    compRatio = getF(p, L"comp_ratio", 3.f);
    compAttackMs = getF(p, L"comp_attack_ms", 5.f);
    compReleaseMs = getF(p, L"comp_release_ms", 80.f);
    compMakeupDb = getF(p, L"comp_makeup_db", 0.f);
    eqLowDb = getF(p, L"eq_low_db", 0.f);
    eqMidDb = getF(p, L"eq_mid_db", 0.f);
    eqHighDb = getF(p, L"eq_high_db", 0.f);
    reverbMix = getF(p, L"reverb_mix", 0.f);
}

void Config::save() const {
    std::wstring p = angelConfigPath();
    putS(p, L"input_id", inputId);
    putS(p, L"output_id", outputId);
    putF(p, L"buffer_size", (float)bufferSize);
    putB(p, L"exclusive", exclusive);
    putF(p, L"device_period_ms", devicePeriodMs);
    putB(p, L"monitor", monitor);
    putF(p, L"monitor_gain_db", monitorGainDb);
    putB(p, L"record_fx", recordFx);
    putB(p, L"hpf", hpf);
    putF(p, L"hpf_hz", hpfHz);
    putB(p, L"comp", comp);
    putF(p, L"comp_threshold_db", compThreshDb);
    putF(p, L"comp_ratio", compRatio);
    putF(p, L"comp_attack_ms", compAttackMs);
    putF(p, L"comp_release_ms", compReleaseMs);
    putF(p, L"comp_makeup_db", compMakeupDb);
    putF(p, L"eq_low_db", eqLowDb);
    putF(p, L"eq_mid_db", eqMidDb);
    putF(p, L"eq_high_db", eqHighDb);
    putF(p, L"reverb_mix", reverbMix);
}

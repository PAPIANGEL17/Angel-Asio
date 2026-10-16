// Prueba de humo: carga el driver como lo haria un DAW (directo por DLL o via COM/registro),
// crea buffers, arranca, cuenta callbacks, verifica posicion de muestras y detiene.
// Uso: asio_smoke_test.exe <ruta\AngelASIO.dll>   |   asio_smoke_test.exe --com
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../src/asio_defs.h"

static const GUID kClsid = { 0x6e1f4a52, 0x8c3b, 0x4d27, { 0x9a, 0x5e, 0x1b, 0x7d, 0x03, 0xc4, 0xf8, 0x62 } };

static std::atomic<long> g_calls{ 0 }, g_badIdx{ 0 }, g_badTime{ 0 };
static std::atomic<long> g_expectIdx{ 0 };
static void* g_out[2][2];
static long g_bufSize = 0;
static bool g_timeInfo = true;
static double g_rate = 48000.0;
static std::atomic<unsigned long long> g_lastPos{ 0 };
static double g_phase = 0.0;

static void fillOutputs(long idx) {
    for (int ch = 0; ch < 2; ch++) {
        int* d = (int*)g_out[ch][idx];
        for (long i = 0; i < g_bufSize; i++) {
            d[i] = (int)(std::sin(g_phase) * 0.1 * 2147483647.0);
            g_phase += 2.0 * 3.14159265358979 * 440.0 / g_rate;
        }
    }
}
static void checkIdx(long idx) {
    long e = g_expectIdx.load();
    if (idx != e) g_badIdx++;
    g_expectIdx = idx ^ 1;
}
static void bufferSwitch(long idx, ASIOBool) { g_calls++; checkIdx(idx); fillOutputs(idx); }
static ASIOTime* bufferSwitchTimeInfo(ASIOTime* t, long idx, ASIOBool) {
    g_calls++; checkIdx(idx);
    unsigned long long pos = ((unsigned long long)t->timeInfo.samplePosition.hi << 32) | t->timeInfo.samplePosition.lo;
    if (!(t->timeInfo.flags & kSamplePositionValid) || t->timeInfo.sampleRate != g_rate || pos < g_lastPos.load()) g_badTime++;
    g_lastPos = pos;
    fillOutputs(idx);
    return nullptr;
}
static void rateChanged(ASIOSampleRate) {}
static long asioMessage(long sel, long value, void*, double*) {
    switch (sel) {
    case kAsioSelectorSupported: return (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo) ? 1 : 0;
    case kAsioEngineVersion: return 2;
    case kAsioSupportsTimeInfo: return g_timeInfo ? 1 : 0;
    }
    return 0;
}

#define CHECK(cond, msg) do { if (!(cond)) { printf("FALLO: %s (linea %d)\n", msg, __LINE__); return false; } } while (0)

static bool runOnce(IASIO* d, bool timeInfo) {
    g_timeInfo = timeInfo; g_calls = 0; g_badIdx = 0; g_badTime = 0; g_expectIdx = 0; g_lastPos = 0; g_phase = 0;
    char name[64] = {};
    CHECK(d->init(nullptr) == ASIOTrue, "init");
    d->getDriverName(name);
    printf("  driver: %s v%ld\n", name, d->getDriverVersion());
    CHECK(strcmp(name, "AMR Angel ASIO") == 0, "nombre");
    long in = 0, out = 0;
    CHECK(d->getChannels(&in, &out) == ASE_OK && in == 2 && out == 2, "canales 2/2");
    long mn, mx, pref, gran;
    CHECK(d->getBufferSize(&mn, &mx, &pref, &gran) == ASE_OK && mn <= pref && pref <= mx, "buffer sizes");
    printf("  buffers: min=%ld max=%ld pref=%ld gran=%ld\n", mn, mx, pref, gran);
    CHECK(d->canSampleRate(48000.0) == ASE_OK, "48k soportado");
    CHECK(d->canSampleRate(12345.0) != ASE_OK, "12345 no soportado");
    CHECK(d->setSampleRate(48000.0) == ASE_OK, "setSampleRate");
    double sr = 0; CHECK(d->getSampleRate(&sr) == ASE_OK && sr == 48000.0, "getSampleRate");
    g_rate = sr;
    ASIOChannelInfo ci = {}; ci.channel = 0; ci.isInput = ASIOTrue;
    CHECK(d->getChannelInfo(&ci) == ASE_OK && ci.type == ASIOSTInt32LSB, "channel info");

    ASIOBufferInfo bi[4] = {};
    bi[0].isInput = ASIOTrue;  bi[0].channelNum = 0;
    bi[1].isInput = ASIOTrue;  bi[1].channelNum = 1;
    bi[2].isInput = ASIOFalse; bi[2].channelNum = 0;
    bi[3].isInput = ASIOFalse; bi[3].channelNum = 1;
    ASIOCallbacks cb = { bufferSwitch, rateChanged, asioMessage, bufferSwitchTimeInfo };
    g_bufSize = 128;
    ASIOError e = d->createBuffers(bi, 4, g_bufSize, &cb);
    if (e != ASE_OK) { char m[128] = {}; d->getErrorMessage(m); printf("FALLO: createBuffers=%ld (%s)\n", e, m); return false; }
    for (int i = 0; i < 2; i++) for (int k = 0; k < 2; k++) g_out[i][k] = bi[2 + i].buffers[k];
    CHECK(bi[0].buffers[0] && bi[0].buffers[1] && bi[0].buffers[0] != bi[0].buffers[1], "punteros de buffer");
    long il = 0, ol = 0; CHECK(d->getLatencies(&il, &ol) == ASE_OK, "latencias");
    printf("  latencias reportadas: in=%ld out=%ld frames (%.2f / %.2f ms)\n", il, ol, il * 1000.0 / sr, ol * 1000.0 / sr);

    for (int round = 0; round < 2; round++) {   // arranque, parada y re-arranque
        g_calls = 0; g_badIdx = 0; g_expectIdx = 0; g_lastPos = 0;
        CHECK(d->start() == ASE_OK, "start");
        Sleep(1000);
        ASIOSamples sp; ASIOTimeStamp ts;
        CHECK(d->getSamplePosition(&sp, &ts) == ASE_OK, "getSamplePosition");
        unsigned long long pos = ((unsigned long long)sp.hi << 32) | sp.lo;
        CHECK(d->stop() == ASE_OK, "stop");
        long calls = g_calls.load();
        double expected = sr / g_bufSize;
        printf("  ronda %d: %ld callbacks en ~1 s (esperados ~%.0f), posicion=%llu, idx incorrectos=%ld, tiempo incorrecto=%ld\n",
               round + 1, calls, expected, pos, g_badIdx.load(), g_badTime.load());
        CHECK(calls >= expected * 0.5 && calls <= expected * 1.6, "cantidad de callbacks fuera de rango");
        CHECK(g_badIdx.load() == 0, "indices de doble buffer alternan");
        CHECK(g_badTime.load() == 0, "informacion de tiempo coherente");
        CHECK(pos > 0, "posicion avanza");
    }
    CHECK(d->disposeBuffers() == ASE_OK, "disposeBuffers");
    return true;
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool viaCom = argc > 1 && strcmp(argv[1], "--com") == 0;
    if (!viaCom && argc < 2) { printf("uso: asio_smoke_test <dll> | --com\n"); return 2; }

    for (int pass = 0; pass < 2; pass++) {
        IASIO* d = nullptr;
        if (viaCom) {
            HRESULT hr = CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kClsid, (void**)&d);
            if (FAILED(hr) || !d) { printf("FALLO: CoCreateInstance hr=0x%08lx (esta registrado el driver?)\n", (unsigned long)hr); return 1; }
        } else {
            HMODULE m = LoadLibraryA(argv[1]);
            if (!m) { printf("FALLO: LoadLibrary %lu\n", GetLastError()); return 1; }
            typedef HRESULT(WINAPI * GCO)(REFCLSID, REFIID, LPVOID*);
            GCO gco = (GCO)GetProcAddress(m, "DllGetClassObject");
            if (!gco) { printf("FALLO: sin DllGetClassObject\n"); return 1; }
            IClassFactory* f = nullptr;
            if (FAILED(gco(kClsid, IID_IClassFactory, (void**)&f)) || !f) { printf("FALLO: DllGetClassObject\n"); return 1; }
            HRESULT hr = f->CreateInstance(nullptr, kClsid, (void**)&d);
            f->Release();
            if (FAILED(hr) || !d) { printf("FALLO: CreateInstance hr=0x%08lx\n", (unsigned long)hr); return 1; }
        }
        printf("Pasada %d (%s, %s)\n", pass + 1, viaCom ? "COM/registro" : "DLL directa", pass == 0 ? "bufferSwitchTimeInfo" : "bufferSwitch clasico");
        bool ok = runOnce(d, pass == 0);
        d->Release();
        if (!ok) return 1;
    }
    printf("TODO OK\n");
    return 0;
}

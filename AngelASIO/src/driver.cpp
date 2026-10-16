#include "common.h"
#include "engine.h"
#include "panel.h"
#include "log.h"
#include <atomic>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <algorithm>

namespace {

const long kMinBuf = 32, kMaxBuf = 2048;

inline int32_t f2i(float x) {
    if (x > 1.f) x = 1.f; else if (x < -1.f) x = -1.f;
    return (int32_t)((double)x * 2147483647.0);
}

inline unsigned long long nowNs() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    unsigned long long q = (unsigned long long)c.QuadPart, fq = (unsigned long long)f.QuadPart;
    return (q / fq) * 1000000000ULL + (q % fq) * 1000000000ULL / fq;
}

class AngelAsio final : public IASIO {
public:
    AngelAsio() : refs(1) { InterlockedIncrement(&g_objCount); handler.owner = this; }
    ~AngelAsio() { disposeBuffers(); InterlockedDecrement(&g_objCount); }

    // --- IUnknown (ASIO usa el CLSID como IID)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, CLSID_AngelAsio)) {
            *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // --- IASIO
    ASIOBool init(void* sysHandle) override {
        hwnd = (HWND)sysHandle;
        cfg.load();
        err = "";
        return ASIOTrue;
    }
    void getDriverName(char* name) override { strcpy(name, ANGEL_DRIVER_NAME); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* s) override { strncpy(s, err.c_str(), 123); s[123] = 0; }

    ASIOError start() override {
        if (!engine) return ASE_NotPresent;
        if (running) return ASE_OK;
        curIdx = 0; samplePos = 0;
        std::string e;
        if (!engine->start(e)) { err = e; angelLog("start fallo: %s", e.c_str()); return ASE_HWMalfunction; }
        running = true;
        return ASE_OK;
    }
    ASIOError stop() override {
        if (engine && running) engine->stop();
        running = false;
        return ASE_OK;
    }
    ASIOError getChannels(long* in, long* out) override {
        if (!in || !out) return ASE_InvalidParameter;
        *in = 2; *out = 2; return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        if (!in || !out) return ASE_InvalidParameter;
        if (engine) { *in = engine->inputLatency(); *out = engine->outputLatency(); }
        else { *in = cfg.bufferSize * 2; *out = cfg.bufferSize * 2; }
        return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        if (!mn || !mx || !pref || !gran) return ASE_InvalidParameter;
        *mn = kMinBuf; *mx = kMaxBuf; *pref = cfg.bufferSize; *gran = -1;
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        static const double ok[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
        for (double d : ok) if (std::fabs(r - d) < 0.5) return ASE_OK;
        return ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override { if (!r) return ASE_InvalidParameter; *r = rate; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (canSampleRate(r) != ASE_OK) return ASE_NoClock;
        if (std::fabs(r - rate) < 0.5) return ASE_OK;
        if (running) return ASE_InvalidMode;
        rate = r;
        if (engine && bufferSize > 0) {   // reabrir el motor con la nueva frecuencia
            engine->close();
            std::string e;
            Config c = cfg; c.load();
            if (!engine->open(rate, (int)bufferSize, c, &handler, e)) { err = e; return ASE_HWMalfunction; }
        }
        if (cb && cb->sampleRateDidChange) cb->sampleRateDidChange(rate);
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override {
        if (!c || !n || *n < 1) return ASE_InvalidParameter;
        c->index = 0; c->associatedChannel = -1; c->associatedGroup = -1; c->isCurrentSource = ASIOTrue;
        strcpy(c->name, "Interno"); *n = 1; return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* sp, ASIOTimeStamp* ts) override {
        if (!sp || !ts) return ASE_InvalidParameter;
        if (!running) return ASE_SPNotAdvancing;
        unsigned long long p = samplePos.load(), t = nowNs();
        sp->hi = (unsigned long)(p >> 32); sp->lo = (unsigned long)(p & 0xffffffffULL);
        ts->hi = (unsigned long)(t >> 32); ts->lo = (unsigned long)(t & 0xffffffffULL);
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info) return ASE_InvalidParameter;
        if (info->channel < 0 || info->channel > 1) return ASE_InvalidParameter;
        info->channelGroup = 0; info->type = ASIOSTInt32LSB;
        bool in = info->isInput != 0;
        info->isActive = (in ? inBuf[info->channel][0] : outBuf[info->channel][0]) ? ASIOTrue : ASIOFalse;
        snprintf(info->name, sizeof info->name, "%s %ld", in ? "Entrada" : "Salida", info->channel + 1);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long num, long size, ASIOCallbacks* callbacks) override {
        if (!infos || num < 1 || !callbacks || !callbacks->bufferSwitch) return ASE_InvalidParameter;
        if (size < kMinBuf || size > kMaxBuf) return ASE_InvalidMode;
        for (long i = 0; i < num; i++) {
            if (infos[i].channelNum < 0 || infos[i].channelNum > 1) return ASE_InvalidMode;
        }
        disposeBuffers();
        size_t per = (size_t)size * sizeof(int32_t);
        mem = (unsigned char*)_aligned_malloc(per * 8, 64);
        if (!mem) return ASE_NoMemory;
        memset(mem, 0, per * 8);
        memset(inBuf, 0, sizeof inBuf); memset(outBuf, 0, sizeof outBuf);
        for (long i = 0; i < num; i++) {
            long ch = infos[i].channelNum;
            void** slot = infos[i].isInput ? inBuf[ch] : outBuf[ch];
            size_t base = ((infos[i].isInput ? 0 : 4) + ch * 2) * per;
            slot[0] = mem + base; slot[1] = mem + base + per;
            infos[i].buffers[0] = slot[0]; infos[i].buffers[1] = slot[1];
        }
        cb = callbacks; bufferSize = size;
        useTimeInfo = false;
        if (cb->asioMessage && cb->bufferSwitchTimeInfo) useTimeInfo = cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;

        Config c; c.load(); cfg = c;
        engine = createEngine();
        std::string e;
        if (!engine->open(rate, (int)size, c, &handler, e)) {
            err = e; angelLog("open fallo: %s", e.c_str());
            engine.reset(); freeBuffers();
            return ASE_HWMalfunction;
        }
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        if (engine) { engine->stop(); running = false; engine->close(); engine.reset(); }
        freeBuffers();
        cb = nullptr; bufferSize = 0;
        return ASE_OK;
    }
    ASIOError controlPanel() override {
        Config c; c.load();
        Config before = c;
        std::string st = engine ? engine->describe() : "Motor detenido (se abre cuando el DAW inicia el audio)";
        if (showPanel(hwnd, st, c)) {
            cfg = c;
            if (engine) engine->updateConfig(c);
            bool needReset = c.inputId != before.inputId || c.outputId != before.outputId || c.bufferSize != before.bufferSize ||
                             c.exclusive != before.exclusive || c.devicePeriodMs != before.devicePeriodMs;
            if (needReset && cb && cb->asioMessage) cb->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
        return ASE_OK;
    }
    ASIOError future(long selector, void*) override { return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_InvalidParameter; }
    ASIOError outputReady() override { return ASE_NotPresent; }

private:
    struct Handler : BlockHandler {
        AngelAsio* owner = nullptr;
        void onBlock(const float* in, float* out, int n) override { owner->block(in, out, n); }
        void onReset() override {
            AngelAsio* o = owner;
            if (o->cb && o->cb->asioMessage) o->cb->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
    };

    void freeBuffers() {
        if (mem) { _aligned_free(mem); mem = nullptr; }
        memset(inBuf, 0, sizeof inBuf); memset(outBuf, 0, sizeof outBuf);
    }

    // Hilo de audio del motor -> convierte, llama al host y recoge la salida.
    void block(const float* in, float* out, int n) {
        ASIOCallbacks* c = cb;
        if (!c) { std::fill(out, out + 2 * n, 0.f); return; }
        long idx = curIdx;
        for (int ch = 0; ch < 2; ch++) {
            if (!inBuf[ch][0]) continue;
            int32_t* d = (int32_t*)inBuf[ch][idx];
            for (int i = 0; i < n; i++) d[i] = f2i(in[2 * i + ch]);
        }
        unsigned long long pos = samplePos.load();
        if (useTimeInfo && c->bufferSwitchTimeInfo) {
            ASIOTime t; memset(&t, 0, sizeof t);
            unsigned long long ns = nowNs();
            t.timeInfo.speed = 1.0;
            t.timeInfo.systemTime.hi = (unsigned long)(ns >> 32); t.timeInfo.systemTime.lo = (unsigned long)(ns & 0xffffffffULL);
            t.timeInfo.samplePosition.hi = (unsigned long)(pos >> 32); t.timeInfo.samplePosition.lo = (unsigned long)(pos & 0xffffffffULL);
            t.timeInfo.sampleRate = rate;
            t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
            c->bufferSwitchTimeInfo(&t, idx, ASIOTrue);
        } else {
            c->bufferSwitch(idx, ASIOTrue);
        }
        std::fill(out, out + 2 * n, 0.f);
        for (int ch = 0; ch < 2; ch++) {
            if (!outBuf[ch][0]) continue;
            const int32_t* s = (const int32_t*)outBuf[ch][idx];
            for (int i = 0; i < n; i++) out[2 * i + ch] = (float)s[i] * (1.0f / 2147483648.0f);
        }
        curIdx = idx ^ 1;
        samplePos.store(pos + (unsigned long long)n);
    }

    LONG refs;
    Handler handler;
    HWND hwnd = nullptr;
    Config cfg;
    std::unique_ptr<Engine> engine;
    std::string err;
    double rate = 48000.0;
    long bufferSize = 0;
    ASIOCallbacks* cb = nullptr;
    bool useTimeInfo = false;
    bool running = false;
    unsigned char* mem = nullptr;
    void* inBuf[2][2] = {};
    void* outBuf[2][2] = {};
    long curIdx = 0;
    std::atomic<unsigned long long> samplePos{ 0 };
};

} // namespace

IASIO* angelCreateDriver() { return new AngelAsio(); }

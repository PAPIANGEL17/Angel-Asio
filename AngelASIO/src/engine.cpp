#include "engine.h"
#include "dsp.h"
#include "log.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#define ANGEL_X86 1
#endif

#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// ---------------------------------------------------------------- utilidades COM
template <class T> struct Ptr {
    T* p = nullptr;
    Ptr() {}
    ~Ptr() { reset(); }
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct ComInit {
    bool ok;
    ComInit() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComInit() { if (ok) CoUninitialize(); }
};

const GUID kSubPCM   = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID kSubFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const PROPERTYKEY kPkeyFriendly = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// ---------------------------------------------------------------- formato de muestras
struct SampleFmt {
    int bytes = 4, channels = 2, blockAlign = 8, validBits = 32;
    bool isFloat = true;
};

inline float readSample(const BYTE* p, const SampleFmt& f) {
    if (f.isFloat) { float v; memcpy(&v, p, 4); return v; }
    switch (f.bytes) {
    case 2: { int16_t v; memcpy(&v, p, 2); return v * (1.f / 32768.f); }
    case 3: { int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)); return v * (1.f / 2147483648.f); }
    case 4: { int32_t v; memcpy(&v, p, 4); return v * (1.f / 2147483648.f); }
    }
    return 0.f;
}
inline void writeSample(BYTE* p, float x, const SampleFmt& f) {
    if (x > 1.f) x = 1.f; else if (x < -1.f) x = -1.f;
    if (f.isFloat) { memcpy(p, &x, 4); return; }
    switch (f.bytes) {
    case 2: { int16_t v = (int16_t)std::lrintf(x * 32767.f); memcpy(p, &v, 2); break; }
    case 3: { int32_t v = (int32_t)std::lrintf(x * 8388607.f); p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break; }
    case 4: { int32_t v = (int32_t)((double)x * 2147483647.0); memcpy(p, &v, 4); break; }
    }
}
void toStereo(const BYTE* src, UINT32 n, const SampleFmt& f, float* dst) {
    for (UINT32 i = 0; i < n; i++) {
        const BYTE* fr = src + (size_t)i * f.blockAlign;
        float l = readSample(fr, f);
        float r = f.channels > 1 ? readSample(fr + f.bytes, f) : l;
        dst[2 * i] = l; dst[2 * i + 1] = r;
    }
}
void fromStereo(const float* src, UINT32 n, const SampleFmt& f, BYTE* dst) {
    for (UINT32 i = 0; i < n; i++) {
        BYTE* fr = dst + (size_t)i * f.blockAlign;
        memset(fr, 0, f.blockAlign);
        float l = src[2 * i], r = src[2 * i + 1];
        if (f.channels == 1) writeSample(fr, (l + r) * 0.5f, f);
        else { writeSample(fr, l, f); writeSample(fr + f.bytes, r, f); }
    }
}
SampleFmt fmtFromWfx(const WAVEFORMATEX* w) {
    SampleFmt f;
    f.channels = w->nChannels; f.bytes = w->wBitsPerSample / 8; f.blockAlign = w->nBlockAlign; f.validBits = w->wBitsPerSample;
    bool flt = (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* e = (const WAVEFORMATEXTENSIBLE*)w;
        flt = IsEqualGUID(e->SubFormat, kSubFloat) != 0;
        f.validBits = e->Samples.wValidBitsPerSample;
    }
    f.isFloat = flt;
    return f;
}
void makeWfx(WAVEFORMATEXTENSIBLE& wf, int ch, int bits, int valid, bool flt, double rate) {
    memset(&wf, 0, sizeof wf);
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = (WORD)ch;
    wf.Format.nSamplesPerSec = (DWORD)rate;
    wf.Format.wBitsPerSample = (WORD)bits;
    wf.Format.nBlockAlign = (WORD)(ch * bits / 8);
    wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.Format.cbSize = 22;
    wf.Samples.wValidBitsPerSample = (WORD)valid;
    wf.dwChannelMask = ch == 1 ? 0x4 : (ch == 2 ? 0x3 : 0);
    wf.SubFormat = flt ? kSubFloat : kSubPCM;
}

// ---------------------------------------------------------------- anillo estereo (un solo hilo)
class Ring {
public:
    void init(size_t frames) { cap = 1; while (cap < frames) cap <<= 1; buf.assign(cap * 2, 0.f); head = tail = 0; }
    void clear() { head = tail = 0; }
    size_t level() const { return tail - head; }
    void drop(size_t n) { head += std::min(n, level()); }
    void push(const float* s, size_t n) {
        if (n > cap) { s += (n - cap) * 2; n = cap; }
        if (level() + n > cap) drop(level() + n - cap);
        for (size_t i = 0; i < n; i++) { size_t k = ((tail + i) & (cap - 1)) * 2; buf[k] = s[2 * i]; buf[k + 1] = s[2 * i + 1]; }
        tail += n;
    }
    void pushZeros(size_t n) {
        if (n > cap) n = cap;
        if (level() + n > cap) drop(level() + n - cap);
        for (size_t i = 0; i < n; i++) { size_t k = ((tail + i) & (cap - 1)) * 2; buf[k] = 0.f; buf[k + 1] = 0.f; }
        tail += n;
    }
    size_t pop(float* d, size_t n) {
        n = std::min(n, level());
        for (size_t i = 0; i < n; i++) { size_t k = ((head + i) & (cap - 1)) * 2; d[2 * i] = buf[k]; d[2 * i + 1] = buf[k + 1]; }
        head += n;
        return n;
    }
private:
    std::vector<float> buf; size_t cap = 1, head = 0, tail = 0;
};

// ---------------------------------------------------------------- nucleo: bloques ASIO + monitoreo
// Todo ocurre en el hilo de audio del motor: sin locks ni memoria dinamica en el camino critico.
class Core {
public:
    void init(int block, double fs, BlockHandler* h, bool hasCapture, int capPeriod, const Config& cfg) {
        B = block; fs_ = fs; handler = h; hasCap = hasCapture; capP = capPeriod;
        inRing.init(32768); outRing.init(32768); monRing.init(32768);
        inBlock.assign((size_t)B * 2, 0.f); outBlock.assign((size_t)B * 2, 0.f);
        fxTmp.assign(16384 * 2, 0.f); monTmp.assign(16384 * 2, 0.f);
        apply(cfg);
        reset();
    }
    void reset() { inRing.clear(); outRing.clear(); monRing.clear(); inRing.pushZeros(B); xruns = 0; }
    void updateConfig(const Config& c) {
        std::lock_guard<std::mutex> g(pm);
        pending = c; pendingGen++;
    }
    long xrunCount() const { return xruns; }

    void pushCapture(const float* in, int frames) {
        if (!hasCap || frames <= 0) return;
        poll();
        if (frames > 16384) frames = 16384;
        const float* toRing = in;
        if (monitor || recordFx) {
            dsp.process(in, fxTmp.data(), monTmp.data(), frames);
            if (recordFx) toRing = fxTmp.data();
            if (monitor) monRing.push(monTmp.data(), frames);
        }
        inRing.push(toRing, frames);
        size_t hi = (size_t)(4 * B + 2 * capP + 1024), lo = (size_t)(2 * B + capP);
        if (inRing.level() > hi) inRing.drop(inRing.level() - lo);
    }

    // Llena "frames" de salida: bloques ASIO bajo demanda + monitoreo directo sumado.
    void render(int frames, float* out) {
        poll();
        if (frames > 16384) frames = 16384;
        while (outRing.level() < (size_t)frames) produceBlock();
        outRing.pop(out, frames);
        if (monitor) {
            size_t n = std::min<size_t>((size_t)frames, monRing.level());
            if (n) {
                monRing.pop(monTmp.data(), n);
                for (size_t i = 0; i < n * 2; i++) out[i] += monTmp[i];
            }
            if (monRing.level() > (size_t)(3 * frames + 512)) monRing.drop(monRing.level() - (size_t)(2 * frames));
        }
    }

private:
    void apply(const Config& c) {
        monitor = c.monitor; recordFx = c.recordFx;
        dsp.configure(fs_, c);
        if (!monitor) monRing.clear();
    }
    void poll() {
        unsigned g = pendingGen.load();
        if (g != appliedGen && pm.try_lock()) {
            Config c = pending; appliedGen = pendingGen.load(); pm.unlock();
            apply(c);
        }
    }
    void produceBlock() {
        size_t got = inRing.pop(inBlock.data(), (size_t)B);
        if (got < (size_t)B) {
            std::fill(inBlock.begin() + got * 2, inBlock.end(), 0.f);
            if (hasCap) xruns++;
        }
        handler->onBlock(inBlock.data(), outBlock.data(), B);
        outRing.push(outBlock.data(), (size_t)B);
    }

    int B = 64, capP = 0; double fs_ = 48000; BlockHandler* handler = nullptr; bool hasCap = false;
    Ring inRing, outRing, monRing; MonitorDsp dsp;
    std::vector<float> inBlock, outBlock, fxTmp, monTmp;
    bool monitor = false, recordFx = false; long xruns = 0;
    std::mutex pm; Config pending; std::atomic<unsigned> pendingGen{ 0 }; unsigned appliedGen = 0;
};

void audioThreadSetup(HANDLE* mmcss) {
#ifdef ANGEL_X86
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    DWORD idx = 0;
    *mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    if (*mmcss) AvSetMmThreadPriority(*mmcss, AVRT_PRIORITY_CRITICAL);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

// ---------------------------------------------------------------- motor WASAPI
struct Stream {
    Ptr<IAudioClient> client; Ptr<IAudioCaptureClient> cap; Ptr<IAudioRenderClient> ren;
    HANDLE event = nullptr; SampleFmt fmt; UINT32 bufferFrames = 0; bool exclusive = false;
    std::string desc;
    void release() {
        cap.reset(); ren.reset(); client.reset();
        if (event) { CloseHandle(event); event = nullptr; }
    }
};

bool getDevice(IMMDeviceEnumerator* en, EDataFlow flow, const std::wstring& id, IMMDevice** dev) {
    if (!id.empty() && SUCCEEDED(en->GetDevice(id.c_str(), dev))) return true;
    return SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, dev));
}

bool negotiateExclusive(IAudioClient* c, double rate, WAVEFORMATEXTENSIBLE& out) {
    static const int chOpts[] = { 2, 1, 4, 6, 8 };
    struct Cand { int bits, valid; bool flt; };
    static const Cand cands[] = { { 32, 24, false }, { 32, 32, false }, { 24, 24, false }, { 16, 16, false }, { 32, 32, true } };
    for (int ch : chOpts)
        for (const Cand& k : cands) {
            makeWfx(out, ch, k.bits, k.valid, k.flt, rate);
            if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &out.Format, nullptr) == S_OK) return true;
        }
    return false;
}

bool finishStream(Stream& s, bool capture, std::string& err) {
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.event) { err = "CreateEvent fallo"; return false; }
    if (FAILED(s.client->SetEventHandle(s.event))) { err = "SetEventHandle fallo"; return false; }
    if (FAILED(s.client->GetBufferSize(&s.bufferFrames))) { err = "GetBufferSize fallo"; return false; }
    HRESULT hr = capture ? s.client->GetService(__uuidof(IAudioCaptureClient), (void**)s.cap.put())
                         : s.client->GetService(__uuidof(IAudioRenderClient), (void**)s.ren.put());
    if (FAILED(hr)) { err = "GetService fallo"; return false; }
    return true;
}

bool tryExclusive(IMMDevice* dev, bool capture, double rate, float periodMs, Stream& s) {
    Ptr<IAudioClient> c;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)c.put()))) return false;
    REFERENCE_TIME defP = 0, minP = 0;
    if (FAILED(c->GetDevicePeriod(&defP, &minP))) return false;
    WAVEFORMATEXTENSIBLE wf;
    if (!negotiateExclusive(c.p, rate, wf)) return false;
    REFERENCE_TIME dur = minP;
    if (periodMs > 0.f) dur = std::max<REFERENCE_TIME>(minP, (REFERENCE_TIME)(periodMs * 10000.0));
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, &wf.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0; c->GetBufferSize(&frames);
        c.reset();
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)c.put()))) return false;
        dur = (REFERENCE_TIME)(10000000.0 * frames / rate + 0.5);
        hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, &wf.Format, nullptr);
    }
    if (FAILED(hr)) { angelLog("exclusivo fallo hr=0x%08lx", (unsigned long)hr); return false; }
    s.client.reset(); s.client.p = c.p; c.p = nullptr;
    s.fmt = fmtFromWfx(&wf.Format); s.exclusive = true;
    std::string e;
    if (!finishStream(s, capture, e)) { angelLog("finishStream: %s", e.c_str()); s.release(); return false; }
    char b[160];
    snprintf(b, sizeof b, "exclusivo %dch %dbit %s, %u frames (%.2f ms)", s.fmt.channels, s.fmt.validBits, s.fmt.isFloat ? "float" : "pcm",
             (unsigned)s.bufferFrames, 1000.0 * s.bufferFrames / rate);
    s.desc = b;
    return true;
}

bool openShared(IMMDevice* dev, bool capture, double rate, Stream& s, std::string& err) {
    Ptr<IAudioClient> c;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)c.put()))) { err = "Activate fallo"; return false; }
    // 1) Windows 10+: IAudioClient3 con el periodo minimo del motor compartido
    {
        Ptr<IAudioClient3> c3;
        if (SUCCEEDED(c.p->QueryInterface(__uuidof(IAudioClient3), (void**)c3.put()))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(c->GetMixFormat(&mix)) && mix) {
                if ((double)mix->nSamplesPerSec == rate) {
                    UINT32 def = 0, fund = 0, minp = 0, maxp = 0;
                    if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &minp, &maxp))) {
                        HRESULT hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minp, mix, nullptr);
                        if (SUCCEEDED(hr)) {
                            s.fmt = fmtFromWfx(mix); s.exclusive = false;
                            CoTaskMemFree(mix);
                            s.client.reset(); s.client.p = c.p; c.p = nullptr;
                            std::string e;
                            if (finishStream(s, capture, e)) {
                                char b[160];
                                snprintf(b, sizeof b, "compartido de baja latencia, periodo %u frames", (unsigned)minp);
                                s.desc = b; return true;
                            }
                            s.release();
                            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)c.put()))) { err = "Activate fallo"; return false; }
                            goto classic;
                        }
                    }
                }
                CoTaskMemFree(mix);
            }
        }
    }
    // el cliente ya pudo haberse usado: reactivar para el camino clasico
    c.reset();
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)c.put()))) { err = "Activate fallo"; return false; }
classic:
    {
        REFERENCE_TIME defP = 0, minP = 0;
        c->GetDevicePeriod(&defP, &minP);
        WAVEFORMATEXTENSIBLE wf;
        makeWfx(wf, 2, 32, 32, true, rate);
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, defP, 0, &wf.Format, nullptr);
        if (FAILED(hr)) { char b[96]; snprintf(b, sizeof b, "Initialize compartido fallo (0x%08lx)", (unsigned long)hr); err = b; return false; }
        s.client.reset(); s.client.p = c.p; c.p = nullptr;
        s.fmt = fmtFromWfx(&wf.Format); s.exclusive = false;
        if (!finishStream(s, capture, err)) { s.release(); return false; }
        char b[160];
        snprintf(b, sizeof b, "compartido clasico, %u frames", (unsigned)s.bufferFrames);
        s.desc = b;
        return true;
    }
}

class WasapiEngine : public Engine {
public:
    ~WasapiEngine() override { close(); }

    bool open(double rate, int block, const Config& cfg, BlockHandler* h, std::string& err) override {
        close();
        ComInit com;
        rate_ = rate; block_ = block; handler_ = h;
        Ptr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)en.put()))) {
            err = "No se pudo crear el enumerador de dispositivos"; return false;
        }
        Ptr<IMMDevice> rd, cd;
        if (!getDevice(en.p, eRender, cfg.outputId, rd.put())) { err = "No hay dispositivo de salida de audio"; return false; }
        bool haveCap = getDevice(en.p, eCapture, cfg.inputId, cd.put());

        std::string e1;
        bool ok = false;
        if (cfg.exclusive) ok = tryExclusive(rd.p, false, rate, cfg.devicePeriodMs, ren_);
        if (!ok && !openShared(rd.p, false, rate, ren_, e1)) { err = "Salida: " + e1; return false; }
        if (haveCap) {
            ok = false;
            if (cfg.exclusive) ok = tryExclusive(cd.p, true, rate, cfg.devicePeriodMs, cap_);
            if (!ok && !openShared(cd.p, true, rate, cap_, e1)) { angelLog("entrada no disponible: %s", e1.c_str()); cap_.release(); }
        }
        core_.init(block, rate, h, cap_.client.p != nullptr, cap_.client ? (int)cap_.bufferFrames : 0, cfg);
        tmp_.assign(std::max<size_t>(ren_.bufferFrames, 16384) * 2, 0.f);
        tmpCap_.assign(std::max<size_t>(cap_.bufferFrames, 16384) * 2, 0.f);
        char b[512];
        snprintf(b, sizeof b, "WASAPI %.0f Hz, bloque ASIO %d | salida: %s | entrada: %s", rate, block, ren_.desc.c_str(),
                 cap_.client ? cap_.desc.c_str() : "NO DISPONIBLE (se entrega silencio)");
        desc_ = b;
        angelLog("%s", b);
        opened_ = true;
        return true;
    }

    bool start(std::string& err) override {
        if (!opened_) { err = "motor cerrado"; return false; }
        if (running_) return true;
        ComInit com;
        ren_.client->Reset(); if (cap_.client) cap_.client->Reset();
        core_.reset();
        // silencio inicial en el buffer de salida
        BYTE* d = nullptr;
        if (SUCCEEDED(ren_.ren->GetBuffer(ren_.bufferFrames, &d))) ren_.ren->ReleaseBuffer(ren_.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        lost_ = false;
        thread_ = CreateThread(nullptr, 0, &WasapiEngine::threadProc, this, 0, nullptr);
        if (!thread_) { err = "No se pudo crear el hilo de audio"; CloseHandle(stopEv_); stopEv_ = nullptr; return false; }
        if (cap_.client && FAILED(cap_.client->Start())) angelLog("capture Start fallo");
        if (FAILED(ren_.client->Start())) {
            err = "No se pudo iniciar la salida de audio";
            SetEvent(stopEv_); WaitForSingleObject(thread_, 2000); CloseHandle(thread_); thread_ = nullptr;
            CloseHandle(stopEv_); stopEv_ = nullptr;
            if (cap_.client) cap_.client->Stop();
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() override {
        if (!running_) return;
        SetEvent(stopEv_);
        WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_); thread_ = nullptr;
        CloseHandle(stopEv_); stopEv_ = nullptr;
        if (ren_.client) ren_.client->Stop();
        if (cap_.client) cap_.client->Stop();
        running_ = false;
    }

    void close() override {
        stop();
        ren_.release(); cap_.release();
        opened_ = false;
    }

    void updateConfig(const Config& c) override { core_.updateConfig(c); }
    int inputLatency() const override { return block_ + (cap_.client ? (int)cap_.bufferFrames : 0); }
    int outputLatency() const override { return block_ + (int)ren_.bufferFrames; }
    std::string describe() const override {
        char x[64]; snprintf(x, sizeof x, " | xruns de entrada: %ld", core_.xrunCount());
        return desc_ + x;
    }

private:
    static DWORD WINAPI threadProc(LPVOID p) { return ((WasapiEngine*)p)->run(); }

    void drainCapture() {
        if (!cap_.cap) return;
        UINT32 n = 0;
        while (SUCCEEDED(cap_.cap->GetNextPacketSize(&n)) && n > 0) {
            BYTE* data = nullptr; UINT32 fr = 0; DWORD flags = 0;
            if (FAILED(cap_.cap->GetBuffer(&data, &fr, &flags, nullptr, nullptr))) break;
            if (fr > tmpCap_.size() / 2) fr = (UINT32)(tmpCap_.size() / 2);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(tmpCap_.begin(), tmpCap_.begin() + fr * 2, 0.f);
            else toStereo(data, fr, cap_.fmt, tmpCap_.data());
            core_.pushCapture(tmpCap_.data(), (int)fr);
            cap_.cap->ReleaseBuffer(fr);
        }
    }

    bool doRender() {
        UINT32 frames = ren_.bufferFrames;
        if (!ren_.exclusive) {
            UINT32 pad = 0;
            if (FAILED(ren_.client->GetCurrentPadding(&pad))) return true;
            frames = ren_.bufferFrames - pad;
        }
        if (frames == 0) return true;
        BYTE* data = nullptr;
        HRESULT hr = ren_.ren->GetBuffer(frames, &data);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return false;
        if (FAILED(hr)) return true;
        core_.render((int)frames, tmp_.data());
        fromStereo(tmp_.data(), frames, ren_.fmt, data);
        ren_.ren->ReleaseBuffer(frames, 0);
        return true;
    }

    DWORD run() {
        ComInit com;
        HANDLE mm = nullptr;
        audioThreadSetup(&mm);
        HANDLE h[3]; int n = 0;
        h[n++] = stopEv_;
        int ri = n; h[n++] = ren_.event;
        if (cap_.client) h[n++] = cap_.event;
        for (;;) {
            DWORD w = WaitForMultipleObjects((DWORD)n, h, FALSE, 1000);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_TIMEOUT) continue;
            drainCapture();
            if (w == WAIT_OBJECT_0 + (DWORD)ri) {
                if (!doRender()) { lost_ = true; handler_->onReset(); break; }
            }
        }
        if (mm) AvRevertMmThreadCharacteristics(mm);
        return 0;
    }

    Stream ren_, cap_; Core core_; BlockHandler* handler_ = nullptr;
    std::vector<float> tmp_, tmpCap_;
    HANDLE thread_ = nullptr, stopEv_ = nullptr;
    bool opened_ = false, running_ = false, lost_ = false;
    double rate_ = 48000; int block_ = 64; std::string desc_;
};

// ---------------------------------------------------------------- motor de reloj virtual (sin dispositivo)
class VirtualEngine : public Engine {
public:
    ~VirtualEngine() override { close(); }
    bool open(double rate, int block, const Config& cfg, BlockHandler* h, std::string&) override {
        close();
        rate_ = rate; block_ = block; handler_ = h;
        core_.init(block, rate, h, false, 0, cfg);
        zeros_.assign((size_t)block * 2, 0.f); out_.assign((size_t)block * 2, 0.f);
        desc_ = "Reloj virtual (sin dispositivo de audio)";
        opened_ = true; return true;
    }
    bool start(std::string& err) override {
        if (!opened_) { err = "motor cerrado"; return false; }
        if (running_) return true;
        core_.reset();
        stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, &VirtualEngine::threadProc, this, 0, nullptr);
        if (!thread_) { err = "No se pudo crear el hilo"; return false; }
        running_ = true; return true;
    }
    void stop() override {
        if (!running_) return;
        SetEvent(stopEv_); WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_); thread_ = nullptr; CloseHandle(stopEv_); stopEv_ = nullptr;
        running_ = false;
    }
    void close() override { stop(); opened_ = false; }
    void updateConfig(const Config& c) override { core_.updateConfig(c); }
    int inputLatency() const override { return block_; }
    int outputLatency() const override { return block_; }
    std::string describe() const override { return desc_; }
private:
    static DWORD WINAPI threadProc(LPVOID p) { return ((VirtualEngine*)p)->run(); }
    DWORD run() {
        HANDLE mm = nullptr; audioThreadSetup(&mm);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        LARGE_INTEGER fq, now; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&now);
        long long step = (long long)((double)fq.QuadPart * block_ / rate_), next = now.QuadPart + step;
        HANDLE h[2] = { stopEv_, timer };
        for (;;) {
            QueryPerformanceCounter(&now);
            long long rem = next - now.QuadPart;
            if (rem > 0) {
                LARGE_INTEGER due; due.QuadPart = -(rem * 10000000LL / fq.QuadPart);
                if (due.QuadPart == 0) due.QuadPart = -1;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                if (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            } else if (WaitForSingleObject(stopEv_, 0) == WAIT_OBJECT_0) break;
            next += step;
            core_.pushCapture(zeros_.data(), 0);
            core_.render(block_, out_.data());
        }
        if (timer) CloseHandle(timer);
        if (mm) AvRevertMmThreadCharacteristics(mm);
        return 0;
    }
    Core core_; BlockHandler* handler_ = nullptr;
    std::vector<float> zeros_, out_;
    HANDLE thread_ = nullptr, stopEv_ = nullptr;
    bool opened_ = false, running_ = false;
    double rate_ = 48000; int block_ = 64; std::string desc_;
};

} // namespace

// ---------------------------------------------------------------- API publica
std::vector<EndpointInfo> listEndpoints(bool capture) {
    std::vector<EndpointInfo> v;
    ComInit com;
    Ptr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)en.put()))) return v;
    Ptr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, col.put()))) return v;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        Ptr<IMMDevice> d;
        if (FAILED(col->Item(i, d.put()))) continue;
        LPWSTR id = nullptr;
        if (FAILED(d->GetId(&id)) || !id) continue;
        EndpointInfo e; e.id = id; CoTaskMemFree(id);
        Ptr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, ps.put()))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(kPkeyFriendly, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) e.name = pv.pwszVal;
            PropVariantClear(&pv);
        }
        if (e.name.empty()) e.name = e.id;
        v.push_back(e);
    }
    return v;
}

std::unique_ptr<Engine> createEngine() {
    wchar_t b[8]; b[0] = 0;
    DWORD n = GetEnvironmentVariableW(L"ANGELASIO_VIRTUAL", b, 8);
    if (n > 0 && b[0] == L'1') return std::unique_ptr<Engine>(new VirtualEngine());
    return std::unique_ptr<Engine>(new WasapiEngine());
}

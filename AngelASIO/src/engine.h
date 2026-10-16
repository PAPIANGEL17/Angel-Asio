#pragma once
#include "config.h"
#include <memory>
#include <string>
#include <vector>

// El motor llama a este objeto desde su hilo de audio en tiempo real, una vez por bloque.
// in/out: estereo intercalado, "frames" muestras por canal.
struct BlockHandler {
    virtual void onBlock(const float* in, float* out, int frames) = 0;
    virtual void onReset() = 0;   // dispositivo perdido -> pedir reset al host
protected:
    ~BlockHandler() {}
};

struct EndpointInfo { std::wstring id, name; };
std::vector<EndpointInfo> listEndpoints(bool capture);

class Engine {
public:
    virtual ~Engine() {}
    virtual bool open(double rate, int block, const Config& cfg, BlockHandler* h, std::string& err) = 0;
    virtual bool start(std::string& err) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    virtual void updateConfig(const Config& cfg) = 0;
    virtual int inputLatency() const = 0;    // frames
    virtual int outputLatency() const = 0;   // frames
    virtual std::string describe() const = 0;
};

std::unique_ptr<Engine> createEngine();   // WASAPI, o reloj virtual si ANGELASIO_VIRTUAL=1

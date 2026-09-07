#include "engine/AudioEngine.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include "io/DeviceFactory.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace rt;

namespace {

// Adapts AudioEngine to the device callback interface. This tiny class is the
// entire coupling between the I/O layer and the DSP layer.
class EngineCallback : public IAudioCallback {
public:
    explicit EngineCallback(AudioEngine& engine) : engine_(engine) {}
    void audioDeviceProcess(const float* in, float* out, FrameCount frames) noexcept override {
        engine_.processInterleaved(in, out, frames);
    }
private:
    AudioEngine& engine_;
};

Backend parseBackend(const std::string& s) {
    if (s == "wasapi") return Backend::Wasapi;
    if (s == "asio")   return Backend::Asio;
    if (s == "alsa")   return Backend::Alsa;
    if (s == "null")   return Backend::Null;
    return Backend::Default;
}

void printUsage() {
    std::cout <<
        "rt-audio -- realtime audio passthrough with effects\n\n"
        "  --list                 enumerate devices and exit\n"
        "  --backend <name>       wasapi | asio | alsa | null   (default: platform default)\n"
        "  --block <frames>       requested block size (default: driver minimum)\n"
        "  --rate <hz>            requested sample rate (default: 48000)\n"
        "  --in <id>              capture device id (default: system default)\n"
        "  --out <id>             render device id (default: system default)\n"
        "  --exclusive            WASAPI exclusive mode\n"
        "  --drive <x>            distortion drive, 1.0 = clean (default: 1.0)\n"
        "  --mix <0..1>           distortion dry/wet (default: 0.0)\n"
        "  --gate <dB>            noise gate threshold (default: -45)\n"
        "  --bypass               start with the chain bypassed\n"
        "  --help\n";
}

} // namespace

int main(int argc, char** argv) {
    DeviceConfig config;
    Backend backend = Backend::Default;
    float drive = 1.0f, mix = 0.0f, gateDb = -45.0f;
    bool listOnly = false, bypass = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };

        if      (arg == "--list")      listOnly = true;
        else if (arg == "--backend")   backend = parseBackend(next());
        else if (arg == "--block")     config.blockFrames = std::stoi(next());
        else if (arg == "--rate")      config.sampleRate = std::stod(next());
        else if (arg == "--in")        config.inputId = next();
        else if (arg == "--out")       config.outputId = next();
        else if (arg == "--exclusive") config.exclusiveMode = true;
        else if (arg == "--drive")     drive = std::stof(next());
        else if (arg == "--mix")       mix = std::stof(next());
        else if (arg == "--gate")      gateDb = std::stof(next());
        else if (arg == "--bypass")    bypass = true;
        else if (arg == "--help")      { printUsage(); return 0; }
        else { std::cerr << "unknown argument: " << arg << "\n"; printUsage(); return 1; }
    }

    try {
        auto device = createAudioDevice(backend);

        if (listOnly) {
            std::cout << "Devices:\n";
            for (auto const& d : device->enumerate()) {
                std::cout << "  " << std::setw(48) << std::left << d.name
                          << " in:"  << d.maxInputChannels
                          << " out:" << d.maxOutputChannels
                          << " @"    << d.defaultSampleRate
                          << (d.isDefaultInput  ? "  [default in]"  : "")
                          << (d.isDefaultOutput ? "  [default out]" : "")
                          << "\n    id: " << d.id << "\n";
            }
            return 0;
        }

        AudioEngine engine;

        // Build the chain BEFORE prepare(). Order matters: gate first so the
        // distortion is not amplifying noise, tone shaping last.
        engine.chain().add(std::make_unique<Biquad>(Biquad::Type::HighPass, 80.0, 0.707));
        engine.chain().add(std::make_unique<NoiseGate>(engine.params()));
        engine.chain().add(std::make_unique<Waveshaper>(engine.params()));
        engine.chain().add(std::make_unique<Biquad>(Biquad::Type::LowShelf, 200.0, 0.707, 2.0));

        engine.params().drive.store(drive);
        engine.params().mix.store(mix);
        engine.params().gateThresholdDb.store(gateDb);
        engine.params().bypass.store(bypass);

        EngineCallback callback(engine);
        device->open(config, &callback);

        const auto opened = device->status();
        engine.prepare(opened.sampleRate, opened.blockFrames, opened.numChannels);

        device->start();
        const auto st = device->status();

        std::cout << "backend      : " << st.backendName << "\n"
                  << "sample rate  : " << st.sampleRate << " Hz\n"
                  << "block        : " << st.blockFrames << " frames ("
                  << std::fixed << std::setprecision(2)
                  << 1000.0 * st.blockFrames / st.sampleRate << " ms)\n"
                  << "channels     : " << st.numChannels << "\n"
                  << "driver RTT   : ~" << st.estimatedRoundTripMs << " ms"
                  << "  (optimistic -- measure with loopback_latency)\n"
                  << "chain latency: " << engine.chain().totalLatencyFrames() << " frames\n\n"
                  << "running. press Enter to stop.\n\n";

        std::atomic<bool> quit{false};
        HistogramSnapshot cumulative;
        std::thread reporter([&] {
            auto& s = engine.stats();
            while (!quit.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const auto window = s.callbackNanos.drain();
                cumulative.add(window);
                const auto p999Ns = cumulative.nsAtPercentile(0.999);
                const auto deadlineNs = s.blockDeadlineNanos.load();
                const double p999Pct = deadlineNs == 0
                    ? 0.0
                    : 100.0 * static_cast<double>(p999Ns) / static_cast<double>(deadlineNs);
                std::cout << std::fixed << std::setprecision(2)
                          << "\r1s p50 " << static_cast<double>(window.nsAtPercentile(0.5)) / 1000.0 << "us"
                          << "  p99 " << static_cast<double>(window.nsAtPercentile(0.99)) / 1000.0 << "us"
                          << "   run p99.9 " << static_cast<double>(p999Ns) / 1000.0 << "us"
                          << " (" << p999Pct << "%)"
                          << "  max " << static_cast<double>(s.peakCallbackNanos.load(std::memory_order_relaxed)) / 1000.0 << "us"
                          << "  xruns " << s.xruns.load();
                if (cumulative.overflow() != 0) std::cout << "  overflow " << cumulative.overflow();
                std::cout << "        " << std::flush;
            }
        });

        std::cin.get();
        quit = true;
        reporter.join();
        device->stop();

        std::cout << std::fixed << std::setprecision(2)
                  << "\n\ntotal callbacks: " << engine.stats().callbackCount.load()
                  << "   xruns: " << engine.stats().xruns.load()
                  << "   run p99.9 " << static_cast<double>(cumulative.nsAtPercentile(0.999)) / 1000.0 << "us"
                  << "   max " << static_cast<double>(engine.stats().peakCallbackNanos.load(std::memory_order_relaxed)) / 1000.0 << "us";
        if (cumulative.overflow() != 0) std::cout << "   overflow " << cumulative.overflow();
        std::cout << "\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\nerror: " << e.what() << "\n";
        return 1;
    }
}

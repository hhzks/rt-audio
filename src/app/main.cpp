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

        const auto st = device->status();
        engine.prepare(st.sampleRate, st.blockFrames, st.numChannels);

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

        device->start();

        std::atomic<bool> quit{false};
        std::thread reporter([&] {
            while (!quit.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto& s = engine.stats();
                std::cout << "\rload " << std::fixed << std::setprecision(1)
                          << (s.loadFactor() * 100.0) << "%   xruns "
                          << s.xruns.load() << "   peak "
                          << std::setprecision(3) << s.peakOutputLevel.load()
                          << "        " << std::flush;
                s.resetPeaks();
            }
        });

        std::cin.get();
        quit = true;
        reporter.join();
        device->stop();

        std::cout << "\n\ntotal callbacks: " << engine.stats().callbackCount.load()
                  << "   xruns: " << engine.stats().xruns.load() << "\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\nerror: " << e.what() << "\n";
        return 1;
    }
}

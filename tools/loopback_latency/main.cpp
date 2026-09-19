#include "engine/AudioEngine.h"
#include "engine/LoopbackProbe.h"
#include "engine/LatencyAnalyzer.h"
#include "engine/LatencyRun.h"
#include "dsp/RigChain.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include "io/DeviceFactory.h"
#include "io/Utf8Console.h"
#include "common/WavIo.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace rt;

namespace {

class ProbeCallback : public IAudioCallback {
public:
    ProbeCallback(LoopbackProbe& probe, AudioEngine* engine)
        : probe_(probe), engine_(engine) {}

    void setEngine(AudioEngine* engine) noexcept { engine_ = engine; }

    void audioDeviceProcess(const float* in, float* out, FrameCount n) noexcept override {
        probe_.process(in, out, n);
        if (engine_) engine_->processInterleaved(out, out, n);
    }
private:
    LoopbackProbe& probe_;
    AudioEngine*   engine_;
};

Backend parseBackend(const std::string& s) {
    if (s == "wasapi") return Backend::Wasapi;
    if (s == "asio")   return Backend::Asio;
    if (s == "alsa")   return Backend::Alsa;
    if (s == "null")   return Backend::Null;
    return Backend::Default;
}

void printUsage() {
    const bool asio = backendAvailable("asio");
    std::cout <<
        "loopback_latency -- measure physical round-trip audio latency\n\n"
        "  --list                    enumerate devices and exit\n"
        "  --in <id>                 capture device id      (REQUIRED)\n"
        "  --out <id>                render device id       (REQUIRED)\n"
        "  --backend <name>          " << backendList() << "   (default: platform default)\n"
        "  --rate <hz>               requested sample rate (default: 48000)\n"
        "  --block <frames>          requested block size (default: driver minimum"
                                  << (asio ? "; ASIO®: driver preferred" : "") << ")\n"
        "  --exclusive               WASAPI exclusive mode\n"
        "  --ring <blocks>           capture ring margin, 1.0 to 2.0 (default: 2.0)\n"
        "  --repeats <n>             sweep repeats (default: 5)\n"
        "  --sweep-ms <ms>           sweep duration (default: 400)\n"
        "  --sweep-lo <hz>           sweep start frequency (default: 200)\n"
        "  --sweep-hi <hz>           sweep end frequency (default: 10000)\n"
        "  --amplitude <0..1>        sweep amplitude (default: 0.5)\n"
        "  --max-latency-ms <ms>     lag search bound (default: 200)\n"
        "  --expect-silence          negative control: a detected peak is a FAILURE\n"
        "  --skip-chain              measure device RTT only, skip phase B\n"
        "  --control-verified        assert --expect-silence has PASSed on this rig\n"
        "  --json <path>             machine-readable result\n"
        "  --save-capture <path>     dump the last captured sweep as WAV\n"
        "  --help\n";
    if (asio) std::cout << "\n" << kAsioTrademarkNotice << "\n";
}

std::string toLowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void warnIfVirtual(const std::string& role, const std::string& name) {
    static const char* const kSuspects[] = {
        "VoiceMeeter", "Sonar", "VB-Audio", "Virtual", "CABLE", "Stereo Mix"
    };
    const std::string lowerName = toLowerCopy(name);
    for (const char* s : kSuspects) {
        if (lowerName.find(toLowerCopy(s)) != std::string::npos) {
            std::cerr << "warning: " << role << " device \"" << name
                      << "\" looks like a virtual/software route (" << s << "). "
                         "A measurement through it would not be physical.\n";
            return;
        }
    }
}

std::string escapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string warmupText(const PhaseResult& p) {
    if (!p.settled) return "not settled after " + std::to_string(p.warmup) + " sweeps";
    return std::to_string(p.warmup) + (p.warmup == 1 ? " sweep" : " sweeps");
}

PhaseResult runToolPhase(IAudioDevice& device, LoopbackProbe& probe, AudioEngine* engine,
                         int repeats, int maxWarmup, int maxLagFrames, double sampleRate,
                         const char* label) {
    PhaseConfig cfg;
    cfg.repeats      = repeats;
    cfg.maxWarmup    = maxWarmup;
    cfg.maxLagFrames = maxLagFrames;
    cfg.sampleRate   = sampleRate;

    PhaseHooks hooks;
    hooks.dropouts = [&device, engine] {
        const DeviceStatus s = device.status();
        std::uint64_t n = s.captureOverruns + s.captureUnderruns + s.xruns;
        if (engine != nullptr) n += engine->stats().xruns.load(std::memory_order_relaxed);
        return n;
    };
    hooks.progress = [label](PhaseEvent e, int repeat, int retry) {
        switch (e) {
        case PhaseEvent::Attempt:
            if (retry > 0)
                std::cerr << "warning: " << label << " repeat " << repeat
                          << ": xrun during sweep, retrying (" << retry << "/3)\n";
            break;
        case PhaseEvent::Discarded:
            std::cerr << "warning: " << label << " repeat " << repeat
                      << ": xrun persisted after 3 retries, discarding\n";
            break;
        case PhaseEvent::Clipped:
            std::cerr << "warning: " << label << " repeat " << repeat
                      << ": capture clipped (>= 0.999 magnitude)\n";
            break;
        case PhaseEvent::Warmup:
            break;
        }
    };
    return runPhase(probe, cfg, hooks);
}

void printRepeats(const std::vector<LatencyResult>& results) {
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::cout << "    repeat " << (i + 1) << ": "
                  << std::fixed << std::setprecision(3) << r.lagMs << " ms"
                  << std::setprecision(2)
                  << "  r=" << r.peakCorrelation
                  << "  PSR=" << r.peakToSidelobe;
        if (!r.valid) std::cout << "  REJECTED (" << r.rejectReason << ")";
        std::cout << "\n";
    }
}

struct StopGuard {
    explicit StopGuard(IAudioDevice& device) : device_(device) {}
    ~StopGuard() { device_.stop(); }
private:
    IAudioDevice& device_;
};

struct ParamRestorer {
    ParamRestorer(ParameterStore& params, const RigChain& rig)
        : params_(params),
          rig_(rig),
          bypass_(params.bypass.load()),
          gateOn_(rig.gate->getParam(NoiseGate::kOn)),
          mix_(rig.shaper->getParam(Waveshaper::kMix)),
          drive_(rig.shaper->getParam(Waveshaper::kDrive)) {}
    ~ParamRestorer() {
        params_.bypass.store(bypass_);
        rig_.gate->setParam(NoiseGate::kOn, gateOn_);
        rig_.shaper->setParam(Waveshaper::kMix, mix_);
        rig_.shaper->setParam(Waveshaper::kDrive, drive_);
    }
private:
    ParameterStore& params_;
    RigChain        rig_;
    bool   bypass_;
    double gateOn_, mix_, drive_;
};

double parseNumber(const std::string& flag, const std::string& v) {
    try {
        std::size_t pos = 0;
        const double d = std::stod(v, &pos);
        if (pos == v.size() && std::isfinite(d)) return d;
    } catch (const std::exception&) {}
    throw std::runtime_error(flag + " expects a number, got \'" + v + "\'");
}

} // namespace

int main(int argc, char** argv) {
    [[maybe_unused]] rt::Utf8Console console;
    std::string inId, outId, jsonPath, saveCapturePath;
    Backend backend = Backend::Default;
    double sampleRate = 48000.0;
    FrameCount blockFrames = 0;
    bool exclusive = false, listOnly = false, expectSilence = false, skipChain = false;
    bool controlVerified = false;
    int repeats = 5;
    SweepConfig sweepCfg;
    double maxLatencyMs = 200.0;
    double ringBlocks = kDefaultRingBlocks;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(arg + " expects a value");
                return argv[++i];
            };
            auto num = [&]() { return parseNumber(arg, next()); };

            if      (arg == "--list")           listOnly = true;
            else if (arg == "--in")             inId = next();
            else if (arg == "--out")            outId = next();
            else if (arg == "--backend")        backend = parseBackend(next());
            else if (arg == "--rate")           sampleRate = num();
            else if (arg == "--block")          blockFrames = static_cast<FrameCount>(num());
            else if (arg == "--exclusive")      exclusive = true;
            else if (arg == "--ring")           ringBlocks = num();
            else if (arg == "--repeats")        repeats = static_cast<int>(num());
            else if (arg == "--sweep-ms")       sweepCfg.seconds = num() / 1000.0;
            else if (arg == "--sweep-lo")       sweepCfg.loHz = num();
            else if (arg == "--sweep-hi")       sweepCfg.hiHz = num();
            else if (arg == "--amplitude")      sweepCfg.amplitude = static_cast<float>(num());
            else if (arg == "--max-latency-ms") maxLatencyMs = num();
            else if (arg == "--expect-silence") expectSilence = true;
            else if (arg == "--skip-chain")     skipChain = true;
            else if (arg == "--control-verified") controlVerified = true;
            else if (arg == "--json")           jsonPath = next();
            else if (arg == "--save-capture")   saveCapturePath = next();
            else if (arg == "--help")           { printUsage(); return 0; }
            else { std::cerr << "unknown argument: " << arg << "\n"; printUsage(); return 1; }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        printUsage();
        return 1;
    }

    if (!validRingBlocks(ringBlocks)) {
        std::cerr << "error: ring margin must be 1.0 to 2.0 blocks, got " << ringBlocks << "\n";
        printUsage();
        return 1;
    }

    sweepCfg.maxLatencySeconds = maxLatencyMs / 1000.0;

    if (!listOnly && !expectSilence && !controlVerified) {
        std::cerr << "error: negative control not verified.\n"
                  << "       Break the physical path (unplug the earbud) and run the same\n"
                  << "       command with --expect-silence. Once it reports PASS, re-run\n"
                  << "       with --control-verified.\n";
        return 2;
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

        if (inId.empty() || outId.empty()) {
            std::cerr << "error: --in and --out are required.\n"
                         "Run with --list to see available device ids.\n";
            return 1;
        }

        std::string inName, outName;
        for (auto const& d : device->enumerate()) {
            if (d.id == inId)  inName = d.name;
            if (d.id == outId) outName = d.name;
        }
        warnIfVirtual("input",  inName);
        warnIfVirtual("output", outName);

        DeviceConfig devCfg;
        devCfg.inputId       = inId;
        devCfg.outputId      = outId;
        devCfg.sampleRate    = sampleRate;
        devCfg.blockFrames   = blockFrames;
        devCfg.exclusiveMode = exclusive;
        devCfg.ringBlocks    = ringBlocks;

        LoopbackProbe probe;
        AudioEngine   engine;
        ProbeCallback callback(probe, nullptr);

        device->open(devCfg, &callback);
        const auto st = device->status();
        const BackendCaps caps = backendCaps(backend);

        probe.prepare(st.sampleRate, st.blockFrames, st.numChannels, sweepCfg);
        if (probe.reference().empty()) {
            std::cerr << "error: invalid sweep configuration -- --sweep-lo and --sweep-hi "
                         "must be positive and different, --sweep-ms positive.\n";
            return 1;
        }
        const auto maxLagFrames = static_cast<int>(sweepCfg.maxLatencySeconds * st.sampleRate);

        std::cout << "device       : in \"" << inName << "\"  out \"" << outName << "\"\n"
                  << "               " << st.backendName << ", " << st.sampleRate << " Hz, "
                  << st.blockFrames << " frames";
        if (caps.ring) std::cout << ", ring " << ringBlocks << " blocks";
        std::cout << "\n";

        PhaseResult phaseA;
        {
            device->start();
            StopGuard stopGuard(*device);
            phaseA = runToolPhase(*device, probe, nullptr, repeats,
                                  expectSilence ? 0 : kMeasurementWarmup, maxLagFrames,
                                  st.sampleRate, "direct");
            device->stop();
        }

        if (expectSilence) {
            if (!phaseA.enoughKept()) {
                std::cerr << "\nerror: too many xruns to trust the control ("
                          << phaseA.kept.size() << " of " << repeats << " repeats kept).\n";
                std::cout << "neg. control : FAIL\n";
                return 1;
            }
            if (phaseA.anyValid()) {
                double worst = 0.0;
                for (auto const& r : phaseA.kept) if (r.valid) worst = std::max(worst, r.lagMs);
                std::cerr << "\nERROR: detected a peak at " << std::fixed << std::setprecision(2)
                          << worst << " ms with no physical path.\n"
                          << "       An audio route is short-circuiting the measurement.\n"
                          << "       Suspects on this machine: VoiceMeeter, Sonar, VB-Audio, "
                             "Virtual, CABLE, Stereo Mix.\n";
                std::cout << "neg. control : FAIL\n";
                return 2;
            }
            std::cout << "neg. control : PASS\n";
            return 0;
        }

        std::cout << "neg. control : asserted by operator (--control-verified)\n"
                  << "warm-up      : " << warmupText(phaseA) << "\n";

        if (!phaseA.passedMajority()) {
            std::cerr << "\nerror: measurement rejected -- " << phaseA.kept.size()
                      << " of " << repeats << " requested repeats survived ("
                      << phaseA.discarded << " discarded to xruns), "
                      << phaseA.validCount() << " of which found a valid peak.\n";
            if (!phaseA.kept.empty() && !phaseA.kept.back().rejectReason.empty())
                std::cerr << "       last reject reason: " << phaseA.kept.back().rejectReason << "\n";
            std::cerr << "       hints: raise --amplitude, disable direct monitoring in RODE "
                         "Central, check the earcup contacts the capsule.\n";
            printRepeats(phaseA.kept);
            return 1;
        }

        if (phaseA.anyClipped)
            std::cerr << "warning: capture clipped during at least one repeat; lower --amplitude.\n";

        const double measuredMs = phaseA.medianMs();
        const double spreadMs   = phaseA.spreadMs();
        const LatencyResult* rep = phaseA.representative();

        const double computedMs = st.estimatedRoundTripMs;
        const double unaccountedMs = measuredMs - computedMs;

        std::cout << std::fixed << std::setprecision(2)
                  << "\ncomputed  (buffers)  " << std::setw(6) << computedMs << " ms\n"
                  << "measured  (loopback) " << std::setw(6) << measuredMs << " ms   +/- "
                  << (spreadMs / 2.0) << " ms  (" << phaseA.validCount() << " repeats, r "
                  << (rep ? rep->peakCorrelation : 0.0)
                  << ", PSR " << (rep ? rep->peakToSidelobe : 0.0) << ")\n"
                  << "unaccounted          " << std::setw(6) << unaccountedMs << " ms\n";

        if (spreadMs > 1.0) {
            std::cout << "\nUNSTABLE: spread across repeats (" << spreadMs << " ms) exceeds 1 ms.\n";
            printRepeats(phaseA.kept);
        }

        FrameCount chainReportedFrames = 0;
        double     chainMeasuredMs     = 0.0;
        bool       chainMeasuredValid  = false;

        if (!skipChain) {
            const RigChain rig = buildRigChain(engine.chain());

            PhaseResult phaseB;
            {
                ParamRestorer restore(engine.params(), rig);
                engine.params().bypass.store(false);
                rig.gate->setParam(NoiseGate::kOn, 0.0);
                rig.shaper->setParam(Waveshaper::kMix, 0.0);
                rig.shaper->setParam(Waveshaper::kDrive, 1.0);

                engine.prepare(st.sampleRate, st.blockFrames, st.numChannels);
                chainReportedFrames = engine.chain().totalLatencyFrames();

                callback.setEngine(&engine);
                device->start();
                StopGuard stopGuard(*device);
                phaseB = runToolPhase(*device, probe, &engine, repeats, kMeasurementWarmup,
                                      maxLagFrames, st.sampleRate, "chain");
                device->stop();
                callback.setEngine(nullptr);
            }

            if (phaseB.anyClipped)
                std::cerr << "warning: capture clipped during chain phase; lower --amplitude.\n";

            if (phaseB.passedMajority()) {
                const double measuredB = phaseB.medianMs();
                const double spreadB   = phaseB.spreadMs();
                chainMeasuredMs    = measuredB - measuredMs;
                chainMeasuredValid = true;

                std::cout << "\nchain latency: reported " << chainReportedFrames << " frames ("
                          << (1000.0 * chainReportedFrames / st.sampleRate)
                          << " ms)   measured " << chainMeasuredMs << " ms   warm-up "
                          << warmupText(phaseB) << "\n";
                if (spreadB > 1.0) {
                    std::cout << "UNSTABLE (chain): spread across repeats (" << spreadB
                              << " ms) exceeds 1 ms.\n";
                    printRepeats(phaseB.kept);
                }
            } else {
                std::cout << "\nchain latency: reported " << chainReportedFrames << " frames ("
                          << (1000.0 * chainReportedFrames / st.sampleRate)
                          << " ms)   measured n/a (chain sweep did not find a reliable peak)\n";
            }
        }

        if (!saveCapturePath.empty()) {
            WavData wav;
            wav.channels   = 1;
            wav.sampleRate = st.sampleRate;
            wav.samples.assign(probe.captured().begin(), probe.captured().end());
            writeWav(saveCapturePath, wav);
            std::cout << "\nwrote capture to " << saveCapturePath << "\n";
        }

        if (!jsonPath.empty()) {
            std::ofstream f(jsonPath);
            f << "{\n"
              << "  \"device\": \"" << escapeJson(st.backendName) << "\",\n"
              << "  \"sampleRate\": " << st.sampleRate << ",\n"
              << "  \"blockFrames\": " << st.blockFrames << ",\n";
            f << "  \"ringBlocks\": ";
            if (caps.ring) f << ringBlocks; else f << "null";
            f << ",\n"
              << "  \"computedMs\": " << computedMs << ",\n"
              << "  \"measuredMs\": " << measuredMs << ",\n"
              << "  \"spreadMs\": " << spreadMs << ",\n"
              << "  \"correlation\": " << (rep ? rep->peakCorrelation : 0.0) << ",\n"
              << "  \"peakToSidelobe\": " << (rep ? rep->peakToSidelobe : 0.0) << ",\n"
              << "  \"unaccountedMs\": " << unaccountedMs << ",\n"
              << "  \"warmupSweeps\": " << phaseA.warmup << ",\n"
              << "  \"settled\": " << (phaseA.settled ? "true" : "false") << ",\n"
              << "  \"chainReportedFrames\": " << chainReportedFrames << ",\n"
              << "  \"chainMeasuredMs\": " << (chainMeasuredValid ? chainMeasuredMs : 0.0) << ",\n"
              << "  \"chainMeasuredValid\": " << (chainMeasuredValid ? "true" : "false") << "\n"
              << "}\n";
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\nerror: " << e.what() << "\n";
        return 1;
    }
}

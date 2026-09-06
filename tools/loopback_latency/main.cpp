#include "engine/AudioEngine.h"
#include "engine/LoopbackProbe.h"
#include "engine/LatencyAnalyzer.h"
#include "dsp/Biquad.h"
#include "dsp/NoiseGate.h"
#include "dsp/Waveshaper.h"
#include "io/DeviceFactory.h"
#include "common/WavIo.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
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
    if (s == "null")   return Backend::Null;
    return Backend::Default;
}

void printUsage() {
    std::cout <<
        "loopback_latency -- measure physical round-trip audio latency\n\n"
        "  --list                    enumerate devices and exit\n"
        "  --in <id>                 capture device id      (REQUIRED)\n"
        "  --out <id>                render device id       (REQUIRED)\n"
        "  --backend <name>          wasapi | null   (default: platform default)\n"
        "  --rate <hz>               requested sample rate (default: 48000)\n"
        "  --block <frames>          requested block size (default: driver minimum)\n"
        "  --exclusive               WASAPI exclusive mode\n"
        "  --repeats <n>             sweep repeats (default: 5)\n"
        "  --sweep-ms <ms>           sweep duration (default: 400)\n"
        "  --sweep-lo <hz>           sweep start frequency (default: 200)\n"
        "  --sweep-hi <hz>           sweep end frequency (default: 10000)\n"
        "  --amplitude <0..1>        sweep amplitude (default: 0.5)\n"
        "  --max-latency-ms <ms>     lag search bound (default: 200)\n"
        "  --expect-silence          negative control: a detected peak is a FAILURE\n"
        "  --skip-chain              measure device RTT only, skip phase B\n"
        "  --json <path>             machine-readable result\n"
        "  --save-capture <path>     dump the last captured sweep as WAV\n"
        "  --help\n";
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

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct SweepAttempt {
    LatencyResult result;
    bool xrun    = false;
    bool clipped = false;
};

SweepAttempt runOneSweep(IAudioDevice& device, LoopbackProbe& probe, AudioEngine* engine,
                          int maxLagFrames, double sampleRate) {
    const auto devXrunsBefore = device.status().captureOverruns;
    const auto engXrunsBefore = engine ? engine->stats().xruns.load(std::memory_order_relaxed)
                                        : std::uint64_t{0};

    probe.arm();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (probe.state() != ProbeState::Done) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("timed out waiting for the sweep to complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto devXrunsAfter = device.status().captureOverruns;
    const auto engXrunsAfter = engine ? engine->stats().xruns.load(std::memory_order_relaxed)
                                       : std::uint64_t{0};

    SweepAttempt attempt;
    attempt.xrun = (devXrunsAfter != devXrunsBefore) || (engXrunsAfter != engXrunsBefore);
    for (float v : probe.captured())
        if (std::fabs(v) >= 0.999f) { attempt.clipped = true; break; }

    attempt.result = analyzeLatency(probe.reference(), probe.captured(), maxLagFrames, sampleRate);
    return attempt;
}

struct PhaseResult {
    std::vector<LatencyResult> kept;
    bool anyClipped = false;
    int  discarded  = 0;

    int validCount() const {
        int n = 0;
        for (auto const& r : kept) if (r.valid) ++n;
        return n;
    }
    bool anyValid() const {
        for (auto const& r : kept) if (r.valid) return true;
        return false;
    }
    bool passedMajority() const {
        return !kept.empty() && validCount() * 2 > static_cast<int>(kept.size());
    }
};

PhaseResult runPhase(IAudioDevice& device, LoopbackProbe& probe, AudioEngine* engine,
                      int repeats, int maxLagFrames, double sampleRate, const char* label) {
    PhaseResult phase;
    for (int i = 0; i < repeats; ++i) {
        SweepAttempt attempt;
        int extra = 0;
        for (;;) {
            attempt = runOneSweep(device, probe, engine, maxLagFrames, sampleRate);
            if (!attempt.xrun || extra >= 3) break;
            std::cerr << "warning: " << label << " repeat " << (i + 1)
                      << ": xrun during sweep, retrying (" << (extra + 1) << "/3)\n";
            ++extra;
        }
        if (attempt.xrun) {
            std::cerr << "warning: " << label << " repeat " << (i + 1)
                      << ": xrun persisted after 3 retries, discarding\n";
            ++phase.discarded;
            continue;
        }
        if (attempt.clipped) {
            phase.anyClipped = true;
            std::cerr << "warning: " << label << " repeat " << (i + 1)
                      << ": capture clipped (>= 0.999 magnitude)\n";
        }
        phase.kept.push_back(attempt.result);
    }
    return phase;
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

struct ParamRestorer {
    explicit ParamRestorer(ParameterStore& params)
        : params_(params),
          bypass_(params.bypass.load()),
          gate_(params.gateThresholdDb.load()),
          mix_(params.mix.load()),
          drive_(params.drive.load()) {}
    ~ParamRestorer() {
        params_.bypass.store(bypass_);
        params_.gateThresholdDb.store(gate_);
        params_.mix.store(mix_);
        params_.drive.store(drive_);
    }
private:
    ParameterStore& params_;
    bool  bypass_;
    float gate_, mix_, drive_;
};

} // namespace

int main(int argc, char** argv) {
    std::string inId, outId, jsonPath, saveCapturePath;
    Backend backend = Backend::Default;
    double sampleRate = 48000.0;
    FrameCount blockFrames = 0;
    bool exclusive = false, listOnly = false, expectSilence = false, skipChain = false;
    int repeats = 5;
    SweepConfig sweepCfg;
    double maxLatencyMs = 200.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };

        if      (arg == "--list")           listOnly = true;
        else if (arg == "--in")             inId = next();
        else if (arg == "--out")            outId = next();
        else if (arg == "--backend")        backend = parseBackend(next());
        else if (arg == "--rate")           sampleRate = std::stod(next());
        else if (arg == "--block")          blockFrames = std::stoi(next());
        else if (arg == "--exclusive")      exclusive = true;
        else if (arg == "--repeats")        repeats = std::stoi(next());
        else if (arg == "--sweep-ms")       sweepCfg.seconds = std::stod(next()) / 1000.0;
        else if (arg == "--sweep-lo")       sweepCfg.loHz = std::stod(next());
        else if (arg == "--sweep-hi")       sweepCfg.hiHz = std::stod(next());
        else if (arg == "--amplitude")      sweepCfg.amplitude = std::stof(next());
        else if (arg == "--max-latency-ms") maxLatencyMs = std::stod(next());
        else if (arg == "--expect-silence") expectSilence = true;
        else if (arg == "--skip-chain")     skipChain = true;
        else if (arg == "--json")           jsonPath = next();
        else if (arg == "--save-capture")   saveCapturePath = next();
        else if (arg == "--help")           { printUsage(); return 0; }
        else { std::cerr << "unknown argument: " << arg << "\n"; printUsage(); return 1; }
    }

    sweepCfg.maxLatencySeconds = maxLatencyMs / 1000.0;

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

        LoopbackProbe probe;
        AudioEngine   engine;
        ProbeCallback callback(probe, nullptr);

        device->open(devCfg, &callback);
        const auto st = device->status();

        probe.prepare(st.sampleRate, st.blockFrames, st.numChannels, sweepCfg);
        const auto maxLagFrames = static_cast<int>(sweepCfg.maxLatencySeconds * st.sampleRate);

        std::cout << "device       : in \"" << inName << "\"  out \"" << outName << "\"\n"
                  << "               " << st.backendName << ", " << st.sampleRate << " Hz, "
                  << st.blockFrames << " frames\n";

        device->start();
        PhaseResult phaseA = runPhase(*device, probe, nullptr, repeats, maxLagFrames,
                                      st.sampleRate, "direct");
        device->stop();

        if (expectSilence) {
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

        std::cout << "neg. control : not verified this run "
                     "-- run once with --exclusive --expect-silence before trusting this number\n";

        if (!phaseA.passedMajority()) {
            std::cerr << "\nerror: measurement rejected -- fewer than half of "
                      << phaseA.kept.size() << " completed repeats found a valid peak.\n";
            if (!phaseA.kept.empty() && !phaseA.kept.back().rejectReason.empty())
                std::cerr << "       last reject reason: " << phaseA.kept.back().rejectReason << "\n";
            std::cerr << "       hints: raise --amplitude, disable direct monitoring in RODE "
                         "Central, check the earcup contacts the capsule.\n";
            printRepeats(phaseA.kept);
            return 1;
        }

        if (phaseA.anyClipped)
            std::cerr << "warning: capture clipped during at least one repeat; lower --amplitude.\n";

        std::vector<double> lagMsValid;
        for (auto const& r : phaseA.kept) if (r.valid) lagMsValid.push_back(r.lagMs);
        const double measuredMs = median(lagMsValid);
        const double spreadMs = *std::max_element(lagMsValid.begin(), lagMsValid.end())
                               - *std::min_element(lagMsValid.begin(), lagMsValid.end());

        const LatencyResult* rep = nullptr;
        for (auto const& r : phaseA.kept) {
            if (!r.valid) continue;
            if (rep == nullptr || std::fabs(r.lagMs - measuredMs) < std::fabs(rep->lagMs - measuredMs))
                rep = &r;
        }

        const double computedMs = st.estimatedRoundTripMs;
        const double unaccountedMs = measuredMs - computedMs;

        std::cout << std::fixed << std::setprecision(2)
                  << "\ncomputed  (driver)   " << std::setw(6) << computedMs << " ms\n"
                  << "measured  (loopback) " << std::setw(6) << measuredMs << " ms   +/- "
                  << (spreadMs / 2.0) << " ms  (" << lagMsValid.size() << " repeats, r "
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
            engine.chain().add(std::make_unique<Biquad>(Biquad::Type::HighPass, 80.0, 0.707));
            engine.chain().add(std::make_unique<NoiseGate>(engine.params()));
            engine.chain().add(std::make_unique<Waveshaper>(engine.params()));
            engine.chain().add(std::make_unique<Biquad>(Biquad::Type::LowShelf, 200.0, 0.707, 2.0));

            PhaseResult phaseB;
            {
                ParamRestorer restore(engine.params());
                engine.params().bypass.store(false);
                engine.params().gateThresholdDb.store(-120.0f);
                engine.params().mix.store(0.0f);
                engine.params().drive.store(1.0f);

                engine.prepare(st.sampleRate, st.blockFrames, st.numChannels);
                chainReportedFrames = engine.chain().totalLatencyFrames();

                callback.setEngine(&engine);
                device->start();
                phaseB = runPhase(*device, probe, &engine, repeats, maxLagFrames,
                                  st.sampleRate, "chain");
                device->stop();
                callback.setEngine(nullptr);
            }

            if (phaseB.anyClipped)
                std::cerr << "warning: capture clipped during chain phase; lower --amplitude.\n";

            if (phaseB.passedMajority()) {
                std::vector<double> lagMsB;
                for (auto const& r : phaseB.kept) if (r.valid) lagMsB.push_back(r.lagMs);
                const double measuredB = median(lagMsB);
                const double spreadB = *std::max_element(lagMsB.begin(), lagMsB.end())
                                      - *std::min_element(lagMsB.begin(), lagMsB.end());
                chainMeasuredMs    = measuredB - measuredMs;
                chainMeasuredValid = true;

                std::cout << "\nchain latency: reported " << chainReportedFrames << " frames ("
                          << (1000.0 * chainReportedFrames / st.sampleRate)
                          << " ms)   measured " << chainMeasuredMs << " ms\n";
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
              << "  \"blockFrames\": " << st.blockFrames << ",\n"
              << "  \"computedMs\": " << computedMs << ",\n"
              << "  \"measuredMs\": " << measuredMs << ",\n"
              << "  \"spreadMs\": " << spreadMs << ",\n"
              << "  \"correlation\": " << (rep ? rep->peakCorrelation : 0.0) << ",\n"
              << "  \"peakToSidelobe\": " << (rep ? rep->peakToSidelobe : 0.0) << ",\n"
              << "  \"unaccountedMs\": " << unaccountedMs << ",\n"
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

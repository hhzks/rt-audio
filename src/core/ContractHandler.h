#pragma once
#include <cstdint>

namespace rt::contracts {

struct Violation {
    const char* file      = nullptr;
    const char* function  = nullptr;
    const char* predicate = nullptr;
    unsigned    line      = 0;
};

std::uint64_t violationCount() noexcept;
bool          firstViolation(Violation& out) noexcept;

// Prints the count and the first violation to stderr when it is destroyed.
class ExitReport {
public:
    ExitReport() noexcept = default;
    ~ExitReport();
    ExitReport(const ExitReport&) = delete("one exit report per program");
    ExitReport& operator=(const ExitReport&) = delete("one exit report per program");
};

} // namespace rt::contracts

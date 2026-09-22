#include "core/ContractHandler.h"

#include <cstring>

namespace {

int checkedIndex(int i, int n) pre(i >= 0 && i < n) { return i; }

} // namespace

int main(int argc, char** argv) {
    rt::contracts::ExitReport report;
    if (argc > 1 && std::strcmp(argv[1], "--none") == 0) return 0;
    return checkedIndex(argc + 4, 2) == argc + 4 ? 0 : 1;
}

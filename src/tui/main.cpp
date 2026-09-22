#include "core/ContractHandler.h"

#include <cstdint>

extern "C" std::int32_t rt_tui_main(void);

int main() {
    rt::contracts::ExitReport report;
    return rt_tui_main();
}

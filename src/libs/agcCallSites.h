#ifndef EMULATOR_SRC_LIBS_AGCCALLSITES_H_
#define EMULATOR_SRC_LIBS_AGCCALLSITES_H_

#include <cstdint>

// GT7 port, diagnostics (KYTY_AGC_CALLSITES=1): which guest code calls each libSceAgc function.
// Each call through PRINT_NAME in agc.cpp records the function and the guest return address (no
// stack walk: the guest stack may lie in tracked pages). Every 30 s an "Agc call sites" report
// lists the calls a second by function, the busiest return addresses and the busiest draw and
// dispatch call sites.
namespace Libs::Graphics::AgcCallSites {

[[nodiscard]] bool Enabled();
void               Record(const char* function, const void* return_address, const void* frame);

} // namespace Libs::Graphics::AgcCallSites

#endif // EMULATOR_SRC_LIBS_AGCCALLSITES_H_

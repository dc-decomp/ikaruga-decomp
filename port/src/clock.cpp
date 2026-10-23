// The real-time clock, natively: the host's local time. Shinobi read the console's clock as two
// 16-bit registers of the AICA through the G2 bus, a count of seconds since 1950-01-01 00:00, and
// converts the count into a date itself.
#include <ctime>

#include "decomp.h"

namespace game {
namespace {
constexpr long long kFrom1950 = (20LL * 365 + 5) * 86400;  // to 1970-01-01, five leap days between
}

undefined4 FUN_8c044a5c(gptr<uint> count) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const auto seconds = static_cast<uint32_t>(static_cast<long long>(now) + local.tm_gmtoff + kFrom1950);
    *count = seconds;
    // Shinobi hands each half to its debug record, which is off unless the game turns it on.
    FUN_8c044be4(seconds >> 16);
    FUN_8c044be4(seconds & 0xFFFFu);
    return 0;
}

}  // namespace game

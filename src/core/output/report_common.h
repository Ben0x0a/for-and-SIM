#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "card_session.h"

namespace forandsim::output {

std::string isoTimestamp(std::chrono::system_clock::time_point tp);
std::string atrHex(const std::vector<uint8_t>& atr);
const char* chvResultString(ChvResult r);

} // namespace forandsim::output

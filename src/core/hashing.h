#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace forandsim {

// Lowercase hex SHA-256 digest of the given bytes.
std::string sha256Hex(const std::vector<uint8_t>& data);

} // namespace forandsim

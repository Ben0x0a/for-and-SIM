#include "hashing.h"

#include "picosha2.h"

namespace forandsim {

std::string sha256Hex(const std::vector<uint8_t>& data) {
    return picosha2::hash256_hex_string(data);
}

} // namespace forandsim

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xv::encoding {

std::string base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64Decode(const std::string& text);

} // namespace xv::encoding

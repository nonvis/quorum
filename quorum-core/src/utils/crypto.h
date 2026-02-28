#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <iomanip>
#include <sstream>

#include <openssl/sha.h>

namespace sui::quorum::crypto {

// SHA-256 hash, returns hex string
inline std::string sha256_hex(std::string_view data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()),
           data.size(), hash);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : hash) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

// SHA-256 hash, returns raw bytes
inline std::vector<uint8_t> sha256(std::string_view data) {
    std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char*>(data.data()),
           data.size(), hash.data());
    return hash;
}

// Hex encode bytes
inline std::string hex_encode(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

} // namespace sui::quorum::crypto

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tax::crypto {

// Secure random bytes using OS entropy
void randomBytes(std::uint8_t* out, std::size_t len);

// SHA-256 hash (returns hex string)
std::string sha256(const std::string& input);
std::string sha256(const std::uint8_t* data, std::size_t len);
std::string sha256Hex(const std::vector<std::uint8_t>& data);

// PBKDF2-HMAC-SHA256
// Returns: "pbkdf2_sha256$<iterations>$<hex_salt>$<hex_hash>"
std::string pbkdf2Hash(const std::string& password,
                       const std::vector<std::uint8_t>& salt,
                       std::uint32_t iterations = 100000,
                       std::size_t dkLen = 32);

// Verify password against stored hash (constant-time comparison)
// Expected format: "pbkdf2_sha256$<iterations>$<hex_salt>$<hex_hash>"
bool verifyPassword(const std::string& password, const std::string& stored_hash);

// Generate a new salt (16 bytes)
std::vector<std::uint8_t> generateSalt();

// Generate random session token (32 bytes)
std::vector<std::uint8_t> generateSessionToken();

// Convert bytes to hex string
std::string bytesToHex(const std::uint8_t* data, std::size_t len);
std::string bytesToHex(const std::vector<std::uint8_t>& data);
std::string bytesToHex(const std::array<std::uint8_t, 32>& data);

// Convert hex string to bytes
std::vector<std::uint8_t> hexToBytes(const std::string& hex);

// Constant-time memory comparison
bool constantTimeEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t len);
bool constantTimeEqual(const std::string& a, const std::string& b);

// Generate SHA-256 hex of raw bytes (for session token hashing)
std::string hashSessionToken(const std::vector<std::uint8_t>& token);

} // namespace tax::crypto
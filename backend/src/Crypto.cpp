// ============================================================================
// Crypto.cpp — Self-contained cryptographic primitives for authentication.
// Implements SHA-256, PBKDF2-HMAC-SHA256, and secure random bytes using
// OS APIs (BCryptGenRandom on Windows, /dev/urandom on POSIX).
// No external dependencies; pure C++17 + platform crypto APIs.
// ============================================================================

#include "Crypto.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <bcrypt.h>
#    pragma comment(lib, "bcrypt.lib")
#else
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace tax::crypto {

// ---------------------------------------------------------------------------
// Platform-specific secure random
// ---------------------------------------------------------------------------

void randomBytes(std::uint8_t* out, std::size_t len)
{
#ifdef _WIN32
    // Use BCryptGenRandom for cryptographically secure random bytes
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RNG_ALGORITHM, nullptr, 0);
    if (status != 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed: " + std::to_string(status));
    }
    status = BCryptGenRandom(hAlg, out, static_cast<ULONG>(len), 0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (status != 0) {
        throw std::runtime_error("BCryptGenRandom failed: " + std::to_string(status));
    }
#else
    // Use /dev/urandom on POSIX
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("Failed to open /dev/urandom");
    }
    std::size_t total_read = 0;
    while (total_read < len) {
        ssize_t n = read(fd, out + total_read, len - total_read);
        if (n <= 0) {
            close(fd);
            throw std::runtime_error("Failed to read from /dev/urandom");
        }
        total_read += static_cast<std::size_t>(n);
    }
    close(fd);
#endif
}

// ---------------------------------------------------------------------------
// SHA-256 implementation (self-contained, no OpenSSL)
// ---------------------------------------------------------------------------

namespace {

static constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline std::uint32_t shr(std::uint32_t x, int n) { return x >> n; }
inline std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (~x & z); }
inline std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline std::uint32_t sigma0(std::uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline std::uint32_t sigma1(std::uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline std::uint32_t gamma0(std::uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ shr(x, 3); }
inline std::uint32_t gamma1(std::uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ shr(x, 10); }

std::array<std::uint8_t, 32> sha256_raw(const std::uint8_t* data, std::size_t len)
{
    std::vector<std::uint8_t> msg(data, data + len);
    const std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8;

    // Padding
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFF));
    }

    // Initial hash values
    std::array<std::uint32_t, 8> h = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Process blocks
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(msg[off + 4*i]) << 24) |
                   (static_cast<std::uint32_t>(msg[off + 4*i + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[off + 4*i + 2]) << 8) |
                   msg[off + 4*i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            std::uint32_t t1 = hh + sigma1(e) + ch(e, f, g) + K[i] + w[i];
            std::uint32_t t2 = sigma0(a) + maj(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    // Convert to bytes
    std::array<std::uint8_t, 32> out;
    for (int i = 0; i < 8; ++i) {
        out[i*4]     = static_cast<std::uint8_t>(h[i] >> 24);
        out[i*4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[i*4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[i*4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

} // anonymous namespace

std::string sha256(const std::string& input)
{
    auto hash = sha256_raw(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    return bytesToHex(hash);
}

std::string sha256(const std::uint8_t* data, std::size_t len)
{
    auto hash = sha256_raw(data, len);
    return bytesToHex(hash);
}

std::string sha256Hex(const std::vector<std::uint8_t>& data)
{
    auto hash = sha256_raw(data.data(), data.size());
    return bytesToHex(hash);
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------

static std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t key_len,
                                                const std::uint8_t* data, std::size_t data_len)
{
    constexpr std::size_t BLOCK_SIZE = 64;
    std::array<std::uint8_t, BLOCK_SIZE> k_ipad{};
    std::array<std::uint8_t, BLOCK_SIZE> k_opad{};

    if (key_len > BLOCK_SIZE) {
        auto hk = sha256_raw(key, key_len);
        std::copy(hk.begin(), hk.end(), k_ipad.begin());
    } else {
        std::copy(key, key + key_len, k_ipad.begin());
    }
    std::copy(k_ipad.begin(), k_ipad.end(), k_opad.begin());

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    std::vector<std::uint8_t> inner;
    inner.insert(inner.end(), k_ipad.begin(), k_ipad.end());
    inner.insert(inner.end(), data, data + data_len);
    auto inner_hash = sha256_raw(inner.data(), inner.size());

    std::vector<std::uint8_t> outer;
    outer.insert(outer.end(), k_opad.begin(), k_opad.end());
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    return sha256_raw(outer.data(), outer.size());
}

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256
// ---------------------------------------------------------------------------

// Internal: computes raw PBKDF2-HMAC-SHA256 derived key (hex string only)
static std::string pbkdf2HashRaw(const std::string& password,
                                 const std::vector<std::uint8_t>& salt,
                                 std::uint32_t iterations,
                                 std::size_t dkLen)
{
    if (dkLen == 0 || dkLen > 32 * 255) { // Practical limit
        throw std::invalid_argument("Invalid dkLen");
    }
    if (iterations == 0) {
        throw std::invalid_argument("Iterations must be > 0");
    }

    const std::uint8_t* pwd = reinterpret_cast<const std::uint8_t*>(password.data());
    const std::size_t pwd_len = password.size();

    std::vector<std::uint8_t> dk;
    dk.reserve(dkLen);

    const std::uint32_t blocks = static_cast<std::uint32_t>((dkLen + 31) / 32);

    for (std::uint32_t block = 1; block <= blocks; ++block) {
        // U_1 = PRF(password, salt || INT_32_BE(block))
        std::array<std::uint8_t, 4> block_be = {
            static_cast<std::uint8_t>((block >> 24) & 0xFF),
            static_cast<std::uint8_t>((block >> 16) & 0xFF),
            static_cast<std::uint8_t>((block >> 8) & 0xFF),
            static_cast<std::uint8_t>(block & 0xFF)
        };

        std::vector<std::uint8_t> salt_block;
        salt_block.insert(salt_block.end(), salt.begin(), salt.end());
        salt_block.insert(salt_block.end(), block_be.begin(), block_be.end());

        auto u = hmac_sha256(pwd, pwd_len, salt_block.data(), salt_block.size());
        auto result = u;

        // U_i = PRF(password, U_{i-1})
        for (std::uint32_t i = 2; i <= iterations; ++i) {
            u = hmac_sha256(pwd, pwd_len, u.data(), u.size());
            for (std::size_t j = 0; j < 32; ++j) {
                result[j] ^= u[j];
            }
        }

        dk.insert(dk.end(), result.begin(), result.end());
    }

    dk.resize(dkLen);
    return bytesToHex(dk);
}

// Public: returns full format "pbkdf2_sha256$<iterations>$<hex_salt>$<hex_hash>"
std::string pbkdf2Hash(const std::string& password,
                       const std::vector<std::uint8_t>& salt,
                       std::uint32_t iterations,
                       std::size_t dkLen)
{
    std::string raw_hash = pbkdf2HashRaw(password, salt, iterations, dkLen);
    return "pbkdf2_sha256$" + std::to_string(iterations) + "$" + bytesToHex(salt) + "$" + raw_hash;
}

bool verifyPassword(const std::string& password, const std::string& stored_hash)
{
    // Expected format: pbkdf2_sha256$<iterations>$<hex_salt>$<hex_hash>
    const std::string prefix = "pbkdf2_sha256";
    if (stored_hash.size() < prefix.size() || stored_hash.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (stored_hash.size() <= prefix.size() || stored_hash[prefix.size()] != '$') {
        return false;
    }

    // Parse iterations (pos1 is the first $ after "pbkdf2_sha256")
    std::size_t pos1 = stored_hash.find('$', prefix.size());
    if (pos1 == std::string::npos) return false;
    // pos2 is the second $ (after iterations)
    std::size_t pos2 = stored_hash.find('$', pos1 + 1);
    if (pos2 == std::string::npos) return false;
    // pos3 is the third $ (after salt)
    std::size_t pos3 = stored_hash.find('$', pos2 + 1);
    if (pos3 == std::string::npos) return false;

    std::string iter_str = stored_hash.substr(pos1 + 1, pos2 - pos1 - 1);
    std::string salt_hex = stored_hash.substr(pos2 + 1, pos3 - pos2 - 1);
    std::string expected_hash = stored_hash.substr(pos3 + 1);

    // Parse iterations
    std::uint32_t iterations = 100000;
    try {
        iterations = static_cast<std::uint32_t>(std::stoul(iter_str));
    } catch (...) {
        return false;
    }

    // Decode salt
    auto salt = hexToBytes(salt_hex);
    if (salt.size() != 16) {
        // Allow other sizes but 16 is standard
    }

    // Compute raw hash (not full format)
    std::string computed = pbkdf2HashRaw(password, salt, iterations, 32);

    // Constant-time comparison
    return constantTimeEqual(computed, expected_hash);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> generateSalt()
{
    std::vector<std::uint8_t> salt(16);
    randomBytes(salt.data(), salt.size());
    return salt;
}

std::vector<std::uint8_t> generateSessionToken()
{
    std::vector<std::uint8_t> token(32);
    randomBytes(token.data(), token.size());
    return token;
}

std::string bytesToHex(const std::uint8_t* data, std::size_t len)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0F]);
    }
    return out;
}

std::string bytesToHex(const std::vector<std::uint8_t>& data)
{
    return bytesToHex(data.data(), data.size());
}

std::string bytesToHex(const std::array<std::uint8_t, 32>& data)
{
    return bytesToHex(data.data(), data.size());
}

std::vector<std::uint8_t> hexToBytes(const std::string& hex)
{
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        char c1 = hex[i];
        char c2 = hex[i + 1];
        std::uint8_t v = 0;
        if (c1 >= '0' && c1 <= '9') v |= (c1 - '0') << 4;
        else if (c1 >= 'a' && c1 <= 'f') v |= (c1 - 'a' + 10) << 4;
        else if (c1 >= 'A' && c1 <= 'F') v |= (c1 - 'A' + 10) << 4;
        else continue;
        if (c2 >= '0' && c2 <= '9') v |= (c2 - '0');
        else if (c2 >= 'a' && c2 <= 'f') v |= (c2 - 'a' + 10);
        else if (c2 >= 'A' && c2 <= 'F') v |= (c2 - 'A' + 10);
        else continue;
        out.push_back(v);
    }
    return out;
}

bool constantTimeEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t len)
{
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < len; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

bool constantTimeEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        // Still do a dummy comparison to avoid timing leak on length
        volatile std::uint8_t dummy = 0;
        for (std::size_t i = 0; i < a.size(); ++i) dummy |= a[i] ^ a[i];
        for (std::size_t i = 0; i < b.size(); ++i) dummy |= b[i] ^ b[i];
        return false;
    }
    return constantTimeEqual(
        reinterpret_cast<const std::uint8_t*>(a.data()),
        reinterpret_cast<const std::uint8_t*>(b.data()),
        a.size()
    );
}

std::string hashSessionToken(const std::vector<std::uint8_t>& token)
{
    return sha256Hex(token);
}

} // namespace tax::crypto
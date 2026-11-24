// crypto_test.cpp — unit tests for cryptographic primitives (PBKDF2, verifyPassword).
//
// Covers: pbkdf2Hash format, verifyPassword round-trip, special characters,
// admin seeding password hashing, and edge cases like wrong password rejection.

#include <string>
#include <vector>

#include "Crypto.hpp"
#include "test_framework.h"

using namespace tax::crypto;
using namespace testfw;

namespace {

TEST(pbkdf2_hash_returns_full_format_string)
{
    auto salt = generateSalt();
    std::string hash = pbkdf2Hash("testpassword123", salt, 100000, 32);

    // Must start with prefix
    CHECK(hash.rfind("pbkdf2_sha256$", 0) == 0);

    // Must have exactly 3 $ separators (4 parts total)
    int dollar_count = 0;
    for (char c : hash) if (c == '$') dollar_count++;
    CHECK_EQ(dollar_count, 3);

    // Parse parts: "pbkdf2_sha256$iterations$salt$hash"
    // pos1 = after "pbkdf2_sha256"
    size_t pos1 = hash.find('$', 13); // "pbkdf2_sha256" is 13 chars
    size_t pos2 = hash.find('$', pos1 + 1);
    size_t pos3 = hash.find('$', pos2 + 1);
    CHECK(pos1 != std::string::npos);
    CHECK(pos2 != std::string::npos);
    CHECK(pos3 != std::string::npos);
    CHECK(pos3 + 1 < hash.size()); // hash part not empty
}

TEST(verify_password_correct_password_returns_true)
{
    auto salt = generateSalt();
    std::string hash = pbkdf2Hash("MySecureP@ssw0rd!", salt, 100000, 32);

    bool ok = verifyPassword("MySecureP@ssw0rd!", hash);
    CHECK(ok);
}

TEST(verify_password_wrong_password_returns_false)
{
    auto salt = generateSalt();
    std::string hash = pbkdf2Hash("CorrectPassword123", salt, 100000, 32);

    bool ok = verifyPassword("WrongPassword456", hash);
    CHECK(!ok);
}

TEST(verify_password_empty_password_returns_false)
{
    auto salt = generateSalt();
    std::string hash = pbkdf2Hash("SomePassword", salt, 100000, 32);

    bool ok = verifyPassword("", hash);
    CHECK(!ok);
}

TEST(verify_password_special_characters_in_password)
{
    auto salt = generateSalt();
    // 13-char password with special chars like !@#$
    std::string password = "P@ssw0rd!@#$!";
    CHECK_EQ(password.size(), 13u);

    std::string hash = pbkdf2Hash(password, salt, 100000, 32);
    bool ok = verifyPassword(password, hash);
    CHECK(ok);
}

TEST(verify_password_unicode_like_chars_in_password)
{
    auto salt = generateSalt();
    std::string password = "Pässwörd123!"; // contains non-ASCII bytes
    std::string hash = pbkdf2Hash(password, salt, 100000, 32);
    bool ok = verifyPassword(password, hash);
    CHECK(ok);
}

TEST(verify_password_case_sensitivity)
{
    auto salt = generateSalt();
    std::string hash = pbkdf2Hash("Password123", salt, 100000, 32);

    CHECK(verifyPassword("Password123", hash));   // exact case
    CHECK(!verifyPassword("password123", hash));  // different case
    CHECK(!verifyPassword("PASSWORD123", hash));  // different case
}

TEST(verify_password_rejects_malformed_stored_hash)
{
    // Missing prefix
    CHECK(!verifyPassword("pass", "sha256$100000$salt$hash"));
    // Wrong iterations format
    CHECK(!verifyPassword("pass", "pbkdf2_sha256$abc$salt$hash"));
    // Missing parts
    CHECK(!verifyPassword("pass", "pbkdf2_sha256$100000$salt"));
    CHECK(!verifyPassword("pass", "pbkdf2_sha256$100000"));
    CHECK(!verifyPassword("pass", ""));
}

TEST(pbkdf2_deterministic_given_same_salt_and_iterations)
{
    auto salt = generateSalt();
    std::string hash1 = pbkdf2Hash("SamePassword", salt, 100000, 32);
    std::string hash2 = pbkdf2Hash("SamePassword", salt, 100000, 32);
    CHECK(hash1 == hash2);
}

TEST(pbkdf2_different_salt_produces_different_hash)
{
    auto salt1 = generateSalt();
    auto salt2 = generateSalt();
    CHECK(salt1 != salt2);

    std::string hash1 = pbkdf2Hash("SamePassword", salt1, 100000, 32);
    std::string hash2 = pbkdf2Hash("SamePassword", salt2, 100000, 32);
    CHECK(hash1 != hash2);
}

TEST(verify_password_constant_time_comparison_not_trivially_broken)
{
    // Just ensure the function doesn't crash and returns correct result
    // for both matching and non-matching hashes of same length.
    auto salt = generateSalt();
    std::string hash = pbkdf2Hash("test", salt, 1000, 32); // lower iterations for speed

    // Many iterations to ensure timing consistency (not measured here, just functional)
    for (int i = 0; i < 100; ++i) {
        CHECK(verifyPassword("test", hash));
        CHECK(!verifyPassword("wrong", hash));
    }
}

} // anonymous namespace

TESTS_BEGIN
RUN_TEST(pbkdf2_hash_returns_full_format_string),
RUN_TEST(verify_password_correct_password_returns_true),
RUN_TEST(verify_password_wrong_password_returns_false),
RUN_TEST(verify_password_empty_password_returns_false),
RUN_TEST(verify_password_special_characters_in_password),
RUN_TEST(verify_password_unicode_like_chars_in_password),
RUN_TEST(verify_password_case_sensitivity),
RUN_TEST(verify_password_rejects_malformed_stored_hash),
RUN_TEST(pbkdf2_deterministic_given_same_salt_and_iterations),
RUN_TEST(pbkdf2_different_salt_produces_different_hash),
RUN_TEST(verify_password_constant_time_comparison_not_trivially_broken),
TESTS_END
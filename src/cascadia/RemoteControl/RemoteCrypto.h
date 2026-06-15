// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteCrypto.h
//
// Small cryptographic helpers built on top of BCrypt: a SHA-1 digest and Base64
// encoding (used to compute the WebSocket Sec-WebSocket-Accept header), and a
// cryptographically-random token generator (used to mint an auth token when the
// user has not configured one).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RemoteControl::Crypto
{
    // Returns the 20-byte SHA-1 digest of the input bytes.
    std::vector<uint8_t> Sha1(const std::string& input);

    // Returns the 32-byte SHA-256 digest of the input bytes.
    std::vector<uint8_t> Sha256(const std::string& input);

    // ----- Token hashing (for storing a user-set token without plaintext) -----
    // MakeTokenHash returns an entry of the form "sha256$<base64-salt>$<hex-hash>"
    // where hash = SHA-256(salt || token). VerifyTokenHash recomputes and
    // compares in constant time. IsHashedToken recognizes the entry prefix.
    std::string MakeTokenHash(std::string_view token);
    bool VerifyTokenHash(std::string_view storedEntry, std::string_view presentedToken);
    bool IsHashedToken(std::string_view value);

    // Standard Base64 encoding (with padding) of the input bytes.
    std::string Base64Encode(const std::vector<uint8_t>& input);

    // Decodes a Base64 string. Returns nullopt on malformed input.
    std::optional<std::vector<uint8_t>> Base64Decode(std::string_view input);

    // Generates a URL-safe random token with at least the requested number of
    // random bytes of entropy (hex-encoded, so the string is twice as long).
    std::string GenerateToken(size_t entropyBytes = 32);

    // Per-user DPAPI encryption used to keep the on-disk token from being read
    // by other users or casual scraping. ProtectString returns Base64 of the
    // DPAPI blob; UnprotectString reverses it (nullopt if it cannot be
    // decrypted, e.g. a different user or corrupted file).
    std::string ProtectString(std::string_view plaintext);
    std::optional<std::string> UnprotectString(std::string_view base64Blob);
}

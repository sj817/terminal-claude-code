// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteCrypto.h"

#include <dpapi.h>
#include <charconv>
#include <stdexcept>

namespace RemoteControl::Crypto
{
    static std::vector<uint8_t> HashWith(LPCWSTR algId, const std::string& input)
    {
        BCRYPT_ALG_HANDLE algHandle{ nullptr };
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algHandle, algId, nullptr, 0)))
        {
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
        }

        // Make sure the algorithm provider is always released, even on error.
        struct AlgCloser
        {
            BCRYPT_ALG_HANDLE h;
            ~AlgCloser() { BCryptCloseAlgorithmProvider(h, 0); }
        } algCloser{ algHandle };

        DWORD hashLength{ 0 };
        DWORD copied{ 0 };
        if (!BCRYPT_SUCCESS(BCryptGetProperty(algHandle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &copied, 0)))
        {
            throw std::runtime_error("BCryptGetProperty(BCRYPT_HASH_LENGTH) failed");
        }

        BCRYPT_HASH_HANDLE hashHandle{ nullptr };
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algHandle, &hashHandle, nullptr, 0, nullptr, 0, 0)))
        {
            throw std::runtime_error("BCryptCreateHash failed");
        }

        struct HashCloser
        {
            BCRYPT_HASH_HANDLE h;
            ~HashCloser() { BCryptDestroyHash(h); }
        } hashCloser{ hashHandle };

        if (!BCRYPT_SUCCESS(BCryptHashData(hashHandle, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0)))
        {
            throw std::runtime_error("BCryptHashData failed");
        }

        std::vector<uint8_t> digest(hashLength);
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hashHandle, digest.data(), hashLength, 0)))
        {
            throw std::runtime_error("BCryptFinishHash failed");
        }

        return digest;
    }

    std::vector<uint8_t> Sha1(const std::string& input)
    {
        return HashWith(BCRYPT_SHA1_ALGORITHM, input);
    }

    std::vector<uint8_t> Sha256(const std::string& input)
    {
        return HashWith(BCRYPT_SHA256_ALGORITHM, input);
    }

    std::string Base64Encode(const std::vector<uint8_t>& input)
    {
        static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        out.reserve(((input.size() + 2) / 3) * 4);

        size_t i = 0;
        for (; i + 3 <= input.size(); i += 3)
        {
            const uint32_t triple = (static_cast<uint32_t>(input[i]) << 16) | (static_cast<uint32_t>(input[i + 1]) << 8) | input[i + 2];
            out.push_back(table[(triple >> 18) & 0x3f]);
            out.push_back(table[(triple >> 12) & 0x3f]);
            out.push_back(table[(triple >> 6) & 0x3f]);
            out.push_back(table[triple & 0x3f]);
        }

        const size_t remaining = input.size() - i;
        if (remaining == 1)
        {
            const uint32_t triple = static_cast<uint32_t>(input[i]) << 16;
            out.push_back(table[(triple >> 18) & 0x3f]);
            out.push_back(table[(triple >> 12) & 0x3f]);
            out.push_back('=');
            out.push_back('=');
        }
        else if (remaining == 2)
        {
            const uint32_t triple = (static_cast<uint32_t>(input[i]) << 16) | (static_cast<uint32_t>(input[i + 1]) << 8);
            out.push_back(table[(triple >> 18) & 0x3f]);
            out.push_back(table[(triple >> 12) & 0x3f]);
            out.push_back(table[(triple >> 6) & 0x3f]);
            out.push_back('=');
        }

        return out;
    }

    std::optional<std::vector<uint8_t>> Base64Decode(std::string_view input)
    {
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        };

        std::vector<uint8_t> out;
        int buffer = 0;
        int bits = 0;
        for (const char c : input)
        {
            if (c == '=' || c == '\r' || c == '\n')
            {
                continue;
            }
            const int v = value(c);
            if (v < 0)
            {
                return std::nullopt;
            }
            buffer = (buffer << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
            }
        }
        return out;
    }

    std::string GenerateToken(size_t entropyBytes)
    {
        std::vector<uint8_t> bytes(entropyBytes);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        {
            throw std::runtime_error("BCryptGenRandom failed");
        }

        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(entropyBytes * 2);
        for (const auto b : bytes)
        {
            out.push_back(hex[(b >> 4) & 0xf]);
            out.push_back(hex[b & 0xf]);
        }
        return out;
    }

    std::string ProtectString(std::string_view plaintext)
    {
        DATA_BLOB in{};
        in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
        in.cbData = static_cast<DWORD>(plaintext.size());

        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"WindowsTerminalRemoteControl", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        {
            throw std::runtime_error("CryptProtectData failed");
        }

        std::vector<uint8_t> blob(out.pbData, out.pbData + out.cbData);
        LocalFree(out.pbData);
        return Base64Encode(blob);
    }

    std::optional<std::string> UnprotectString(std::string_view base64Blob)
    {
        const auto blob = Base64Decode(base64Blob);
        if (!blob || blob->empty())
        {
            return std::nullopt;
        }

        DATA_BLOB in{};
        in.pbData = const_cast<BYTE*>(blob->data());
        in.cbData = static_cast<DWORD>(blob->size());

        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        {
            return std::nullopt;
        }

        std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        return result;
    }

    namespace
    {
        std::string ToHex(const std::vector<uint8_t>& bytes)
        {
            static constexpr char hex[] = "0123456789abcdef";
            std::string out;
            out.reserve(bytes.size() * 2);
            for (const auto b : bytes)
            {
                out.push_back(hex[(b >> 4) & 0xf]);
                out.push_back(hex[b & 0xf]);
            }
            return out;
        }

        bool ConstantTimeEqualsLocal(std::string_view a, std::string_view b)
        {
            unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
            const size_t count = a.size() < b.size() ? a.size() : b.size();
            for (size_t i = 0; i < count; ++i)
            {
                diff |= static_cast<unsigned char>(a[i] ^ b[i]);
            }
            return diff == 0 && a.size() == b.size();
        }

        constexpr std::string_view HashPrefix{ "pbkdf2$" };

        // PBKDF2 cost. Stored in each entry, so this can be raised later without
        // breaking existing tokens. Kept modest because verification runs per
        // request; the token itself is the real secret.
        constexpr uint32_t DefaultIterations = 100000;
        constexpr size_t SaltLen = 16;
        constexpr size_t KeyLen = 32; // PBKDF2-HMAC-SHA256 output

        // PBKDF2-HMAC-SHA256 via BCrypt.
        std::vector<uint8_t> Pbkdf2(std::string_view password, const std::vector<uint8_t>& salt, uint32_t iterations)
        {
            BCRYPT_ALG_HANDLE alg{ nullptr };
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
            {
                throw std::runtime_error("BCryptOpenAlgorithmProvider(PBKDF2) failed");
            }
            struct AlgCloser
            {
                BCRYPT_ALG_HANDLE h;
                ~AlgCloser() { BCryptCloseAlgorithmProvider(h, 0); }
            } closer{ alg };

            std::vector<uint8_t> out(KeyLen);
            const auto status = BCryptDeriveKeyPBKDF2(
                alg,
                reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                static_cast<ULONG>(password.size()),
                const_cast<PUCHAR>(salt.data()),
                static_cast<ULONG>(salt.size()),
                iterations,
                out.data(),
                static_cast<ULONG>(out.size()),
                0);
            if (!BCRYPT_SUCCESS(status))
            {
                throw std::runtime_error("BCryptDeriveKeyPBKDF2 failed");
            }
            return out;
        }
    }

    bool IsHashedToken(std::string_view value)
    {
        return value.size() > HashPrefix.size() && value.substr(0, HashPrefix.size()) == HashPrefix;
    }

    std::string MakeTokenHash(std::string_view token)
    {
        std::vector<uint8_t> salt(SaltLen);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        {
            throw std::runtime_error("BCryptGenRandom failed");
        }

        const auto dk = Pbkdf2(token, salt, DefaultIterations);
        // Format: pbkdf2$<iterations>$<base64-salt>$<hex-hash>
        return std::string{ HashPrefix } + std::to_string(DefaultIterations) + "$" + Base64Encode(salt) + "$" + ToHex(dk);
    }

    bool VerifyTokenHash(std::string_view storedEntry, std::string_view presentedToken)
    {
        if (!IsHashedToken(storedEntry))
        {
            return false;
        }
        // Parse "pbkdf2$<iterations>$<base64-salt>$<hex-hash>".
        const auto afterPrefix = storedEntry.substr(HashPrefix.size());
        const auto sep1 = afterPrefix.find('$');
        if (sep1 == std::string_view::npos)
        {
            return false;
        }
        const auto sep2 = afterPrefix.find('$', sep1 + 1);
        if (sep2 == std::string_view::npos)
        {
            return false;
        }
        const auto itersStr = afterPrefix.substr(0, sep1);
        const auto saltB64 = afterPrefix.substr(sep1 + 1, sep2 - sep1 - 1);
        const auto expectedHex = afterPrefix.substr(sep2 + 1);

        uint32_t iterations = 0;
        if (std::from_chars(itersStr.data(), itersStr.data() + itersStr.size(), iterations).ec != std::errc{} || iterations == 0)
        {
            return false;
        }

        const auto salt = Base64Decode(saltB64);
        if (!salt)
        {
            return false;
        }

        const auto actualHex = ToHex(Pbkdf2(presentedToken, *salt, iterations));
        return ConstantTimeEqualsLocal(expectedHex, actualHex);
    }
}

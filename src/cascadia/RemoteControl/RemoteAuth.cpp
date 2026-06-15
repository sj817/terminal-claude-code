// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteAuth.h"
#include "RemoteCrypto.h"

namespace RemoteControl
{
    bool ConstantTimeEquals(std::string_view a, std::string_view b)
    {
        // Fold the length difference into the result so the comparison takes the
        // same shape regardless of where (or whether) the strings differ.
        unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
        const size_t count = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < count; ++i)
        {
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        }
        return diff == 0 && a.size() == b.size();
    }

    std::optional<std::string> ExtractBearerToken(std::string_view value)
    {
        // Skip leading whitespace.
        size_t start = value.find_first_not_of(" \t");
        if (start == std::string_view::npos)
        {
            return std::nullopt;
        }
        value.remove_prefix(start);

        constexpr std::string_view prefix{ "Bearer " };
        if (value.size() < prefix.size())
        {
            return std::nullopt;
        }

        // The scheme name is case-insensitive per RFC 7235.
        for (size_t i = 0; i < prefix.size() - 1; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            {
                return std::nullopt;
            }
        }
        if (value[prefix.size() - 1] != ' ')
        {
            return std::nullopt;
        }

        auto token = value.substr(prefix.size());
        // Trim trailing whitespace.
        const auto end = token.find_last_not_of(" \t\r\n");
        if (end == std::string_view::npos)
        {
            return std::nullopt;
        }
        return std::string{ token.substr(0, end + 1) };
    }

    bool IsAuthorized(std::string_view expectedToken,
                      const std::optional<std::string>& headerToken,
                      const std::optional<std::string>& queryToken)
    {
        // Only hashed tokens ("sha256$salt$hash") are accepted. A plaintext or
        // empty expected token authorizes no one - we never compare plaintext on
        // disk, so a scraped settings file cannot yield a usable token.
        if (!Crypto::IsHashedToken(expectedToken))
        {
            return false;
        }

        const auto matches = [&](const std::string& presented) {
            return Crypto::VerifyTokenHash(expectedToken, presented);
        };

        if (headerToken && matches(*headerToken))
        {
            return true;
        }
        if (queryToken && matches(*queryToken))
        {
            return true;
        }
        return false;
    }
}

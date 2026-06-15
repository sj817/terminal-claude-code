// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteAuth.h
//
// Bearer-token authentication for the remote control API. Every request must
// present the configured token, either in an "Authorization: Bearer <token>"
// header or (as a development-only convenience) a "?token=<token>" query
// parameter.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace RemoteControl
{
    // Performs a length-independent, constant-time comparison of two tokens so
    // that an attacker cannot learn the token byte-by-byte via timing.
    bool ConstantTimeEquals(std::string_view a, std::string_view b);

    // Extracts the bearer token from an Authorization header value (the part
    // after "Bearer "), or nullopt if the header is missing/not a bearer token.
    std::optional<std::string> ExtractBearerToken(std::string_view authorizationHeaderValue);

    // Returns true if the presented credentials match the expected token.
    // `headerToken` is the value already extracted from the Authorization header
    // (may be nullopt), `queryToken` is the value of the ?token= query parameter
    // (may be nullopt). An empty expected token never authorizes anyone.
    bool IsAuthorized(std::string_view expectedToken,
                      const std::optional<std::string>& headerToken,
                      const std::optional<std::string>& queryToken);
}

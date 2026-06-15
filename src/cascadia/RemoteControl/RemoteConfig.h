// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteConfig.h
//
// The resolved runtime configuration for the remote control server. The host
// builds this from the user's settings (Model::RemoteControlSettings), resolving
// an empty token to a freshly generated one before the server is started.

#pragma once

#include <cstdint>
#include <string>

namespace RemoteControl
{
    struct RemoteConfig
    {
        bool enabled{ false };
        // The address to bind to, as a UTF-8 string (e.g. "127.0.0.1"). Anything
        // other than a loopback address is an explicit, warned-about choice.
        std::string host{ "127.0.0.1" };
        uint16_t port{ 9177 };
        // The bearer token required for every request. Always non-empty by the
        // time the server starts (the host generates one if the user left it
        // blank).
        std::string token;
        bool allowInput{ true };
        bool allowSnapshot{ true };
        bool allowWebSocket{ true };

        bool IsLoopbackHost() const noexcept
        {
            return host == "127.0.0.1" || host == "::1" || host == "localhost";
        }
    };
}

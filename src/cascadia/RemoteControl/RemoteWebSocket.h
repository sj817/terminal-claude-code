// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteWebSocket.h
//
// A minimal RFC 6455 WebSocket implementation: the opening-handshake accept-key
// computation plus text/close frame encoding and (masked) client-frame decoding.
// Only the pieces the streaming endpoint needs are implemented.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "RemoteHttp.h"

namespace RemoteControl
{
    enum class WsOpcode : uint8_t
    {
        Continuation = 0x0,
        Text = 0x1,
        Binary = 0x2,
        Close = 0x8,
        Ping = 0x9,
        Pong = 0xA,
    };

    struct WsDecodedFrame
    {
        bool fin{ true };
        WsOpcode opcode{ WsOpcode::Text };
        std::string payload;
    };

    // True if the request is a WebSocket upgrade (correct Upgrade/Connection
    // headers and a Sec-WebSocket-Key present).
    bool IsWebSocketUpgrade(const HttpRequest& request);

    // Computes the Sec-WebSocket-Accept value for a given Sec-WebSocket-Key.
    std::string ComputeAcceptKey(std::string_view secWebSocketKey);

    // Builds the 101 Switching Protocols handshake response.
    std::string BuildHandshakeResponse(std::string_view acceptKey);

    // Encodes a server->client text frame (unmasked, as required for server
    // frames). `utf8Payload` must already be UTF-8.
    std::string EncodeTextFrame(std::string_view utf8Payload);

    // Encodes a server->client close frame with the given status code.
    std::string EncodeCloseFrame(uint16_t code = 1000);

    // Encodes a pong frame echoing the given application data.
    std::string EncodePongFrame(std::string_view applicationData);

    // Attempts to decode a single frame from the front of `buffer`.
    //   > 0 : success; the number of bytes consumed (out is filled)
    //   = 0 : not enough data yet; wait for more
    //   < 0 : protocol error; the connection should be closed
    int TryDecodeFrame(std::string_view buffer, WsDecodedFrame& out);
}

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteWebSocket.h"
#include "RemoteCrypto.h"

#include <algorithm>

namespace RemoteControl
{
    // The magic GUID from RFC 6455 section 1.3.
    static constexpr std::string_view WebSocketMagic{ "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" };

    static bool ContainsTokenCaseInsensitive(std::string_view haystack, std::string_view token)
    {
        std::string lowered{ haystack };
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string needle{ token };
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered.find(needle) != std::string::npos;
    }

    bool IsWebSocketUpgrade(const HttpRequest& request)
    {
        const auto upgrade = request.Header("upgrade");
        const auto connection = request.Header("connection");
        const auto key = request.Header("sec-websocket-key");
        if (!upgrade || !connection || !key)
        {
            return false;
        }
        return ContainsTokenCaseInsensitive(*upgrade, "websocket") &&
               ContainsTokenCaseInsensitive(*connection, "upgrade") &&
               !key->empty();
    }

    std::string ComputeAcceptKey(std::string_view secWebSocketKey)
    {
        std::string combined{ secWebSocketKey };
        combined.append(WebSocketMagic);
        const auto digest = Crypto::Sha1(combined);
        return Crypto::Base64Encode(digest);
    }

    std::string BuildHandshakeResponse(std::string_view acceptKey)
    {
        std::string response;
        response.append("HTTP/1.1 101 Switching Protocols\r\n");
        response.append("Upgrade: websocket\r\n");
        response.append("Connection: Upgrade\r\n");
        response.append("Sec-WebSocket-Accept: ").append(acceptKey).append("\r\n");
        response.append("\r\n");
        return response;
    }

    static std::string EncodeFrame(WsOpcode opcode, std::string_view payload)
    {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode))); // FIN + opcode

        const size_t len = payload.size();
        if (len <= 125)
        {
            frame.push_back(static_cast<char>(len));
        }
        else if (len <= 0xffff)
        {
            frame.push_back(static_cast<char>(126));
            frame.push_back(static_cast<char>((len >> 8) & 0xff));
            frame.push_back(static_cast<char>(len & 0xff));
        }
        else
        {
            frame.push_back(static_cast<char>(127));
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> shift) & 0xff));
            }
        }

        frame.append(payload);
        return frame;
    }

    std::string EncodeTextFrame(std::string_view utf8Payload)
    {
        return EncodeFrame(WsOpcode::Text, utf8Payload);
    }

    std::string EncodeCloseFrame(uint16_t code)
    {
        char payload[2] = { static_cast<char>((code >> 8) & 0xff), static_cast<char>(code & 0xff) };
        return EncodeFrame(WsOpcode::Close, std::string_view{ payload, sizeof(payload) });
    }

    std::string EncodePongFrame(std::string_view applicationData)
    {
        return EncodeFrame(WsOpcode::Pong, applicationData);
    }

    int TryDecodeFrame(std::string_view buffer, WsDecodedFrame& out)
    {
        if (buffer.size() < 2)
        {
            return 0;
        }

        const uint8_t b0 = static_cast<uint8_t>(buffer[0]);
        const uint8_t b1 = static_cast<uint8_t>(buffer[1]);

        out.fin = (b0 & 0x80) != 0;
        out.opcode = static_cast<WsOpcode>(b0 & 0x0f);

        const bool masked = (b1 & 0x80) != 0;
        // All client-to-server frames MUST be masked (RFC 6455 5.1).
        if (!masked)
        {
            return -1;
        }

        uint64_t payloadLen = b1 & 0x7f;
        size_t offset = 2;

        if (payloadLen == 126)
        {
            if (buffer.size() < offset + 2)
            {
                return 0;
            }
            payloadLen = (static_cast<uint64_t>(static_cast<uint8_t>(buffer[offset])) << 8) |
                         static_cast<uint8_t>(buffer[offset + 1]);
            offset += 2;
        }
        else if (payloadLen == 127)
        {
            if (buffer.size() < offset + 8)
            {
                return 0;
            }
            payloadLen = 0;
            for (size_t i = 0; i < 8; ++i)
            {
                payloadLen = (payloadLen << 8) | static_cast<uint8_t>(buffer[offset + i]);
            }
            offset += 8;
        }

        // Guard against absurd frame sizes from a misbehaving client.
        if (payloadLen > (16ull * 1024 * 1024))
        {
            return -1;
        }

        const size_t maskOffset = offset;
        if (buffer.size() < maskOffset + 4)
        {
            return 0;
        }
        const size_t dataOffset = maskOffset + 4;
        if (buffer.size() < dataOffset + payloadLen)
        {
            return 0;
        }

        const uint8_t mask[4] = {
            static_cast<uint8_t>(buffer[maskOffset]),
            static_cast<uint8_t>(buffer[maskOffset + 1]),
            static_cast<uint8_t>(buffer[maskOffset + 2]),
            static_cast<uint8_t>(buffer[maskOffset + 3]),
        };

        out.payload.resize(static_cast<size_t>(payloadLen));
        for (size_t i = 0; i < payloadLen; ++i)
        {
            out.payload[i] = static_cast<char>(static_cast<uint8_t>(buffer[dataOffset + i]) ^ mask[i % 4]);
        }

        return static_cast<int>(dataOffset + payloadLen);
    }
}

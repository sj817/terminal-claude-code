// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteJson.h
//
// JSON (de)serialization for the remote control API, built on the repo's bundled
// JsonCpp. Also hosts the UTF-8 / wide-string and ISO-8601 timestamp helpers the
// rest of the server relies on.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "IRemoteHost.h"

namespace RemoteControl::Json
{
    std::string Utf8FromWide(std::wstring_view wide);
    std::wstring WideFromUtf8(std::string_view utf8);

    // Formats a time point as an ISO-8601 UTC timestamp (e.g.
    // "2026-06-14T12:34:56.789Z").
    std::string IsoTimestamp(std::chrono::system_clock::time_point tp);

    // ----- HTTP response bodies -----
    std::string HealthJson();
    std::string SessionsListJson(const std::vector<SessionMetadata>& sessions);
    std::string SessionJson(const SessionMetadata& session);
    std::string SnapshotJson(const std::string& sessionId, const SnapshotData& snapshot);
    std::string OkJson();
    std::string ErrorJson(std::string_view message);

    // ----- HTTP request bodies -----
    // POST .../input : { "data": "..." } -> the wide input string.
    std::optional<std::wstring> ParseInputBody(std::string_view body);
    // POST .../key : { "key": "..." } -> the key name (UTF-8).
    std::optional<std::string> ParseKeyBody(std::string_view body);
    // POST .../resize : { "cols": N, "rows": N }.
    bool ParseResizeBody(std::string_view body, int& cols, int& rows);

    // ----- WebSocket server -> client messages -----
    // Every server event shares a standard envelope: "type", "sessionId" and
    // "timestamp" (ISO-8601), followed by type-specific fields.
    std::string WsSnapshotMessage(const std::string& sessionId, const SnapshotData& snapshot);
    std::string WsOutputMessage(const std::string& sessionId, std::wstring_view data);
    std::string WsExitMessage(const std::string& sessionId, int32_t code);
    std::string WsErrorMessage(const std::string& sessionId, std::string_view message);

    // ----- WebSocket client -> server messages -----
    struct WsClientMessage
    {
        std::string type; // "input" | "key" | "resize"
        std::wstring data; // for "input"
        std::string key; // for "key"
        int cols{ 0 }; // for "resize"
        int rows{ 0 }; // for "resize"
    };
    bool ParseWsClientMessage(std::string_view json, WsClientMessage& out);
}

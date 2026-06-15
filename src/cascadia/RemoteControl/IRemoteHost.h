// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// IRemoteHost.h
//
// The boundary between the (WinRT-free) remote control server and the rest of
// the application. The host - implemented in the WindowsTerminal layer - knows
// how to enumerate panes, read their ControlCore, write input and subscribe to
// output. The server only ever talks to the application through this interface,
// which keeps the server testable and free of any XAML / cppwinrt dependency.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RemoteControl
{
    // Metadata describing one attachable terminal session (i.e. one pane). All
    // strings are UTF-8 except the wide ones, which carry text that may contain
    // arbitrary terminal content.
    struct SessionMetadata
    {
        std::string sessionId;
        std::string windowId;
        std::string tabId;
        std::string paneId;
        std::wstring title;
        std::wstring profileName;
        std::optional<std::wstring> processName;
        std::optional<std::wstring> cwd;
        int rows{ 0 };
        int cols{ 0 };
        bool isFocused{ false };
        bool isAlive{ true };
        int remoteAttachedCount{ 0 };
        std::chrono::system_clock::time_point createdAt{};
        std::optional<std::chrono::system_clock::time_point> lastOutputAt;
    };

    // A run of contiguous viewport cells sharing one visual style. Colors are
    // resolved 0x00RRGGBB values.
    struct CellRun
    {
        std::wstring text;
        uint32_t foreground{ 0 };
        uint32_t background{ 0 };
        bool bold{ false };
        bool italic{ false };
        bool underline{ false };
        int row{ 0 };
    };

    // A snapshot of a session's visible viewport. `text` is always populated;
    // `cells` carries the colored representation when it was requested.
    struct SnapshotData
    {
        std::wstring text;
        int cols{ 0 };
        int rows{ 0 };
        int cursorX{ 0 };
        int cursorY{ 0 };
        bool hasColor{ false };
        std::vector<CellRun> cells;
    };

    // Invoked when new output bytes arrive for a subscribed session. This is
    // called from a background thread and MUST NOT block - implementations
    // enqueue the data and return immediately.
    using OutputCallback = std::function<void(std::wstring_view)>;

    // Invoked when a subscribed session's connection closes. The argument is the
    // process exit code if known, or -1 otherwise.
    using ExitCallback = std::function<void(int32_t)>;

    struct IRemoteHost
    {
        virtual ~IRemoteHost() = default;

        // Returns metadata for every currently attachable session.
        virtual std::vector<SessionMetadata> ListSessions() = 0;

        // Fills `out` with the metadata for a single session. Returns false if no
        // such session exists.
        virtual bool TryGetSession(const std::string& sessionId, SessionMetadata& out) = 0;

        // Writes raw input to the session's connection (PTY/stdin). Returns false
        // if the session does not exist.
        virtual bool WriteInput(const std::string& sessionId, std::wstring_view data) = 0;

        // Fills `out` with a snapshot of the session's visible viewport. When
        // `includeColor` is true, the colored `cells` are populated as well.
        // Returns false if the session does not exist.
        virtual bool GetSnapshot(const std::string& sessionId, bool includeColor, SnapshotData& out) = 0;

        // Records a desired remote view size. In this first version it does not
        // resize the underlying PTY (so the local display is never disturbed);
        // it merely validates the session exists.
        virtual bool Resize(const std::string& sessionId, int cols, int rows) = 0;

        // Subscribes to the session's output. Returns a non-zero token on success
        // (pass it to Unsubscribe), or 0 if the session does not exist.
        virtual uint64_t Subscribe(const std::string& sessionId, OutputCallback onOutput, ExitCallback onExit) = 0;

        // Cancels a subscription previously created with Subscribe.
        virtual void Unsubscribe(const std::string& sessionId, uint64_t token) = 0;
    };
}

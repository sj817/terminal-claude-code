// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteSessionRegistry.h
//
// A process-wide registry of attachable terminal sessions. The application
// registers a session (and a small set of WinRT-free callbacks for acting on it)
// when a pane is created, and unregisters it when the pane is closed. The
// registry implements IRemoteHost, so the API server talks only to this object.
//
// Threading: the registry never holds its own lock while calling back into the
// application (those callbacks take the terminal lock), which avoids any lock
// inversion with the terminal's output path that calls OnOutput().

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "IRemoteHost.h"

namespace RemoteControl
{
    class RemoteSessionRegistry final : public IRemoteHost
    {
    public:
        // Static fields known when the session is registered.
        struct RegisterInfo
        {
            std::string windowId;
            std::string tabId;
            std::string paneId;
            std::wstring profileName;
        };

        // WinRT-free callbacks that act on the underlying ControlCore. They are
        // invoked WITHOUT the registry lock held.
        struct Backend
        {
            std::function<bool(std::wstring_view)> writeInput;
            std::function<bool(bool includeColor, SnapshotData&)> getSnapshot;
            // Fills the dynamic fields: title, rows, cols, cwd, processName, isAlive.
            std::function<void(SessionMetadata&)> fillMetadata;
        };

        static RemoteSessionRegistry& Instance();

        void Register(const std::string& sessionId, RegisterInfo info, Backend backend);
        void Unregister(const std::string& sessionId);

        // Called from the connection's output thread when new bytes arrive.
        void OnOutput(const std::string& sessionId, std::wstring_view data);
        // Called when a session's connection closes.
        void OnExit(const std::string& sessionId, int32_t code);
        // Records which session currently has keyboard focus.
        void SetFocused(const std::string& sessionId);

        // IRemoteHost
        std::vector<SessionMetadata> ListSessions() override;
        bool TryGetSession(const std::string& sessionId, SessionMetadata& out) override;
        bool WriteInput(const std::string& sessionId, std::wstring_view data) override;
        bool GetSnapshot(const std::string& sessionId, bool includeColor, SnapshotData& out) override;
        bool Resize(const std::string& sessionId, int cols, int rows) override;
        uint64_t Subscribe(const std::string& sessionId, OutputCallback onOutput, ExitCallback onExit) override;
        void Unsubscribe(const std::string& sessionId, uint64_t token) override;

    private:
        struct Subscriber
        {
            OutputCallback onOutput;
            ExitCallback onExit;
        };

        struct Entry
        {
            RegisterInfo info;
            Backend backend;
            std::chrono::system_clock::time_point createdAt;
            std::optional<std::chrono::system_clock::time_point> lastOutputAt;
            std::map<uint64_t, Subscriber> subscribers;
        };

        // Builds the static portion of the metadata under the lock; the caller
        // then fills the dynamic portion via the backend WITHOUT the lock.
        std::mutex _mutex;
        std::map<std::string, Entry> _sessions;
        std::string _focusedSessionId;
        uint64_t _nextToken{ 1 };
    };
}

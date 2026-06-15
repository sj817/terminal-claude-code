// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteApiServer.h
//
// A small localhost HTTP + WebSocket server that exposes the remote control API.
// It owns a listening socket and a background accept thread, dispatches requests
// through RemoteHttpRouter logic, and streams output over WebSocket. All access
// to the application goes through an IRemoteHost.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "IRemoteHost.h"
#include "RemoteConfig.h"
#include "RemoteLog.h"

// NOTE: this header deliberately avoids <winsock2.h> so it can be included from
// translation units (e.g. in TerminalApp) that have already pulled in windows.h.
// Socket handles are stored as uintptr_t and cast to SOCKET inside the .cpp.

namespace RemoteControl
{
    class RemoteApiServer
    {
    public:
        RemoteApiServer(RemoteConfig config, IRemoteHost* host, std::shared_ptr<RemoteLog> log = nullptr);
        ~RemoteApiServer();

        RemoteApiServer(const RemoteApiServer&) = delete;
        RemoteApiServer& operator=(const RemoteApiServer&) = delete;

        // Binds and starts listening on a background thread. Returns false if the
        // socket could not be bound (e.g. the port is already in use) - in that
        // case the server stays stopped.
        bool Start();

        // Stops listening and tears down all active connections. Safe to call
        // multiple times; also called by the destructor.
        void Stop();

        bool IsRunning() const noexcept;

    private:
        void _acceptLoop();
        void _handleConnection(uintptr_t client, const std::string& peer);
        void _handleHttp(uintptr_t client, const std::string& peer, const struct HttpRequest& request);
        void _handleWebSocketStream(uintptr_t client, const std::string& peer, const struct HttpRequest& request, const std::string& sessionId);

        // Returns false (and triggers lockdown) if the input is a catastrophic
        // whole-system/whole-drive destructive command.
        bool _guardInput(const std::string& sessionId, const std::string& peer, std::wstring_view data);
        void _enterLockdown(std::string_view reason, const std::string& sessionId, const std::string& peer);
        void _closeAllConnections();

        void _logInfo(std::string_view event, std::string_view fields = {}) const;
        void _logWarn(std::string_view event, std::string_view fields = {}) const;

        // Tracks an active client socket so Stop() can force it closed.
        void _trackSocket(uintptr_t s);
        void _untrackSocket(uintptr_t s);

        RemoteConfig _config;
        IRemoteHost* _host;
        std::shared_ptr<RemoteLog> _log;

        // Set when a catastrophic command is detected; every request then returns
        // 403 until the server is restarted (or the setting is toggled).
        std::atomic<bool> _lockdown{ false };

        // Stored as uintptr_t (== SOCKET width) to keep winsock out of this header.
        uintptr_t _listenSocket{ ~static_cast<uintptr_t>(0) };
        std::thread _acceptThread;
        std::atomic<bool> _running{ false };
        bool _wsaStarted{ false };

        mutable std::mutex _socketsMutex;
        std::set<uintptr_t> _activeSockets;

        // Counts in-flight connection handlers so Stop() can wait for them.
        std::mutex _connMutex;
        std::condition_variable _connCv;
        int _activeConnections{ 0 };
    };
}

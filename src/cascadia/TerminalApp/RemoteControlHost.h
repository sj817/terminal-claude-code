// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteControlHost.h
//
// Owns the lifetime of the remote control API server and translates the user's
// settings into the server's runtime configuration. Lives as a single instance
// on AppLogic so the server is started once per process.

#pragma once

#include <memory>
#include <string>

#include "RemoteConfig.h"

namespace RemoteControl
{
    class RemoteApiServer;
}

namespace winrt::TerminalApp::implementation
{
    class RemoteControlHost
    {
    public:
        // The constructor and destructor are defined out-of-line in the .cpp so
        // that the unique_ptr<RemoteApiServer> member (an incomplete type here)
        // is only ever created/destroyed where RemoteApiServer is complete.
        RemoteControlHost();
        ~RemoteControlHost();

        RemoteControlHost(const RemoteControlHost&) = delete;
        RemoteControlHost& operator=(const RemoteControlHost&) = delete;

        // Starts, stops or restarts the server to match the given settings.
        void ApplySettings(const winrt::Microsoft::Terminal::Settings::Model::RemoteControlSettings& settings);
        void Stop();

    private:
        std::unique_ptr<RemoteControl::RemoteApiServer> _server;
        RemoteControl::RemoteConfig _currentConfig;
        bool _running{ false };
    };
}

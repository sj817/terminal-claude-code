// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteControlHost.h"

#include "RemoteApiServer.h"
#include "RemoteSessionRegistry.h"
#include "RemoteCrypto.h"
#include "RemoteLog.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <shlobj.h>

using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        // The per-user directory we keep remote-control state in. Uses the same
        // unpackaged path WT uses (and, when packaged, the redirected per-package
        // LocalAppData), so it is naturally isolated per user.
        std::filesystem::path StateDirectory()
        {
            PWSTR rawPath = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath)) || rawPath == nullptr)
            {
                return {};
            }
            std::filesystem::path path{ rawPath };
            CoTaskMemFree(rawPath);
            path /= L"Microsoft";
            path /= L"Windows Terminal";
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            return path;
        }

    }

    RemoteControlHost::RemoteControlHost() = default;

    RemoteControlHost::~RemoteControlHost()
    {
        Stop();
    }

    void RemoteControlHost::Stop()
    {
        if (_server)
        {
            _server->Stop();
            _server.reset();
        }
        _running = false;
    }

    void RemoteControlHost::ApplySettings(const RemoteControlSettings& settings)
    {
        // RemoteControlSettings is a WinRT struct (value type), so its members are
        // accessed as fields rather than method calls.
        RemoteControl::RemoteConfig config;
        config.enabled = settings.Enabled;
        config.host = winrt::to_string(settings.Host);
        config.port = static_cast<uint16_t>(settings.Port);
        config.allowInput = settings.AllowInput;
        config.allowSnapshot = settings.AllowSnapshot;
        config.allowWebSocket = settings.AllowWebSocket;
        // The token is stored only as a salted hash (set by the user in Settings).
        // We never auto-generate one and never accept a plaintext token.
        config.token = winrt::to_string(settings.Token);

        // Refuse to run without a properly hashed token, so a misconfigured or
        // plaintext token never results in an open or unusable server.
        const bool hasValidToken = config.enabled && RemoteControl::Crypto::IsHashedToken(config.token);

        if (!hasValidToken)
        {
            if (config.enabled)
            {
                OutputDebugStringA("RemoteControl: enabled but no hashed token set; not starting. Set a token in Settings > Remote control.\n");
            }
            Stop();
            _currentConfig = config;
            return;
        }

        // Only (re)start when something that affects the listening socket or auth
        // actually changed, to avoid tearing down active connections on every
        // unrelated settings reload.
        const bool needsRestart = !_running ||
                                  config.host != _currentConfig.host ||
                                  config.port != _currentConfig.port ||
                                  config.token != _currentConfig.token ||
                                  config.allowInput != _currentConfig.allowInput ||
                                  config.allowSnapshot != _currentConfig.allowSnapshot ||
                                  config.allowWebSocket != _currentConfig.allowWebSocket;
        if (!needsRestart)
        {
            _currentConfig = config;
            return;
        }

        Stop();

        std::wstring logPath;
        if (const auto dir = StateDirectory(); !dir.empty())
        {
            logPath = (dir / L"remote-control.log").wstring();
        }
        auto log = std::make_shared<RemoteControl::RemoteLog>(logPath);

        _server = std::make_unique<RemoteControl::RemoteApiServer>(config, &RemoteControl::RemoteSessionRegistry::Instance(), std::move(log));
        _running = _server->Start();
        if (!_running)
        {
            _server.reset();
        }
        _currentConfig = config;
    }
}

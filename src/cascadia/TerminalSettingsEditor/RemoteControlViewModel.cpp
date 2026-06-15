// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteControlViewModel.h"

#include "RemoteControlViewModel.g.cpp"

#include "../RemoteControl/RemoteCrypto.h"

#include <algorithm>
#include <cmath>

using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    RemoteControlViewModel::RemoteControlViewModel(CascadiaSettings settings) noexcept :
        _settings{ std::move(settings) }
    {
    }

    Model::RemoteControlSettings RemoteControlViewModel::_get() const
    {
        return _settings.GlobalSettings().RemoteControl();
    }

    void RemoteControlViewModel::_set(const Model::RemoteControlSettings& value)
    {
        _settings.GlobalSettings().RemoteControl(value);
    }

    bool RemoteControlViewModel::RemoteControlEnabled()
    {
        return _get().Enabled;
    }
    void RemoteControlViewModel::RemoteControlEnabled(bool value)
    {
        auto s = _get();
        if (s.Enabled != value)
        {
            s.Enabled = value;
            _set(s);
            _NotifyChanges(L"RemoteControlEnabled");
        }
    }

    winrt::hstring RemoteControlViewModel::RemoteControlHost()
    {
        return _get().Host;
    }

    double RemoteControlViewModel::RemoteControlPort()
    {
        return static_cast<double>(_get().Port);
    }
    void RemoteControlViewModel::RemoteControlPort(double value)
    {
        // NumberBox can hand back NaN when cleared; clamp to a valid TCP port.
        uint32_t port = _get().Port;
        if (!std::isnan(value))
        {
            const auto clamped = std::clamp(value, 1.0, 65535.0);
            port = static_cast<uint32_t>(clamped);
        }
        auto s = _get();
        if (s.Port != port)
        {
            s.Port = port;
            _set(s);
            _NotifyChanges(L"RemoteControlPort");
        }
    }

    void RemoteControlViewModel::TokenInput(const winrt::hstring& value)
    {
        if (_tokenInput != value)
        {
            _tokenInput = value;
            _NotifyChanges(L"TokenInput");
        }
    }

    bool RemoteControlViewModel::IsTokenConfigured()
    {
        return !_get().Token.empty();
    }

    void RemoteControlViewModel::ApplyToken()
    {
        auto s = _get();
        const auto plain = winrt::to_string(_tokenInput);
        // Empty input clears the custom token (server then auto-generates one,
        // stored DPAPI-encrypted). A non-empty token is stored only as a salted
        // hash, so settings.json never contains the plaintext.
        s.Token = plain.empty() ? winrt::hstring{} : winrt::to_hstring(::RemoteControl::Crypto::MakeTokenHash(plain));
        _set(s);
        _tokenInput = winrt::hstring{};
        _NotifyChanges(L"TokenInput", L"IsTokenConfigured");
    }

    bool RemoteControlViewModel::AllowRemoteInput()
    {
        return _get().AllowInput;
    }
    void RemoteControlViewModel::AllowRemoteInput(bool value)
    {
        auto s = _get();
        if (s.AllowInput != value)
        {
            s.AllowInput = value;
            _set(s);
            _NotifyChanges(L"AllowRemoteInput");
        }
    }

    bool RemoteControlViewModel::AllowRemoteSnapshot()
    {
        return _get().AllowSnapshot;
    }
    void RemoteControlViewModel::AllowRemoteSnapshot(bool value)
    {
        auto s = _get();
        if (s.AllowSnapshot != value)
        {
            s.AllowSnapshot = value;
            _set(s);
            _NotifyChanges(L"AllowRemoteSnapshot");
        }
    }

    bool RemoteControlViewModel::AllowRemoteWebSocket()
    {
        return _get().AllowWebSocket;
    }
    void RemoteControlViewModel::AllowRemoteWebSocket(bool value)
    {
        auto s = _get();
        if (s.AllowWebSocket != value)
        {
            s.AllowWebSocket = value;
            _set(s);
            _NotifyChanges(L"AllowRemoteWebSocket");
        }
    }

}

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RemoteControlViewModel.g.h"
#include "Utils.h"
#include "ViewModelHelpers.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct RemoteControlViewModel : RemoteControlViewModelT<RemoteControlViewModel>, ViewModelHelper<RemoteControlViewModel>
    {
        explicit RemoteControlViewModel(Model::CascadiaSettings settings) noexcept;

        bool RemoteControlEnabled();
        void RemoteControlEnabled(bool value);

        winrt::hstring RemoteControlHost();

        double RemoteControlPort();
        void RemoteControlPort(double value);

        winrt::hstring TokenInput() const { return _tokenInput; }
        void TokenInput(const winrt::hstring& value);
        bool IsTokenConfigured();
        void ApplyToken();

        bool AllowRemoteInput();
        void AllowRemoteInput(bool value);

        bool AllowRemoteSnapshot();
        void AllowRemoteSnapshot(bool value);

        bool AllowRemoteWebSocket();
        void AllowRemoteWebSocket(bool value);

    private:
        Model::CascadiaSettings _settings{ nullptr };
        winrt::hstring _tokenInput;

        // RemoteControl is a WinRT struct (value type). We read the whole struct,
        // mutate a local copy, then write it back as a unit so changes persist.
        Model::RemoteControlSettings _get() const;
        void _set(const Model::RemoteControlSettings& value);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(RemoteControlViewModel);
}

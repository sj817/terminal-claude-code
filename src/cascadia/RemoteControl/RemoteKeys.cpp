// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteKeys.h"

#include <algorithm>

namespace RemoteControl
{
    std::optional<std::wstring> KeyNameToSequence(std::string_view keyName)
    {
        std::string key{ keyName };
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // The mapping below matches the API contract exactly. These are the same
        // sequences a real terminal emits for these keys.
        if (key == "enter")
        {
            return std::wstring{ L"\r" };
        }
        if (key == "tab")
        {
            return std::wstring{ L"\t" };
        }
        if (key == "escape")
        {
            return std::wstring{ L"\x1b" };
        }
        if (key == "ctrl-c")
        {
            return std::wstring{ L"\x03" };
        }
        if (key == "ctrl-d")
        {
            return std::wstring{ L"\x04" };
        }
        if (key == "up")
        {
            return std::wstring{ L"\x1b[A" };
        }
        if (key == "down")
        {
            return std::wstring{ L"\x1b[B" };
        }
        if (key == "right")
        {
            return std::wstring{ L"\x1b[C" };
        }
        if (key == "left")
        {
            return std::wstring{ L"\x1b[D" };
        }
        if (key == "backspace")
        {
            return std::wstring{ L"\x7f" };
        }
        if (key == "delete")
        {
            return std::wstring{ L"\x1b[3~" };
        }

        return std::nullopt;
    }
}

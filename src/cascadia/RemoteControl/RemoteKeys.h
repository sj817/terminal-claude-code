// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteKeys.h
//
// Maps the small set of named special keys accepted by the API to the raw
// terminal input sequences they produce.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace RemoteControl
{
    // Translates a key name (e.g. "enter", "ctrl-c", "up") into the wide-string
    // input sequence that should be written to the terminal. Returns nullopt for
    // an unknown key name. Names are matched case-insensitively.
    std::optional<std::wstring> KeyNameToSequence(std::string_view keyName);
}

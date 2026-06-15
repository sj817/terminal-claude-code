// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteGuard.h
//
// A safety check for remote input. It detects commands that would wipe the
// entire filesystem root or a whole drive (e.g. "rm -rf /", "del /s C:\",
// "format C:", "Remove-Item -Recurse C:\"). Deleting an ordinary directory
// (e.g. "rm -rf ./build" or "del /s logs\") is deliberately NOT matched.
//
// When the server sees such a command it refuses to forward it, drops every
// active connection, and locks itself down (see RemoteApiServer).

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace RemoteControl
{
    // If `input` contains a whole-system / whole-drive destructive command,
    // returns a short category string naming the match (for logging). Otherwise
    // returns nullopt.
    std::optional<std::string> MatchCatastrophicCommand(std::wstring_view input);
}

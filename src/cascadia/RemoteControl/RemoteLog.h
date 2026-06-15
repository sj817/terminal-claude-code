// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteLog.h
//
// A small thread-safe, size-capped rotating file logger for the remote control
// server. Lines are structured: "<ISO-8601> [LEVEL] <event> <key=value ...>".
// It deliberately never logs input/output contents - only metadata such as
// byte lengths, session ids and peer addresses.

#pragma once

#include <mutex>
#include <string>
#include <string_view>

namespace RemoteControl
{
    enum class LogLevel
    {
        Info,
        Warn,
        Critical,
    };

    class RemoteLog
    {
    public:
        // `filePath` is the log file; it rotates to "<file>.1" once it exceeds
        // maxBytes. If filePath is empty, logging is a no-op (besides debug out).
        explicit RemoteLog(std::wstring filePath, size_t maxBytes = 1024 * 1024);

        void Info(std::string_view event, std::string_view fields = {});
        void Warn(std::string_view event, std::string_view fields = {});
        void Critical(std::string_view event, std::string_view fields = {});

    private:
        void _write(LogLevel level, std::string_view event, std::string_view fields);

        std::mutex _mutex;
        std::wstring _path;
        size_t _maxBytes;
    };
}

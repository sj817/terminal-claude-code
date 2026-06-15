// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteLog.h"
#include "RemoteJson.h" // for IsoTimestamp

#include <filesystem>
#include <fstream>

namespace RemoteControl
{
    RemoteLog::RemoteLog(std::wstring filePath, size_t maxBytes) :
        _path{ std::move(filePath) },
        _maxBytes{ maxBytes }
    {
    }

    void RemoteLog::Info(std::string_view event, std::string_view fields)
    {
        _write(LogLevel::Info, event, fields);
    }

    void RemoteLog::Warn(std::string_view event, std::string_view fields)
    {
        _write(LogLevel::Warn, event, fields);
    }

    void RemoteLog::Critical(std::string_view event, std::string_view fields)
    {
        _write(LogLevel::Critical, event, fields);
    }

    void RemoteLog::_write(LogLevel level, std::string_view event, std::string_view fields)
    {
        const char* levelStr = level == LogLevel::Critical ? "CRITICAL" : (level == LogLevel::Warn ? "WARN" : "INFO");

        std::string line;
        line.append(Json::IsoTimestamp(std::chrono::system_clock::now()));
        line.append(" [").append(levelStr).append("] ");
        line.append(event);
        if (!fields.empty())
        {
            line.push_back(' ');
            line.append(fields);
        }
        line.push_back('\n');

        // Always mirror to the debugger output for live diagnosis.
        OutputDebugStringA(("RemoteControl: " + line).c_str());

        if (_path.empty())
        {
            return;
        }

        std::lock_guard<std::mutex> guard{ _mutex };
        try
        {
            std::error_code ec;
            const std::filesystem::path path{ _path };
            if (const auto size = std::filesystem::file_size(path, ec); !ec && size + line.size() > _maxBytes)
            {
                // Rotate: replace "<file>.1" with the current file.
                std::filesystem::path rotated{ _path };
                rotated += L".1";
                std::filesystem::remove(rotated, ec);
                std::filesystem::rename(path, rotated, ec);
            }

            std::ofstream out{ path, std::ios::app | std::ios::binary };
            if (out)
            {
                out.write(line.data(), static_cast<std::streamsize>(line.size()));
            }
        }
        catch (...)
        {
            // Logging must never throw into the server.
        }
    }
}

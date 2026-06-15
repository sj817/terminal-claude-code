// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteJson.h"

#include <ctime>

namespace RemoteControl::Json
{
    namespace
    {
        constexpr std::string_view AppName{ "WindowsTerminalRemote" };
        constexpr std::string_view ApiVersion{ "0.1.0" };

        std::string Serialize(const ::Json::Value& value)
        {
            ::Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            builder["emitUTF8"] = true;
            return ::Json::writeString(builder, value);
        }

        bool Parse(std::string_view text, ::Json::Value& out)
        {
            ::Json::CharReaderBuilder builder;
            const std::unique_ptr<::Json::CharReader> reader{ builder.newCharReader() };
            std::string errors;
            return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
        }

        std::string ColorToHex(uint32_t rrggbb)
        {
            char buffer[8] = {};
            std::snprintf(buffer, sizeof(buffer), "#%06x", rrggbb & 0xffffff);
            return std::string{ buffer };
        }

        // Appends a "cells" array (rows of style runs) to `root` when the snapshot
        // carries color. Each row is an array of { text, fg, bg, bold, italic,
        // underline } runs.
        void AppendCells(::Json::Value& root, const SnapshotData& snapshot)
        {
            if (!snapshot.hasColor)
            {
                return;
            }

            ::Json::Value rows{ ::Json::arrayValue };
            ::Json::Value currentRow{ ::Json::arrayValue };
            int currentRowIndex = -1;
            for (const auto& run : snapshot.cells)
            {
                if (run.row != currentRowIndex)
                {
                    if (currentRowIndex >= 0)
                    {
                        rows.append(currentRow);
                    }
                    currentRow = ::Json::Value{ ::Json::arrayValue };
                    currentRowIndex = run.row;
                }
                ::Json::Value cell{ ::Json::objectValue };
                cell["text"] = Utf8FromWide(run.text);
                cell["fg"] = ColorToHex(run.foreground);
                cell["bg"] = ColorToHex(run.background);
                cell["bold"] = run.bold;
                cell["italic"] = run.italic;
                cell["underline"] = run.underline;
                currentRow.append(cell);
            }
            if (currentRowIndex >= 0)
            {
                rows.append(currentRow);
            }
            root["cells"] = rows;
        }

        // Builds the standard event envelope shared by every server -> client
        // WebSocket message.
        ::Json::Value MakeEnvelope(std::string_view type, const std::string& sessionId)
        {
            ::Json::Value root{ ::Json::objectValue };
            root["type"] = std::string{ type };
            root["sessionId"] = sessionId;
            root["timestamp"] = IsoTimestamp(std::chrono::system_clock::now());
            return root;
        }
    }

    std::string Utf8FromWide(std::wstring_view wide)
    {
        if (wide.empty())
        {
            return {};
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::wstring WideFromUtf8(std::string_view utf8)
    {
        if (utf8.empty())
        {
            return {};
        }
        const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
        return out;
    }

    std::string IsoTimestamp(std::chrono::system_clock::time_point tp)
    {
        using namespace std::chrono;
        const auto secs = time_point_cast<seconds>(tp);
        const auto ms = duration_cast<milliseconds>(tp - secs).count();
        const std::time_t t = system_clock::to_time_t(secs);
        std::tm tmUtc{};
        gmtime_s(&tmUtc, &t);

        char buffer[40] = {};
        // e.g. 2026-06-14T12:34:56.789Z
        const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                                           tmUtc.tm_year + 1900,
                                           tmUtc.tm_mon + 1,
                                           tmUtc.tm_mday,
                                           tmUtc.tm_hour,
                                           tmUtc.tm_min,
                                           tmUtc.tm_sec,
                                           static_cast<long long>(ms));
        return std::string{ buffer, static_cast<size_t>(written > 0 ? written : 0) };
    }

    std::string HealthJson()
    {
        ::Json::Value root{ ::Json::objectValue };
        root["ok"] = true;
        root["app"] = std::string{ AppName };
        root["version"] = std::string{ ApiVersion };
        return Serialize(root);
    }

    static ::Json::Value SessionToValue(const SessionMetadata& s)
    {
        ::Json::Value v{ ::Json::objectValue };
        v["sessionId"] = s.sessionId;
        v["windowId"] = s.windowId;
        v["tabId"] = s.tabId;
        v["paneId"] = s.paneId;
        v["title"] = Utf8FromWide(s.title);
        v["profileName"] = Utf8FromWide(s.profileName);
        v["processName"] = s.processName ? ::Json::Value{ Utf8FromWide(*s.processName) } : ::Json::Value{ ::Json::nullValue };
        v["cwd"] = s.cwd ? ::Json::Value{ Utf8FromWide(*s.cwd) } : ::Json::Value{ ::Json::nullValue };
        v["rows"] = s.rows;
        v["cols"] = s.cols;
        v["isFocused"] = s.isFocused;
        v["isAlive"] = s.isAlive;
        v["remoteAttachedCount"] = s.remoteAttachedCount;
        v["createdAt"] = IsoTimestamp(s.createdAt);
        v["lastOutputAt"] = s.lastOutputAt ? ::Json::Value{ IsoTimestamp(*s.lastOutputAt) } : ::Json::Value{ ::Json::nullValue };
        return v;
    }

    std::string SessionsListJson(const std::vector<SessionMetadata>& sessions)
    {
        ::Json::Value root{ ::Json::objectValue };
        ::Json::Value array{ ::Json::arrayValue };
        for (const auto& s : sessions)
        {
            array.append(SessionToValue(s));
        }
        root["sessions"] = array;
        return Serialize(root);
    }

    std::string SessionJson(const SessionMetadata& session)
    {
        return Serialize(SessionToValue(session));
    }

    std::string SnapshotJson(const std::string& sessionId, const SnapshotData& snapshot)
    {
        ::Json::Value root{ ::Json::objectValue };
        root["sessionId"] = sessionId;
        root["cols"] = snapshot.cols;
        root["rows"] = snapshot.rows;
        root["cursorX"] = snapshot.cursorX;
        root["cursorY"] = snapshot.cursorY;
        root["text"] = Utf8FromWide(snapshot.text);
        root["timestamp"] = IsoTimestamp(std::chrono::system_clock::now());
        AppendCells(root, snapshot);
        return Serialize(root);
    }

    std::string OkJson()
    {
        ::Json::Value root{ ::Json::objectValue };
        root["ok"] = true;
        return Serialize(root);
    }

    std::string ErrorJson(std::string_view message)
    {
        ::Json::Value root{ ::Json::objectValue };
        root["ok"] = false;
        root["error"] = std::string{ message };
        return Serialize(root);
    }

    std::optional<std::wstring> ParseInputBody(std::string_view body)
    {
        ::Json::Value root;
        if (!Parse(body, root) || !root.isObject() || !root.isMember("data") || !root["data"].isString())
        {
            return std::nullopt;
        }
        return WideFromUtf8(root["data"].asString());
    }

    std::optional<std::string> ParseKeyBody(std::string_view body)
    {
        ::Json::Value root;
        if (!Parse(body, root) || !root.isObject() || !root.isMember("key") || !root["key"].isString())
        {
            return std::nullopt;
        }
        return root["key"].asString();
    }

    bool ParseResizeBody(std::string_view body, int& cols, int& rows)
    {
        ::Json::Value root;
        if (!Parse(body, root) || !root.isObject())
        {
            return false;
        }
        if (!root.isMember("cols") || !root.isMember("rows") || !root["cols"].isInt() || !root["rows"].isInt())
        {
            return false;
        }
        cols = root["cols"].asInt();
        rows = root["rows"].asInt();
        return true;
    }

    std::string WsSnapshotMessage(const std::string& sessionId, const SnapshotData& snapshot)
    {
        auto root = MakeEnvelope("snapshot", sessionId);
        root["cols"] = snapshot.cols;
        root["rows"] = snapshot.rows;
        root["text"] = Utf8FromWide(snapshot.text);
        root["cursorX"] = snapshot.cursorX;
        root["cursorY"] = snapshot.cursorY;
        AppendCells(root, snapshot);
        return Serialize(root);
    }

    std::string WsOutputMessage(const std::string& sessionId, std::wstring_view data)
    {
        auto root = MakeEnvelope("output", sessionId);
        root["data"] = Utf8FromWide(data);
        return Serialize(root);
    }

    std::string WsExitMessage(const std::string& sessionId, int32_t code)
    {
        auto root = MakeEnvelope("exit", sessionId);
        root["code"] = code;
        return Serialize(root);
    }

    std::string WsErrorMessage(const std::string& sessionId, std::string_view message)
    {
        auto root = MakeEnvelope("error", sessionId);
        root["message"] = std::string{ message };
        return Serialize(root);
    }

    bool ParseWsClientMessage(std::string_view json, WsClientMessage& out)
    {
        ::Json::Value root;
        if (!Parse(json, root) || !root.isObject() || !root.isMember("type") || !root["type"].isString())
        {
            return false;
        }
        out.type = root["type"].asString();
        if (out.type == "input")
        {
            if (!root.isMember("data") || !root["data"].isString())
            {
                return false;
            }
            out.data = WideFromUtf8(root["data"].asString());
        }
        else if (out.type == "key")
        {
            if (!root.isMember("key") || !root["key"].isString())
            {
                return false;
            }
            out.key = root["key"].asString();
        }
        else if (out.type == "resize")
        {
            if (!root.isMember("cols") || !root.isMember("rows") || !root["cols"].isInt() || !root["rows"].isInt())
            {
                return false;
            }
            out.cols = root["cols"].asInt();
            out.rows = root["rows"].asInt();
        }
        else
        {
            return false;
        }
        return true;
    }
}

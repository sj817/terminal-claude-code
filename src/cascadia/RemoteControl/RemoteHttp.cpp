// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteHttp.h"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace RemoteControl
{
    static std::string ToLower(std::string_view s)
    {
        std::string out{ s };
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    static std::string_view Trim(std::string_view s)
    {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos)
        {
            return {};
        }
        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::optional<std::string> HttpRequest::Header(std::string_view name) const
    {
        const auto it = headers.find(ToLower(name));
        if (it == headers.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<std::string> HttpRequest::QueryValue(std::string_view name) const
    {
        const auto it = query.find(std::string{ name });
        if (it == query.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    size_t HttpRequest::ContentLength() const
    {
        if (const auto value = Header("content-length"))
        {
            size_t result{ 0 };
            const auto& s = *value;
            const auto trimmed = Trim(s);
            if (std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), result).ec == std::errc{})
            {
                return result;
            }
        }
        return 0;
    }

    std::string UrlDecode(std::string_view input, bool plusAsSpace)
    {
        std::string out;
        out.reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i)
        {
            const char c = input[i];
            if (c == '%' && i + 2 < input.size())
            {
                const auto hexValue = [](char ch) -> int {
                    if (ch >= '0' && ch <= '9')
                    {
                        return ch - '0';
                    }
                    if (ch >= 'a' && ch <= 'f')
                    {
                        return ch - 'a' + 10;
                    }
                    if (ch >= 'A' && ch <= 'F')
                    {
                        return ch - 'A' + 10;
                    }
                    return -1;
                };
                const int hi = hexValue(input[i + 1]);
                const int lo = hexValue(input[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            if (plusAsSpace && c == '+')
            {
                out.push_back(' ');
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    static void ParseQuery(std::string_view queryString, std::map<std::string, std::string>& out)
    {
        size_t pos = 0;
        while (pos < queryString.size())
        {
            auto amp = queryString.find('&', pos);
            if (amp == std::string_view::npos)
            {
                amp = queryString.size();
            }
            const auto pair = queryString.substr(pos, amp - pos);
            const auto eq = pair.find('=');
            if (eq == std::string_view::npos)
            {
                if (!pair.empty())
                {
                    out[UrlDecode(pair, true)] = std::string{};
                }
            }
            else
            {
                out[UrlDecode(pair.substr(0, eq), true)] = UrlDecode(pair.substr(eq + 1), true);
            }
            pos = amp + 1;
        }
    }

    bool ParseRequestHead(std::string_view head, HttpRequest& out)
    {
        // Request line.
        auto lineEnd = head.find("\r\n");
        if (lineEnd == std::string_view::npos)
        {
            return false;
        }
        const auto requestLine = head.substr(0, lineEnd);

        const auto firstSpace = requestLine.find(' ');
        if (firstSpace == std::string_view::npos)
        {
            return false;
        }
        const auto secondSpace = requestLine.find(' ', firstSpace + 1);
        if (secondSpace == std::string_view::npos)
        {
            return false;
        }

        out.method = std::string{ requestLine.substr(0, firstSpace) };
        out.target = std::string{ requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1) };

        // Split target into path + query.
        const auto qpos = out.target.find('?');
        if (qpos == std::string::npos)
        {
            out.path = UrlDecode(out.target);
        }
        else
        {
            out.path = UrlDecode(std::string_view{ out.target }.substr(0, qpos));
            ParseQuery(std::string_view{ out.target }.substr(qpos + 1), out.query);
        }

        // Headers.
        size_t pos = lineEnd + 2;
        while (pos < head.size())
        {
            auto nextEnd = head.find("\r\n", pos);
            if (nextEnd == std::string_view::npos)
            {
                nextEnd = head.size();
            }
            const auto line = head.substr(pos, nextEnd - pos);
            if (line.empty())
            {
                break;
            }
            const auto colon = line.find(':');
            if (colon != std::string_view::npos)
            {
                const auto name = ToLower(Trim(line.substr(0, colon)));
                const auto value = std::string{ Trim(line.substr(colon + 1)) };
                out.headers[name] = value;
            }
            pos = nextEnd + 2;
        }

        return !out.method.empty() && !out.path.empty();
    }

    std::string_view ReasonPhrase(int statusCode)
    {
        switch (statusCode)
        {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 426:
            return "Upgrade Required";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        default:
            return "OK";
        }
    }

    std::string BuildResponse(int statusCode, std::string_view contentType, std::string_view body, std::string_view extraHeaders)
    {
        std::ostringstream os;
        os << "HTTP/1.1 " << statusCode << ' ' << ReasonPhrase(statusCode) << "\r\n";
        if (!contentType.empty())
        {
            os << "Content-Type: " << contentType << "\r\n";
        }
        os << "Content-Length: " << body.size() << "\r\n";
        os << "Connection: close\r\n";
        os << "Cache-Control: no-store\r\n";
        if (!extraHeaders.empty())
        {
            os << extraHeaders;
        }
        os << "\r\n";
        os << body;
        return os.str();
    }

    std::string BuildJsonResponse(int statusCode, std::string_view jsonBody)
    {
        return BuildResponse(statusCode, "application/json; charset=utf-8", jsonBody);
    }
}

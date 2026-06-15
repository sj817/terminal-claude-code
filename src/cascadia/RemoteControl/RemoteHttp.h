// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RemoteHttp.h
//
// A deliberately small HTTP/1.1 request parser and response builder. This is not
// a general-purpose web server; it understands just enough to service the remote
// control API over a localhost socket.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace RemoteControl
{
    struct HttpRequest
    {
        std::string method;
        std::string target; // raw request target, including any query string
        std::string path; // URL-decoded path, without the query string
        std::map<std::string, std::string> query; // decoded query parameters
        std::map<std::string, std::string> headers; // header names are lower-cased
        std::string body;

        std::optional<std::string> Header(std::string_view name) const;
        std::optional<std::string> QueryValue(std::string_view name) const;
        size_t ContentLength() const;
    };

    // Parses the request line and headers from `head` (the bytes up to and
    // including the blank line that terminates the headers). Returns false if the
    // request is malformed. The body is NOT parsed here - the caller reads it
    // separately using ContentLength().
    bool ParseRequestHead(std::string_view head, HttpRequest& out);

    // Percent-decodes a URL component (also turning '+' into space for query
    // values when `plusAsSpace` is true).
    std::string UrlDecode(std::string_view input, bool plusAsSpace = false);

    // Builds a complete HTTP response. `extraHeaders` is appended verbatim and
    // each entry should already end in CRLF.
    std::string BuildResponse(int statusCode,
                              std::string_view contentType,
                              std::string_view body,
                              std::string_view extraHeaders = {});

    // Convenience wrapper that returns an application/json response.
    std::string BuildJsonResponse(int statusCode, std::string_view jsonBody);

    // Returns the canonical reason phrase for a status code (e.g. "Not Found").
    std::string_view ReasonPhrase(int statusCode);
}

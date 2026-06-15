// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteGuard.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace RemoteControl
{
    namespace
    {
        // Lower-cases and trims a token.
        std::wstring Lower(std::wstring_view sv)
        {
            std::wstring s{ sv };
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return s;
        }

        // Splits a command segment into whitespace-delimited tokens, stripping
        // surrounding single/double quotes from each token.
        std::vector<std::wstring> Tokenize(std::wstring_view segment)
        {
            std::vector<std::wstring> tokens;
            size_t i = 0;
            while (i < segment.size())
            {
                while (i < segment.size() && (segment[i] == L' ' || segment[i] == L'\t'))
                {
                    ++i;
                }
                if (i >= segment.size())
                {
                    break;
                }
                const auto start = i;
                while (i < segment.size() && segment[i] != L' ' && segment[i] != L'\t')
                {
                    ++i;
                }
                auto tok = segment.substr(start, i - start);
                // Strip matching surrounding quotes.
                if (tok.size() >= 2 && (tok.front() == L'"' || tok.front() == L'\'') && tok.back() == tok.front())
                {
                    tok = tok.substr(1, tok.size() - 2);
                }
                tokens.emplace_back(Lower(tok));
            }
            return tokens;
        }

        // The "command word" with any path prefix removed (/bin/rm -> rm).
        std::wstring CommandName(const std::wstring& token)
        {
            auto pos = token.find_last_of(L"/\\");
            return pos == std::wstring::npos ? token : token.substr(pos + 1);
        }

        // Drops leading command wrappers (sudo, env VAR=val, time, nice, ...) so
        // "sudo rm -rf /" is matched the same as "rm -rf /".
        std::vector<std::wstring> StripLeadingWrappers(std::vector<std::wstring> tokens)
        {
            static const wchar_t* wrappers[] = { L"sudo", L"doas", L"command", L"env", L"time", L"nice", L"builtin", L"exec", L"setsid", L"stdbuf" };
            size_t i = 0;
            while (i < tokens.size())
            {
                const auto name = CommandName(tokens[i]);
                bool isWrapper = false;
                for (const auto* w : wrappers)
                {
                    if (name == w)
                    {
                        isWrapper = true;
                        break;
                    }
                }
                // An environment assignment like FOO=bar preceding the command.
                if (!isWrapper && !name.empty() && (std::iswalpha(name[0]) || name[0] == L'_') && name.find(L'=') != std::wstring::npos)
                {
                    isWrapper = true;
                }
                if (!isWrapper)
                {
                    break;
                }
                ++i;
            }
            return { tokens.begin() + i, tokens.end() };
        }

        // True if the target string refers to the filesystem root or an entire
        // drive: "/", "/*", "\", "X:", "X:\", "X:\*" (and quoted/trailing-dot
        // variants). NOT true for "/tmp/foo", "./build", "X:\logs", etc.
        bool IsRootTarget(std::wstring t)
        {
            if (t.empty())
            {
                return false;
            }
            // Drop a trailing "*" or "." or "/" or "\" so "/", "/*", "/." all collapse.
            while (t.size() > 1 && (t.back() == L'*' || t.back() == L'.'))
            {
                t.pop_back();
            }
            if (t == L"/" || t == L"\\")
            {
                return true;
            }
            // Drive root: exactly "x:" or "x:\" or "x:/".
            if (t.size() == 2 && std::iswalpha(t[0]) && t[1] == L':')
            {
                return true;
            }
            if (t.size() == 3 && std::iswalpha(t[0]) && t[1] == L':' && (t[2] == L'\\' || t[2] == L'/'))
            {
                return true;
            }
            // Environment-based system root references.
            if (t == L"%systemdrive%" || t == L"%systemroot%" || t == L"$env:systemdrive" || t == L"$env:systemroot")
            {
                return true;
            }
            return false;
        }

        bool HasFlag(const std::vector<std::wstring>& tokens, size_t from, std::wstring_view shortChars, std::initializer_list<std::wstring_view> longFlags)
        {
            for (size_t i = from; i < tokens.size(); ++i)
            {
                const auto& t = tokens[i];
                if (t.size() >= 2 && t[0] == L'-' && t[1] != L'-')
                {
                    // Short flag bundle like -rf: require all requested chars present.
                    bool all = true;
                    for (const auto c : shortChars)
                    {
                        if (t.find(c, 1) == std::wstring::npos)
                        {
                            all = false;
                            break;
                        }
                    }
                    if (all && !shortChars.empty())
                    {
                        return true;
                    }
                }
                for (const auto& lf : longFlags)
                {
                    if (t == lf)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        bool AnyRootTarget(const std::vector<std::wstring>& tokens, size_t from)
        {
            for (size_t i = from; i < tokens.size(); ++i)
            {
                // Skip POSIX option flags ("-rf"). Windows switches like "/s" are
                // not skipped here, but IsRootTarget rejects them anyway while
                // still accepting POSIX targets such as "/" and "/*".
                if (!tokens[i].empty() && tokens[i][0] == L'-')
                {
                    continue;
                }
                if (IsRootTarget(tokens[i]))
                {
                    return true;
                }
            }
            return false;
        }

        // POSIX: rm -r ... /     (recursive required; force optional)
        bool MatchRm(const std::vector<std::wstring>& t)
        {
            if (t.empty())
            {
                return false;
            }
            const auto cmd = CommandName(t[0]);
            if (cmd != L"rm")
            {
                return false;
            }
            const bool recursive = HasFlag(t, 1, L"r", { L"--recursive", L"-r", L"--no-preserve-root" });
            if (!recursive)
            {
                return false;
            }
            return AnyRootTarget(t, 1);
        }

        // PowerShell: Remove-Item -Recurse ... C:\   (and aliases ri/rmdir/rd/rm/del)
        bool MatchPowerShellRemove(const std::vector<std::wstring>& t)
        {
            if (t.empty())
            {
                return false;
            }
            const auto cmd = CommandName(t[0]);
            if (cmd != L"remove-item" && cmd != L"ri")
            {
                return false;
            }
            const bool recursive = HasFlag(t, 1, L"", { L"-recurse", L"-r" });
            if (!recursive)
            {
                return false;
            }
            return AnyRootTarget(t, 1);
        }

        // CMD: "del /s ... <drive root>" or "rd /s ... <drive root>".
        bool MatchCmdDelete(const std::vector<std::wstring>& t)
        {
            if (t.empty())
            {
                return false;
            }
            const auto cmd = CommandName(t[0]);
            const bool isDel = (cmd == L"del" || cmd == L"erase");
            const bool isRd = (cmd == L"rd" || cmd == L"rmdir");
            if (!isDel && !isRd)
            {
                return false;
            }
            // /s = recurse subdirectories.
            bool recursive = false;
            for (size_t i = 1; i < t.size(); ++i)
            {
                if (t[i] == L"/s" || t[i] == L"/s/q" || t[i] == L"/q/s")
                {
                    recursive = true;
                }
            }
            if (!recursive)
            {
                return false;
            }
            return AnyRootTarget(t, 1);
        }

        // format C:   (any drive)
        bool MatchFormat(const std::vector<std::wstring>& t)
        {
            if (t.empty())
            {
                return false;
            }
            if (CommandName(t[0]) != L"format")
            {
                return false;
            }
            for (size_t i = 1; i < t.size(); ++i)
            {
                if (IsRootTarget(t[i]))
                {
                    return true;
                }
            }
            return false;
        }
    }

    std::optional<std::string> MatchCatastrophicCommand(std::wstring_view input)
    {
        // Split into command segments on separators so "echo hi && rm -rf /" is
        // still caught.
        std::wstring segment;
        auto flush = [&segment]() -> std::optional<std::string> {
            const auto tokens = StripLeadingWrappers(Tokenize(segment));
            segment.clear();
            if (MatchRm(tokens))
            {
                return "rm-root";
            }
            if (MatchPowerShellRemove(tokens))
            {
                return "remove-item-root";
            }
            if (MatchCmdDelete(tokens))
            {
                return "del-root";
            }
            if (MatchFormat(tokens))
            {
                return "format-drive";
            }
            return std::nullopt;
        };

        for (const auto c : input)
        {
            if (c == L'\r' || c == L'\n' || c == L';' || c == L'&' || c == L'|')
            {
                if (auto hit = flush())
                {
                    return hit;
                }
            }
            else
            {
                segment.push_back(c);
            }
        }
        return flush();
    }
}

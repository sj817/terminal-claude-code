// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteSessionRegistry.h"

namespace RemoteControl
{
    RemoteSessionRegistry& RemoteSessionRegistry::Instance()
    {
        static RemoteSessionRegistry instance;
        return instance;
    }

    void RemoteSessionRegistry::Register(const std::string& sessionId, RegisterInfo info, Backend backend)
    {
        std::lock_guard<std::mutex> guard{ _mutex };
        auto& entry = _sessions[sessionId];
        entry.info = std::move(info);
        entry.backend = std::move(backend);
        if (entry.createdAt.time_since_epoch().count() == 0)
        {
            entry.createdAt = std::chrono::system_clock::now();
        }
    }

    void RemoteSessionRegistry::Unregister(const std::string& sessionId)
    {
        // Move out the subscribers so we can notify them of the exit without the
        // lock held.
        std::map<uint64_t, Subscriber> subscribers;
        {
            std::lock_guard<std::mutex> guard{ _mutex };
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return;
            }
            subscribers = std::move(it->second.subscribers);
            _sessions.erase(it);
            if (_focusedSessionId == sessionId)
            {
                _focusedSessionId.clear();
            }
        }

        for (auto& [token, sub] : subscribers)
        {
            if (sub.onExit)
            {
                sub.onExit(-1);
            }
        }
    }

    void RemoteSessionRegistry::OnOutput(const std::string& sessionId, std::wstring_view data)
    {
        std::vector<OutputCallback> callbacks;
        {
            std::lock_guard<std::mutex> guard{ _mutex };
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return;
            }
            it->second.lastOutputAt = std::chrono::system_clock::now();
            callbacks.reserve(it->second.subscribers.size());
            for (auto& [token, sub] : it->second.subscribers)
            {
                if (sub.onOutput)
                {
                    callbacks.push_back(sub.onOutput);
                }
            }
        }

        for (auto& cb : callbacks)
        {
            cb(data);
        }
    }

    void RemoteSessionRegistry::OnExit(const std::string& sessionId, int32_t code)
    {
        std::vector<ExitCallback> callbacks;
        {
            std::lock_guard<std::mutex> guard{ _mutex };
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return;
            }
            callbacks.reserve(it->second.subscribers.size());
            for (auto& [token, sub] : it->second.subscribers)
            {
                if (sub.onExit)
                {
                    callbacks.push_back(sub.onExit);
                }
            }
        }

        for (auto& cb : callbacks)
        {
            cb(code);
        }
    }

    void RemoteSessionRegistry::SetFocused(const std::string& sessionId)
    {
        std::lock_guard<std::mutex> guard{ _mutex };
        _focusedSessionId = sessionId;
    }

    std::vector<SessionMetadata> RemoteSessionRegistry::ListSessions()
    {
        // Snapshot the static fields + backends under the lock, then fill the
        // dynamic fields outside the lock.
        struct Pending
        {
            SessionMetadata meta;
            std::function<void(SessionMetadata&)> fillMetadata;
        };
        std::vector<Pending> pending;

        {
            std::lock_guard<std::mutex> guard{ _mutex };
            pending.reserve(_sessions.size());
            for (auto& [id, entry] : _sessions)
            {
                SessionMetadata meta;
                meta.sessionId = id;
                meta.windowId = entry.info.windowId;
                meta.tabId = entry.info.tabId;
                meta.paneId = entry.info.paneId;
                meta.profileName = entry.info.profileName;
                meta.createdAt = entry.createdAt;
                meta.lastOutputAt = entry.lastOutputAt;
                meta.remoteAttachedCount = static_cast<int>(entry.subscribers.size());
                meta.isFocused = (id == _focusedSessionId);
                pending.push_back({ std::move(meta), entry.backend.fillMetadata });
            }
        }

        std::vector<SessionMetadata> result;
        result.reserve(pending.size());
        for (auto& p : pending)
        {
            if (p.fillMetadata)
            {
                p.fillMetadata(p.meta);
            }
            result.push_back(std::move(p.meta));
        }
        return result;
    }

    bool RemoteSessionRegistry::TryGetSession(const std::string& sessionId, SessionMetadata& out)
    {
        SessionMetadata meta;
        std::function<void(SessionMetadata&)> fillMetadata;
        {
            std::lock_guard<std::mutex> guard{ _mutex };
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return false;
            }
            meta.sessionId = sessionId;
            meta.windowId = it->second.info.windowId;
            meta.tabId = it->second.info.tabId;
            meta.paneId = it->second.info.paneId;
            meta.profileName = it->second.info.profileName;
            meta.createdAt = it->second.createdAt;
            meta.lastOutputAt = it->second.lastOutputAt;
            meta.remoteAttachedCount = static_cast<int>(it->second.subscribers.size());
            meta.isFocused = (sessionId == _focusedSessionId);
            fillMetadata = it->second.backend.fillMetadata;
        }

        if (fillMetadata)
        {
            fillMetadata(meta);
        }
        out = std::move(meta);
        return true;
    }

    bool RemoteSessionRegistry::WriteInput(const std::string& sessionId, std::wstring_view data)
    {
        std::function<bool(std::wstring_view)> writeInput;
        {
            std::lock_guard<std::mutex> guard{ _mutex };
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return false;
            }
            writeInput = it->second.backend.writeInput;
        }
        return writeInput ? writeInput(data) : false;
    }

    bool RemoteSessionRegistry::GetSnapshot(const std::string& sessionId, bool includeColor, SnapshotData& out)
    {
        std::function<bool(bool, SnapshotData&)> getSnapshot;
        {
            std::lock_guard<std::mutex> guard{ _mutex };
            const auto it = _sessions.find(sessionId);
            if (it == _sessions.end())
            {
                return false;
            }
            getSnapshot = it->second.backend.getSnapshot;
        }
        return getSnapshot ? getSnapshot(includeColor, out) : false;
    }

    bool RemoteSessionRegistry::Resize(const std::string& sessionId, int /*cols*/, int /*rows*/)
    {
        // This version only validates that the session exists; it does not resize
        // the underlying PTY so that the local display is never disturbed.
        std::lock_guard<std::mutex> guard{ _mutex };
        return _sessions.find(sessionId) != _sessions.end();
    }

    uint64_t RemoteSessionRegistry::Subscribe(const std::string& sessionId, OutputCallback onOutput, ExitCallback onExit)
    {
        std::lock_guard<std::mutex> guard{ _mutex };
        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end())
        {
            return 0;
        }
        const uint64_t token = _nextToken++;
        it->second.subscribers.emplace(token, Subscriber{ std::move(onOutput), std::move(onExit) });
        return token;
    }

    void RemoteSessionRegistry::Unsubscribe(const std::string& sessionId, uint64_t token)
    {
        std::lock_guard<std::mutex> guard{ _mutex };
        const auto it = _sessions.find(sessionId);
        if (it == _sessions.end())
        {
            return;
        }
        it->second.subscribers.erase(token);
    }
}

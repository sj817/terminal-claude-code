// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RemoteApiServer.h"

#include "RemoteHttp.h"
#include "RemoteWebSocket.h"
#include "RemoteJson.h"
#include "RemoteKeys.h"
#include "RemoteAuth.h"
#include "RemoteGuard.h"

#include <deque>
#include <memory>
#include <sstream>
#include <vector>

namespace RemoteControl
{
    namespace
    {
        // The header stores socket handles as uintptr_t to avoid pulling winsock
        // into its includers; these helpers convert at the boundary.
        constexpr uintptr_t InvalidSocketHandle = ~static_cast<uintptr_t>(0);
        inline SOCKET AsSocket(uintptr_t h) noexcept { return static_cast<SOCKET>(h); }
        inline uintptr_t AsHandle(SOCKET s) noexcept { return static_cast<uintptr_t>(s); }

        constexpr size_t MaxHeaderBytes = 64 * 1024;
        constexpr size_t MaxBodyBytes = 4 * 1024 * 1024;
        // Cap the per-connection outbound queue so a slow remote reader can never
        // cause unbounded memory growth (and never back-pressures the terminal).
        constexpr size_t MaxQueuedBytes = 8 * 1024 * 1024;

        bool SendAll(SOCKET s, std::string_view data)
        {
            size_t sent = 0;
            while (sent < data.size())
            {
                const int chunk = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
                if (chunk == SOCKET_ERROR || chunk == 0)
                {
                    return false;
                }
                sent += static_cast<size_t>(chunk);
            }
            return true;
        }

        // Reads from the socket until `delimiter` is found, returning the index
        // just past the delimiter, or 0 on error/limit. Already-received bytes
        // accumulate in `buffer`.
        size_t RecvUntil(SOCKET s, std::string& buffer, std::string_view delimiter, size_t maxBytes)
        {
            char temp[8192];
            for (;;)
            {
                if (const auto pos = buffer.find(delimiter); pos != std::string::npos)
                {
                    return pos + delimiter.size();
                }
                if (buffer.size() > maxBytes)
                {
                    return 0;
                }
                const int received = ::recv(s, temp, sizeof(temp), 0);
                if (received <= 0)
                {
                    return 0;
                }
                buffer.append(temp, static_cast<size_t>(received));
            }
        }

        bool RecvExact(SOCKET s, std::string& buffer, size_t needed)
        {
            char temp[8192];
            while (buffer.size() < needed)
            {
                const int received = ::recv(s, temp, sizeof(temp), 0);
                if (received <= 0)
                {
                    return false;
                }
                buffer.append(temp, static_cast<size_t>(received));
            }
            return true;
        }

        // Color is included by default; a client can opt out with ?color=0.
        bool WantsColor(const HttpRequest& request)
        {
            if (const auto value = request.QueryValue("color"))
            {
                return *value != "0" && *value != "false";
            }
            return true;
        }

        std::vector<std::string> SplitPath(const std::string& path)
        {
            std::vector<std::string> segments;
            size_t pos = 0;
            while (pos < path.size())
            {
                if (path[pos] == '/')
                {
                    ++pos;
                    continue;
                }
                const auto next = path.find('/', pos);
                const auto end = next == std::string::npos ? path.size() : next;
                segments.push_back(path.substr(pos, end - pos));
                pos = end;
            }
            return segments;
        }

        // Formats an accepted peer address as "ip:port" for logging.
        std::string FormatPeer(const sockaddr_storage& addr)
        {
            char host[INET6_ADDRSTRLEN] = {};
            char serv[16] = {};
            if (getnameinfo(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr), host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
            {
                return std::string{ host } + ":" + serv;
            }
            return "unknown";
        }
    }

    RemoteApiServer::RemoteApiServer(RemoteConfig config, IRemoteHost* host, std::shared_ptr<RemoteLog> log) :
        _config{ std::move(config) },
        _host{ host },
        _log{ std::move(log) }
    {
    }

    RemoteApiServer::~RemoteApiServer()
    {
        Stop();
    }

    bool RemoteApiServer::IsRunning() const noexcept
    {
        return _running.load(std::memory_order_acquire);
    }

    void RemoteApiServer::_logInfo(std::string_view event, std::string_view fields) const
    {
        if (_log)
        {
            _log->Info(event, fields);
        }
        else
        {
            OutputDebugStringA(("RemoteControl: " + std::string{ event } + " " + std::string{ fields } + "\n").c_str());
        }
    }

    void RemoteApiServer::_logWarn(std::string_view event, std::string_view fields) const
    {
        if (_log)
        {
            _log->Warn(event, fields);
        }
        else
        {
            OutputDebugStringA(("RemoteControl: " + std::string{ event } + " " + std::string{ fields } + "\n").c_str());
        }
    }

    void RemoteApiServer::_closeAllConnections()
    {
        std::lock_guard<std::mutex> guard{ _socketsMutex };
        for (const auto s : _activeSockets)
        {
            shutdown(AsSocket(s), SD_BOTH);
            closesocket(AsSocket(s));
        }
        _activeSockets.clear();
    }

    void RemoteApiServer::_enterLockdown(std::string_view reason, const std::string& sessionId, const std::string& peer)
    {
        const bool wasLocked = _lockdown.exchange(true, std::memory_order_acq_rel);
        std::ostringstream fields;
        fields << "reason=" << reason << " sessionId=" << sessionId << " peer=" << peer;
        if (_log)
        {
            _log->Critical("lockdown.destructiveCommandBlocked", fields.str());
        }
        if (!wasLocked)
        {
            // Drop every active connection. New requests will get 403 until the
            // server is restarted (or the remoteControl setting toggled).
            _closeAllConnections();
        }
    }

    bool RemoteApiServer::_guardInput(const std::string& sessionId, const std::string& peer, std::wstring_view data)
    {
        if (const auto hit = MatchCatastrophicCommand(data))
        {
            _enterLockdown(*hit, sessionId, peer);
            return false;
        }
        return true;
    }

    bool RemoteApiServer::Start()
    {
        if (_running.load(std::memory_order_acquire))
        {
            return true;
        }

        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            _logWarn("server.startFailed", "stage=WSAStartup");
            return false;
        }
        _wsaStarted = true;

        if (!_config.IsLoopbackHost())
        {
            _logWarn("server.nonLoopbackBind", "host=" + _config.host + " note=reachable-from-network");
        }

        // Resolve the bind address. AI_NUMERICHOST keeps us from doing surprise
        // DNS lookups; the host is expected to be a literal IP for loopback.
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

        const std::string portStr = std::to_string(_config.port);
        addrinfo* result = nullptr;
        if (getaddrinfo(_config.host.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr)
        {
            _logWarn("server.startFailed", "stage=getaddrinfo host=" + _config.host);
            WSACleanup();
            _wsaStarted = false;
            return false;
        }

        const SOCKET listenSocket = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (listenSocket == INVALID_SOCKET)
        {
            _logWarn("server.startFailed", "stage=socket");
            freeaddrinfo(result);
            WSACleanup();
            _wsaStarted = false;
            return false;
        }
        _listenSocket = AsHandle(listenSocket);

        if (::bind(listenSocket, result->ai_addr, static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR)
        {
            _logWarn("server.startFailed", "stage=bind port=" + std::to_string(_config.port) + " note=port-in-use?");
            freeaddrinfo(result);
            closesocket(listenSocket);
            _listenSocket = InvalidSocketHandle;
            WSACleanup();
            _wsaStarted = false;
            return false;
        }
        freeaddrinfo(result);

        if (::listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
        {
            _logWarn("server.startFailed", "stage=listen");
            closesocket(listenSocket);
            _listenSocket = InvalidSocketHandle;
            WSACleanup();
            _wsaStarted = false;
            return false;
        }

        _running.store(true, std::memory_order_release);
        _acceptThread = std::thread{ &RemoteApiServer::_acceptLoop, this };

        _logInfo("server.listening", "host=" + _config.host + " port=" + std::to_string(_config.port) + " allowInput=" + (_config.allowInput ? "1" : "0"));
        return true;
    }

    void RemoteApiServer::Stop()
    {
        if (!_running.exchange(false, std::memory_order_acq_rel))
        {
            // Never started (or already stopped). Still clean up WSA if needed.
            if (_wsaStarted)
            {
                WSACleanup();
                _wsaStarted = false;
            }
            return;
        }

        // Close the listening socket to break the accept loop.
        if (_listenSocket != InvalidSocketHandle)
        {
            closesocket(AsSocket(_listenSocket));
            _listenSocket = InvalidSocketHandle;
        }

        // Force all active client sockets closed to unblock their recv() calls.
        {
            std::lock_guard<std::mutex> guard{ _socketsMutex };
            for (const auto s : _activeSockets)
            {
                shutdown(AsSocket(s), SD_BOTH);
                closesocket(AsSocket(s));
            }
            _activeSockets.clear();
        }

        if (_acceptThread.joinable())
        {
            _acceptThread.join();
        }

        // Wait for any in-flight connection handlers to finish.
        {
            std::unique_lock<std::mutex> lock{ _connMutex };
            _connCv.wait(lock, [this] { return _activeConnections == 0; });
        }

        if (_wsaStarted)
        {
            WSACleanup();
            _wsaStarted = false;
        }

        _logInfo("server.stopped");
    }

    void RemoteApiServer::_trackSocket(uintptr_t s)
    {
        std::lock_guard<std::mutex> guard{ _socketsMutex };
        _activeSockets.insert(s);
    }

    void RemoteApiServer::_untrackSocket(uintptr_t s)
    {
        std::lock_guard<std::mutex> guard{ _socketsMutex };
        _activeSockets.erase(s);
    }

    void RemoteApiServer::_acceptLoop()
    {
        while (_running.load(std::memory_order_acquire))
        {
            sockaddr_storage addr{};
            int addrLen = sizeof(addr);
            const SOCKET client = ::accept(AsSocket(_listenSocket), reinterpret_cast<sockaddr*>(&addr), &addrLen);
            if (client == INVALID_SOCKET)
            {
                if (!_running.load(std::memory_order_acquire))
                {
                    break;
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lock{ _connMutex };
                ++_activeConnections;
            }
            const uintptr_t clientHandle = AsHandle(client);
            const std::string peer = FormatPeer(addr);
            _trackSocket(clientHandle);

            // Each connection is handled on its own detached thread. Stop() waits
            // on _activeConnections rather than joining these directly.
            std::thread{ [this, clientHandle, client, peer] {
                _handleConnection(clientHandle, peer);
                _untrackSocket(clientHandle);
                shutdown(client, SD_BOTH);
                closesocket(client);
                {
                    std::lock_guard<std::mutex> lock{ _connMutex };
                    --_activeConnections;
                }
                _connCv.notify_all();
            } }.detach();
        }
    }

    void RemoteApiServer::_handleConnection(uintptr_t clientHandle, const std::string& peer)
    {
        const SOCKET client = AsSocket(clientHandle);
        std::string buffer;
        const size_t headEnd = RecvUntil(client, buffer, "\r\n\r\n", MaxHeaderBytes);
        if (headEnd == 0)
        {
            return;
        }

        HttpRequest request;
        if (!ParseRequestHead(std::string_view{ buffer }.substr(0, headEnd), request))
        {
            SendAll(client, BuildJsonResponse(400, Json::ErrorJson("malformed request")));
            return;
        }

        // Read the body, if any. Anything already read past the header start is
        // the beginning of the body.
        const size_t contentLength = request.ContentLength();
        if (contentLength > MaxBodyBytes)
        {
            SendAll(client, BuildJsonResponse(400, Json::ErrorJson("request body too large")));
            return;
        }
        if (contentLength > 0)
        {
            std::string body = buffer.substr(headEnd);
            if (!RecvExact(client, body, contentLength))
            {
                return;
            }
            request.body = body.substr(0, contentLength);
        }

        // Authentication applies to every request, HTTP and WebSocket alike.
        std::optional<std::string> headerToken;
        if (const auto authHeader = request.Header("authorization"))
        {
            headerToken = ExtractBearerToken(*authHeader);
        }
        const auto queryToken = request.QueryValue("token");
        if (!IsAuthorized(_config.token, headerToken, queryToken))
        {
            _logWarn("auth.denied", "peer=" + peer + " path=" + request.path);
            SendAll(client, BuildJsonResponse(401, Json::ErrorJson("missing or invalid token")));
            return;
        }

        // If a destructive command tripped the kill-switch, refuse everything
        // until the server is restarted.
        if (_lockdown.load(std::memory_order_acquire))
        {
            SendAll(client, BuildJsonResponse(403, Json::ErrorJson("server locked down after a destructive command was blocked")));
            return;
        }

        // WebSocket streaming is routed separately so it can perform the upgrade
        // handshake instead of producing a normal response.
        if (IsWebSocketUpgrade(request))
        {
            const auto segments = SplitPath(request.path);
            if (segments.size() == 4 && segments[0] == "v1" && segments[1] == "sessions" && segments[3] == "stream")
            {
                if (!_config.allowWebSocket)
                {
                    SendAll(client, BuildJsonResponse(403, Json::ErrorJson("websocket disabled")));
                    return;
                }
                _handleWebSocketStream(clientHandle, peer, request, segments[2]);
                return;
            }
            SendAll(client, BuildJsonResponse(404, Json::ErrorJson("unknown websocket endpoint")));
            return;
        }

        _handleHttp(clientHandle, peer, request);
    }

    void RemoteApiServer::_handleHttp(uintptr_t clientHandle, const std::string& peer, const HttpRequest& request)
    {
        const SOCKET client = AsSocket(clientHandle);
        const auto segments = SplitPath(request.path);
        const auto& method = request.method;

        // GET /v1/health
        if (segments.size() == 2 && segments[0] == "v1" && segments[1] == "health")
        {
            if (method != "GET")
            {
                SendAll(client, BuildJsonResponse(405, Json::ErrorJson("method not allowed")));
                return;
            }
            SendAll(client, BuildJsonResponse(200, Json::HealthJson()));
            return;
        }

        // Everything else lives under /v1/sessions
        if (segments.size() < 2 || segments[0] != "v1" || segments[1] != "sessions")
        {
            SendAll(client, BuildJsonResponse(404, Json::ErrorJson("not found")));
            return;
        }

        // GET /v1/sessions
        if (segments.size() == 2)
        {
            if (method != "GET")
            {
                SendAll(client, BuildJsonResponse(405, Json::ErrorJson("method not allowed")));
                return;
            }
            SendAll(client, BuildJsonResponse(200, Json::SessionsListJson(_host->ListSessions())));
            return;
        }

        const std::string& sessionId = segments[2];

        // GET /v1/sessions/:id
        if (segments.size() == 3)
        {
            if (method != "GET")
            {
                SendAll(client, BuildJsonResponse(405, Json::ErrorJson("method not allowed")));
                return;
            }
            SessionMetadata meta;
            if (!_host->TryGetSession(sessionId, meta))
            {
                SendAll(client, BuildJsonResponse(404, Json::ErrorJson("session not found")));
                return;
            }
            SendAll(client, BuildJsonResponse(200, Json::SessionJson(meta)));
            return;
        }

        if (segments.size() != 4)
        {
            SendAll(client, BuildJsonResponse(404, Json::ErrorJson("not found")));
            return;
        }

        const std::string& action = segments[3];

        // GET /v1/sessions/:id/snapshot
        if (action == "snapshot")
        {
            if (method != "GET")
            {
                SendAll(client, BuildJsonResponse(405, Json::ErrorJson("method not allowed")));
                return;
            }
            if (!_config.allowSnapshot)
            {
                SendAll(client, BuildJsonResponse(403, Json::ErrorJson("snapshot disabled")));
                return;
            }
            SnapshotData snapshot;
            if (!_host->GetSnapshot(sessionId, WantsColor(request), snapshot))
            {
                SendAll(client, BuildJsonResponse(404, Json::ErrorJson("session not found")));
                return;
            }
            SendAll(client, BuildJsonResponse(200, Json::SnapshotJson(sessionId, snapshot)));
            return;
        }

        // The remaining actions are all POST.
        if (method != "POST")
        {
            SendAll(client, BuildJsonResponse(405, Json::ErrorJson("method not allowed")));
            return;
        }

        // POST /v1/sessions/:id/close - not implemented in this version.
        if (action == "close")
        {
            SendAll(client, BuildJsonResponse(501, Json::ErrorJson("close not implemented")));
            return;
        }

        // POST /v1/sessions/:id/resize - records the desired remote view size.
        if (action == "resize")
        {
            int cols = 0;
            int rows = 0;
            if (!Json::ParseResizeBody(request.body, cols, rows))
            {
                SendAll(client, BuildJsonResponse(400, Json::ErrorJson("expected { cols, rows }")));
                return;
            }
            if (!_host->Resize(sessionId, cols, rows))
            {
                SendAll(client, BuildJsonResponse(404, Json::ErrorJson("session not found")));
                return;
            }
            SendAll(client, BuildJsonResponse(200, Json::OkJson()));
            return;
        }

        // input / key / interrupt are all writes; gate them on allowInput.
        if (action == "input" || action == "key" || action == "interrupt")
        {
            if (!_config.allowInput)
            {
                SendAll(client, BuildJsonResponse(403, Json::ErrorJson("input disabled")));
                return;
            }

            std::wstring toWrite;
            if (action == "input")
            {
                const auto data = Json::ParseInputBody(request.body);
                if (!data)
                {
                    SendAll(client, BuildJsonResponse(400, Json::ErrorJson("expected { data }")));
                    return;
                }
                toWrite = *data;
            }
            else if (action == "key")
            {
                const auto keyName = Json::ParseKeyBody(request.body);
                if (!keyName)
                {
                    SendAll(client, BuildJsonResponse(400, Json::ErrorJson("expected { key }")));
                    return;
                }
                const auto seq = KeyNameToSequence(*keyName);
                if (!seq)
                {
                    SendAll(client, BuildJsonResponse(400, Json::ErrorJson("unknown key")));
                    return;
                }
                toWrite = *seq;
            }
            else // interrupt
            {
                toWrite = L"\x03";
            }

            // Kill-switch: refuse whole-system/whole-drive destructive commands.
            if (!_guardInput(sessionId, peer, toWrite))
            {
                SendAll(client, BuildJsonResponse(403, Json::ErrorJson("destructive command blocked; server locked down")));
                return;
            }

            if (!_host->WriteInput(sessionId, toWrite))
            {
                SendAll(client, BuildJsonResponse(404, Json::ErrorJson("session not found")));
                return;
            }

            // Log the source and size of remote input, but never its contents.
            _logInfo("input", "session=" + sessionId + " action=" + action + " peer=" + peer + " chars=" + std::to_string(toWrite.size()));

            SendAll(client, BuildJsonResponse(200, Json::OkJson()));
            return;
        }

        SendAll(client, BuildJsonResponse(404, Json::ErrorJson("not found")));
    }

    void RemoteApiServer::_handleWebSocketStream(uintptr_t clientHandle, const std::string& peer, const HttpRequest& request, const std::string& sessionId)
    {
        const SOCKET client = AsSocket(clientHandle);
        SessionMetadata meta;
        if (!_host->TryGetSession(sessionId, meta))
        {
            SendAll(client, BuildJsonResponse(404, Json::ErrorJson("session not found")));
            return;
        }

        const auto key = request.Header("sec-websocket-key");
        if (!key)
        {
            SendAll(client, BuildJsonResponse(400, Json::ErrorJson("missing Sec-WebSocket-Key")));
            return;
        }
        if (!SendAll(client, BuildHandshakeResponse(ComputeAcceptKey(*key))))
        {
            return;
        }

        // Shared outbound state between this (reader) thread, the writer thread,
        // and the host's output callbacks. Held by shared_ptr so the callbacks
        // remain safe even if they race teardown.
        struct StreamState
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<std::string> frames; // already-encoded WS frames
            size_t queuedBytes{ 0 };
            bool closing{ false };
            bool overflowed{ false };
        };
        auto state = std::make_shared<StreamState>();

        const auto enqueue = [state](std::string frame) {
            std::lock_guard<std::mutex> guard{ state->mutex };
            if (state->closing)
            {
                return;
            }
            if (state->queuedBytes + frame.size() > MaxQueuedBytes)
            {
                state->overflowed = true;
                state->closing = true;
                state->cv.notify_all();
                return;
            }
            state->queuedBytes += frame.size();
            state->frames.push_back(std::move(frame));
            state->cv.notify_all();
        };

        // Send an initial snapshot frame.
        {
            SnapshotData snapshot;
            if (_host->GetSnapshot(sessionId, WantsColor(request), snapshot))
            {
                enqueue(EncodeTextFrame(Json::WsSnapshotMessage(sessionId, snapshot)));
            }
        }

        // Subscribe to the session's output.
        const auto onOutput = [state, enqueue, sessionId](std::wstring_view data) {
            enqueue(EncodeTextFrame(Json::WsOutputMessage(sessionId, data)));
        };
        const auto onExit = [state, enqueue, sessionId](int32_t code) {
            enqueue(EncodeTextFrame(Json::WsExitMessage(sessionId, code)));
            std::lock_guard<std::mutex> guard{ state->mutex };
            state->closing = true;
            state->cv.notify_all();
        };

        const uint64_t token = _host->Subscribe(sessionId, onOutput, onExit);
        if (token == 0)
        {
            SendAll(client, EncodeTextFrame(Json::WsErrorMessage(sessionId, "session not found")));
            SendAll(client, EncodeCloseFrame());
            return;
        }

        // Writer thread: drains the queue and sends frames.
        std::thread writer{ [this, client, state, sessionId] {
            for (;;)
            {
                std::deque<std::string> batch;
                bool overflowed = false;
                {
                    std::unique_lock<std::mutex> lock{ state->mutex };
                    state->cv.wait(lock, [&] { return !state->frames.empty() || state->closing; });
                    batch.swap(state->frames);
                    state->queuedBytes = 0;
                    overflowed = state->overflowed;
                }

                for (auto& frame : batch)
                {
                    if (!SendAll(client, frame))
                    {
                        std::lock_guard<std::mutex> guard{ state->mutex };
                        state->closing = true;
                        return;
                    }
                }

                if (overflowed)
                {
                    SendAll(client, EncodeTextFrame(Json::WsErrorMessage(sessionId, "output buffer overflow; disconnecting")));
                    SendAll(client, EncodeCloseFrame());
                    return;
                }

                std::lock_guard<std::mutex> guard{ state->mutex };
                if (state->closing && state->frames.empty())
                {
                    SendAll(client, EncodeCloseFrame());
                    return;
                }
            }
        } };

        // Reader loop: this thread reads client frames (input/key/resize).
        std::string buffer;
        char temp[8192];
        bool readerDone = false;
        while (!readerDone)
        {
            WsDecodedFrame frame;
            int consumed = TryDecodeFrame(buffer, frame);
            if (consumed < 0)
            {
                break;
            }
            if (consumed == 0)
            {
                const int received = ::recv(client, temp, sizeof(temp), 0);
                if (received <= 0)
                {
                    break;
                }
                buffer.append(temp, static_cast<size_t>(received));
                continue;
            }

            buffer.erase(0, static_cast<size_t>(consumed));

            switch (frame.opcode)
            {
            case WsOpcode::Close:
                readerDone = true;
                break;
            case WsOpcode::Ping:
                enqueue(EncodePongFrame(frame.payload));
                break;
            case WsOpcode::Pong:
                break;
            case WsOpcode::Text:
            case WsOpcode::Binary:
            {
                Json::WsClientMessage msg;
                if (Json::ParseWsClientMessage(frame.payload, msg))
                {
                    if (msg.type == "input" && _config.allowInput)
                    {
                        // Kill-switch on the WebSocket input path too.
                        if (!_guardInput(sessionId, peer, msg.data))
                        {
                            SendAll(client, EncodeTextFrame(Json::WsErrorMessage(sessionId, "destructive command blocked; server locked down")));
                            SendAll(client, EncodeCloseFrame());
                            readerDone = true;
                            break;
                        }
                        _host->WriteInput(sessionId, msg.data);
                        _logInfo("input", "transport=ws session=" + sessionId + " peer=" + peer + " chars=" + std::to_string(msg.data.size()));
                    }
                    else if (msg.type == "key" && _config.allowInput)
                    {
                        if (const auto seq = KeyNameToSequence(msg.key))
                        {
                            _host->WriteInput(sessionId, *seq);
                        }
                    }
                    else if (msg.type == "resize")
                    {
                        _host->Resize(sessionId, msg.cols, msg.rows);
                    }
                }
                break;
            }
            default:
                break;
            }

            // Stop reading if the writer side has decided to close.
            {
                std::lock_guard<std::mutex> guard{ state->mutex };
                if (state->closing)
                {
                    readerDone = true;
                }
            }
        }

        // Tear down: stop further callbacks, wake the writer, and join it.
        _host->Unsubscribe(sessionId, token);
        {
            std::lock_guard<std::mutex> guard{ state->mutex };
            state->closing = true;
            state->cv.notify_all();
        }
        if (writer.joinable())
        {
            writer.join();
        }
    }
}

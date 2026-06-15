// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "RemoteKeys.h"
#include "RemoteAuth.h"
#include "RemoteHttp.h"
#include "RemoteWebSocket.h"
#include "RemoteJson.h"
#include "RemoteGuard.h"
#include "RemoteCrypto.h"

using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace RemoteControlUnitTests
{
    class RemoteControlTests
    {
        TEST_CLASS(RemoteControlTests);

        TEST_METHOD(KeyNameToSequence_KnownKeys);
        TEST_METHOD(KeyNameToSequence_UnknownAndCase);
        TEST_METHOD(Auth_ConstantTimeEquals);
        TEST_METHOD(Auth_ExtractBearerToken);
        TEST_METHOD(Auth_IsAuthorized);
        TEST_METHOD(Http_ParseRequestHead);
        TEST_METHOD(Http_UrlDecode);
        TEST_METHOD(WebSocket_ComputeAcceptKey);
        TEST_METHOD(WebSocket_DecodeMaskedFrame);
        TEST_METHOD(Json_ParseRequestBodies);
        TEST_METHOD(Json_SnapshotRoundTrip);
        TEST_METHOD(Json_ColoredCells);
        TEST_METHOD(Json_EventEnvelope);
        TEST_METHOD(Guard_CatastrophicPositives);
        TEST_METHOD(Guard_SafeNegatives);
        TEST_METHOD(Crypto_DpapiRoundTrip);
        TEST_METHOD(Crypto_TokenHash);
        TEST_METHOD(Auth_HashedToken);
        TEST_METHOD(Crypto_ExternalEntry);
        TEST_METHOD(Crypto_StrictEntryValidation);
    };

    void RemoteControlTests::KeyNameToSequence_KnownKeys()
    {
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("enter") == std::wstring{ L"\r" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("tab") == std::wstring{ L"\t" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("escape") == std::wstring{ L"\x1b" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("ctrl-c") == std::wstring{ L"\x03" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("ctrl-d") == std::wstring{ L"\x04" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("up") == std::wstring{ L"\x1b[A" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("down") == std::wstring{ L"\x1b[B" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("right") == std::wstring{ L"\x1b[C" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("left") == std::wstring{ L"\x1b[D" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("backspace") == std::wstring{ L"\x7f" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("delete") == std::wstring{ L"\x1b[3~" });
    }

    void RemoteControlTests::KeyNameToSequence_UnknownAndCase()
    {
        VERIFY_IS_FALSE(RemoteControl::KeyNameToSequence("nope").has_value());
        // Matching is case-insensitive.
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("ENTER") == std::wstring{ L"\r" });
        VERIFY_IS_TRUE(RemoteControl::KeyNameToSequence("Ctrl-C") == std::wstring{ L"\x03" });
    }

    void RemoteControlTests::Auth_ConstantTimeEquals()
    {
        VERIFY_IS_TRUE(RemoteControl::ConstantTimeEquals("abc123", "abc123"));
        VERIFY_IS_FALSE(RemoteControl::ConstantTimeEquals("abc123", "abc124"));
        VERIFY_IS_FALSE(RemoteControl::ConstantTimeEquals("abc", "abcd"));
        VERIFY_IS_FALSE(RemoteControl::ConstantTimeEquals("", "x"));
    }

    void RemoteControlTests::Auth_ExtractBearerToken()
    {
        const auto a = RemoteControl::ExtractBearerToken("Bearer mytoken");
        VERIFY_IS_TRUE(a.has_value() && *a == "mytoken");

        // Scheme is case-insensitive.
        const auto b = RemoteControl::ExtractBearerToken("bearer  spaced ");
        VERIFY_IS_TRUE(b.has_value());

        VERIFY_IS_FALSE(RemoteControl::ExtractBearerToken("Basic abc").has_value());
        VERIFY_IS_FALSE(RemoteControl::ExtractBearerToken("Bearer ").has_value());
    }

    void RemoteControlTests::Auth_IsAuthorized()
    {
        // The expected token must be a stored hash; the presented token is the
        // plaintext, verified against it.
        const auto expected = RemoteControl::Crypto::MakeTokenHash("secret");
        VERIFY_IS_TRUE(RemoteControl::IsAuthorized(expected, std::optional<std::string>{ "secret" }, std::nullopt));
        VERIFY_IS_TRUE(RemoteControl::IsAuthorized(expected, std::nullopt, std::optional<std::string>{ "secret" }));
        VERIFY_IS_FALSE(RemoteControl::IsAuthorized(expected, std::optional<std::string>{ "nope" }, std::nullopt));
        VERIFY_IS_FALSE(RemoteControl::IsAuthorized(expected, std::nullopt, std::nullopt));
        // An empty / plaintext expected token authorizes no one.
        VERIFY_IS_FALSE(RemoteControl::IsAuthorized("", std::optional<std::string>{ "" }, std::nullopt));
        VERIFY_IS_FALSE(RemoteControl::IsAuthorized("secret", std::optional<std::string>{ "secret" }, std::nullopt));
    }

    void RemoteControlTests::Http_ParseRequestHead()
    {
        const std::string raw =
            "POST /v1/sessions/abc/input?token=xyz HTTP/1.1\r\n"
            "Host: 127.0.0.1:9177\r\n"
            "Authorization: Bearer t0k3n\r\n"
            "Content-Length: 9\r\n"
            "\r\n";

        RemoteControl::HttpRequest req;
        VERIFY_IS_TRUE(RemoteControl::ParseRequestHead(raw, req));
        VERIFY_IS_TRUE(req.method == "POST");
        VERIFY_IS_TRUE(req.path == "/v1/sessions/abc/input");
        VERIFY_IS_TRUE(req.QueryValue("token").has_value() && *req.QueryValue("token") == "xyz");
        VERIFY_IS_TRUE(req.Header("authorization").has_value() && *req.Header("authorization") == "Bearer t0k3n");
        VERIFY_ARE_EQUAL(static_cast<size_t>(9), req.ContentLength());
    }

    void RemoteControlTests::Http_UrlDecode()
    {
        VERIFY_IS_TRUE(RemoteControl::UrlDecode("a%20b") == "a b");
        VERIFY_IS_TRUE(RemoteControl::UrlDecode("a+b", true) == "a b");
        VERIFY_IS_TRUE(RemoteControl::UrlDecode("%2Fv1%2F") == "/v1/");
    }

    void RemoteControlTests::WebSocket_ComputeAcceptKey()
    {
        // The canonical example from RFC 6455 section 1.3.
        const auto accept = RemoteControl::ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
        VERIFY_IS_TRUE(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    }

    void RemoteControlTests::WebSocket_DecodeMaskedFrame()
    {
        // A masked, single-frame text message "Hi" (RFC 6455 masking).
        const unsigned char mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
        const char text[2] = { 'H', 'i' };
        std::string frame;
        frame.push_back(static_cast<char>(0x81)); // FIN + text
        frame.push_back(static_cast<char>(0x82)); // masked + len 2
        for (const auto m : mask)
        {
            frame.push_back(static_cast<char>(m));
        }
        for (size_t i = 0; i < 2; ++i)
        {
            frame.push_back(static_cast<char>(static_cast<unsigned char>(text[i]) ^ mask[i % 4]));
        }

        RemoteControl::WsDecodedFrame decoded;
        const int consumed = RemoteControl::TryDecodeFrame(frame, decoded);
        VERIFY_ARE_EQUAL(static_cast<int>(frame.size()), consumed);
        VERIFY_IS_TRUE(decoded.opcode == RemoteControl::WsOpcode::Text);
        VERIFY_IS_TRUE(decoded.payload == "Hi");

        // An unmasked client frame must be rejected.
        std::string unmasked;
        unmasked.push_back(static_cast<char>(0x81));
        unmasked.push_back(static_cast<char>(0x02));
        unmasked.push_back('H');
        unmasked.push_back('i');
        RemoteControl::WsDecodedFrame ignored;
        VERIFY_IS_TRUE(RemoteControl::TryDecodeFrame(unmasked, ignored) < 0);
    }

    void RemoteControlTests::Json_ParseRequestBodies()
    {
        const auto input = RemoteControl::Json::ParseInputBody(R"({"data":"echo hello\r"})");
        VERIFY_IS_TRUE(input.has_value() && *input == std::wstring{ L"echo hello\r" });

        const auto key = RemoteControl::Json::ParseKeyBody(R"({"key":"ctrl-c"})");
        VERIFY_IS_TRUE(key.has_value() && *key == "ctrl-c");

        int cols = 0;
        int rows = 0;
        VERIFY_IS_TRUE(RemoteControl::Json::ParseResizeBody(R"({"cols":120,"rows":30})", cols, rows));
        VERIFY_ARE_EQUAL(120, cols);
        VERIFY_ARE_EQUAL(30, rows);

        VERIFY_IS_FALSE(RemoteControl::Json::ParseInputBody(R"({"nope":1})").has_value());
        VERIFY_IS_FALSE(RemoteControl::Json::ParseInputBody("not json").has_value());
    }

    void RemoteControlTests::Json_SnapshotRoundTrip()
    {
        RemoteControl::SnapshotData snap;
        snap.text = L"line one\r\nline two";
        snap.cols = 80;
        snap.rows = 24;
        snap.cursorX = 3;
        snap.cursorY = 1;

        const auto json = RemoteControl::Json::SnapshotJson("session-1", snap);
        // The body should contain the key fields. (Full structural parsing is
        // covered indirectly by the request-body parser tests.)
        VERIFY_IS_TRUE(json.find("\"sessionId\":\"session-1\"") != std::string::npos);
        VERIFY_IS_TRUE(json.find("\"cols\":80") != std::string::npos);
        VERIFY_IS_TRUE(json.find("\"rows\":24") != std::string::npos);
        VERIFY_IS_TRUE(json.find("\"cursorX\":3") != std::string::npos);
        VERIFY_IS_TRUE(json.find("line one") != std::string::npos);

        // A client message parses back into the expected fields.
        RemoteControl::Json::WsClientMessage msg;
        VERIFY_IS_TRUE(RemoteControl::Json::ParseWsClientMessage(R"({"type":"resize","cols":100,"rows":40})", msg));
        VERIFY_IS_TRUE(msg.type == "resize");
        VERIFY_ARE_EQUAL(100, msg.cols);
        VERIFY_ARE_EQUAL(40, msg.rows);
    }

    void RemoteControlTests::Json_ColoredCells()
    {
        RemoteControl::SnapshotData snap;
        snap.text = L"ab";
        snap.cols = 2;
        snap.rows = 1;
        snap.hasColor = true;
        RemoteControl::CellRun run;
        run.text = L"ab";
        run.foreground = 0xffffff;
        run.background = 0x000000;
        run.bold = true;
        run.row = 0;
        snap.cells.push_back(run);

        const auto json = RemoteControl::Json::SnapshotJson("s", snap);
        VERIFY_IS_TRUE(json.find("\"cells\":") != std::string::npos);
        VERIFY_IS_TRUE(json.find("\"fg\":\"#ffffff\"") != std::string::npos);
        VERIFY_IS_TRUE(json.find("\"bg\":\"#000000\"") != std::string::npos);
        VERIFY_IS_TRUE(json.find("\"bold\":true") != std::string::npos);

        // Without color, no cells are emitted.
        RemoteControl::SnapshotData plain;
        plain.text = L"x";
        const auto plainJson = RemoteControl::Json::SnapshotJson("s", plain);
        VERIFY_IS_TRUE(plainJson.find("\"cells\":") == std::string::npos);
    }

    void RemoteControlTests::Json_EventEnvelope()
    {
        // Every server event carries type, sessionId and timestamp.
        const auto out = RemoteControl::Json::WsOutputMessage("sess-7", L"hi");
        VERIFY_IS_TRUE(out.find("\"type\":\"output\"") != std::string::npos);
        VERIFY_IS_TRUE(out.find("\"sessionId\":\"sess-7\"") != std::string::npos);
        VERIFY_IS_TRUE(out.find("\"timestamp\":") != std::string::npos);

        const auto err = RemoteControl::Json::WsErrorMessage("sess-7", "boom");
        VERIFY_IS_TRUE(err.find("\"type\":\"error\"") != std::string::npos);
        VERIFY_IS_TRUE(err.find("\"sessionId\":\"sess-7\"") != std::string::npos);
        VERIFY_IS_TRUE(err.find("\"timestamp\":") != std::string::npos);

        const auto exited = RemoteControl::Json::WsExitMessage("sess-7", 0);
        VERIFY_IS_TRUE(exited.find("\"type\":\"exit\"") != std::string::npos);
        VERIFY_IS_TRUE(exited.find("\"code\":0") != std::string::npos);
    }

    void RemoteControlTests::Guard_CatastrophicPositives()
    {
        // Each of these wipes the whole root / a whole drive and MUST be caught.
        const wchar_t* bad[] = {
            L"rm -rf /",
            L"rm -rf /*",
            L"rm -fr /",
            L"rm --recursive --force /",
            L"sudo rm -rf --no-preserve-root /",
            L"echo hi && rm -rf /",
            L"del /s /q C:\\",
            L"rd /s C:\\",
            L"format C:",
            L"Remove-Item -Recurse -Force C:\\",
            L"Remove-Item -Recurse C:\\*",
        };
        for (const auto* cmd : bad)
        {
            const auto hit = RemoteControl::MatchCatastrophicCommand(cmd);
            VERIFY_IS_TRUE(hit.has_value(), WEX::Common::String().Format(L"expected catastrophic: %s", cmd));
        }
    }

    void RemoteControlTests::Guard_SafeNegatives()
    {
        // Ordinary deletes of specific directories/files must NOT be caught.
        const wchar_t* ok[] = {
            L"rm -rf /tmp/foo",
            L"rm -rf ./build",
            L"rm file.txt",
            L"rm -rf node_modules",
            L"del /s /q logs\\",
            L"Remove-Item -Recurse .\\bin",
            L"echo rm -rf / is dangerous",  // a discussion, not a command (best-effort: 'echo' command)
            L"git commit -m 'rm stuff'",
            L"ls /",
        };
        for (const auto* cmd : ok)
        {
            const auto hit = RemoteControl::MatchCatastrophicCommand(cmd);
            VERIFY_IS_FALSE(hit.has_value(), WEX::Common::String().Format(L"false positive: %s", cmd));
        }
    }

    void RemoteControlTests::Crypto_DpapiRoundTrip()
    {
        const std::string secret = "my-secret-token-12345";
        const auto blob = RemoteControl::Crypto::ProtectString(secret);
        VERIFY_IS_FALSE(blob.empty());
        VERIFY_IS_TRUE(blob.find(secret) == std::string::npos); // not plaintext
        const auto back = RemoteControl::Crypto::UnprotectString(blob);
        VERIFY_IS_TRUE(back.has_value() && *back == secret);
        // Garbage in -> no value out (no throw).
        VERIFY_IS_FALSE(RemoteControl::Crypto::UnprotectString("not-a-valid-blob").has_value());
    }

    void RemoteControlTests::Crypto_TokenHash()
    {
        const auto entry = RemoteControl::Crypto::MakeTokenHash("my-token");
        VERIFY_IS_TRUE(RemoteControl::Crypto::IsHashedToken(entry));
        VERIFY_IS_TRUE(entry.find("my-token") == std::string::npos); // plaintext absent
        VERIFY_IS_TRUE(RemoteControl::Crypto::VerifyTokenHash(entry, "my-token"));
        VERIFY_IS_FALSE(RemoteControl::Crypto::VerifyTokenHash(entry, "wrong"));
        // Two hashes of the same token differ (random salt) but both verify.
        const auto entry2 = RemoteControl::Crypto::MakeTokenHash("my-token");
        VERIFY_IS_TRUE(entry != entry2);
        VERIFY_IS_TRUE(RemoteControl::Crypto::VerifyTokenHash(entry2, "my-token"));
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("plaintext"));
    }

    void RemoteControlTests::Auth_HashedToken()
    {
        const auto entry = RemoteControl::Crypto::MakeTokenHash("secret");
        // IsAuthorized verifies a presented plaintext against the stored hash.
        VERIFY_IS_TRUE(RemoteControl::IsAuthorized(entry, std::optional<std::string>{ "secret" }, std::nullopt));
        VERIFY_IS_TRUE(RemoteControl::IsAuthorized(entry, std::nullopt, std::optional<std::string>{ "secret" }));
        VERIFY_IS_FALSE(RemoteControl::IsAuthorized(entry, std::optional<std::string>{ "nope" }, std::nullopt));
        // Plaintext expected tokens are NOT accepted (hash-only).
        VERIFY_IS_FALSE(RemoteControl::IsAuthorized("plainsecret", std::optional<std::string>{ "plainsecret" }, std::nullopt));
        // An empty expected token authorizes no one.
        VERIFY_IS_FALSE(RemoteControl::IsAuthorized("", std::optional<std::string>{ "" }, std::nullopt));
    }

    void RemoteControlTests::Crypto_ExternalEntry()
    {
        // Entry produced by an external tool (.NET Rfc2898DeriveBytes, SHA-256,
        // 100000 iterations, salt 00..0f). Proves our BCrypt PBKDF2 matches a
        // standard implementation bit-for-bit.
        const std::string entry = "pbkdf2$100000$AAECAwQFBgcICQoLDA0ODw==$f77e67478ef730ab0f3b1bdbbb0d4ad4136e80d078bc2019c566196b9083ef6c";
        VERIFY_IS_TRUE(RemoteControl::Crypto::IsHashedToken(entry));
        VERIFY_IS_TRUE(RemoteControl::Crypto::VerifyTokenHash(entry, "manualsecret"));
        VERIFY_IS_FALSE(RemoteControl::Crypto::VerifyTokenHash(entry, "wrong"));
        // The old sha256$ format is no longer recognized.
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("sha256$abc$def"));
    }

    void RemoteControlTests::Crypto_StrictEntryValidation()
    {
        // A well-formed entry (16-byte salt, 64-hex hash, >= 10000 iterations).
        const std::string saltB64 = "AAECAwQFBgcICQoLDA0ODw=="; // 16 bytes
        const std::string hex64 = "f77e67478ef730ab0f3b1bdbbb0d4ad4136e80d078bc2019c566196b9083ef6c";
        VERIFY_IS_TRUE(RemoteControl::Crypto::IsHashedToken("pbkdf2$100000$" + saltB64 + "$" + hex64));

        // Iterations below the floor -> rejected.
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("pbkdf2$5$" + saltB64 + "$" + hex64));
        // Wrong hash length -> rejected.
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("pbkdf2$100000$" + saltB64 + "$abcd"));
        // Non-hex hash of the right length -> rejected.
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("pbkdf2$100000$" + saltB64 + "$" + std::string(64, 'z')));
        // Salt that doesn't decode to 16 bytes -> rejected.
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("pbkdf2$100000$AAAA$" + hex64));
        // Non-numeric iterations / missing parts -> rejected.
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("pbkdf2$abc$" + saltB64 + "$" + hex64));
        VERIFY_IS_FALSE(RemoteControl::Crypto::IsHashedToken("pbkdf2$100000$" + saltB64));

        // VerifyTokenHash also rejects a downgraded entry outright.
        VERIFY_IS_FALSE(RemoteControl::Crypto::VerifyTokenHash("pbkdf2$1$" + saltB64 + "$" + hex64, "manualsecret"));
    }
}

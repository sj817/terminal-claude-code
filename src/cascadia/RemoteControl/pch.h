// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// pch.h
// Precompiled header for the RemoteControl static library.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOMCX
#define NOHELP
#define NOCOMM

// winsock2.h must come before windows.h so that the older winsock.h (pulled in
// by windows.h) does not conflict with it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

// These libraries are consumed by this static lib and need to flow through to
// the final link of the host executable.
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <json/json.h>

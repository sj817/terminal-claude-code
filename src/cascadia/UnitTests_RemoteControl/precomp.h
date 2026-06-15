/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- precomp.h

Abstract:
- Precompiled header for the RemoteControl unit tests.
--*/

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "LibraryIncludes.h"

#include <WexTestClass.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

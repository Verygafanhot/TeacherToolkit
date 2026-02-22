#pragma once

// Require Windows 7 or later for Shell_NotifyIcon V4, SetDisplayConfig, etc.
#include <WinSDKVer.h>
#define _WIN32_WINNT 0x0601   // Windows 7+
#include <SDKDDKVer.h>

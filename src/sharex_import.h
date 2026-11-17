#pragma once
// Private helpers shared by app.cpp / output.cpp / recorder.cpp / sharex_import.cpp (not by other modules).
#include "common.h"

// Hotkey text. Ini form: "Ctrl+Shift+PrintScreen" (key = .NET Keys name, as ShareX writes it).
// Display form, like ShareX: "Ctrl + Shift + Print Screen".
bool ParseHotkeyText(const std::wstring& text, UINT& mods, UINT& vk);  // mods = MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_WIN
std::wstring HotkeyIniText(UINT mods, UINT vk);
std::wstring HotkeyDisplayText(UINT mods, UINT vk);

// zipshot.ini helpers.
std::wstring ZipshotIni();
int IniInt(const wchar_t* section, const wchar_t* key, int def);
std::wstring IniString(const wchar_t* section, const wchar_t* key, const wchar_t* def);
void EnsureUnicodeIni();  // creates an empty UTF-16 ini so WritePrivateProfileString keeps non-ASCII paths

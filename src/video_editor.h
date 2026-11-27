#pragma once
#include <string>

bool IsVideoFile(const std::wstring& path);
void OpenVideoEditor(const std::wstring& path);
bool VideoEditorBusy();

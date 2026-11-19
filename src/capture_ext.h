#pragma once
// Region-capture options and screen-recorder controls shared by app.cpp / editor.cpp / recorder.cpp / mainwindow.cpp.
#include "common.h"

// editor.cpp: ShareX RegionCaptureOptions.UseLightResizeNodes ([region] UseLightResizeNodes), for whoever draws resize nodes.
bool RegionLightNodes();

// app.cpp: [region] LastRegion=x,y,w,h in virtual-screen pixels (ShareX "Capture last region").
bool LoadLastRegion(RECT& r);
void SaveLastRegion(const RECT& r);

// recorder.cpp: ShareX ScreenRecordForm controls.
void RecorderHotkey();         // hotkey pressed again: start (waiting), abort (countdown), stop (recording/paused)
void RecorderPauseResume();
void RecorderAbort();
bool RecorderPaused();
bool RecorderFollowEnabled();
void RecorderToggleFollow();
// dshow audio capture devices as ffmpeg lists them (`ffmpeg -list_devices true -f dshow -i dummy`).
std::vector<std::wstring> ListAudioDevices();

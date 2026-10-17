#pragma once
#include <cstdint>
#include <string>

struct GamepadState;

// Test automation ("TAS" scripts): sequential controller input timed by the guest
// clock, with waits on game state, screenshots, memory dumps and per-frame telemetry.
// Everything here only feeds controller input or reads guest memory; game code is
// never changed. See docs/tas.md for the script language.
void Tas_Load(const char* path);
void Tas_SetTelemetry(const char* csvPath);
bool Tas_Active();
// Fills player 1's input while a script runs (guest thread). Returns false when no
// script is loaded.
bool Tas_Poll(GamepadState& out);
// Once per presented frame (GX thread): telemetry rows and screenshot requests.
void Tas_OnPresent(void (*saveShot)(const char* path));
// Disc file reads, for "until file <name>".
void Tas_OnFileRead(const std::string& path);

// Read-only probes the recompiler places at a few game instructions: they record a
// register value (usually an object's address) and change nothing else.
extern "C" uint32_t g_tasProbe[16];

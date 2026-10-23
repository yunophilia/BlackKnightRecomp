#pragma once
#include <string>
#include <vector>

// Settings panel shared by the launcher and the in-game overlay (Dear ImGui). Every
// change is written to config.ini at once. In game, settings that can change live are
// applied immediately; the others ask whether to restart the game.
enum class SettingsHost
{
    Launcher,
    InGame,
};

void SettingsUI_Draw(SettingsHost host);
// In game: the user confirmed "Restart the game?" (the overlay restarts the process).
bool SettingsUI_TakeRestartRequest();

// The installed release (game ID) the Mods tab checks mods against (mod.ini "releases");
// rescans the mods folder.
void SettingsUI_SetRelease(const std::string& gameId);
// Titles of the mods that load with the current settings, in load order.
std::vector<std::string> SettingsUI_ActiveMods();

// The build version, right-aligned on the current line (the bottom button row).
void SettingsUI_DrawVersion();

// Dark theme with Sonic-blue accents.
void SettingsUI_ApplyStyle();

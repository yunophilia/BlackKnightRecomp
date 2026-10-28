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
// The Mods section (its own tab next to the settings in the launcher and in game).
void SettingsUI_DrawMods(SettingsHost host);
// Test aid: BKR_UI_SHOT_SUBTAB=Mods, the Mods section opens first.
bool SettingsUI_ShotMods();
// Controller tab switching: LB/RB step through the top tabs (outer), LT/RT through the tabs
// inside a page. Call before the tab bar with the tab selected last frame; returns the tab
// to select now, or -1.
int SettingsUI_PadTab(int selected, int count, bool outer);
// BeginTabItem that selects the tab when index == pick (from SettingsUI_PadTab, or a test
// aid when select is set) and then moves the controller/keyboard focus onto it.
bool SettingsUI_TabItem(const char* name, int index, int pick, bool select = false);
// In game: the user confirmed "Restart the game?" (the overlay restarts the process).
bool SettingsUI_TakeRestartRequest();

// The installed release (game ID and data folder) the Mods section checks mods against
// (mod.ini "releases", the disc's files for installs); rescans the mods folder.
void SettingsUI_SetRelease(const std::string& gameId, const std::string& dataDir);
// Titles of the mods that load with the current settings, in load order.
std::vector<std::string> SettingsUI_ActiveMods();

// The build version, right-aligned on the current line (the bottom button row).
void SettingsUI_DrawVersion();

// Fallback fonts for text the main font lacks (CJK mod names); call after adding it.
void SettingsUI_AddFallbackFonts();

// Dark theme with Sonic-blue accents.
void SettingsUI_ApplyStyle();

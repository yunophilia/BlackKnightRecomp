#pragma once
#include "settings_ui.h"

#include <string>
#include <vector>

// The Mods tab (launcher and in game): the installed mods (on/off, load
// order, updates, removal) and a browser of the game's mods on GameBanana that downloads
// and installs them (mods/mod_install.h).

// Draws the tab. True when the mod setup changed (config.ini [Mods] or the installed
// mods): the caller saves the settings and, in game, offers to restart. help is set to the
// description of the hovered item.
bool ModsUI_Draw(SettingsHost host, const char*& help);
void ModsUI_SetRelease(const std::string& gameId, const std::string& dataDir);
std::vector<std::string> ModsUI_ActiveMods();

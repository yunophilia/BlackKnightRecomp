#pragma once

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

// Dusklight-style dark theme with gold accents.
void SettingsUI_ApplyStyle();

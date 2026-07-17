#pragma once

#include <user/sdk/color.hpp>

namespace hsrc::sdk::settings {

bool open();
bool open_category(const char *id);
bool open_deeplink(const char *uri);

/* Raw preference from /etc/os-settings.ini (general.appearance). */
enum class Appearance : int {
    Light = 0,
    Dark = 1,
    Auto = 2,
};

/* Resolved paint mode (Auto currently maps to Light). */
enum class ThemeMode : int {
    Light = 0,
    Dark = 1,
};

/* Shared palette for default OS apps (settings / files / terminal / activity). */
struct AppTheme {
    Color bg;
    Color sidebar;
    Color chrome;
    Color border;
    Color text;
    Color text_dim;
    Color text_soft;
    Color accent;
    Color accent_soft;
    Color hover;
    Color card;
    Color button;
    Color inset;
    Color warn;
    Color danger;
    Color danger_soft;
    Color panel; /* content header / “white” surface */
    Color term_bg;
    Color term_fg;
    Color term_dim;
    Color term_accent;
    Color term_err;
};

Appearance appearance();
ThemeMode theme_mode();
const AppTheme &theme();

/* Re-read /etc/os-settings.ini (throttled + cached). Returns true if theme changed. */
bool refresh_theme();

} // namespace hsrc::sdk::settings

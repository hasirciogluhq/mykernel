#include <user/sdk/settings.hpp>

#include <kernel/vfs.h>
#include <user/sdk/fs.hpp>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/syscall.hpp>
#include <user/string.h>

namespace {

constexpr const char *kSettingsTitle = "System Settings";
constexpr const char *kSettingsClass = "os.settings";
constexpr const char *kSettingsPath = "/applications/os-settings.mke";
constexpr const char *kRunDir = "/run";
constexpr const char *kDeepLinkPath = "/run/settings.deeplink";
constexpr const char *kDefaultDeepLink = "settings://general";
constexpr const char *kIniPath = "/etc/os-settings.ini";
constexpr const char *kAppearanceKey = "general.appearance";
constexpr size_t kDeepLinkBytes = 128;
constexpr size_t kIniBytes = 2048;

using hsrc::sdk::Color;
using hsrc::sdk::rgb;
using hsrc::sdk::rgba;
using hsrc::sdk::settings::Appearance;
using hsrc::sdk::settings::AppTheme;
using hsrc::sdk::settings::StatusInfo;
using hsrc::sdk::settings::ThemeMode;

void append_text(char *dst, size_t dst_size, const char *src);

Appearance g_appearance = Appearance::Light;
ThemeMode g_theme_mode = ThemeMode::Light;
bool g_theme_loaded = false;
/* True once /etc/os-settings.ini exists (or we created the default). */
bool g_ini_present = false;
/* Skip re-open storms while the ini is known-missing. */
unsigned g_absent_skip = 0;
constexpr unsigned kAbsentRetryEvery = 256; /* refresh_theme calls while missing */
constexpr const char *kDefaultIni =
    "general.appearance=Light\n"
    "desktop.dock-size=Medium\n"
    "dock.magnification=On\n"
    "dock.mag-size=18\n"
    "dock.mag-range=132\n"
    "dock.mag-speed=Normal\n"
    "dock.pin.monitor=On\n"
    "dock.pin.terminal=On\n"
    "dock.pin.files=On\n"
    "dock.pin.settings=On\n"
    "mouse.natural-scroll=On\n"
    "mouse.wheel-lines=3\n"
    "status.wifi_connected=1\n"
    "status.wifi_bars=3\n"
    "status.battery_percent=78\n"
    "status.battery_charging=0\n";

constexpr const char *kWifiConnectedKey = "status.wifi_connected";
constexpr const char *kWifiBarsKey = "status.wifi_bars";
constexpr const char *kBatteryPercentKey = "status.battery_percent";
constexpr const char *kBatteryChargingKey = "status.battery_charging";

StatusInfo g_status{};
bool g_status_loaded = false;

constexpr AppTheme kLightTheme = {
    rgb(255, 255, 255),          /* bg */
    rgb(247, 247, 245),          /* sidebar */
    rgb(250, 250, 248),          /* chrome */
    rgba(55, 53, 47, 22),        /* border */
    rgb(50, 48, 44),             /* text */
    rgb(115, 114, 110),          /* text_dim */
    rgba(71, 70, 68, 153),       /* text_soft */
    rgb(35, 131, 226),           /* accent */
    rgba(35, 131, 226, 28),      /* accent_soft */
    rgb(242, 241, 238),          /* hover */
    rgb(247, 247, 245),          /* card */
    rgba(242, 241, 238, 153),    /* button */
    rgba(242, 241, 238, 153),    /* inset */
    rgb(176, 96, 32),            /* warn */
    rgb(196, 66, 66),            /* danger */
    rgba(196, 66, 66, 30),       /* danger_soft */
    rgb(255, 255, 255),          /* panel */
    rgb(250, 250, 248),          /* term_bg */
    rgb(40, 40, 48),             /* term_fg */
    rgb(120, 120, 130),          /* term_dim */
    rgb(30, 140, 80),            /* term_accent */
    rgb(180, 60, 60),            /* term_err */
};

constexpr AppTheme kDarkTheme = {
    rgb(28, 28, 30),             /* bg */
    rgb(36, 36, 38),             /* sidebar */
    rgb(40, 40, 44),             /* chrome */
    rgba(255, 255, 255, 28),     /* border */
    rgb(235, 235, 240),          /* text */
    rgb(160, 160, 168),          /* text_dim */
    rgba(200, 200, 210, 153),    /* text_soft */
    rgb(70, 150, 235),           /* accent */
    rgba(70, 150, 235, 40),      /* accent_soft */
    rgb(48, 48, 52),             /* hover */
    rgb(36, 36, 40),             /* card */
    rgba(60, 60, 66, 180),       /* button */
    rgba(20, 20, 24, 180),       /* inset */
    rgb(220, 160, 80),           /* warn */
    rgb(220, 90, 90),            /* danger */
    rgba(220, 90, 90, 40),       /* danger_soft */
    rgb(32, 32, 36),             /* panel */
    rgb(18, 18, 22),             /* term_bg */
    rgb(220, 220, 225),          /* term_fg */
    rgb(120, 120, 130),          /* term_dim */
    rgb(90, 200, 120),           /* term_accent */
    rgb(240, 110, 110),          /* term_err */
};

ThemeMode resolve_mode(Appearance appearance)
{
    if (appearance == Appearance::Dark)
        return ThemeMode::Dark;
    /* Auto has no clock/sensor yet — fall back to Light. */
    return ThemeMode::Light;
}

Appearance parse_appearance_value(const char *value)
{
    if (!value || !value[0])
        return Appearance::Light;
    if (strcmp(value, "Dark") == 0)
        return Appearance::Dark;
    if (strcmp(value, "Auto") == 0)
        return Appearance::Auto;
    return Appearance::Light;
}

/* status: 1 = read ok, 0 = missing/empty (use default), -1 = other error */
Appearance read_appearance_from_ini(int *status)
{
    if (status)
        *status = 0;

    int fd = (int)hsrc::sdk::open(kIniPath, O_RDONLY);
    if (fd < 0) {
        if (status)
            *status = 0; /* treat as absent — expected before first Settings save */
        return Appearance::Light;
    }

    char buf[kIniBytes];
    memset(buf, 0, sizeof(buf));
    long nread = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
    (void)hsrc::sdk::close(fd);
    if (nread <= 0) {
        if (status)
            *status = 0;
        return Appearance::Light;
    }

    if (status)
        *status = 1;

    int start = 0;
    for (long i = 0; i <= nread; i++) {
        if (buf[i] != '\n' && buf[i] != '\r' && buf[i] != 0)
            continue;
        buf[i] = 0;
        char *line = buf + start;
        start = (int)i + 1;
        if (!line[0])
            continue;

        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (*eq != '=')
            continue;
        *eq++ = 0;
        if (strcmp(line, kAppearanceKey) != 0)
            continue;
        return parse_appearance_value(eq);
    }
    return Appearance::Light;
}

bool ensure_default_ini()
{
    (void)hsrc::sdk::mkdir("/etc", 0755);
    int fd = (int)hsrc::sdk::open(kIniPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    const size_t len = strlen(kDefaultIni);
    long wrote = hsrc::sdk::write(fd, kDefaultIni, len);
    (void)hsrc::sdk::close(fd);
    if (wrote == (long)len) {
        g_ini_present = true;
        return true;
    }
    return false;
}

void apply_theme(Appearance appearance)
{
    g_appearance = appearance;
    g_theme_mode = resolve_mode(appearance);
    g_theme_loaded = true;
}

void load_theme_from_disk(bool create_if_missing)
{
    int status = 0;
    Appearance next = read_appearance_from_ini(&status);
    if (status == 1) {
        g_ini_present = true;
        apply_theme(next);
        return;
    }
    /* Missing/empty: keep Light defaults and optionally seed the file once. */
    apply_theme(Appearance::Light);
    if (create_if_missing && ensure_default_ini())
        g_ini_present = true;
    else
        g_ini_present = false;
}

void ensure_theme_loaded()
{
    if (g_theme_loaded)
        return;
    load_theme_from_disk(true);
}

const char *appearance_name(Appearance appearance)
{
    if (appearance == Appearance::Dark)
        return "Dark";
    if (appearance == Appearance::Auto)
        return "Auto";
    return "Light";
}

int parse_int_clamped(const char *value, int lo, int hi, int fallback)
{
    if (!value || !value[0])
        return fallback;
    int sign = 1;
    const char *p = value;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    int v = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        any = true;
    }
    if (!any)
        return fallback;
    v *= sign;
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

bool parse_bool01(const char *value, bool fallback)
{
    if (!value || !value[0])
        return fallback;
    if (strcmp(value, "1") == 0 || strcmp(value, "On") == 0 ||
        strcmp(value, "true") == 0 || strcmp(value, "True") == 0)
        return true;
    if (strcmp(value, "0") == 0 || strcmp(value, "Off") == 0 ||
        strcmp(value, "false") == 0 || strcmp(value, "False") == 0)
        return false;
    return fallback;
}

void apply_status_defaults()
{
    g_status.wifi_connected = true;
    g_status.wifi_bars = 3;
    g_status.battery_percent = 78;
    g_status.battery_charging = false;
    g_status_loaded = true;
}

void load_status_from_ini_buf(char *buf, long nread)
{
    apply_status_defaults();
    if (!buf || nread <= 0)
        return;

    int start = 0;
    for (long i = 0; i <= nread; i++) {
        if (buf[i] != '\n' && buf[i] != '\r' && buf[i] != 0)
            continue;
        buf[i] = 0;
        char *line = buf + start;
        start = (int)i + 1;
        if (!line[0])
            continue;
        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (*eq != '=')
            continue;
        *eq++ = 0;
        if (strcmp(line, kWifiConnectedKey) == 0)
            g_status.wifi_connected = parse_bool01(eq, true);
        else if (strcmp(line, kWifiBarsKey) == 0)
            g_status.wifi_bars = parse_int_clamped(eq, 0, 3, 3);
        else if (strcmp(line, kBatteryPercentKey) == 0)
            g_status.battery_percent = parse_int_clamped(eq, 0, 100, 78);
        else if (strcmp(line, kBatteryChargingKey) == 0)
            g_status.battery_charging = parse_bool01(eq, false);
    }
}

void load_status_from_disk()
{
    int fd = (int)hsrc::sdk::open(kIniPath, O_RDONLY);
    if (fd < 0) {
        apply_status_defaults();
        return;
    }
    char buf[kIniBytes];
    memset(buf, 0, sizeof(buf));
    long nread = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
    (void)hsrc::sdk::close(fd);
    load_status_from_ini_buf(buf, nread);
}

/* Rewrite or insert a single key=value line; preserves other keys. */
bool upsert_ini_key(const char *key, const char *value)
{
    if (!key || !key[0] || !value)
        return false;

    char buf[kIniBytes];
    memset(buf, 0, sizeof(buf));
    long nread = 0;
    int fd = (int)hsrc::sdk::open(kIniPath, O_RDONLY);
    if (fd >= 0) {
        nread = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
        (void)hsrc::sdk::close(fd);
        if (nread < 0)
            nread = 0;
    }

    char out[kIniBytes];
    memset(out, 0, sizeof(out));
    bool replaced = false;
    int start = 0;
    for (long i = 0; i <= nread; i++) {
        if (i < nread && buf[i] != '\n' && buf[i] != '\r')
            continue;
        buf[i] = 0;
        char *line = buf + start;
        start = (int)i + 1;
        if (!line[0])
            continue;

        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (*eq == '=') {
            *eq = 0;
            if (strcmp(line, key) == 0) {
                append_text(out, sizeof(out), key);
                append_text(out, sizeof(out), "=");
                append_text(out, sizeof(out), value);
                append_text(out, sizeof(out), "\n");
                replaced = true;
                continue;
            }
            *eq = '=';
        }
        append_text(out, sizeof(out), line);
        append_text(out, sizeof(out), "\n");
    }
    if (!replaced) {
        append_text(out, sizeof(out), key);
        append_text(out, sizeof(out), "=");
        append_text(out, sizeof(out), value);
        append_text(out, sizeof(out), "\n");
    }

    (void)hsrc::sdk::mkdir("/etc", 0755);
    fd = (int)hsrc::sdk::open(kIniPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    size_t len = strlen(out);
    long wrote = hsrc::sdk::write(fd, out, len);
    (void)hsrc::sdk::close(fd);
    if (wrote == (long)len) {
        g_ini_present = true;
        return true;
    }
    return false;
}

void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    dst[0] = 0;
    if (!src)
        return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

void append_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src)
        return;
    size_t len = strlen(dst);
    if (len >= dst_size)
        return;
    strncpy(dst + len, src, dst_size - len - 1);
    dst[dst_size - 1] = 0;
}

bool write_deeplink_file(const char *uri)
{
    (void)hsrc::sdk::mkdir(kRunDir, 0755);

    int fd = (int)hsrc::sdk::open(kDeepLinkPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;

    char buf[kDeepLinkBytes];
    memset(buf, 0, sizeof(buf));
    copy_text(buf, sizeof(buf), uri && uri[0] ? uri : kDefaultDeepLink);
    size_t len = strlen(buf);
    if (len == 0) {
        copy_text(buf, sizeof(buf), kDefaultDeepLink);
        len = strlen(buf);
    }

    long wrote = hsrc::sdk::write(fd, buf, len);
    (void)hsrc::sdk::close(fd);
    return wrote == (long)len;
}

void reveal_settings_window(long wid)
{
    if (wid < 0)
        return;

    hsrc::sdk::WindowOptions opts;
    if (hsrc::sdk::window_get((int)wid, opts)) {
        opts.visible = true;
        opts.minimized = false;
        (void)hsrc::sdk::window_set((int)wid, opts);
    }

    (void)hsrc::sdk::syscall2(SYS_WM_SHOW, wid, 1);
    (void)hsrc::sdk::syscall1(SYS_WM_FOCUS, wid);
}

long find_settings_window()
{
    long wid = hsrc::sdk::syscall1(SYS_WM_FIND, (long)kSettingsTitle);
    if (wid >= 0)
        return wid;

    /* Class lookup — one syscall, no id-space probe storm. */
    wid = hsrc::sdk::syscall1(SYS_WM_FIND_CLASS, (long)kSettingsClass);
    if (wid >= 0)
        return wid;
    return hsrc::sdk::syscall1(SYS_WM_FIND_CLASS, (long)"os-settings");
}

} // namespace

namespace hsrc::sdk::settings {

bool open()
{
    return open_category("general");
}

bool open_category(const char *id)
{
    char uri[kDeepLinkBytes];
    memset(uri, 0, sizeof(uri));
    copy_text(uri, sizeof(uri), "settings://");
    append_text(uri, sizeof(uri), (id && id[0]) ? id : "general");
    return open_deeplink(uri);
}

bool open_deeplink(const char *uri)
{
    const char *target = (uri && uri[0]) ? uri : kDefaultDeepLink;
    if (!write_deeplink_file(target))
        return false;

    long wid = find_settings_window();
    if (wid >= 0) {
        reveal_settings_window(wid);
        return true;
    }

    long pid = hsrc::sdk::process::spawn(kSettingsPath);
    return pid > 0;
}

Appearance appearance()
{
    ensure_theme_loaded();
    return g_appearance;
}

ThemeMode theme_mode()
{
    ensure_theme_loaded();
    return g_theme_mode;
}

const AppTheme &theme()
{
    ensure_theme_loaded();
    return g_theme_mode == ThemeMode::Dark ? kDarkTheme : kLightTheme;
}

bool refresh_theme()
{
    /*
     * Apps poll this from their frame loops. A missing /etc/os-settings.ini
     * used to be re-opened every few yields → VFS + serial spam starved input.
     * Seed a default file once; while still absent, negative-cache hard.
     * When the file exists, each explicit refresh_theme() re-reads (callers
     * already throttle via kThemePollEvery).
     */
    if (!g_theme_loaded) {
        load_theme_from_disk(true);
        g_absent_skip = 0;
        return true;
    }

    if (!g_ini_present) {
        if (g_absent_skip + 1 < kAbsentRetryEvery) {
            g_absent_skip++;
            return false;
        }
        g_absent_skip = 0;
    }

    Appearance prev_appearance = g_appearance;
    ThemeMode prev_mode = g_theme_mode;
    load_theme_from_disk(!g_ini_present);
    if (g_ini_present)
        g_absent_skip = 0;
    return prev_mode != g_theme_mode || prev_appearance != g_appearance;
}

bool set_appearance(Appearance appearance)
{
    if (!upsert_ini_key(kAppearanceKey, appearance_name(appearance)))
        return false;
    apply_theme(appearance);
    g_ini_present = true;
    return true;
}

bool toggle_theme()
{
    ensure_theme_loaded();
    Appearance next = (g_theme_mode == ThemeMode::Dark) ? Appearance::Light
                                                         : Appearance::Dark;
    return set_appearance(next);
}

const StatusInfo &status()
{
    if (!g_status_loaded)
        load_status_from_disk();
    return g_status;
}

bool refresh_status()
{
    /* Status keys change rarely (Settings save). Avoid open/read/close storms. */
    static unsigned s_skip;
    constexpr unsigned kReloadEvery = 8; /* caller already rate-limits */

    if (g_status_loaded && ++s_skip < kReloadEvery)
        return false;
    s_skip = 0;

    StatusInfo prev = g_status;
    load_status_from_disk();
    return prev.wifi_connected != g_status.wifi_connected ||
           prev.wifi_bars != g_status.wifi_bars ||
           prev.battery_percent != g_status.battery_percent ||
           prev.battery_charging != g_status.battery_charging;
}

} // namespace hsrc::sdk::settings

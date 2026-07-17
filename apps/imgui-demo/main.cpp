#include <stddef.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_ugx.h"

#include <user/gx.h>
#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/syscall.hpp>

namespace {

using hsrc::sdk::ChromeHit;
using hsrc::sdk::Color;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kChromeTitleH;
using hsrc::sdk::settings::refresh_theme;
using hsrc::sdk::settings::theme;

constexpr int kWinW = 720;
constexpr int kWinH = 480;
constexpr int kThemePollEvery = 240;

Window g_win;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev{};
int g_clicks = 0;
bool g_dirty = true;
bool g_imgui_ready = false;
int g_theme_poll = 0;

bool refresh_window_options()
{
    if (!g_win.get_options(g_win_opts))
        return false;
    return true;
}

void apply_imgui_theme()
{
    const auto &t = theme();
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *c = style.Colors;

    auto to_im = [](Color col) -> ImVec4 {
        return ImVec4((float)hsrc::sdk::color_r(col) / 255.0f,
                      (float)hsrc::sdk::color_g(col) / 255.0f,
                      (float)hsrc::sdk::color_b(col) / 255.0f,
                      (float)hsrc::sdk::color_a(col) / 255.0f);
    };

    c[ImGuiCol_WindowBg] = to_im(t.panel);
    c[ImGuiCol_ChildBg] = to_im(t.card);
    c[ImGuiCol_PopupBg] = to_im(t.card);
    c[ImGuiCol_Border] = to_im(t.border);
    c[ImGuiCol_Text] = to_im(t.text);
    c[ImGuiCol_TextDisabled] = to_im(t.text_dim);
    c[ImGuiCol_TitleBg] = to_im(t.chrome);
    c[ImGuiCol_TitleBgActive] = to_im(t.chrome);
    c[ImGuiCol_FrameBg] = to_im(t.inset);
    c[ImGuiCol_FrameBgHovered] = to_im(t.hover);
    c[ImGuiCol_FrameBgActive] = to_im(t.accent_soft);
    c[ImGuiCol_Button] = to_im(t.button);
    c[ImGuiCol_ButtonHovered] = to_im(t.hover);
    c[ImGuiCol_ButtonActive] = to_im(t.accent);
    c[ImGuiCol_Header] = to_im(t.accent_soft);
    c[ImGuiCol_HeaderHovered] = to_im(t.hover);
    c[ImGuiCol_HeaderActive] = to_im(t.accent);
    c[ImGuiCol_CheckMark] = to_im(t.accent);
    c[ImGuiCol_SliderGrab] = to_im(t.accent);
    c[ImGuiCol_SliderGrabActive] = to_im(t.accent);
    c[ImGuiCol_Separator] = to_im(t.border);
}

void paint()
{
    if (!g_win.ok() || !g_imgui_ready)
        return;
    if (!refresh_window_options())
        return;

    const auto &t = theme();
    hsrc::sdk::Surface &s = g_win.surface();

    /* Client body under chrome. */
    s.fill(0, kChromeTitleH, g_win_opts.w, g_win_opts.h - kChromeTitleH, t.bg);
    s.draw_window_chrome(g_win_opts.w, g_win_opts.title, g_win_opts,
                         t.chrome, t.text, t.border);

    ImGui_ImplUgx_RenderDrawData(ImGui::GetDrawData(), s, kChromeTitleH);
    g_win.damage();
}

void handle_chrome_click(const Input &in)
{
    if (!refresh_window_options())
        return;
    if (g_win_opts.minimized || !g_win_opts.visible)
        return;

    const int lx = in.mouse_x - g_win_opts.x;
    const int ly = in.mouse_y - g_win_opts.y;
    if (lx < 0 || ly < 0 || lx >= g_win_opts.w || ly >= g_win_opts.h)
        return;

    ChromeHit chrome = g_win.hit_chrome(lx, ly, g_win_opts);
    if (chrome != ChromeHit::None) {
        (void)g_win.handle_chrome_hit(chrome);
        (void)refresh_window_options();
        g_dirty = true;
    }
}

} // namespace

extern "C" void mke_main(void)
{
    ImGui::SetAllocatorFunctions(
        [](size_t sz, void *) -> void * { return malloc(sz); },
        [](void *p, void *) { free(p); },
        nullptr);

    if (!hsrc::sdk::screen_info(g_screen) || g_screen.width == 0) {
        for (;;)
            hsrc::sdk::yield(32u);
    }

    (void)refresh_theme();
    (void)hsrc::sdk::process::console_show(
        (pid_t)hsrc::sdk::process::getpid(), false);

    WindowOptions opts;
    opts.w = kWinW;
    opts.h = kWinH;
    opts.x = g_screen.width > (uint32_t)kWinW
                 ? (int)(g_screen.width - kWinW) / 2
                 : 40;
    opts.y = g_screen.height > (uint32_t)kWinH
                 ? (int)(g_screen.height - kWinH) / 2
                 : 40;
    opts.radius = 10;
    opts.rounded = true;
    opts.shadow = true;
    opts.framed = true;
    opts.background = false;
    opts.accept_focus = true;
    opts.resizable = false;
    opts.closable = true;
    opts.can_minimize = true;
    opts.can_maximize = true;
    opts.capture_keys = true;
    opts.set_title("ImGui Demo");
    opts.set_class_name("imgui.demo");

    if (!g_win.create(opts))
        hsrc::sdk::exit(1);
    (void)refresh_window_options();

    g_win.show(true);
    g_win.focus();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    apply_imgui_theme();

    ImGuiStyle &style = ImGui::GetStyle();
    style.AntiAliasedLines = false;
    style.AntiAliasedFill = false;
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    if (!ImGui_ImplUgx_Init())
        hsrc::sdk::exit(1);
    g_imgui_ready = true;
    g_dirty = true;

    for (;;) {
        if (!g_win.ok()) {
            (void)g_win.close();
            hsrc::sdk::exit(0);
        }

        g_theme_poll++;
        if (g_theme_poll >= kThemePollEvery) {
            g_theme_poll = 0;
            if (refresh_theme()) {
                apply_imgui_theme();
                g_dirty = true;
            }
        }

        (void)refresh_window_options();

        Input in{};
        if (hsrc::sdk::input(in)) {
            const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev.buttons);
            const bool interactive = !g_win_opts.minimized && g_win_opts.visible;

            if (interactive && (pressed & UGX_BTN_LEFT) && in.hits(g_win.id()))
                handle_chrome_click(in);

            ImGui_ImplUgx_NewFrame(g_win, in, g_win_opts, g_prev.buttons,
                                   kChromeTitleH);
            ImGui::NewFrame();

            const float pad = 12.0f;
            ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2((float)g_win_opts.w - pad * 2.0f,
                                            (float)(g_win_opts.h - kChromeTitleH) -
                                                pad * 2.0f),
                                     ImGuiCond_FirstUseEver);
            ImGui::Begin("Hello mykernel", nullptr,
                         ImGuiWindowFlags_NoCollapse);
            ImGui::TextUnformatted("Dear ImGui on MKDX / ugx");
            ImGui::Text("hit=%d focus=%d", (int)in.hit_id, (int)in.focus_id);
            if (ImGui::Button("Click me")) {
                g_clicks++;
                g_dirty = true;
            }
            ImGui::SameLine();
            ImGui::Text("Clicks: %d", g_clicks);
            ImGui::Separator();
            ImGui::TextWrapped(
                "Client-drawn chrome, hit_id click gating, wheel + keys.");
            static char buf[64] = "type here";
            if (ImGui::InputText("Name", buf, sizeof(buf)))
                g_dirty = true;
            ImGui::End();

            ImGui::Render();
            /* Soft-float raster is expensive — only repaint when UI moved. */
            if (ImGui::IsAnyItemActive() || ImGui::IsAnyItemHovered() ||
                ImGui::GetIO().WantTextInput ||
                (in.buttons != g_prev.buttons) || in.wheel != 0)
                g_dirty = true;
            g_prev = in;
        }

        if (g_dirty && !g_win_opts.minimized) {
            paint();
            (void)hsrc::sdk::present();
            g_dirty = false;
            hsrc::sdk::yield(0);
        } else {
            /* Idle: Blocked sleep — bare yield(0)/yield(2) spun Ready forever. */
            hsrc::sdk::yield(g_win_opts.minimized ? 32u : 12u);
        }
    }
}

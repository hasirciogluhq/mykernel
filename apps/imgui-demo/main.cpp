#include <stddef.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_ugx.h"

#include <user/gx.h>
#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/sync.hpp>
#include <user/sdk/syscall.hpp>
#include <user/sdk/thread.hpp>

namespace {

using hsrc::sdk::Color;
using hsrc::sdk::GxDevice;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kChromeTitleH;
using hsrc::sdk::kGxWaitForever;
using hsrc::sdk::settings::refresh_theme;
using hsrc::sdk::settings::theme;

constexpr int kWinW = 720;
constexpr int kWinH = 480;
constexpr int kThemePollEvery = 240;

Window g_win;
GxDevice g_gx;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev{};
int g_clicks = 0;
bool g_imgui_ready = false;
bool g_dirty = true;
bool g_was_minimized = false;
int g_theme_poll = 0;

bool refresh_window_options()
{
    if (!g_win.get_options(g_win_opts))
        return false;
    if (g_was_minimized && !g_win_opts.minimized && g_win_opts.visible)
        g_dirty = true;
    g_was_minimized = g_win_opts.minimized;
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

    ImGui_ImplUgx_RenderDrawData(ImGui::GetDrawData(), s, kChromeTitleH);
    /* Present publishes once — no Window::damage here. */
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
            hsrc::sdk::this_thread::sleep_for(1000u);
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
    if (!g_gx.create(g_win))
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
    g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);

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
                g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);
                g_dirty = true;
            }
        }

        (void)refresh_window_options();

        const uint32_t wait_to =
            g_win_opts.minimized ? 200u : kGxWaitForever;
        Input in = g_gx.wait(wait_to);
        const bool dragging = g_gx.dragging();

        /* Global input seq wakes all apps; skip compose unless this window cares.
         * During titlebar drag, only dirty content (not every mouse hop). */
        const bool now_relevant =
            in.hit_id == g_win.id() || in.focus_id == g_win.id();
        const bool was_relevant =
            g_prev.hit_id == g_win.id() || g_prev.focus_id == g_win.id();
        const bool input_relevant =
            !dragging && (now_relevant || was_relevant);

        if (!dragging) {
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
            if (ImGui::Button("Click me"))
                g_clicks++;
            ImGui::SameLine();
            ImGui::Text("Clicks: %d", g_clicks);
            ImGui::Separator();
            ImGui::TextWrapped(
                "Client paint; chrome on kernel publish.");
            static char buf[64] = "type here";
            (void)ImGui::InputText("Name", buf, sizeof(buf));
            ImGui::End();

            ImGui::Render();
            g_prev = in;
        } else {
            g_prev = in;
        }

        /*
         * Present when dirty or (non-drag) meaningful input.
         * Avoids idle mouse-on-other-window and drag-hop republish storms.
         */
        if (!g_win_opts.minimized && (g_dirty || input_relevant)) {
            (void)g_gx.begin_scene();
            paint();
            (void)g_gx.end_scene();
            (void)g_gx.present();
            g_dirty = false;
        }
    }
}

#include <coroutine>
#include <stddef.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_ugx.h"

#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/syscall.hpp>

namespace {

using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;

struct FrameAwaiter {
    bool await_ready() const noexcept { return false; }
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise>) const noexcept {}
    void await_resume() const noexcept {}
};

struct AppTask {
    struct promise_type {
        AppTask get_return_object()
        {
            return AppTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept
        {
            for (;;)
                ;
        }
    };

    std::coroutine_handle<promise_type> h{};

    explicit AppTask(std::coroutine_handle<promise_type> handle) : h(handle) {}
    AppTask(const AppTask &) = delete;
    AppTask &operator=(const AppTask &) = delete;
    AppTask(AppTask &&o) noexcept : h(o.h) { o.h = {}; }
    ~AppTask()
    {
        if (h)
            h.destroy();
    }

    bool done() const { return !h || h.done(); }
    void resume()
    {
        if (h && !h.done())
            h.resume();
    }
};

Window g_win;
Input g_prev{};
int g_clicks = 0;

AppTask run_demo()
{
    ScreenInfo screen{};
    if (!hsrc::sdk::screen_info(screen) || screen.width == 0) {
        for (;;) {
            co_await FrameAwaiter{};
            hsrc::sdk::yield();
        }
    }

    WindowOptions opts;
    opts.w = 640;
    opts.h = 420;
    opts.x = screen.width > 640 ? (int)(screen.width - 640) / 2 : 40;
    opts.y = screen.height > 420 ? (int)(screen.height - 420) / 2 : 40;
    opts.background = true;
    opts.rounded = true;
    opts.shadow = true;
    opts.radius = 10;
    opts.resizable = false;
    opts.capture_keys = true;
    opts.set_title("ImGui Demo");
    opts.set_class_name("imgui.demo");

    if (!g_win.create(opts)) {
        for (;;) {
            co_await FrameAwaiter{};
            hsrc::sdk::yield();
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplUgx_Init()) {
        for (;;) {
            co_await FrameAwaiter{};
            hsrc::sdk::yield();
        }
    }

    for (;;) {
        Input in{};
        (void)hsrc::sdk::input(in);

        ImGui_ImplUgx_NewFrame(g_win, in, g_prev.buttons);
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 200), ImGuiCond_FirstUseEver);
        ImGui::Begin("Hello mykernel");
        ImGui::TextUnformatted("Dear ImGui + ugx software backend");
        if (ImGui::Button("Click me"))
            g_clicks++;
        ImGui::Text("Clicks: %d", g_clicks);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplUgx_RenderDrawData(ImGui::GetDrawData(), g_win.surface());
        g_win.damage();
        (void)hsrc::sdk::present();

        g_prev = in;
        co_await FrameAwaiter{};
        hsrc::sdk::yield();
    }
}

} // namespace

extern "C" void mke_main(void)
{
    ImGui::SetAllocatorFunctions(
        [](size_t sz, void *) -> void * { return malloc(sz); },
        [](void *p, void *) { free(p); },
        nullptr);

    AppTask task = run_demo();
    for (;;) {
        if (!task.done())
            task.resume();
        hsrc::sdk::yield();
    }
}

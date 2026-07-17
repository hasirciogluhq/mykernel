#include "imgui.h"

#include "imgui_impl_ugx.h"

#include <user/sdk/syscall.hpp>
#include <kernel/syscall.h>
#include <user/gx.h>
#include <drivers/keyboard.h>

using hsrc::sdk::WindowOptions;

namespace {

struct BackendData {
    int      win_id = -1;
    uint8_t *font_pixels = nullptr;
    int      font_w = 0;
    int      font_h = 0;
    uint32_t frame = 0;
};

BackendData *bd()
{
    return reinterpret_cast<BackendData *>(ImGui::GetIO().BackendRendererUserData);
}

uint32_t blend_argb(uint32_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a == 0)
        return dst;
    if (a == 255)
        return (255u << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    const uint32_t dr = (dst >> 16) & 0xFFu;
    const uint32_t dg = (dst >> 8) & 0xFFu;
    const uint32_t db = dst & 0xFFu;
    const uint32_t ia = 255u - a;
    const uint32_t or_ = (r * a + dr * ia) / 255u;
    const uint32_t og = (g * a + dg * ia) / 255u;
    const uint32_t ob = (b * a + db * ia) / 255u;
    return (255u << 24) | (or_ << 16) | (og << 8) | ob;
}

void put_pixel(hsrc::sdk::Surface &surf, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (!surf.valid() || x < 0 || y < 0 ||
        (uint32_t)x >= surf.width() || (uint32_t)y >= surf.height())
        return;
    uint32_t *p = reinterpret_cast<uint32_t *>(surf.pixels());
    uint32_t &dst = p[(uint32_t)y * surf.stride() + (uint32_t)x];
    dst = blend_argb(dst, r, g, b, a);
}

void sample_font(BackendData *bd, float u, float v, uint8_t &r, uint8_t &g, uint8_t &bl, uint8_t &a)
{
    if (!bd || !bd->font_pixels || bd->font_w <= 0 || bd->font_h <= 0) {
        r = g = bl = a = 255;
        return;
    }
    int x = (int)(u * (float)bd->font_w);
    int y = (int)(v * (float)bd->font_h);
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= bd->font_w)
        x = bd->font_w - 1;
    if (y >= bd->font_h)
        y = bd->font_h - 1;
    /* RGBA32 atlas */
    const uint8_t *px = bd->font_pixels + ((size_t)y * (size_t)bd->font_w + (size_t)x) * 4u;
    r = px[0];
    g = px[1];
    bl = px[2];
    a = px[3];
}

void draw_triangle(hsrc::sdk::Surface &surf, BackendData *b,
                   const ImDrawVert &v0, const ImDrawVert &v1, const ImDrawVert &v2,
                   int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    float min_x = v0.pos.x, max_x = v0.pos.x;
    float min_y = v0.pos.y, max_y = v0.pos.y;
    if (v1.pos.x < min_x) min_x = v1.pos.x;
    if (v2.pos.x < min_x) min_x = v2.pos.x;
    if (v1.pos.x > max_x) max_x = v1.pos.x;
    if (v2.pos.x > max_x) max_x = v2.pos.x;
    if (v1.pos.y < min_y) min_y = v1.pos.y;
    if (v2.pos.y < min_y) min_y = v2.pos.y;
    if (v1.pos.y > max_y) max_y = v1.pos.y;
    if (v2.pos.y > max_y) max_y = v2.pos.y;

    int x0 = (int)min_x;
    int y0 = (int)min_y;
    int x1 = (int)max_x + 1;
    int y1 = (int)max_y + 1;
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1)
        return;

    const float area =
        (v1.pos.x - v0.pos.x) * (v2.pos.y - v0.pos.y) -
        (v1.pos.y - v0.pos.y) * (v2.pos.x - v0.pos.x);
    if (area == 0.0f)
        return;
    const float inv = 1.0f / area;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = ((v1.pos.x - px) * (v2.pos.y - py) - (v1.pos.y - py) * (v2.pos.x - px)) * inv;
            float w1 = ((v2.pos.x - px) * (v0.pos.y - py) - (v2.pos.y - py) * (v0.pos.x - px)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                continue;

            float u = w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x;
            float v = w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y;

            auto unpack = [](ImU32 c, float &r, float &g, float &b, float &a) {
                r = (float)((c >> IM_COL32_R_SHIFT) & 0xFFu);
                g = (float)((c >> IM_COL32_G_SHIFT) & 0xFFu);
                b = (float)((c >> IM_COL32_B_SHIFT) & 0xFFu);
                a = (float)((c >> IM_COL32_A_SHIFT) & 0xFFu);
            };
            float r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2;
            unpack(v0.col, r0, g0, b0, a0);
            unpack(v1.col, r1, g1, b1, a1);
            unpack(v2.col, r2, g2, b2, a2);

            float cr = w0 * r0 + w1 * r1 + w2 * r2;
            float cg = w0 * g0 + w1 * g1 + w2 * g2;
            float cb = w0 * b0 + w1 * b1 + w2 * b2;
            float ca = w0 * a0 + w1 * a1 + w2 * a2;

            uint8_t tr, tg, tb, ta;
            sample_font(b, u, v, tr, tg, tb, ta);

            uint8_t out_r = (uint8_t)((cr * (float)tr) / 255.0f);
            uint8_t out_g = (uint8_t)((cg * (float)tg) / 255.0f);
            uint8_t out_b = (uint8_t)((cb * (float)tb) / 255.0f);
            uint8_t out_a = (uint8_t)((ca * (float)ta) / 255.0f);
            put_pixel(surf, x, y, out_r, out_g, out_b, out_a);
        }
    }
}

} // namespace

bool ImGui_ImplUgx_Init()
{
    ImGuiIO &io = ImGui::GetIO();
    IM_ASSERT(io.BackendRendererUserData == nullptr);

    BackendData *b = IM_NEW(BackendData)();
    io.BackendRendererUserData = b;
    io.BackendRendererName = "imgui_impl_ugx";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    unsigned char *pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    b->font_pixels = pixels;
    b->font_w = w;
    b->font_h = h;
    io.Fonts->SetTexID((ImTextureID)(uintptr_t)1);
    return true;
}

void ImGui_ImplUgx_Shutdown()
{
    ImGuiIO &io = ImGui::GetIO();
    BackendData *b = bd();
    if (!b)
        return;
    io.Fonts->SetTexID(0);
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    IM_DELETE(b);
}

void ImGui_ImplUgx_NewFrame(hsrc::sdk::Window &win, const hsrc::sdk::Input &in,
                            uint8_t prev_buttons)
{
    BackendData *b = bd();
    if (!b)
        return;

    b->win_id = win.id();
    ImGuiIO &io = ImGui::GetIO();

    WindowOptions opts;
    int win_x = 0, win_y = 0;
    if (win.get_options(opts)) {
        io.DisplaySize = ImVec2((float)opts.w, (float)opts.h);
        win_x = opts.x;
        win_y = opts.y;
    } else if (win.surface().valid()) {
        io.DisplaySize = ImVec2((float)win.surface().width(), (float)win.surface().height());
    }

    b->frame++;
    io.DeltaTime = 1.0f / 60.0f;

    const int lx = in.mouse_x - win_x;
    const int ly = in.mouse_y - win_y;
    io.AddMousePosEvent((float)lx, (float)ly);

    const bool focused = (in.focus_id == win.id());
    (void)prev_buttons;
    io.AddMouseButtonEvent(0, focused && (in.buttons & UGX_BTN_LEFT) != 0);
    io.AddMouseButtonEvent(1, focused && (in.buttons & UGX_BTN_RIGHT) != 0);
    io.AddMouseButtonEvent(2, focused && (in.buttons & UGX_BTN_MIDDLE) != 0);

    io.AddKeyEvent(ImGuiMod_Shift, (in.mods & KBD_MOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, (in.mods & KBD_MOD_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (in.mods & KBD_MOD_ALT) != 0);

    if (focused) {
        for (;;) {
            long k = hsrc::sdk::syscall1(SYS_WM_POP_KEY, win.id());
            if (k < 0)
                break;
            io.AddInputCharacter((unsigned int)k);
        }
    }
}

void ImGui_ImplUgx_RenderDrawData(ImDrawData *draw_data, hsrc::sdk::Surface &surf)
{
    BackendData *b = bd();
    if (!draw_data || !surf.valid() || !b)
        return;

    surf.clear(hsrc::sdk::rgb(30, 30, 34));

    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList *cmd_list = draw_data->CmdLists[n];
        const ImDrawVert *vtx = cmd_list->VtxBuffer.Data;
        const ImDrawIdx *idx = cmd_list->IdxBuffer.Data;

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd *pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback) {
                pcmd->UserCallback(cmd_list, pcmd);
                continue;
            }

            int clip_x0 = (int)pcmd->ClipRect.x;
            int clip_y0 = (int)pcmd->ClipRect.y;
            int clip_x1 = (int)pcmd->ClipRect.z;
            int clip_y1 = (int)pcmd->ClipRect.w;
            if (clip_x0 < 0) clip_x0 = 0;
            if (clip_y0 < 0) clip_y0 = 0;
            if (clip_x1 > (int)surf.width()) clip_x1 = (int)surf.width();
            if (clip_y1 > (int)surf.height()) clip_y1 = (int)surf.height();

            for (unsigned int i = 0; i < pcmd->ElemCount; i += 3) {
                const ImDrawIdx i0 = idx[pcmd->IdxOffset + i + 0];
                const ImDrawIdx i1 = idx[pcmd->IdxOffset + i + 1];
                const ImDrawIdx i2 = idx[pcmd->IdxOffset + i + 2];
                draw_triangle(surf, b,
                              vtx[pcmd->VtxOffset + i0],
                              vtx[pcmd->VtxOffset + i1],
                              vtx[pcmd->VtxOffset + i2],
                              clip_x0, clip_y0, clip_x1, clip_y1);
            }
        }
    }
}

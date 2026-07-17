#pragma once

#include <user/sdk/gfx.hpp>

struct ImDrawData;

bool ImGui_ImplUgx_Init();
void ImGui_ImplUgx_Shutdown();
void ImGui_ImplUgx_NewFrame(hsrc::sdk::Window &win, const hsrc::sdk::Input &in,
                            uint8_t prev_buttons);
void ImGui_ImplUgx_RenderDrawData(ImDrawData *draw_data, hsrc::sdk::Surface &surf);

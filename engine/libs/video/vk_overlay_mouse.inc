/* ---- [mouse] the mouse camera block of the Controls page (agent/mouse, 2026-11-11) ----
 * Included by vk_overlay.cpp before tab_controls (needs section, hint, combo_w,
 * extra_set); [menu-e] the rows themselves are vk_overlay_menu.inc's (page_controls).
 * Settings (not in kVar): PS3_MOUSE_MODE (direct / stick), PS3_MOUSE_SENS,
 * PS3_MOUSE_AIM_SENS -- live through cellPad's setters, kept in the settings
 * file as other lines (extra_set), shown from cellPad's current values every
 * frame. The direct mode itself: libs/input/pad_mouse_direct.h. */
#include "../../include/ps3emu/ps3_mouse_camera.h"
extern "C" void cellPad_host_mouse_settings(int* on, float* sens, int* invert_x, int* invert_y);
extern "C" void cellPad_host_set_mouse_settings(float sens, int invert_x, int invert_y);

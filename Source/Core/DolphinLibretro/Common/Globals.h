#pragma once

#include <libretro.h>
#include <mutex>
#include <vector>

#include "Core/ActionReplay.h"
#include "Core/GeckoCode.h"
#include "Core/IOS/USB/Emulated/Microphone.h"

namespace ExpansionInterface
{
class CEXIMic;
}

namespace Libretro
{
extern retro_environment_t environ_cb;
extern bool g_emuthread_launched;
extern std::vector<Gecko::GeckoCode> g_gecko_codes;
extern std::vector<ActionReplay::ARCode> g_ar_codes;
inline constexpr unsigned g_gbplayer_subsystem_id = 0x101;

namespace Input
{
extern retro_microphone_interface g_microphone_interface;
extern bool g_has_microphone_support;
extern bool g_gc_mic_button[4];
extern std::vector<IOS::HLE::USB::Microphone*> g_active_microphones;
// Every GameCube microphone that exists, whichever slot it is in. A CEXIMic
// adds itself on construction and removes itself on destruction, under the lock.
extern std::mutex g_gc_microphones_lock;
extern std::vector<ExpansionInterface::CEXIMic*> g_gc_microphones;
} // namespace Input

namespace Video
{
extern retro_video_refresh_t video_cb;
extern struct retro_hw_render_callback hw_render;
}  // namespace Video
}  // namespace Libretro

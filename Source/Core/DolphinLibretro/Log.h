#pragma once

#include <string>

#include <libretro.h>

namespace Libretro
{
namespace Log
{
void Init();
void Shutdown();

// Straight to the frontend at RETRO_LOG_WARN, past Dolphin's own log types and
// levels: for the few lines a frontend that shows only warnings must still see,
// such as which bus a cable joined.
void Warn(const std::string& line);
}
}  // namespace Libretro

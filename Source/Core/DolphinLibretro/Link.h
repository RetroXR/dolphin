// Copyright 2026 RetroXR
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <libretro.h>

namespace Libretro
{
// The frontend's link bus (RETRO_ENVIRONMENT_GET_LINK_INTERFACE), or nullptr
// when it has none. Asked for once, on first use, and shared by every cable
// this core can be on.
const retro_link_interface* GetLinkInterface();
}  // namespace Libretro

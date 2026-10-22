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

// Take every broadband adapter off the bus. An unload calls this before it stops
// the machine: a CPU thread parked waiting on a console that has stopped too
// would otherwise hold the stop up for ever (EthernetLink.cpp).
void DetachEthernetLinks();
}  // namespace Libretro

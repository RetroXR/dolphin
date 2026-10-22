// Copyright 2026 RetroXR
// SPDX-License-Identifier: GPL-2.0-or-later

// The frontend-hosted link bus, asked for once and shared by every emulated
// cable this core can be on. Each wire protocol attaches its own ports on it.

#include "DolphinLibretro/Link.h"

#include "Common/Logging/Log.h"

#include "DolphinLibretro/Common/Globals.h"

namespace Libretro
{
namespace
{
const retro_link_interface* g_link = nullptr;
bool g_link_probed = false;
}  // namespace

const retro_link_interface* GetLinkInterface()
{
  if (g_link_probed)
    return g_link;
  g_link_probed = true;

  if (!environ_cb)
    return nullptr;

  // The frontend FILLS THE STRUCT, it does not hand back a pointer to one. The
  // address of a pointer variable is the wrong thing to pass and does not fail
  // politely: six function pointers land on eight bytes of stack and the process
  // dies somewhere else entirely.
  //
  // Experimental number first, then the plain one, so a core built today keeps
  // working against a frontend that later adopts the unflagged value.
  static retro_link_interface stored{};
  retro_link_interface probe{};
  if (environ_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE, &probe) ||
      environ_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE_FINAL, &probe))
  {
    if (probe.attach && probe.detach && probe.peers && probe.send && probe.recv && probe.advance)
    {
      stored = probe;
      g_link = &stored;
    }
  }

  if (g_link)
    INFO_LOG_FMT(CORE, "Link: the frontend hosts a link bus");
  return g_link;
}
}  // namespace Libretro

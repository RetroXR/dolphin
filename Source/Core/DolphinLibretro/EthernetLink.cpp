// Copyright 2026 RetroXR
// SPDX-License-Identifier: GPL-2.0-or-later

// A broadband adapter's Ethernet port on the frontend's link bus.
//
// The pacing, the frames and DHCP all live in the adapter (Core/HW/EXI/BBA/
// LinkBBA.cpp); this is only the carriage, the libretro end of
// EthernetLinkTransport. One message is one Ethernet frame exactly as it is on
// the wire, broadcast to every other console on the cable, which is a hub.

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Host.h"

#include "DolphinLibretro/Link.h"
#include "DolphinLibretro/LinkInterface.h"
#include "DolphinLibretro/Log.h"

namespace Libretro
{
namespace
{
// Named the same in every core that speaks it -- the PS2's network adapter too,
// so a GameCube and a PS2 can share a hub and hear each other's broadcasts as
// they would on a real one.
constexpr const char ETHERNET_PROTOCOL[] = "ethernet";

// The link port the adapter attaches on. Ports 0-3 are the controller sockets,
// where a Game Boy Advance cable goes.
constexpr unsigned ETHERNET_PORT = 4;

class EthernetTransport;

// Every transport alive, so an unload can take them all off the bus before it
// stops the machine (DetachEthernetLinks).
std::mutex g_live_mutex;
std::vector<EthernetTransport*> g_live;

class EthernetTransport final : public EthernetLinkTransport
{
public:
  EthernetTransport(const retro_link_interface* link, retro_link_port_t* handle)
      : m_link(link), m_handle(handle)
  {
    std::lock_guard lock(g_live_mutex);
    g_live.push_back(this);
  }

  ~EthernetTransport() override
  {
    {
      std::lock_guard lock(g_live_mutex);
      g_live.erase(std::remove(g_live.begin(), g_live.end(), this), g_live.end());
    }
    Detach();
  }

  // Leaving the bus is also what returns a CPU thread parked in Advance.
  void Detach()
  {
    if (!m_detached.exchange(true))
      m_link->detach(m_handle);
  }

  unsigned Peers() override
  {
    if (m_detached)
      return 0;
    unsigned count = 0;
    return m_link->peers(m_handle, &count) >= 0 && count >= 2 ? count : 0;
  }

  bool Advance(u64 now, u64 safe, u64 request) override
  {
    if (m_detached)
      return false;
    u32 wake = RETRO_LINK_WAKE_NONE;
    m_link->advance(m_handle, now, safe, request, &wake);
    return (wake & RETRO_LINK_WAKE_TOPOLOGY) != 0;
  }

  void Send(const u8* frame, size_t len, u64 tick) override
  {
    if (!m_detached)
      m_link->send(m_handle, tick, RETRO_LINK_BROADCAST, frame, len);
  }

  bool Receive(u8* frame, size_t* len, u64* tick) override
  {
    if (m_detached)
      return false;
    unsigned from = 0;
    return m_link->recv(m_handle, tick, &from, frame, len);
  }

private:
  const retro_link_interface* m_link;
  retro_link_port_t* m_handle;
  std::atomic<bool> m_detached{false};
};
}  // namespace

void DetachEthernetLinks()
{
  std::lock_guard lock(g_live_mutex);
  for (EthernetTransport* transport : g_live)
    transport->Detach();
}
}  // namespace Libretro

std::unique_ptr<EthernetLinkTransport> Host_CreateEthernetLinkTransport(u64 ticks_per_second)
{
  const retro_link_interface* link = Libretro::GetLinkInterface();
  if (!link || !ticks_per_second)
    return nullptr;

  retro_link_port_t* handle =
      link->attach(Libretro::ETHERNET_PORT, Libretro::ETHERNET_PROTOCOL, ticks_per_second);
  if (!handle)
  {
    Libretro::Log::Warn(
        fmt::format("BBA link: the frontend refused a link on port {}", Libretro::ETHERNET_PORT));
    return nullptr;
  }

  Libretro::Log::Warn(fmt::format("BBA link: port {} on the frontend's bus at {} Hz",
                                  Libretro::ETHERNET_PORT, ticks_per_second));
  return std::make_unique<Libretro::EthernetTransport>(link, handle);
}

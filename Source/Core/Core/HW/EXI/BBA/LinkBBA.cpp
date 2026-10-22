// Copyright 2026 RetroXR
// SPDX-License-Identifier: GPL-2.0-or-later

// The broadband adapter's Ethernet port, cabled to other consoles the frontend
// is running, through a bus the frontend hosts (Host.h, EthernetLinkTransport).
//
// The far end is a hub. Every frame this adapter sends goes to every other
// console on the cable, and every frame they send arrives here; the adapter's
// own address filter (RecvMACFilter) keeps what is meant for it. What the bus
// adds over a socket is a clock. A frame is stamped with the tick it lands at,
// and is handed to the adapter when this console reaches that tick, on the CPU
// thread -- not from a read thread whenever the host happens to have it -- so
// what a game hears depends on emulated time alone.
//
// The pacing is the PS2 network adapter's, and its i.LINK cable's before it: a
// console promises the others not to send anything that lands within the next
// millisecond, may run only as far as every other console has promised, and
// meets the cable twice per promise so that consoles a little apart run
// together instead of taking turns.
//
// DHCP is answered inside the console, as a router on the cable would, with an
// address that is this console's alone: 10.x.y.z/8, x.y.z the tail of its MAC.
// No request goes on the cable, so no two consoles ever answer each other.

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/System.h"

namespace ExpansionInterface
{
namespace
{
CoreTiming::EventType* s_et_link_tick = nullptr;

// The longest frame a hub passes on, a tagged one included, and the shortest the
// wire pads every frame to.
constexpr size_t FRAME_MAX = 1518;
constexpr size_t FRAME_MIN = 60;
// Frames received and not yet due. Past this the rest wait on the bus, which
// keeps them in order: back-pressure, not loss.
constexpr size_t RX_QUEUE = 512;

constexpr u16 DHCP_SERVER_PORT = 67;
constexpr u16 DHCP_CLIENT_PORT = 68;
}  // namespace

void CEXIETHERNET::Init(CoreTiming::CoreTimingManager& core_timing)
{
  s_et_link_tick = core_timing.RegisterEvent("BBALinkTick", LinkTickCallback);
}

void CEXIETHERNET::LinkTickCallback(Core::System& system, u64, s64)
{
  IEXIDevice* device = system.GetExpansionInterface().GetDevice(Slot::SP1);
  if (!device || device->m_device_type != EXIDeviceType::EthernetLink)
    return;
  auto* eth = static_cast<CEXIETHERNET*>(device);
  if (eth->m_link_interface)
    eth->m_link_interface->Tick();
}

CEXIETHERNET::LinkNetworkInterface::LinkNetworkInterface(CEXIETHERNET* eth_ref)
    : NetworkInterface(eth_ref)
{
  // Nintendo's adapter OUI and three random bytes, made here rather than read
  // from the shared setting (see the constructor). The first of the three is
  // also the second byte of this console's IP, so it is never 0 or 255.
  m_mac = Common::GenerateMacAddress(Common::MACConsumer::BBA);
  m_mac[3] = static_cast<u8>(1 + m_mac[3] % 254);
}

CEXIETHERNET::LinkNetworkInterface::~LinkNetworkInterface()
{
  Deactivate();
}

bool CEXIETHERNET::LinkNetworkInterface::Activate()
{
  if (m_transport)
    return true;

  // Joined on first use, not at construction: the frontend needs this console's
  // tick rate, which the system timers do not have while the machine is still
  // being assembled. A game that never brings its adapter up never joins, and so
  // never holds up the other consoles on the cable either.
  const u64 ticks_per_second = m_eth_ref->m_system.GetSystemTimers().GetTicksPerSecond();
  if (!ticks_per_second)
    return false;
  m_transport = Host_CreateEthernetLinkTransport(ticks_per_second);
  if (!m_transport)
  {
    WARN_LOG_FMT(SP1, "BBA link: the frontend has no link bus; the adapter has nothing plugged in");
    return false;
  }

  u64 ahead_us = 1000;
  if (const char* env = std::getenv("DOLPHIN_ETH_AHEAD_US"))
  {
    const long v = std::strtol(env, nullptr, 10);
    if (v > 0)
    {
      ahead_us = static_cast<u64>(v);
      WARN_LOG_FMT(SP1, "BBA link: promise {} us, stretch {} us (DOLPHIN_ETH_AHEAD_US)", v, v / 2);
    }
  }
  m_ahead = ticks_per_second * ahead_us / 1000000;
  m_grain = std::max<u64>(m_ahead / 2, 1);
  m_grain_alone = ticks_per_second / 60;
  m_gap = ticks_per_second / 4;
  m_have_raw = false;
  m_safe = m_next_rv = m_last_stamp = 0;
  m_peers = 0;
  m_rx.clear();

  WARN_LOG_FMT(SP1, "BBA link: on the cable as {}, DHCP offers 10.{}.{}.{}/8",
               Common::MacAddressToString(m_mac), m_mac[3], m_mac[4], m_mac[5]);

  // Meet the cable at once: the others are held where this console joined until
  // it says where it stands.
  Schedule();
  return true;
}

void CEXIETHERNET::LinkNetworkInterface::Deactivate()
{
  if (!m_transport)
    return;
  WARN_LOG_FMT(SP1, "BBA link: off the cable ({} frames sent, {} received)", m_sent, m_received);
  // Leaving the bus; the event finds no transport and stops rescheduling.
  m_transport.reset();
  m_rx.clear();
  m_peers = 0;
}

bool CEXIETHERNET::LinkNetworkInterface::IsActivated()
{
  return m_transport != nullptr;
}

bool CEXIETHERNET::LinkNetworkInterface::RecvInit()
{
  return true;
}

void CEXIETHERNET::LinkNetworkInterface::RecvStart()
{
  m_recv_enabled = true;
}

void CEXIETHERNET::LinkNetworkInterface::RecvStop()
{
  m_recv_enabled = false;
}

u64 CEXIETHERNET::LinkNetworkInterface::Now()
{
  const u64 raw = m_eth_ref->m_system.GetCoreTiming().GetTicks();
  if (!m_have_raw)
  {
    m_have_raw = true;
    m_last_raw = raw;
    return m_now;
  }
  if (raw > m_last_raw && raw - m_last_raw <= m_gap)
    m_now += raw - m_last_raw;
  m_last_raw = raw;
  return m_now;
}

bool CEXIETHERNET::LinkNetworkInterface::SendFrame(const u8* frame, u32 size)
{
  if (!AnswerDHCP(frame, size) && m_transport && m_peers >= 2 && size >= 14 && size <= FRAME_MAX)
  {
    // Stamped no earlier than the promise last made, which is what lets the
    // other consoles run up to it without asking.
    u64 tick = std::max({Now(), m_safe, m_last_stamp});
    m_last_stamp = tick;

    u8 buf[FRAME_MAX];
    std::memcpy(buf, frame, size);
    size_t len = size;
    if (len < FRAME_MIN)
    {
      std::memset(buf + len, 0, FRAME_MIN - len);
      len = FRAME_MIN;
    }
    m_transport->Send(buf, len, tick);
    ++m_sent;
  }

  // On a hub the frame has left once it is on the wire, whoever hears it.
  m_eth_ref->SendComplete();
  return true;
}

// A DHCP request, answered here and never put on the cable. Returns false for
// anything else.
bool CEXIETHERNET::LinkNetworkInterface::AnswerDHCP(const u8* frame, u32 size)
{
  const Common::PacketView view(frame, size);
  const std::optional<u16> ethertype = view.GetEtherType();
  if (!ethertype || *ethertype != Common::IPV4_ETHERTYPE)
    return false;
  const std::optional<u8> ip_proto = view.GetIPProto();
  if (!ip_proto || *ip_proto != IPPROTO_UDP)
    return false;
  const std::optional<Common::UDPPacket> udp = view.GetUDPPacket();
  if (!udp || ntohs(udp->udp_header.destination_port) != DHCP_SERVER_PORT)
    return false;
  if (!m_transport)
    return true;

  const Common::DHCPPacket dhcp(udp->data);
  const Common::DHCPBody& request = dhcp.body;

  const u8 client_bytes[4] = {10, m_mac[3], m_mac[4], m_mac[5]};
  const u8 router_bytes[4] = {10, 0, 0, 1};
  u32 client_ip;
  u32 router_ip;
  std::memcpy(&client_ip, client_bytes, sizeof(client_ip));
  std::memcpy(&router_ip, router_bytes, sizeof(router_ip));
  // Nothing on the cable answers for the router; it exists for the lease.
  constexpr Common::MACAddress router_mac{0x00, 0x09, 0xbf, 0xff, 0xff, 0xfe};

  sockaddr_in from{};
  sockaddr_in to{};
  from.sin_family = AF_INET;
  from.sin_addr.s_addr = router_ip;
  from.sin_port = htons(DHCP_SERVER_PORT);
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = client_ip;
  to.sin_port = htons(DHCP_CLIENT_PORT);

  const std::vector<u8> router(router_bytes, router_bytes + 4);
  constexpr auto lease_24h = std::to_array<u8>({0, 1, 0x51, 0x80});

  Common::DHCPPacket reply;
  reply.body = Common::DHCPBody(request.transaction_id, m_mac, client_ip, router_ip);
  // An offer to a discover, an acknowledgement to anything else.
  (dhcp.options.empty() || dhcp.options[0].size() < 3 || dhcp.options[0].at(2) == 1) ?
      reply.AddOption(53, {2}) :
      reply.AddOption(53, {5});
  reply.AddOption(54, router);
  reply.AddOption(51, lease_24h);
  reply.AddOption(58, lease_24h);
  reply.AddOption(59, lease_24h);
  reply.AddOption(1, {255, 0, 0, 0});
  reply.AddOption(28, {10, 255, 255, 255});
  reply.AddOption(3, router);
  reply.AddOption(6, router);
  reply.AddOption(255, {});

  const Common::UDPPacket response(m_mac, router_mac, from, to, reply.Build());
  Frame f;
  f.tick = Now();
  f.data = response.Build();
  const auto at = std::upper_bound(m_rx.begin(), m_rx.end(), f.tick,
                                   [](u64 t, const Frame& e) { return t < e.tick; });
  m_rx.insert(at, std::move(f));
  Schedule();
  return true;
}

void CEXIETHERNET::LinkNetworkInterface::Tick()
{
  if (!m_transport)
    return;
  const u64 now = Now();
  if (now >= m_next_rv)
    Rendezvous(now);
  Deliver();
  Schedule();
}

void CEXIETHERNET::LinkNetworkInterface::Rendezvous(u64 now)
{
  const bool linked = m_peers >= 2;
  const u64 grain = linked ? m_grain : m_grain_alone;
  const u64 ahead = linked ? m_ahead : m_grain_alone;

  // Published before anything is read: a console parked on this one's promise
  // cannot move until it has been told the promise moved. Blocks, on the CPU
  // thread, until the others have promised far enough -- as the GBA cable does.
  m_safe = std::max(m_safe, now + ahead);
  m_transport->Advance(now, m_safe, now + grain);

  const unsigned peers = m_transport->Peers();
  if (peers != m_peers)
  {
    WARN_LOG_FMT(SP1, "BBA link: the cable now joins {} console(s){}", peers ? peers : 1,
                 peers >= 2 ? "" : " -- nothing on the other end");
  }
  m_peers = peers;
  Drain();
  m_next_rv = now + grain;
}

void CEXIETHERNET::LinkNetworkInterface::Drain()
{
  u8 buf[FRAME_MAX];
  while (m_rx.size() < RX_QUEUE)
  {
    size_t len = sizeof(buf);
    u64 tick = 0;
    if (!m_transport->Receive(buf, &len, &tick))
      break;
    if (len < 14)
      continue;
    Frame f;
    f.tick = tick;
    f.data.assign(buf, buf + len);
    const auto at = std::upper_bound(m_rx.begin(), m_rx.end(), tick,
                                     [](u64 t, const Frame& e) { return t < e.tick; });
    m_rx.insert(at, std::move(f));
  }
}

void CEXIETHERNET::LinkNetworkInterface::Deliver()
{
  const u64 now = Now();
  while (!m_rx.empty() && m_rx.front().tick <= now)
  {
    Frame f = std::move(m_rx.front());
    m_rx.pop_front();
    // A receiver that is switched off hears nothing, as on the hardware.
    if (!m_recv_enabled)
      continue;
    const u32 len = static_cast<u32>(std::min<size_t>(f.data.size(), BBA_RECV_SIZE));
    std::memcpy(m_eth_ref->mRecvBuffer.get(), f.data.data(), len);
    m_eth_ref->mRecvBufferLength = len;
    m_eth_ref->RecvHandlePacket();
    ++m_received;
  }
}

void CEXIETHERNET::LinkNetworkInterface::Schedule()
{
  if (!m_transport || !s_et_link_tick)
    return;
  const u64 now = Now();
  u64 next = m_next_rv;
  if (!m_rx.empty())
    next = std::min(next, m_rx.front().tick);
  const s64 delta = next > now ? static_cast<s64>(next - now) : 0;
  auto& core_timing = m_eth_ref->m_system.GetCoreTiming();
  core_timing.RemoveEvent(s_et_link_tick);
  core_timing.ScheduleEvent(delta, s_et_link_tick);
}
}  // namespace ExpansionInterface

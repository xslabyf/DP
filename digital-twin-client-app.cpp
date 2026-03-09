#include "digital-twin-client-app.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/nstime.h"
#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"

// TraCI iba tu
#include "ns3/traci-client.h"
#include "ns3/sumo-TraCIAPI.h"

#include <sstream>
#include <iostream>
#include <cctype>
#include <cstring>

// POSIX
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("DigitalTwinClientApp");

static bool
SetNonBlocking (int fd)
{
  int flags = ::fcntl (fd, F_GETFL, 0);
  if (flags < 0) return false;
  if (::fcntl (fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
  return true;
}

TypeId
DigitalTwinClientApp::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::DigitalTwinClientApp")
    .SetParent<Application> ()
    .SetGroupName ("Automotive")
    .AddConstructor<DigitalTwinClientApp> ()

    .AddAttribute ("Enabled",
                   "Zapne/vypne aplikáciu.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&DigitalTwinClientApp::m_enabled),
                   MakeBooleanChecker ())

    // Primárne: IP ako string
    .AddAttribute ("RemoteIp",
                   "IP adresa servera (telemetria).",
                   StringValue ("127.0.0.1"),
                   MakeStringAccessor (&DigitalTwinClientApp::m_remoteIp),
                   MakeStringChecker ())

    .AddAttribute ("RemotePort",
                   "UDP port servera (kam ide telemetria auto->server).",
                   UintegerValue (5005),
                   MakeUintegerAccessor (&DigitalTwinClientApp::m_remotePort),
                   MakeUintegerChecker<uint16_t> ())

    .AddAttribute ("TxInterval",
                   "Ako často posielať telemetriu na server.",
                   TimeValue (MilliSeconds (100)),
                   MakeTimeAccessor (&DigitalTwinClientApp::m_txInterval),
                   MakeTimeChecker ())

    .AddAttribute ("Interval",
                         "Kompatibilita so starším kódom (alias pre TxInterval).",
                         TimeValue (MilliSeconds (100)),
                         MakeTimeAccessor (&DigitalTwinClientApp::m_txInterval),
                         MakeTimeChecker ())

    .AddAttribute ("ListenPort",
                   "UDP port, na ktorom autá počúvajú príkazy zo servera (server->autá).",
                   UintegerValue (5006),
                   MakeUintegerAccessor (&DigitalTwinClientApp::m_listenPort),
                   MakeUintegerChecker<uint16_t> ())

    .AddAttribute ("RxPollInterval",
                   "Ako často sa polluje recvfrom() (non-blocking).",
                   TimeValue (MilliSeconds (50)),
                   MakeTimeAccessor (&DigitalTwinClientApp::m_rxPollInterval),
                   MakeTimeChecker ())

    // ✅ Kompatibilita: ak niekde v scenári stále nastavuješ RemoteAddress
    .AddAttribute ("RemoteAddress",
                   "Kompatibilita: InetSocketAddress(IP,port) pre server.",
                   AddressValue (InetSocketAddress (Ipv4Address ("127.0.0.1"), 5005)),
                   MakeAddressAccessor (&DigitalTwinClientApp::SetRemoteAddress,
                                        &DigitalTwinClientApp::GetRemoteAddress),
                   MakeAddressChecker ());

  return tid;
}

DigitalTwinClientApp::DigitalTwinClientApp ()
  : m_remoteIp ("127.0.0.1"),
    m_remotePort (5005),
    m_txInterval (MilliSeconds (100)),
    m_enabled (true),
    m_listenPort (5006),
    m_rxPollInterval (MilliSeconds (50)),
    m_traci (nullptr),
    m_txEvent (),
    m_rxEvent (),
    m_txFd (-1),
    m_rxFd (-1),
    m_txDstReady (false),
    m_hasOverride (false),
    m_overrideUntil (Seconds (0))
{
  std::memset (&m_txDst, 0, sizeof (m_txDst));
}

DigitalTwinClientApp::~DigitalTwinClientApp ()
{
  if (m_txFd >= 0) { ::close (m_txFd); m_txFd = -1; }
  if (m_rxFd >= 0) { ::close (m_rxFd); m_rxFd = -1; }
}

void
DigitalTwinClientApp::SetTraci (Ptr<TraciClient> traci, const std::string& sumoVehId)
{
  m_traci = traci;
  m_sumoVehId = sumoVehId;
}

// ---- RemoteAddress compatibility ----
void
DigitalTwinClientApp::SetRemoteAddress (Address addr)
{
  InetSocketAddress isa = InetSocketAddress::ConvertFrom (addr);

  // Ipv4Address -> string (bez ToString(), kompatibilné)
  std::ostringstream oss;
  oss << isa.GetIpv4 ();
  m_remoteIp = oss.str ();

  // ak chceš, nech RemoteAddress prepíše aj port:
  m_remotePort = isa.GetPort ();

  // znovu priprav TX destináciu (ak už beží appka)
  m_txDstReady = false;
}

Address
DigitalTwinClientApp::GetRemoteAddress () const
{
  return InetSocketAddress (Ipv4Address (m_remoteIp.c_str ()), m_remotePort);
}

// ----------------- POSIX UDP -----------------

void
DigitalTwinClientApp::EnsureTxSocket ()
{
  if (m_txFd >= 0 && m_txDstReady)
    return;

  if (m_txFd < 0)
  {
    m_txFd = ::socket (AF_INET, SOCK_DGRAM, 0);
    if (m_txFd < 0)
    {
      NS_LOG_ERROR ("[DT] socket() failed: " << std::strerror(errno));
      return;
    }
  }

  std::memset (&m_txDst, 0, sizeof (m_txDst));
  m_txDst.sin_family = AF_INET;
  m_txDst.sin_port   = htons (m_remotePort);

  if (::inet_pton (AF_INET, m_remoteIp.c_str (), &m_txDst.sin_addr) != 1)
  {
    NS_LOG_ERROR ("[DT] inet_pton failed for RemoteIp=" << m_remoteIp);
    m_txDstReady = false;
    return;
  }

  m_txDstReady = true;
}

void
DigitalTwinClientApp::EnsureRxSocket ()
{
  if (m_rxFd >= 0)
    return;

  m_rxFd = ::socket (AF_INET, SOCK_DGRAM, 0);
  if (m_rxFd < 0)
  {
    NS_LOG_ERROR ("[DT] RX socket() failed: " << std::strerror(errno));
    return;
  }

  int one = 1;
  ::setsockopt (m_rxFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
  ::setsockopt (m_rxFd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

  sockaddr_in addr;
  std::memset (&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons (m_listenPort);
  addr.sin_addr.s_addr = htonl (INADDR_ANY);

  if (::bind (m_rxFd, (sockaddr*)&addr, sizeof(addr)) < 0)
  {
    NS_LOG_ERROR ("[DT] bind(:" << m_listenPort << ") failed: " << std::strerror(errno));
    ::close (m_rxFd);
    m_rxFd = -1;
    return;
  }

  if (!SetNonBlocking (m_rxFd))
  {
    NS_LOG_WARN ("[DT] failed to set RX socket non-blocking");
  }

  NS_LOG_INFO ("[DT] Listening for server commands on UDP port " << m_listenPort);
}

bool
DigitalTwinClientApp::SendUdp (const uint8_t* data, uint32_t len)
{
  EnsureTxSocket ();
  if (m_txFd < 0 || !m_txDstReady)
    return false;

  ssize_t sent = ::sendto (m_txFd,
                           (const void*)data,
                           (size_t)len,
                           0,
                           (sockaddr*)&m_txDst,
                           sizeof (m_txDst));
  if (sent < 0)
  {
    NS_LOG_ERROR ("[DT] sendto() failed: " << std::strerror(errno));
    return false;
  }
  return true;
}

// ----------------- ns-3 lifecycle -----------------

void
DigitalTwinClientApp::StartApplication ()
{
  EnsureTxSocket ();
  EnsureRxSocket ();

  ScheduleNextTx ();
  ScheduleNextRxPoll ();
}

void
DigitalTwinClientApp::StopApplication ()
{
  if (m_txEvent.IsRunning ())
    Simulator::Cancel (m_txEvent);

  if (m_rxEvent.IsRunning ())
    Simulator::Cancel (m_rxEvent);

  if (m_txFd >= 0) { ::close (m_txFd); m_txFd = -1; }
  if (m_rxFd >= 0) { ::close (m_rxFd); m_rxFd = -1; }
}

void
DigitalTwinClientApp::ScheduleNextTx ()
{
  m_txEvent = Simulator::Schedule (m_txInterval, &DigitalTwinClientApp::SendOnce, this);
}

void
DigitalTwinClientApp::ScheduleNextRxPoll ()
{
  m_rxEvent = Simulator::Schedule (m_rxPollInterval, &DigitalTwinClientApp::PollRx, this);
}

// ----------------- TX: telemetry -----------------

void
DigitalTwinClientApp::SendOnce ()
{
  if (!m_enabled)
  {
    ScheduleNextTx ();
    return;
  }

  // ak práve beží override a už vypršal, tak reset (SUMO: -1 = default)
  if (m_hasOverride && Simulator::Now () >= m_overrideUntil && m_traci && !m_sumoVehId.empty ())
  {
    try {
      m_traci->TraCIAPI::vehicle.setSpeed (m_sumoVehId, -1.0);
      NS_LOG_INFO ("[DT] override expired -> reset speed for " << m_sumoVehId);
    } catch (...) {
      NS_LOG_WARN ("[DT] reset speed failed for " << m_sumoVehId);
    }
    m_hasOverride = false;
  }

  if (!m_traci || m_sumoVehId.empty ())
  {
    ScheduleNextTx ();
    return;
  }

  auto pos = m_traci->TraCIAPI::vehicle.getPosition (m_sumoVehId);
  auto ll  = m_traci->TraCIAPI::simulation.convertXYtoLonLat (pos.x, pos.y);

  double speed   = m_traci->TraCIAPI::vehicle.getSpeed (m_sumoVehId);
  double heading = m_traci->TraCIAPI::vehicle.getAngle (m_sumoVehId);

  std::ostringstream ss;
  ss << "{"
     << "\"id\":\"" << m_sumoVehId << "\","
     << "\"timestamp\":" << (uint64_t)Simulator::Now ().GetMilliSeconds () << ","
     << "\"latitude\":" << ll.y << ","
     << "\"longitude\":" << ll.x << ","
     << "\"speed\":" << speed << ","
     << "\"heading\":" << heading
     << "}";

  std::string payload = ss.str ();
  SendUdp ((const uint8_t*)payload.data (), (uint32_t)payload.size ());

  ScheduleNextTx ();
}

// ---- mini JSON parsing (demo) ----
bool
DigitalTwinClientApp::ExtractJsonString (const std::string& s, const std::string& key, std::string& out)
{
  std::string k = "\"" + key + "\"";
  size_t p = s.find (k);
  if (p == std::string::npos) return false;
  p = s.find (":", p);
  if (p == std::string::npos) return false;
  p = s.find ("\"", p);
  if (p == std::string::npos) return false;
  size_t q = s.find ("\"", p + 1);
  if (q == std::string::npos) return false;
  out = s.substr (p + 1, q - (p + 1));
  return true;
}

bool
DigitalTwinClientApp::ExtractJsonNumber (const std::string& s, const std::string& key, double& out)
{
  std::string k = "\"" + key + "\"";
  size_t p = s.find (k);
  if (p == std::string::npos) return false;
  p = s.find (":", p);
  if (p == std::string::npos) return false;
  p++;

  while (p < s.size () && std::isspace ((unsigned char)s[p]))
    p++;

  size_t q = p;
  while (q < s.size () && (std::isdigit((unsigned char)s[q]) || s[q] == '-' || s[q] == '+' || s[q] == '.' || s[q] == 'e' || s[q] == 'E'))
    q++;

  if (q == p) return false;

  try {
    out = std::stod (s.substr (p, q - p));
    return true;
  } catch (...) {
    return false;
  }
}

void
DigitalTwinClientApp::ApplyCommandFromServer (const std::string& json)
{
  std::string targetId;
  if (!ExtractJsonString (json, "targetId", targetId))
    return;

  if (targetId != m_sumoVehId)
    return;

  std::string cmd;
  if (!ExtractJsonString (json, "cmd", cmd))
    return;

  if (cmd != "SLOW")
    return;

  double newSpeed = 5.0;
  ExtractJsonNumber (json, "speed", newSpeed);

  double durationMs = 3000.0;
  ExtractJsonNumber (json, "durationMs", durationMs);

  if (!m_traci || m_sumoVehId.empty ())
    return;

  try {
      std::cout << "[AUTO] PRIKAZ SLOW pre "
                << m_sumoVehId
                << " nova rychlost = " << newSpeed
                << " na " << durationMs << " ms"
                << std::endl;
    m_traci->TraCIAPI::vehicle.setSpeed (m_sumoVehId, newSpeed);
    m_hasOverride = true;
    m_overrideUntil = Simulator::Now () + MilliSeconds ((uint64_t)durationMs);

    NS_LOG_INFO ("[DT] SLOW applied to " << m_sumoVehId
                 << " speed=" << newSpeed
                 << " durationMs=" << durationMs);
  } catch (...) {
    NS_LOG_WARN ("[DT] setSpeed failed for " << m_sumoVehId);
  }
}

// ----------------- RX: server commands -----------------

void
DigitalTwinClientApp::PollRx ()
{
  if (!m_enabled)
  {
    ScheduleNextRxPoll ();
    return;
  }

  EnsureRxSocket ();
  if (m_rxFd < 0)
  {
    ScheduleNextRxPoll ();
    return;
  }

  for (;;)
  {
    uint8_t buf[2048];
    sockaddr_in src;
    socklen_t srclen = sizeof(src);
    ssize_t n = ::recvfrom (m_rxFd, buf, sizeof(buf), 0, (sockaddr*)&src, &srclen);

    if (n < 0)
    {
      if (errno == EWOULDBLOCK || errno == EAGAIN)
        break;
      NS_LOG_ERROR ("[DT] recvfrom error: " << std::strerror(errno));
      break;
    }
    if (n == 0)
      break;

    std::string msg ((const char*)buf, (size_t)n);
    ApplyCommandFromServer (msg);
  }

  ScheduleNextRxPoll ();
}

} // namespace ns3

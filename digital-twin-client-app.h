#ifndef DIGITAL_TWIN_CLIENT_APP_H
#define DIGITAL_TWIN_CLIENT_APP_H

#include "ns3/application.h"
#include "ns3/address.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/type-id.h"

#include <string>
#include <cstdint>

// POSIX
#include <netinet/in.h> // sockaddr_in

namespace ns3 {

class TraciClient;

class DigitalTwinClientApp : public Application
{
public:
  static TypeId GetTypeId ();

  DigitalTwinClientApp ();
  ~DigitalTwinClientApp () override;

  void SetTraci (Ptr<TraciClient> traci, const std::string& sumoVehId);

private:
  void StartApplication () override;
  void StopApplication () override;

  // periodic tasks
  void ScheduleNextTx ();
  void SendOnce ();

  void ScheduleNextRxPoll ();
  void PollRx ();

  // POSIX UDP helpers
  void EnsureTxSocket ();
  void EnsureRxSocket ();
  bool SendUdp (const uint8_t* data, uint32_t len);

  // Compatibility for RemoteAddress attribute
  void SetRemoteAddress (Address addr);
  Address GetRemoteAddress () const;

  // very small JSON helpers (bez externých knižníc)
  static bool ExtractJsonString (const std::string& s, const std::string& key, std::string& out);
  static bool ExtractJsonNumber (const std::string& s, const std::string& key, double& out);

  void ApplyCommandFromServer (const std::string& json);

private:
  // ===== Attributes =====
  std::string m_remoteIp;     // server IP (digitálne dvojča)
  uint16_t    m_remotePort;   // server UDP port (príjem telemetrie)
  Time        m_txInterval;
  bool        m_enabled;

  // server -> autá (broadcast/unicast), všetci počúvajú na jednom porte
  uint16_t m_listenPort;
  Time     m_rxPollInterval;

  // ===== TraCI =====
  Ptr<TraciClient> m_traci;
  std::string      m_sumoVehId;

  // ===== Scheduling =====
  EventId m_txEvent;
  EventId m_rxEvent;

  // ===== POSIX UDP state =====
  int m_txFd;
  int m_rxFd;

  sockaddr_in m_txDst;
  bool m_txDstReady;

  // ===== demo logic (spomalenie) =====
  bool     m_hasOverride;
  Time     m_overrideUntil;
};

} // namespace ns3

#endif // DIGITAL_TWIN_CLIENT_APP_H

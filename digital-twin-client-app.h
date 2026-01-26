#pragma once

#include "ns3/application.h"
#include "ns3/address.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/socket.h"
#include "ns3/nstime.h"

#include <string>

namespace ns3 {

class TraciClient; // forward declaration

class DigitalTwinClientApp : public Application
{
public:
  static TypeId GetTypeId ();
  DigitalTwinClientApp ();

  void SetRemote (Address remote, uint16_t port);
  void SetInterval (Time t);

  // tu si dovolíš “napichnúť” Traci zvonka:
  void SetTraci (Ptr<TraciClient> traci, const std::string& sumoVehId);

private:
  void StartApplication () override;
  void StopApplication () override;

  void EnsureSocket ();
  void ScheduleNext ();
  void SendOnce ();

  Ptr<Socket> m_socket;
  Address m_remote;
  uint16_t m_port;
  Time m_interval;
  bool m_enabled;
  EventId m_sendEvent;

  // TraCI údaje
  Ptr<TraciClient> m_traci;
  std::string m_sumoVehId;
};

} // namespace ns3

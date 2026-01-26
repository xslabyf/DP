#include "digital-twin-client-app.h"

#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/simulator.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/inet-socket-address.h"
#include "ns3/log.h"

// TraCI len TU:
#include "ns3/traci-client.h"
#include "ns3/sumo-TraCIAPI.h"   // podľa toho čo potrebuješ

#include <sstream>

NS_LOG_COMPONENT_DEFINE("DigitalTwinClientApp");


namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("DigitalTwinClientApp");

TypeId
DigitalTwinClientApp::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::DigitalTwinClientApp")
    .SetParent<Application> ()
    .SetGroupName ("Automotive")
    .AddConstructor<DigitalTwinClientApp> ()

    .AddAttribute ("RemoteAddress",
                   "IP adresa servera (digitálneho dvojčaťa).",
                   AddressValue (InetSocketAddress (Ipv4Address ("127.0.0.1"), 5005)),
                   MakeAddressAccessor (&DigitalTwinClientApp::m_remote),
                   MakeAddressChecker ())

    .AddAttribute ("RemotePort",
                   "UDP port servera.",
                   UintegerValue (5005),
                   MakeUintegerAccessor (&DigitalTwinClientApp::m_port),
                   MakeUintegerChecker<uint16_t> ())

    .AddAttribute ("Interval",
                   "Ako často posielať správy.",
                   TimeValue (MilliSeconds (100)),
                   MakeTimeAccessor (&DigitalTwinClientApp::m_interval),
                   MakeTimeChecker ());
  return tid;
}

DigitalTwinClientApp::DigitalTwinClientApp ()
  : m_port (5005),
    m_interval (MilliSeconds (100)),
    m_enabled (true)
{
}

void
DigitalTwinClientApp::SetTraci (Ptr<TraciClient> traci, const std::string& sumoVehId)
{
  m_traci = traci;
  m_sumoVehId = sumoVehId;
}

void
DigitalTwinClientApp::EnsureSocket ()
{
  if (m_socket) return;

  m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
  InetSocketAddress remote = InetSocketAddress::ConvertFrom (m_remote);
  remote.SetPort (m_port);
  m_socket->Connect (remote);
}

void
DigitalTwinClientApp::StartApplication ()
{
  EnsureSocket ();
  ScheduleNext ();
}

void
DigitalTwinClientApp::StopApplication ()
{
  if (m_sendEvent.IsRunning ())
    Simulator::Cancel (m_sendEvent);

  if (m_socket)
    m_socket->Close ();
}

void
DigitalTwinClientApp::ScheduleNext ()
{
  m_sendEvent = Simulator::Schedule (m_interval, &DigitalTwinClientApp::SendOnce, this);
}

void
DigitalTwinClientApp::SendOnce ()
{
  if (!m_enabled || !m_traci || m_sumoVehId.empty ())
  {
    ScheduleNext ();
    return;
  }

  // 1) pozícia v SUMO (XY) + konverzia do lon/lat
  auto pos = m_traci->TraCIAPI::vehicle.getPosition (m_sumoVehId);
  auto ll  = m_traci->TraCIAPI::simulation.convertXYtoLonLat (pos.x, pos.y);

  // 2) rýchlosť, heading
  double speed = m_traci->TraCIAPI::vehicle.getSpeed (m_sumoVehId);
  double heading = m_traci->TraCIAPI::vehicle.getAngle (m_sumoVehId);

  // 3) payload (JSON)
  std::ostringstream ss;
  ss << "{"
     << "\"id\":\"" << m_sumoVehId << "\","
     << "\"timestamp\":" << (uint64_t)Simulator::Now().GetMilliSeconds() << ","
     << "\"latitude\":" << ll.y << ","
     << "\"longitude\":" << ll.x << ","
     << "\"speed\":" << speed << ","
     << "\"heading\":" << heading
     << "}";

  std::string payload = ss.str ();
  Ptr<Packet> p = Create<Packet> ((uint8_t*)payload.data (), payload.size ());
  int sent = m_socket->Send (p);
  if (sent < 0)
  {
    NS_LOG_ERROR ("DT UDP send failed, errno=" << m_socket->GetErrno ());
  }
  else
  {
    NS_LOG_INFO ("DT UDP sent bytes=" << sent << " to " << InetSocketAddress::ConvertFrom(m_remote).GetIpv4()
                                      << ":" << m_port);
  }



  ScheduleNext ();
}

} // namespace ns3

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014 Universidad de la República - Uruguay
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Matias Richart <mrichart@fing.edu.uy>
 */

/**
 * This example program is designed to illustrate the behavior of two
 * power/rate-adaptive WiFi rate controls; namely, ns3::ParfWifiManager
 * and ns3::AparfWifiManager.
 *
 * The output of this is typically two plot files, named throughput-parf.plt
 * (or throughput-aparf.plt, if Aparf is used) and power-parf.plt If
 * Gnuplot program is available, one can use it to convert the plt file
 * into an eps file, by running:
 * \code{.sh}
 *   gnuplot throughput-parf.plt
 * \endcode
 * Also, to enable logging of rate and power changes to the terminal, set this
 * environment variable:
 * \code{.sh}
 *   export NS_LOG=PowerAdaptationDistance=level_info
 * \endcode
 *
 * This simulation consist of 2 nodes, one AP and one STA.
 * The AP generates UDP traffic with a CBR of 54 Mbps to the STA.
 * The AP can use any power and rate control mechanism and the STA uses
 * only Minstrel rate control.
 * The STA can be configured to move away from (or towards to) the AP.
 * By default, the AP is at coordinate (0,0,0) and the STA starts at
 * coordinate (5,0,0) (meters) and moves away on the x axis by 1 meter every
 * second.
 *
 * The output consists of:
 * - A plot of average throughput vs. distance.
 * - A plot of average transmit power vs. distance.
 * - (if logging is enabled) the changes of power and rate to standard output.
 *
 * The Average Transmit Power is defined as an average of the power
 * consumed per measurement interval, expressed in milliwatts.  The
 * power level for each frame transmission is reported by the simulator,
 * and the energy consumed is obtained by multiplying the power by the
 * frame duration.  At every 'stepTime' (defaulting to 1 second), the
 * total energy for the collection period is divided by the step time
 * and converted from dbm to milliwatt units, and this average is
 * plotted against time.
 *
 * When neither Parf nor Aparf is selected as the rate control, the
 * generation of the plot of average transmit power vs distance is suppressed
 * since the other Wifi rate controls do not support the necessary callbacks
 * for computing the average power.
 *
 * To display all the possible arguments and their defaults:
 * \code{.sh}
 *   ./waf --run "power-adaptation-distance --help"
 * \endcode
 *
 * Example usage (selecting Aparf rather than Parf):
 * \code{.sh}
 *   ./waf --run "power-adaptation-distance --manager=ns3::AparfWifiManager --outputFileName=aparf"
 * \endcode
 *
 * Another example (moving towards the AP):
 * \code{.sh}
 *   ./waf --run "power-adaptation-distance --manager=ns3::AparfWifiManager --outputFileName=aparf --stepsSize=-1 --STA1_x=200"
 * \endcode
 *
 * To enable the log of rate and power changes:
 * \code{.sh}
 *   export NS_LOG=PowerAdaptationDistance=level_info
 * \endcode
 *
 * Managers:
 *  - ns3::ParfWifiManager
 *  - ns3::AparfWifiManager
 *  - ns3::RrpaaWifiManager
 *  - ns3::PrcsWifiManager
 *  - ns3::MinstrelBluesWifiManager
 */

#include <sstream>
#include <fstream>
#include <math.h>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/stats-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("PowerAdaptationDistance");

class EnergyModel {
	public:
		EnergyModel (std::string device);

    std::string getDevice() { return m_device; }
		double getTotalEnergy() { return m_total_energy; }

		// tx
    void computeModel(double time, int mcs, int txp) {
    	txp = pow(10, txp/10); // Transform dBm to mW
    	m_total_energy += (m_intercept_tx + m_mcs_beta_tx*mcs + m_txp_beta*txp) * time;
    }
		// rx
    void computeModel(double time, int mcs) {
    	m_total_energy += (m_intercept_rx + m_mcs_beta_rx*mcs) * time;
    }
		// idle
		void computeModel(double time) {
			m_total_energy += m_rho_idle * time;
		}

	private:
		std::string m_device;
		double m_rho_idle; 			// W
		double m_intercept_tx;  // W
		double m_mcs_beta_tx;   // Mbps
		double m_txp_beta;      // mW
		double m_intercept_rx;  // W
		double m_mcs_beta_rx;   // Mbps
		double m_total_energy;  // J
};

EnergyModel::EnergyModel (std::string device)
{
	m_device = device;
	if (device.compare("htc") == 0) {
		m_rho_idle = 0.63527;
		m_intercept_tx = 0.354;
		m_mcs_beta_tx = 0.0052;
		m_txp_beta = 0.021;
		m_intercept_rx = 0.013;
		m_mcs_beta_rx = 0.00643;
	} else if (device.compare("linksys") == 0) {
		m_rho_idle = 2.73;
		m_intercept_tx = 0.54;
		m_mcs_beta_tx = 0.0028;
		m_txp_beta = 0.075;
		m_intercept_rx = 0.14;
		m_mcs_beta_rx = 0.0130;
	} else if (device.compare("rpi") == 0) {
		m_rho_idle = 2.2203;
    m_intercept_tx = 0.478;
		m_mcs_beta_tx = 0.0008;
		m_txp_beta = 0.044;
		m_intercept_rx = -0.0062;
		m_mcs_beta_rx = 0.00146;
	} else if (device.compare("galaxy") == 0) {
		m_rho_idle = 0.59159;
		m_intercept_tx = 0.572;
    m_mcs_beta_tx = 0.0017;
    m_txp_beta = 0.0105;
    m_intercept_rx = 0.0409;
    m_mcs_beta_rx = 0.00173;
	} else if (device.compare("soekris") == 0) {
		m_rho_idle = 3.56;
    m_intercept_tx = 0.17;
    m_mcs_beta_tx = 0.017;
    m_txp_beta = 0.101;
    m_intercept_rx = 0.010;
    m_mcs_beta_rx = 0.0237;
  }
	m_total_energy = 0;
}

// packet size generated at the AP
static const uint32_t packetSize = 1420;
NodeContainer wifiApNodes;
NodeContainer wifiStaNodes;
NetDeviceContainer wifiApDevices;
NetDeviceContainer wifiStaDevices;
NetDeviceContainer wifiDevices;
std::string transportProtocol = "ns3::UdpSocketFactory";
ApplicationContainer apps_source;

typedef std::vector<std::pair<Time,WifiMode> > TxTime;

std::map<Mac48Address, uint32_t> actualPower;
std::map<Mac48Address, WifiMode> actualMode;
Ptr<WifiPhy> myPhy;
double init = 0;
double end = 0;
double txTime = 0;
double rxTime = 0;
uint32_t totalBytes = 0;
double totalEnergy = 0;
TxTime timeTable;
std::vector<EnergyModel> models;

double t = 0;

Time GetCalcTxTime (WifiMode mode)
{
  for (TxTime::const_iterator i = timeTable.begin (); i != timeTable.end (); i++) {
    if (mode == i->second)
      return i->first;
  }
  NS_ASSERT (false);
  return Seconds (0);
}

void PhyTxCallback (std::string path, Ptr<const Packet> packet)
{
  WifiMacHeader head;
  packet->PeekHeader (head);
  Mac48Address dest = head.GetAddr1 ();

  if (head.GetType() == WIFI_MAC_DATA) {
    t = GetCalcTxTime (actualMode[dest]).GetSeconds ();
    for (int i=0; i < models.size(); i++)
	    models.at(i).computeModel(t, actualMode[dest].GetDataRate()/1e6, actualPower[dest]);
	  NS_LOG_INFO ((Simulator::Now ()).GetSeconds () << " DATA: t " << t << ", rate " << actualMode[dest].GetDataRate()/1e6 << ", power " << actualPower[dest]);
    txTime += t;
    if (!init)
        init = Simulator::Now ().GetSeconds ();
    end = Simulator::Now().GetSeconds();
  }
}

void PhyRxBeginCallback (std::string path, Ptr<const Packet> packet)
{
  WifiMacHeader head;
  packet->PeekHeader (head);

  if (head.IsAck() && t > 0)
    t = Simulator::Now().GetSeconds();
}

void PhyRxOkCallback (std::string path, Ptr<const Packet> packet, double snr, WifiMode mode, enum WifiPreamble preamble)
{
  WifiMacHeader head;
  packet->PeekHeader (head);

  if (head.IsAck() && t > 0) {
    t = Simulator::Now().GetSeconds() - t;
    for (int i=0; i < models.size(); i++)
      models.at(i).computeModel(t, mode.GetDataRate()/1e6);
    NS_LOG_INFO ((Simulator::Now ()).GetSeconds () << " ACK: t " << t << ", rate " << mode.GetDataRate()/1e6);
    rxTime += t;
    t = 0;
  }
}

void PowerCallback (std::string path, uint8_t power, Mac48Address dest)
{
  double txPowerBaseDbm = myPhy->GetTxPowerStart ();
  double txPowerEndDbm = myPhy->GetTxPowerEnd ();
  uint32_t nTxPower = myPhy->GetNTxPower ();
  double dbm;
  if (nTxPower > 1)
    {
      dbm = txPowerBaseDbm + power * (txPowerEndDbm - txPowerBaseDbm) / (nTxPower - 1);
    }
  else
    {
      NS_ASSERT_MSG (txPowerBaseDbm == txPowerEndDbm, "cannot have TxPowerEnd != TxPowerStart with TxPowerLevels == 1");
      dbm = txPowerBaseDbm;
    }
  actualPower[dest] = dbm;
  NS_LOG_INFO ((Simulator::Now ()).GetSeconds () << " " << dest << " Power " << (int)power);
}

void BluesPowerCallback (std::string path, std::string type, uint8_t power, Mac48Address dest)
{
  double   txPowerBaseDbm = myPhy->GetTxPowerStart ();
  double   txPowerEndDbm = myPhy->GetTxPowerEnd ();
  uint32_t nTxPower = myPhy->GetNTxPower ();
  double dbm;
  if (nTxPower > 1)
    {
      dbm = txPowerBaseDbm + power * (txPowerEndDbm - txPowerBaseDbm) / (nTxPower - 1);
    }
  else
    {
      NS_ASSERT_MSG (txPowerBaseDbm == txPowerEndDbm, "cannot have TxPowerEnd != TxPowerStart with TxPowerLevels == 1");
      dbm = txPowerBaseDbm;
    }
  actualPower[dest] = dbm;
  NS_LOG_INFO ((Simulator::Now ()).GetSeconds () << " station: " << dest << ", frame sent with " << type << " power: " << (int)power);
}

void RateCallback (std::string path, uint32_t rate, Mac48Address dest)
{
  actualMode[dest] = myPhy->GetMode (rate);
  NS_LOG_INFO ((Simulator::Now ()).GetSeconds () << " " << dest << " Rate " <<  rate);
}

void BluesRateCallback (std::string path, std::string type, uint32_t rate, Mac48Address dest)
{
  actualMode[dest] = myPhy->GetMode (rate);
  NS_LOG_INFO ((Simulator::Now ()).GetSeconds () << " station: " << dest << ", frame sent with " << type << " rate: " <<  rate);
}

void CstCallback (std::string path, double cst, Mac48Address dest)
{
  NS_LOG_INFO ((Simulator::Now ()).GetSeconds () << " " << dest << " CST " <<  cst);
}

void RxCallback (std::string path, Ptr<const Packet> packet, const Address &from)
{
  totalBytes += packet->GetSize ();
}

static void StaMacAssoc (Mac48Address maddr)
{
  //Configure the IP stack
  InternetStackHelper stack;
  stack.Install (wifiApNodes);
  stack.Install (wifiStaNodes);
  Ipv4AddressHelper address;
  address.SetBase ("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer i = address.Assign (wifiDevices);
  Ipv4Address sinkAddress = i.GetAddress (0);
  uint16_t port = 9;

  //Configure the CBR generator
  PacketSinkHelper sink (transportProtocol, InetSocketAddress (sinkAddress, port));
  ApplicationContainer apps_sink = sink.Install (wifiStaNodes.Get (0));
  apps_sink.Start (Seconds (0.0));

  if (transportProtocol.compare("ns3::UdpSocketFactory") == 0) {
      OnOffHelper onoff ("ns3::UdpSocketFactory", InetSocketAddress (sinkAddress, port));
      onoff.SetConstantRate (DataRate ("54Mb/s"), packetSize);
      apps_source = onoff.Install (wifiApNodes.Get (0));
  } else {
      BulkSendHelper source ("ns3::TcpSocketFactory", InetSocketAddress (sinkAddress, port));
      source.SetAttribute("MaxBytes", UintegerValue(0));
      apps_source = source.Install (wifiApNodes.Get (0));
  }
  apps_source.Start(Seconds(0.0));

  Config::Connect ("/NodeList/1/ApplicationList/*/$ns3::PacketSink/Rx",
                   MakeCallback (&RxCallback));
}

static void StaMacDeAssoc (Mac48Address maddr)
{
  apps_source.Stop(Seconds(0.0));
}

int main (int argc, char *argv[])
{
  double maxPower = 17;
  double minPower = 0;
  uint32_t powerLevels = 18;

  uint32_t rtsThreshold = 2346;
  std::string manager = "ns3::ParfWifiManager";
  std::string outputFileName = "parf";
  int ap1_x = 0;
  int ap1_y = 0;
  int sta1_x = -113;
  int sta1_y = 5;
  double speed = 3;
  uint32_t simuTime = 38;
  bool enablePcap = false;

  CommandLine cmd;
  cmd.AddValue ("manager", "PRC Manager", manager);
  cmd.AddValue ("rtsThreshold", "RTS threshold", rtsThreshold);
  cmd.AddValue ("outputFileName", "Output filename", outputFileName);
  cmd.AddValue ("simuTime", "Time to simulate", simuTime);
  cmd.AddValue ("maxPower", "Maximum available transmission level (dbm).", maxPower);
  cmd.AddValue ("minPower", "Minimum available transmission level (dbm).", minPower);
  cmd.AddValue ("powerLevels", "Number of transmission power levels available between "
                "TxPowerStart and TxPowerEnd included.", powerLevels);
  cmd.AddValue ("transportProtocol", "Transport protocol of the CBR traffic", transportProtocol);
  cmd.AddValue ("AP1_x", "Position of AP1 in x coordinate", ap1_x);
  cmd.AddValue ("AP1_y", "Position of AP1 in y coordinate", ap1_y);
  cmd.AddValue ("STA1_x", "Position of STA1 in x coordinate", sta1_x);
  cmd.AddValue ("STA1_y", "Position of STA1 in y coordinate", sta1_y);
  cmd.AddValue ("speed", "Linear velocity for STA1", speed);
  cmd.AddValue ("enablePcap", "Enable pcap logging", enablePcap);
  cmd.Parse (argc, argv);

  std::string devices[] = {"htc", "linksys","rpi", "galaxy", "soekris"};

  for (int i=0; i < (sizeof(devices)/sizeof(devices[0])); i++)
    models.push_back(EnergyModel(devices[i]));

  // Define the APs
  wifiApNodes.Create (1);

  //Define the STAs
  wifiStaNodes.Create (1);

  WifiHelper wifi = WifiHelper::Default ();
  wifi.SetStandard (WIFI_PHY_STANDARD_80211a);
  NqosWifiMacHelper wifiMac = NqosWifiMacHelper::Default ();
  YansWifiPhyHelper wifiPhy = YansWifiPhyHelper::Default ();
  YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default ();

  wifiPhy.SetChannel (wifiChannel.Create ());

  //Configure the STA node
  wifi.SetRemoteStationManager ("ns3::MinstrelWifiManager", "RtsCtsThreshold", UintegerValue (rtsThreshold));
  wifiPhy.Set ("TxPowerStart", DoubleValue (maxPower));
  wifiPhy.Set ("TxPowerEnd", DoubleValue (maxPower));

  Ssid ssid = Ssid ("AP");
  wifiMac.SetType ("ns3::StaWifiMac", "Ssid", SsidValue (ssid),
                   "ActiveProbing", BooleanValue (false));
  wifiStaDevices.Add (wifi.Install (wifiPhy, wifiMac, wifiStaNodes.Get (0)));

  //Configure the AP node
  wifi.SetRemoteStationManager (manager, "DefaultTxPowerLevel", UintegerValue (maxPower), "RtsCtsThreshold", UintegerValue (rtsThreshold));
  wifiPhy.Set ("TxPowerStart", DoubleValue (minPower));
  wifiPhy.Set ("TxPowerEnd", DoubleValue (maxPower));
  wifiPhy.Set ("TxPowerLevels", UintegerValue (powerLevels));

  ssid = Ssid ("AP");
  wifiMac.SetType ("ns3::ApWifiMac", "Ssid", SsidValue (ssid));
  wifiApDevices.Add (wifi.Install (wifiPhy, wifiMac, wifiApNodes.Get (0)));

  wifiDevices.Add (wifiStaDevices);
  wifiDevices.Add (wifiApDevices);

  // Configure the mobility.
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
  //Initial position of AP and STA
  positionAlloc->Add (Vector (ap1_x, ap1_y, 0.0));
  NS_LOG_INFO ("Setting initial AP position to " << Vector (ap1_x, ap1_y, 0.0));
  positionAlloc->Add (Vector (sta1_x, sta1_y, 0.0));
  NS_LOG_INFO ("Setting initial STA position to " << Vector (sta1_x, sta1_y, 0.0));
  mobility.SetPositionAllocator (positionAlloc);
  mobility.SetMobilityModel ("ns3::ConstantVelocityMobilityModel");
  mobility.Install (wifiApNodes.Get (0));
  mobility.Install (wifiStaNodes.Get (0));
  NS_LOG_INFO ("Setting STA y-speed to " << speed);
  wifiStaNodes.Get(0)->GetObject<ConstantVelocityMobilityModel>()->SetVelocity(Vector(speed, 0.0, 0.0));

  Ptr<NetDevice> device = wifiApDevices.Get (0);
  Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice> (device);
  myPhy = wifiDevice->GetPhy ();
  uint32_t nModes = myPhy->GetNModes ();
  for (uint32_t i = 0; i < nModes; i++) {
    WifiMode mode = myPhy->GetMode (i);
    WifiTxVector txVector;
    txVector.SetMode (mode);
    timeTable.push_back (std::make_pair (myPhy->CalculateTxDuration (packetSize, txVector, WIFI_PREAMBLE_LONG, myPhy->GetFrequency (), 0, 0), mode));
  }
  for (uint32_t j = 0; j < wifiStaDevices.GetN (); j++) {
    Ptr<NetDevice> staDevice = wifiStaDevices.Get (j);
    Ptr<WifiNetDevice> wifiStaDevice = DynamicCast<WifiNetDevice> (staDevice);
    Mac48Address addr = wifiStaDevice->GetMac ()->GetAddress ();
    actualPower[addr] = 17;
    actualMode[addr] = myPhy->GetMode(0);
  }
  actualMode[Mac48Address ("ff:ff:ff:ff:ff:ff")] = myPhy->GetMode(0);

  //------------------------------------------------------------
  //-- Setup stats and data collection
  //--------------------------------------------

  Ptr<WifiNetDevice> wifiStaDevice = DynamicCast<WifiNetDevice> (wifiStaDevices.Get (0));
  wifiStaDevice->GetMac()->TraceConnectWithoutContext("Assoc", MakeCallback(&StaMacAssoc));
  wifiStaDevice->GetMac()->TraceConnectWithoutContext("DeAssoc", MakeCallback(&StaMacDeAssoc));

  //Register packet receptions to calculate throughput/energy
  Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/Phy/PhyTxBegin",
                   MakeCallback (&PhyTxCallback));
  Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/Phy/PhyRxBegin",
		               MakeCallback (&PhyRxBeginCallback));
  Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/Phy/State/RxOk",
                   MakeCallback (&PhyRxOkCallback));

  //Register power and rate changes to calculate the Average Transmit Power
  if (manager.find ("ns3::MinstrelBlues") == 0) {
    Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/$" + manager + "/PowerChange",
                       MakeCallback (&BluesPowerCallback));
    Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/$" + manager + "/RateChange",
                       MakeCallback (&BluesRateCallback));
  } else {
    Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/$" + manager + "/PowerChange",
                     MakeCallback (&PowerCallback));
    Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/$" + manager + "/RateChange",
                     MakeCallback (&RateCallback));
  }

  if (manager.find ("ns3::Prcs") == 0) {
  	Config::Connect ("/NodeList/0/DeviceList/*/$ns3::WifiNetDevice/RemoteStationManager/$" + manager + "/CstChange",
  			             MakeCallback (CstCallback));
  }

  if (enablePcap)
    wifiPhy.EnablePcapAll (outputFileName);

  Simulator::Stop (Seconds (simuTime));
  Simulator::Run ();
  Simulator::Destroy ();

  for (int i=0; i < models.size(); i++) {
    models.at(i).computeModel(end - init - txTime - rxTime);
    std::cout <<
      end << " " <<
      init << " " <<
      txTime << " " <<
			rxTime << " " <<
      totalBytes << " " <<
      models.at(i).getDevice() << " " <<
      models.at(i).getTotalEnergy() <<
    std::endl;
	}

  return 0;
}

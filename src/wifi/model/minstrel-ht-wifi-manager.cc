/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2009 Duy Nguyen
 * Copyright (c) 2015 Ghada Badawy
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
 * Author: Duy Nguyen <duy@soe.ucsc.edu>
 *         Ghada Badawy <gbadawy@gmail.com>
 *         Matias Richart <mrichart@fing.edu.uy>
 *
 * Some Comments:
 *
 * 1) By default, Minstrel applies the multi-rate retry(the core of Minstrel
 *    algorithm). Otherwise, please use ConstantRateWifiManager instead.
 *
 * 2) Currently it doesn't support aggregation. It is not considered in tx time calculations
 *    and in retries.
 *
 * reference: http://lwn.net/Articles/376765/
 */

#include "minstrel-ht-wifi-manager.h"
#include "wifi-phy.h"
#include "ns3/random-variable-stream.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/wifi-mac.h"
#include "ns3/assert.h"
#include <vector>

#define Min(a,b) ((a < b) ? a : b)

NS_LOG_COMPONENT_DEFINE ("MinstrelHtWifiManager");


namespace ns3 {


struct MinstrelHtWifiRemoteStation : public WifiRemoteStation
{
  void DisposeStation ();
  Time m_nextStatsUpdate;   //!< Time when stats will be updated.

  /**
   * To keep track of the current position in the our random sample table
   * going row by row from 1st column until the 10th column(Minstrel defines 10)
   * then we wrap back to the row 1 col 1.
   * note: there are many other ways to do this.
   */
  uint32_t m_col, m_index;

  uint32_t m_maxTpRate;     //!< The highest throughput rate.
  uint32_t m_maxTpRate2;    //!< The second highest throughput rate.
  uint32_t m_maxProbRate;   //!< The rate with highest probability of success.

  uint32_t m_frameCount;    //!< Total number of frames transmitted as of now.
  uint32_t m_sampleCount;   //!< How many packets we have sample so far.

  bool m_isSampling;        //!< A flag to indicate we are currently sampling.
  uint32_t m_sampleRate;    //!< The current sample rate.
  bool  m_sampleRateSlower; //!< A flag to indicate sample rate is slower.
  uint32_t m_sampleGroup;   //!< The group that the sample rate belongs to.
  uint32_t m_numSamplesSlow;//!< Number of times a slow rate was sampled.

  uint32_t m_shortRetry;    //!< Number of short retries (such as control frames).
  uint32_t m_longRetry;     //!< Number of long retries (such as data packets).
  uint32_t m_err;           //!< Number of retry errors (all retransmission attempts failed).

  uint32_t m_txRate;        //!< Current transmission rate.

  uint32_t m_nSupportedMcs; //!< Number of supported rates by the remote station.

  HtSampleRate m_sampleTable;   //!< Sample rates table.
  McsGroupData m_mcsTable;      //!< Table of groups with stats.
};

void
MinstrelHtWifiRemoteStation::DisposeStation ()
{
  std::vector<std::vector<uint32_t> > ().swap (m_sampleTable);
  for (uint8_t j = 0; j < m_mcsTable.size (); j++)
    {
      std::vector<struct HtRateInfo> ().swap (m_mcsTable[j].m_minstrelTable);
    }
  std::vector<struct GroupInfo> ().swap (m_mcsTable);
}

struct MinstrelHtWifiRemoteStationAux : public WifiRemoteStation
{
  void DisposeStation ();
  MinstrelHtWifiRemoteStation *m_htStation;
  MinstrelWifiRemoteStation *m_legacyStation;
  bool m_isHt;
  bool m_initialized; //!< For initializing variables.
};

void
MinstrelHtWifiRemoteStationAux::DisposeStation ()
{
  if (m_htStation)
    {
      m_htStation->DisposeStation();
    }
}

NS_OBJECT_ENSURE_REGISTERED (MinstrelHtWifiManager);

TypeId
MinstrelHtWifiManager::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MinstrelHtWifiManager")
    .SetParent<WifiRemoteStationManager> ()
    .AddConstructor<MinstrelHtWifiManager> ()
    .SetGroupName ("Wifi")
    .AddAttribute ("UpdateStatistics",
                   "The interval between updating statistics table ",
                   TimeValue (Seconds (0.1)),
                   MakeTimeAccessor (&MinstrelHtWifiManager::m_updateStats),
                   MakeTimeChecker ())
    .AddAttribute ("LookAroundRate",
                   "the percentage to try other rates",
                   DoubleValue (10),
                   MakeDoubleAccessor (&MinstrelHtWifiManager::m_lookAroundRate),
                   MakeDoubleChecker<double> (0, 100))
    .AddAttribute ("EWMA",
                   "EWMA level",
                   DoubleValue (75),
                   MakeDoubleAccessor (&MinstrelHtWifiManager::m_ewmaLevel),
                   MakeDoubleChecker<double> (0, 100))
    .AddAttribute ("SampleColumn",
                   "The number of columns used for sampling",
                   UintegerValue (10),
                   MakeUintegerAccessor (&MinstrelHtWifiManager::m_nSampleCol),
                   MakeUintegerChecker <uint32_t> ())
    .AddAttribute ("PacketLength",
                   "The packet length used for calculating mode TxTime",
                   UintegerValue (1200),
                   MakeUintegerAccessor (&MinstrelHtWifiManager::m_frameLength),
                   MakeUintegerChecker <uint32_t> ())
    .AddTraceSource ("RateChange",
                     "The transmission rate has change",
                     MakeTraceSourceAccessor (&MinstrelHtWifiManager::m_rateChange),
                     "ns3::MinstrelHtWifiManager::RateChangeTracedCallback")
  ;
  return tid;
}

MinstrelHtWifiManager::MinstrelHtWifiManager ()
{
  NS_LOG_FUNCTION (this);
  m_uniformRandomVariable = CreateObject<UniformRandomVariable> ();
  // Also create the legacy manager in case non-HT station want to associate.
  m_legacyManager = new MinstrelWifiManager();
}

MinstrelHtWifiManager::~MinstrelHtWifiManager ()
{
  NS_LOG_FUNCTION (this);
  for (uint32_t i = 0; i < N_GROUPS; i++)
    {
      m_minstrelGroups[i].calcTxTime.clear ();
    }
}

int64_t
MinstrelHtWifiManager::AssignStreams (int64_t stream)
{
  NS_LOG_FUNCTION (this << stream);
  m_uniformRandomVariable->SetStream (stream);
  m_legacyManager->AssignStreams(stream);
  return 1;
}

void
MinstrelHtWifiManager::SetupPhy (Ptr<WifiPhy> phy)
{
  NS_LOG_FUNCTION (this << phy);

  /**
   *  Initialize m_groups array with all the possible groups.
   */

  /**
   * Initialize the groups array.
   */
  m_minstrelGroups = MinstrelMcsGroups (N_GROUPS);
  for (uint8_t streams = 1; streams <= MAX_SUPPORTED_STREAMS; streams++)
    {
      for (uint8_t sgi = 0; sgi <= 1; sgi++)
        {
          uint32_t chWidth = 20;

          m_minstrelGroups[GetGroupId (streams, sgi, 0)].streams = streams;
          m_minstrelGroups[GetGroupId (streams, sgi, 0)].sgi = sgi;
          m_minstrelGroups[GetGroupId (streams, sgi, 0)].chWidth = chWidth;
          for (uint8_t i = 0; i < MAX_GROUP_RATES; i++)
            {
              WifiMode mode = phy->GetMcs (i);
              AddCalcTxTime (GetGroupId (streams, sgi, 0), mode, CalculateTxDuration (phy, streams, sgi, chWidth, mode));
            }
          NS_LOG_DEBUG ("Initialized group " << GetGroupId (streams, sgi, 0) << ": (" << (uint32_t)streams << "," << (uint32_t)sgi << "," << chWidth << ")");

          chWidth = 40;

          m_minstrelGroups[GetGroupId (streams, sgi, 1)].streams = streams;
          m_minstrelGroups[GetGroupId (streams, sgi, 1)].sgi = sgi;
          m_minstrelGroups[GetGroupId (streams, sgi, 1)].chWidth = chWidth;
          for (uint8_t i = 0; i < MAX_GROUP_RATES; i++)
            {
              WifiMode mode = phy->GetMcs (i);
              AddCalcTxTime (GetGroupId (streams, sgi, 1), mode, CalculateTxDuration (phy, streams, sgi, chWidth, mode));
            }
          NS_LOG_DEBUG ("Initialized group " << GetGroupId (streams, sgi, 1) << ": (" << (uint32_t)streams << "," << (uint32_t)sgi << "," << chWidth << ")");
        }
    }
  m_legacyManager->SetupPhy(phy);
  WifiRemoteStationManager::SetupPhy (phy);
}

Time
MinstrelHtWifiManager::CalculateTxDuration (Ptr<WifiPhy> phy, uint8_t streams, uint8_t sgi, uint32_t chWidth, WifiMode mode)
{
  NS_LOG_FUNCTION (this << phy << (int)streams << (int)sgi << chWidth << mode);

  WifiTxVector txvector;
  txvector.SetNss (streams);
  txvector.SetShortGuardInterval (sgi);
  txvector.SetChannelWidth (chWidth);
  txvector.SetNess (0);
  txvector.SetStbc (phy->GetStbc ());
  txvector.SetMode (mode);
  return phy->CalculateTxDuration (m_frameLength, txvector, WIFI_PREAMBLE_HT_MF, phy->GetFrequency (), NORMAL_MPDU,0);
}

Time
MinstrelHtWifiManager::GetCalcTxTime (uint32_t groupId, WifiMode mode) const
{
  NS_LOG_FUNCTION (this << groupId << mode);

  for (TxTime::const_iterator i = m_minstrelGroups[groupId].calcTxTime.begin (); i != m_minstrelGroups[groupId].calcTxTime.end (); i++)
    {
      if (mode == i->second)
        {
          return i->first;
        }
    }
  NS_ASSERT (false);
  return Seconds (0);
}

void
MinstrelHtWifiManager::AddCalcTxTime (uint32_t groupId, WifiMode mode, Time t)
{
  NS_LOG_FUNCTION (this << groupId << mode << t);

  m_minstrelGroups[groupId].calcTxTime.push_back (std::make_pair (t, mode));
}

WifiRemoteStation *
MinstrelHtWifiManager::DoCreateStation (void) const
{
  NS_LOG_FUNCTION (this);

  MinstrelHtWifiRemoteStationAux *station = new MinstrelHtWifiRemoteStationAux ();

  /**
   * Assume the station is HT.
   * When correct information available it will be checked.
   */
  station->m_htStation = new MinstrelHtWifiRemoteStation ();
  station->m_initialized = false;
  station->m_isHt = true;

  station->m_htStation->m_nextStatsUpdate = Simulator::Now ();
  station->m_htStation->m_col = 0;
  station->m_htStation->m_index = 0;
  station->m_htStation->m_maxTpRate = 0;
  station->m_htStation->m_maxTpRate2 = 0;
  station->m_htStation->m_maxProbRate = 0;
  station->m_htStation->m_frameCount = 0;
  station->m_htStation->m_sampleCount = 0;
  station->m_htStation->m_isSampling = false;
  station->m_htStation->m_sampleRate = 0;
  station->m_htStation->m_sampleRateSlower = false;
  station->m_htStation->m_shortRetry = 0;
  station->m_htStation->m_longRetry = 0;
  station->m_htStation->m_err = 0;
  station->m_htStation->m_txRate = 0;
  station->m_htStation->m_sampleGroup = 0;
  station->m_htStation->m_numSamplesSlow = 0;
  station->m_htStation->m_nSupportedMcs = 0;
  return station;
}

void
MinstrelHtWifiManager::CheckInit (MinstrelHtWifiRemoteStationAux *station)
{
  NS_LOG_FUNCTION (this << station);

  if (!station->m_initialized)
    {
      // Note: we appear to be doing late initialization of the table
      // to make sure that the set of supported rates has been initialized
      // before we perform our own initialization.
      if (!GetHtSupported(station))
        {
          station->m_isHt = false;
          station->m_legacyStation = m_legacyManager->CreateMinstrelStation();
          m_legacyManager->CheckInit(station->m_legacyStation);
        }
      else
        {
          station->m_htStation->m_nSupportedMcs = GetNMcsSupported (station);
          InitSampleTable (station->m_htStation);
          RateInit (station->m_htStation);
        }
      station->m_initialized = true;
    }
}

void
MinstrelHtWifiManager::DoReportRxOk (WifiRemoteStation *st,
                                     double rxSnr, WifiMode txMode)
{
  NS_LOG_FUNCTION (this << st);

  NS_LOG_DEBUG ("DoReportRxOk m_txRate=" << ((MinstrelHtWifiRemoteStation *)st)->m_txRate);
}

void
MinstrelHtWifiManager::DoReportRtsFailed (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);

  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *)st;

  CheckInit (station);
  if (!station->m_initialized)
    {
      return;
    }

  if (!station->m_isHt)
    {
      NS_LOG_DEBUG ("DoReportRtsFailed m_txRate=" << station->m_legacyStation->m_txrate);
      station->m_legacyStation->m_shortRetry++;
    }
  else
    {
      NS_LOG_DEBUG ("DoReportRtsFailed m_txRate=" << station->m_htStation->m_txRate);
      station->m_htStation->m_shortRetry++;
    }
}

void
MinstrelHtWifiManager::DoReportRtsOk (WifiRemoteStation *st, double ctsSnr, WifiMode ctsMode, double rtsSnr)
{
  NS_LOG_FUNCTION (this << st);

  NS_LOG_DEBUG ("self=" << st << " rts ok");
}

void
MinstrelHtWifiManager::DoReportFinalRtsFailed (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);

  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *)st;
  NS_LOG_DEBUG ("Final RTS failed");

  CheckInit (station);
  if (!station->m_initialized)
    {
      return;
    }

  if (!station->m_isHt)
    {
      m_legacyManager->UpdateRetry (station->m_legacyStation);
      station->m_legacyStation->m_err++;
    }
  else
    {
      UpdateRetry (station->m_htStation);
      station->m_htStation->m_err++;
    }
}

void
MinstrelHtWifiManager::DoReportDataFailed (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);

  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *)st;
  /**
   *
   * Retry Chain table is implemented here
   *
   * Try |         LOOKAROUND RATE              | NORMAL RATE
   *     | random < best    | random > best     |
   * --------------------------------------------------------------
   *  1  | Best throughput  | Random rate       | Best throughput
   *  2  | Random rate      | Best throughput   | Next best throughput
   *  3  | Best probability | Best probability  | Best probability
   *  4  | Lowest Baserate  | Lowest baserate   | Lowest baserate
   *
   * Note: For clarity, multiple blocks of if's and else's are used
   * After a failing 7 times, DoReportFinalDataFailed will be called
   * Following implementation in linux, in MinstrelHT Lowest baserate is not used.
   * Explanation can be found here: http://marc.info/?l=linux-wireless&m=144602778611966&w=2
   */

  CheckInit (station);
  if (!station->m_initialized)
    {
      return;
    }

  if (!station->m_isHt)
    {
      //TODO call appropiate function in minstrel
    }
  else
    {
      MinstrelHtWifiRemoteStation *ht_station = station->m_htStation;

      ht_station->m_longRetry++;

      /**
       * Get the ids for all rates.
       */
      uint32_t currentRateId = GetRateId (ht_station->m_txRate);
      uint32_t currentGroupId = GetGroupId (ht_station->m_txRate);
      uint32_t maxTpRateId = GetRateId (ht_station->m_maxTpRate);
      uint32_t maxTpGroupId = GetGroupId (ht_station->m_maxTpRate);
      uint32_t maxTp2RateId = GetRateId (ht_station->m_maxTpRate2);
      uint32_t maxTp2GroupId = GetGroupId (ht_station->m_maxTpRate2);
      uint32_t maxProbRateId = GetRateId (ht_station->m_maxProbRate);
      uint32_t maxProbGroupId = GetGroupId (ht_station->m_maxProbRate);
      uint32_t sampleRateId = GetRateId (ht_station->m_sampleRate);
      uint32_t sampleGroupId = GetGroupId (ht_station->m_sampleRate);

      ht_station->m_mcsTable[currentGroupId].m_minstrelTable[currentRateId].numRateAttempt++; // Increment the attempts counter for the rate used.

      NS_LOG_DEBUG ("DoReportDataFailed " << ht_station << "\t rate " << ht_station->m_txRate << "\tlongRetry \t" << ht_station->m_longRetry);

      /// For normal rate, we're not currently sampling random rates.
      if (!ht_station->m_isSampling)
        {
          /// Use best throughput rate.
          if (ht_station->m_longRetry <  ht_station->m_mcsTable[maxTpGroupId].m_minstrelTable[maxTpRateId].adjustedRetryCount)
            {
              NS_LOG_DEBUG ("Not Sampling use the same rate again");
              ht_station->m_txRate = ht_station->m_maxTpRate;  //!<  There are still a few retries.
            }

          /// Use second best throughput rate.
          else if (ht_station->m_longRetry < ( ht_station->m_mcsTable[maxTpGroupId].m_minstrelTable[maxTpRateId].adjustedRetryCount +
                                             ht_station->m_mcsTable[maxTp2GroupId].m_minstrelTable[maxTp2RateId].adjustedRetryCount))
            {
              NS_LOG_DEBUG ("Not Sampling use the Max TP2");
              ht_station->m_txRate = ht_station->m_maxTpRate2;
            }

          /// Use best probability rate.
          else if (ht_station->m_longRetry <= ( ht_station->m_mcsTable[maxTpGroupId].m_minstrelTable[maxTpRateId].adjustedRetryCount +
                                             ht_station->m_mcsTable[maxTp2GroupId].m_minstrelTable[maxTp2RateId].adjustedRetryCount +
                                             ht_station->m_mcsTable[maxProbGroupId].m_minstrelTable[maxProbRateId].adjustedRetryCount))
            {
              NS_LOG_DEBUG ("Not Sampling use Max Prob");
              ht_station->m_txRate = ht_station->m_maxProbRate;
            }
          else
            {
              NS_ASSERT_MSG(false,"Max retries reached and m_longRetry not cleared properly.");
            }
        }

      /// For look-around rate, we're currently sampling random rates.
      else
        {
          /// Use random rate.
          if (ht_station->m_longRetry <  ht_station->m_mcsTable[sampleGroupId].m_minstrelTable[sampleRateId].adjustedRetryCount)
            {
              NS_LOG_DEBUG ("Sampling use the same sample rate");
              ht_station->m_txRate = ht_station->m_sampleRate;    ///< keep using it
            }

          /// Use the best rate.
          else if (ht_station->m_longRetry < ( ht_station->m_mcsTable[maxTpGroupId].m_minstrelTable[maxTpRateId].adjustedRetryCount +
                                             ht_station->m_mcsTable[sampleGroupId].m_minstrelTable[sampleRateId].adjustedRetryCount))
            {
              NS_LOG_DEBUG ("Sampling use the MaxTP rate");
              ht_station->m_txRate = ht_station->m_maxTpRate;
            }

          /// Use the best probability rate.
          else if (ht_station->m_longRetry <= ( ht_station->m_mcsTable[maxTpGroupId].m_minstrelTable[maxTpRateId].adjustedRetryCount +
                                             ht_station->m_mcsTable[sampleGroupId].m_minstrelTable[sampleRateId].adjustedRetryCount +
                                             ht_station->m_mcsTable[maxProbGroupId].m_minstrelTable[maxProbRateId].adjustedRetryCount))
            {
              NS_LOG_DEBUG ("Sampling use the MaxProb rate");
              ht_station->m_txRate = ht_station->m_maxProbRate;
            }
          else
            {
              NS_ASSERT_MSG(false,"Max retries reached and m_longRetry not cleared properly.");
            }
        }
      NS_LOG_DEBUG ("Next rate to use TxRate = " << ht_station->m_txRate);
    }


}

void
MinstrelHtWifiManager::DoReportDataOk (WifiRemoteStation *st,
                                       double ackSnr, WifiMode ackMode, double dataSnr)
{
  NS_LOG_FUNCTION (this << st << ackSnr << ackMode << dataSnr);
  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *) st;

  CheckInit (station);
  if (!station->m_initialized)
    {
      return;
    }

  if (!station->m_isHt)
    {
      MinstrelWifiRemoteStation *legacy_station = station->m_legacyStation;
      legacy_station->m_isSampling = false;
      legacy_station->m_sampleRateSlower = false;

      legacy_station->m_minstrelTable[legacy_station->m_txrate].numRateSuccess++;
      legacy_station->m_minstrelTable[legacy_station->m_txrate].numRateAttempt++;

      m_legacyManager->UpdateRetry (legacy_station);

      legacy_station->m_packetCount++;

      if (legacy_station->m_nModes >= 1)
        {
          legacy_station->m_txrate = m_legacyManager->FindRate(station->m_legacyStation);
        }
    }
  else
    {
      MinstrelHtWifiRemoteStation *ht_station = station->m_htStation;
      NS_LOG_DEBUG ("Data OK - Txrate = " << ht_station->m_txRate  );

      ht_station->m_isSampling = false;
      ht_station->m_sampleRateSlower = false;

      uint32_t rateId = GetRateId (ht_station->m_txRate);
      uint32_t groupId = GetGroupId (ht_station->m_txRate);
      ht_station->m_mcsTable[groupId].m_minstrelTable[rateId].numRateSuccess++;
      ht_station->m_mcsTable[groupId].m_minstrelTable[rateId].numRateAttempt++;

      UpdateRetry (ht_station);

      ht_station->m_frameCount++;

      UpdateStats (ht_station);

      if (ht_station->m_nSupportedMcs >= 1)
        {
          ht_station->m_txRate = FindRate (ht_station);
        }
      NS_LOG_DEBUG ("Next rate to use TxRate = " << ht_station->m_txRate  );
    }
}

void
MinstrelHtWifiManager::DoReportFinalDataFailed (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);
  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *) st;

  CheckInit (station);
  if (!station->m_initialized)
    {
      return;
    }

  if (!station->m_isHt)
    {
      MinstrelWifiRemoteStation *legacy_station = station->m_legacyStation;
      NS_LOG_DEBUG ("DoReportFinalDataFailed m_txrate = " << legacy_station->m_txrate);

      legacy_station->m_isSampling = false;
      legacy_station->m_sampleRateSlower = false;

      m_legacyManager->UpdateRetry (legacy_station);

      legacy_station->m_err++;

      if (legacy_station->m_nModes >= 1)
        {
          legacy_station->m_txrate =m_legacyManager-> FindRate (legacy_station);
        }
    }
  else
    {
      MinstrelHtWifiRemoteStation *ht_station = station->m_htStation;

      NS_LOG_DEBUG ("DoReportFinalDataFailed - TxRate=" << ht_station->m_txRate);

      ht_station->m_isSampling = false;
      ht_station->m_sampleRateSlower = false;

      UpdateRetry (ht_station);

      ht_station->m_err++;

      UpdateStats (ht_station);

      if (ht_station->m_nSupportedMcs >= 1)
        {
          ht_station->m_txRate = FindRate (ht_station);
        }
      NS_LOG_DEBUG ("Next rate to use TxRate = " << ht_station->m_txRate  );
    }
}

void
MinstrelHtWifiManager::UpdateRetry (MinstrelHtWifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
  station->m_shortRetry = 0;
  station->m_longRetry = 0;

}
void
MinstrelHtWifiManager::DoDisposeStation (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);
  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *) st;
  station->DisposeStation ();
}

WifiTxVector
MinstrelHtWifiManager::DoGetDataTxVector (WifiRemoteStation *st,
                                          uint32_t size)
{
  NS_LOG_FUNCTION (this << st << size);
  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *) st;

  if (!station->m_initialized)
    {
      CheckInit (station);
    }

  if (!station->m_isHt)
    {
      //TODO
      return WifiTxVector();
    }
  else
    {
      MinstrelHtWifiRemoteStation *ht_station = station->m_htStation;

      NS_LOG_DEBUG ("DoGetDataMode m_txRate=" << ht_station->m_txRate);

      uint32_t rateId = GetRateId (ht_station->m_txRate);
      uint32_t groupId = GetGroupId (ht_station->m_txRate);
      McsGroup group = m_minstrelGroups[groupId];

      // Check consistency of rate selected.
      if ((group.sgi && !GetShortGuardInterval (ht_station)) || group.chWidth > GetChannelWidth (ht_station)  ||  (uint32_t) group.streams > GetNumberOfReceiveAntennas (ht_station))
        {
          NS_ASSERT_MSG (false,"Inconsistent group selected. Group: (" << (uint32_t)group.streams << "," << (uint32_t)group.sgi << "," << group.chWidth << ")" <<
                         " Station capabilities: (" << GetNumberOfReceiveAntennas (ht_station) << "," << GetShortGuardInterval (ht_station) << "," << GetChannelWidth (ht_station) << ")");
        }

      return WifiTxVector (GetMcsSupported (ht_station, rateId), GetDefaultTxPowerLevel (), GetLongRetryCount (ht_station), group.sgi, group.streams, GetNess (ht_station), group.chWidth, GetAggregation (ht_station), GetStbc (ht_station));
    }
}

WifiTxVector
MinstrelHtWifiManager::DoGetRtsTxVector (WifiRemoteStation *st)
{
  NS_LOG_FUNCTION (this << st);
  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *) st;

  if (!station->m_initialized)
    {
      CheckInit (station);
    }

  if (!station->m_isHt)
    {
      //TODO
      return WifiTxVector();
    }
  else
    {
      MinstrelHtWifiRemoteStation *ht_station = station->m_htStation;
      NS_LOG_DEBUG ("DoGetRtsMode m_txRate=" << ht_station->m_txRate);

      /* RTS is sent in a non-HT frame. RTS with HT is not supported yet in NS3.
       * When supported, decision of using HT has to follow rules in Section 9.7.6 from 802.11-2012.
       * From Sec. 9.7.6.5: "A frame other than a BlockAckReq or BlockAck that is carried in a
       * non-HT PPDU shall be transmitted by the STA using a rate no higher than the highest
       * rate in  the BSSBasicRateSet parameter that is less than or equal to the rate or
       * non-HT reference rate (see 9.7.9) of the previously transmitted frame that was
       * directed to the same receiving STA. If no rate in the BSSBasicRateSet parameter meets
       * these conditions, the control frame shall be transmitted at a rate no higher than the
       * highest mandatory rate of the attached PHY that is less than or equal to the rate
       * or non-HT reference rate (see 9.7.9) of the previously transmitted frame that was
       * directed to the same receiving STA."
       */

      // As we are in Minstrel HT, assume the last rate was an HT rate.
      WifiMode lastRate = GetMcsSupported(ht_station, GetRateId(ht_station->m_txRate));
      uint8_t streams = m_minstrelGroups[GetGroupId(ht_station->m_txRate)].streams;
      WifiMode referenceRate = lastRate.GetNonHtReferenceRate(streams);
      uint64_t lastDataRate = referenceRate.GetDataRate(20,false,1);
      uint32_t nBasicRates = GetNBasicModes();

      WifiMode rtsRate;
      bool rateFound = false;

      for (uint32_t i = 0; i < nBasicRates; i++)
        {
          uint64_t rate = GetBasicMode(i).GetDataRate(20,false,1);
          if (rate <= lastDataRate)
            {
              rtsRate = GetBasicMode(i);
              rateFound = true;
            }
        }

      Ptr<WifiPhy> phy = GetPhy();
      uint32_t nSupportRates = phy->GetNModes();

      if (!rateFound)
        {
          for (uint32_t i = 0; i < nSupportRates; i++)
            {
              uint64_t rate = phy->GetMode(i).GetDataRate(20,false,1);
              if (rate <= lastDataRate)
                {
                  rtsRate = phy->GetMode(i);
                  rateFound = true;
                }
            }
        }

      NS_ASSERT(!rateFound);

      uint32_t channelWidth = GetChannelWidth (ht_station);
        if (channelWidth > 20 && channelWidth != 22)
          {
            //avoid to use legacy rate adaptation algorithms for IEEE 802.11n/ac
            channelWidth = 20;
          }
        return WifiTxVector (GetSupported (ht_station, 0), GetDefaultTxPowerLevel (), GetShortRetryCount (ht_station), false, 1, 0, channelWidth, GetAggregation (ht_station), false);
    }
}

bool
MinstrelHtWifiManager::DoNeedDataRetransmission (WifiRemoteStation *st, Ptr<const Packet> packet, bool normally)
{
  NS_LOG_FUNCTION (this << st << packet << normally);

  MinstrelHtWifiRemoteStationAux *station = (MinstrelHtWifiRemoteStationAux *)st;

  CheckInit (station);
  if (!station->m_initialized)
    {
      return normally;
    }

  if (!station->m_isHt)
    {
      //TODO
      return normally;
    }
  else
    {
      MinstrelHtWifiRemoteStation *ht_station = station->m_htStation;

      uint32_t maxProbRateId = GetRateId (ht_station->m_maxProbRate);
      uint32_t maxProbGroupId = GetGroupId (ht_station->m_maxProbRate);
      uint32_t maxTpRateId = GetRateId (ht_station->m_maxTpRate);
      uint32_t maxTpGroupId = GetGroupId (ht_station->m_maxTpRate);
      uint32_t maxTp2RateId = GetRateId (ht_station->m_maxTpRate2);
      uint32_t maxTp2GroupId = GetGroupId (ht_station->m_maxTpRate2);
      uint32_t sampleRateId = GetRateId (ht_station->m_sampleRate);
      uint32_t sampleGroupId = GetGroupId (ht_station->m_sampleRate);

      if (!ht_station->m_isSampling)
        {
          uint32_t maxRetries = ht_station->m_mcsTable[maxTpGroupId].m_minstrelTable[maxTpRateId].adjustedRetryCount +
                                ht_station->m_mcsTable[maxTp2GroupId].m_minstrelTable[maxTp2RateId].adjustedRetryCount +
                                ht_station->m_mcsTable[maxProbGroupId].m_minstrelTable[maxProbRateId].adjustedRetryCount;
          if (ht_station->m_longRetry >= maxRetries)
            {
              NS_LOG_DEBUG ("No re-transmission allowed. Retries: " <<  ht_station->m_longRetry << " Max retries: " << maxRetries);
              return false;
            }
          else
            {
              NS_LOG_DEBUG ("Re-transmit. Retries: " <<  ht_station->m_longRetry << " Max retries: " << maxRetries);
              return true;
            }
        }
      else
        {
          uint32_t maxRetries = ht_station->m_mcsTable[sampleGroupId].m_minstrelTable[sampleRateId].adjustedRetryCount +
                                ht_station->m_mcsTable[maxTpGroupId].m_minstrelTable[maxTpRateId].adjustedRetryCount +
                                ht_station->m_mcsTable[maxProbGroupId].m_minstrelTable[maxProbRateId].adjustedRetryCount;
          if (ht_station->m_longRetry >= maxRetries)
            {
              NS_LOG_DEBUG ("No re-transmission allowed. Retries: " <<  ht_station->m_longRetry << " Max retries: " << maxRetries);
              return false;
            }
          else
            {
              NS_LOG_DEBUG ("Re-transmit. Retries: " <<  ht_station->m_longRetry << " Max retries: " << maxRetries);
              return true;
            }
        }
    }
}

bool
MinstrelHtWifiManager::IsLowLatency (void) const
{
  NS_LOG_FUNCTION (this);
  return true;
}

uint32_t
MinstrelHtWifiManager::GetNextSample (MinstrelHtWifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);

  uint32_t sampleGroup = station->m_sampleGroup;

  uint32_t index = station->m_mcsTable[sampleGroup].m_index;
  uint32_t col = station->m_mcsTable[sampleGroup].m_col;

  uint32_t sampleIndex = station->m_sampleTable[index][col];

  uint32_t rateIndex = GetIndex (sampleGroup, sampleIndex);
  NS_LOG_DEBUG ("Next Sample is " << rateIndex );

  SetNextSample (station); //Calculate the next sample rate.

  return rateIndex;
}

void
MinstrelHtWifiManager::SetNextSample (MinstrelHtWifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
  do
    {
      station->m_sampleGroup++;
      station->m_sampleGroup %= N_GROUPS;
    }
  while (!station->m_mcsTable[station->m_sampleGroup].m_supported);

  station->m_mcsTable[station->m_sampleGroup].m_index++;
  if (station->m_mcsTable[station->m_sampleGroup].m_index >= station->m_nSupportedMcs)
    {
      station->m_mcsTable[station->m_sampleGroup].m_index = 0;
      station->m_mcsTable[station->m_sampleGroup].m_col++;
      if (station->m_mcsTable[station->m_sampleGroup].m_col >= m_nSampleCol)
        {
          station->m_mcsTable[station->m_sampleGroup].m_col = 0;
        }
    }
}

uint32_t
MinstrelHtWifiManager::FindRate (MinstrelHtWifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
  NS_LOG_DEBUG ("FindRate " << "packet=" << station->m_frameCount );

  if ((station->m_sampleCount + station->m_frameCount) == 0)
    {
      return station->m_maxTpRate;
    }

  /// For determining when to try a sample rate.
  int coinFlip = m_uniformRandomVariable->GetInteger (0, 100) % 2;

  /**
   * if we are below the target of look around rate percentage, look around
   * note: do it randomly by flipping a coin instead sampling
   * all at once until it reaches the look around rate
   */
  if ( (((100 * station->m_sampleCount) / (station->m_sampleCount + station->m_frameCount )) < m_lookAroundRate)
       && (coinFlip == 1 ))
    {
      //SAMPLING
      NS_LOG_DEBUG ("Obtaining a sampling rate");
      /// Now go through the table and find an index rate.
      uint32_t sampleIdx = GetNextSample (station);
      NS_LOG_DEBUG ("Sampling rate = " << sampleIdx);


      //Evaluate if the sampling rate selected should be used.
      uint32_t sampleGroupId = GetGroupId (sampleIdx);
      uint32_t sampleRateId = GetRateId (sampleIdx);
      McsGroup sampleGroup = m_minstrelGroups[sampleGroupId];
      HtRateInfo sampleRateInfo = station->m_mcsTable[sampleGroupId].m_minstrelTable[sampleRateId];

      /**
       * Sampling might add some overhead (RTS, no aggregation)
       * to the frame. Hence, don't use sampling for the currently
       * used rates.
       *
       * Also do not sample if the probability is already higher than 95%
       * to avoid wasting airtime.
       */
      NS_LOG_DEBUG ("Use sample rate? MaxTpRate= " << station->m_maxTpRate << " CurrentRate= " << station->m_txRate <<
                    " SampleRate= " << sampleIdx << " SampleProb= " << sampleRateInfo.ewmaProb);
      if (sampleIdx != station->m_maxTpRate && sampleIdx != station->m_maxTpRate2 &&
          sampleIdx != station->m_maxProbRate && sampleRateInfo.ewmaProb <= 95*180)
        {

          /*
           * Make sure that lower rates get sampled only occasionally,
           * if the link is working perfectly.
           */

          uint32_t maxTpGroupId = GetGroupId (station->m_maxTpRate);
          uint32_t maxTp2GroupId = GetGroupId (station->m_maxTpRate2);
          uint32_t maxTp2RateId = GetRateId (station->m_maxTpRate2);
          uint32_t maxProbGroupId = GetGroupId (station->m_maxProbRate);
          uint32_t maxProbRateId = GetRateId (station->m_maxProbRate);

          uint8_t maxTpStreams = m_minstrelGroups[maxTpGroupId].streams;
          uint8_t sampleStreams = m_minstrelGroups[sampleGroupId].streams;

          Time sampleDuration = sampleRateInfo.perfectTxTime;
          Time maxTp2Duration = station->m_mcsTable[maxTp2GroupId].m_minstrelTable[maxTp2RateId].perfectTxTime;
          Time maxProbDuration = station->m_mcsTable[maxProbGroupId].m_minstrelTable[maxProbRateId].perfectTxTime;

          NS_LOG_DEBUG ("Use sample rate? SampleDuration= " << sampleDuration << " maxTp2Duration= " << maxTp2Duration <<
                        " maxProbDuration= " << maxProbDuration << " sampleStreams= " << (uint32_t)sampleStreams <<
                        " maxTpStreams= " << (uint32_t)maxTpStreams);
          if (sampleDuration < maxTp2Duration || (sampleStreams <= maxTpStreams - 1 && sampleDuration < maxProbDuration))
            {
              /// Start sample count.
              station->m_sampleCount++;

              /// Set flag that we are currently sampling.
              station->m_isSampling = true;

              /// Bookkeeping for resetting stuff.
              if (station->m_frameCount >= 10000)
                {
                  station->m_sampleCount = 0;
                  station->m_frameCount = 0;
                }

              /// set the rate that we're currently sampling
              station->m_sampleRate = sampleIdx;

              uint64_t dataRate = GetMcsSupported (station, GetRateId (station->m_maxTpRate)).GetDataRate(sampleGroup.chWidth, sampleGroup.sgi, sampleGroup.streams);
              m_rateChange (dataRate, station->m_state->m_address);

              NS_LOG_DEBUG ("FindRate " << "sampleRate=" << sampleIdx);
              return sampleIdx;
            }
          else
            {
              station->m_numSamplesSlow++;
              if (sampleRateInfo.numSamplesSkipped >= 20 && station->m_numSamplesSlow <= 2)
                {
                  /// Start sample count.
                  station->m_sampleCount++;

                  /// Set flag that we are currently sampling.
                  station->m_isSampling = true;

                  /// Bookkeeping for resetting stuff.
                  if (station->m_frameCount >= 10000)
                    {
                      station->m_sampleCount = 0;
                      station->m_frameCount = 0;
                    }

                  /// set the rate that we're currently sampling
                  station->m_sampleRate = sampleIdx;

                  uint64_t dataRate = GetMcsSupported (station, GetRateId (station->m_maxTpRate)).GetDataRate(sampleGroup.chWidth, sampleGroup.sgi, sampleGroup.streams);
                  m_rateChange (dataRate, station->m_state->m_address);

                  NS_LOG_DEBUG ("FindRate " << "sampleRate=" << sampleIdx);
                  return sampleIdx;
                }
            }
        }
    }
  ///	Continue using the best rate.
  McsGroup maxTpGroup = m_minstrelGroups[GetGroupId (station->m_maxTpRate)];
  uint64_t dataRate = GetMcsSupported (station, GetRateId (station->m_maxTpRate)).GetDataRate(maxTpGroup.chWidth, maxTpGroup.sgi, maxTpGroup.streams);
  m_rateChange (dataRate, station->m_state->m_address);

  NS_LOG_DEBUG ("FindRate " << "maxTpRrate=" << station->m_maxTpRate);
  return station->m_maxTpRate;
}
uint32_t
MinstrelHtWifiManager::GetIndex (uint32_t groupid, uint32_t mcsIndex)
{
  NS_LOG_FUNCTION (this << groupid << mcsIndex);
  uint32_t index;
  index = groupid * MAX_GROUP_RATES + mcsIndex;
  return index;
}
void
MinstrelHtWifiManager::UpdateStats (MinstrelHtWifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
  if (Simulator::Now () <  station->m_nextStatsUpdate)
    {
      return;
    }

  NS_LOG_DEBUG ("Updating stats=" << this);

  station->m_nextStatsUpdate = Simulator::Now () + m_updateStats;

  station->m_numSamplesSlow = 0;

  Time txTime;
  uint32_t tempProb;


  /// Update throughput and EWMA for each rate inside each group.
  for (uint32_t j = 0; j < N_GROUPS; j++)
    {
      if (station->m_mcsTable[j].m_supported)
        {
          for (uint32_t i = 0; i < station->m_nSupportedMcs; i++)
            {
              /// Calculate the perfect tx time for this rate.
              txTime =  station->m_mcsTable[j].m_minstrelTable[i].perfectTxTime;

              /// Just for initialization.
              if (txTime.GetMicroSeconds () == 0)
                {
                  txTime = Seconds (1);
                }

              NS_LOG_DEBUG (i << " " << GetMcsSupported (station, i) <<
                            "\t attempt=" << station->m_mcsTable[j].m_minstrelTable[i].numRateAttempt <<
                            "\t success=" << station->m_mcsTable[j].m_minstrelTable[i].numRateSuccess);

              /// If we've attempted something.
              if (station->m_mcsTable[j].m_minstrelTable[i].numRateAttempt)
                {
                  station->m_mcsTable[j].m_minstrelTable[i].numSamplesSkipped = 0;
                  /**
                   * Calculate the probability of success.
                   * Assume probability scales from 0 to 18000.
                   */
                  tempProb = (station->m_mcsTable[j].m_minstrelTable[i].numRateSuccess * 18000) / station->m_mcsTable[j].m_minstrelTable[i].numRateAttempt;

                  /// Bookeeping.
                  station->m_mcsTable[j].m_minstrelTable[i].prob = tempProb;

                  /// EWMA probability (cast for gcc 3.4 compatibility).
                  tempProb = static_cast<uint32_t> (((tempProb * (100 - m_ewmaLevel)) + (station->m_mcsTable[j].m_minstrelTable[i].ewmaProb * m_ewmaLevel) ) / 100);

                  station->m_mcsTable[j].m_minstrelTable[i].ewmaProb = tempProb;

                  /**
                   * Calculating throughput.
                   * Do not account throughput if sucess prob is below 10% (as done in minstrel_hc linux implementation).
                   */
                  if (tempProb < 10*180)
                    station->m_mcsTable[j].m_minstrelTable[i].throughput = 0;
                  else
                    {
                      /**
                       * For the throughput calculation, limit the probability value to 90% to
                       * account for collision related packet error rate fluctuation.
                       */
                      if (tempProb > 90*180)
                        station->m_mcsTable[j].m_minstrelTable[i].throughput = 90*180 * (1000000 / txTime.GetMicroSeconds ());
                      else
                        station->m_mcsTable[j].m_minstrelTable[i].throughput = tempProb * (1000000 / txTime.GetMicroSeconds ());
                    }
                }
              else
                {
                  station->m_mcsTable[j].m_minstrelTable[i].numSamplesSkipped++;
                }

              /// Bookeeping.
              station->m_mcsTable[j].m_minstrelTable[i].numRateSuccess = 0;
              station->m_mcsTable[j].m_minstrelTable[i].numRateAttempt = 0;

              /// Sample less often below 10% and  above 95% of success.
              if ((station->m_mcsTable[j].m_minstrelTable[i].ewmaProb > 17100) || (station->m_mcsTable[j].m_minstrelTable[i].ewmaProb < 1800))
                {
                  /**
                   * retry count denotes the number of retries permitted for each rate
                   * # retry_count/2
                   */

                  if (station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount > 2)
                    {
                      station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount = 2;
                    }
                  else
                    {
                      station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount = station->m_mcsTable[j].m_minstrelTable[i].retryCount;
                    }
                }
              else
                {
                  station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount = station->m_mcsTable[j].m_minstrelTable[i].retryCount;
                }

              /// If it's 0 allow one retry limit.
              if (station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount == 0)
                {
                  station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount = 1;
                }
            }
        }
    }

  uint32_t max_prob = 0, index_max_prob = 0, max_tp = 0, index_max_tp = 0, index_max_tp2 = 0;
  uint32_t index = 0;

  /// For each group get the max_tp and max_tp2.
  for (uint32_t j = 0; j < N_GROUPS; j++)
    {
      max_prob = 0;
      index_max_prob = GetIndex (j,0);
      max_tp = 0;
      index_max_tp = GetIndex (j,0);
      index_max_tp2 = GetIndex (j,0);

      if (station->m_mcsTable[j].m_supported)
        {
          /// Go find maximum throughput, second maximum throughput and high probability of success rates.
          for (uint32_t i = 0; i < station->m_nSupportedMcs; i++)
            {
              index = GetIndex (j,i);

              NS_LOG_DEBUG ("throughput" << station->m_mcsTable[j].m_minstrelTable[i].throughput <<
                            "\n ewma" << station->m_mcsTable[j].m_minstrelTable[i].ewmaProb);

              if (max_tp < station->m_mcsTable[j].m_minstrelTable[i].throughput)
                {
                  index_max_tp = index;
                  max_tp = station->m_mcsTable[j].m_minstrelTable[i].throughput;
                }

              if (max_prob < station->m_mcsTable[j].m_minstrelTable[i].ewmaProb)
                {
                  index_max_prob = index;
                  max_prob = station->m_mcsTable[j].m_minstrelTable[i].ewmaProb;
                }
            }
          max_tp = 0;
          /// Find the second maximum throughput rate.
          for (uint32_t i = 0; i < station->m_nSupportedMcs; i++)
            {
              index = GetIndex (j,i);

              if ((i != index_max_tp) && (max_tp < station->m_mcsTable[j].m_minstrelTable[i].throughput))
                {
                  index_max_tp2 = index;
                  max_tp = station->m_mcsTable[j].m_minstrelTable[i].throughput;
                }
            }

          station->m_mcsTable[j].m_maxTpRate = index_max_tp;
          station->m_mcsTable[j].m_maxTpRate2 = index_max_tp2;
          station->m_mcsTable[j].m_maxProbRate = index_max_prob;
          NS_LOG_DEBUG ("Group: " << j << " max tp=" << index_max_tp << "\nmax tp2=" << index_max_tp2 << "\nmax prob=" << index_max_prob);
        }
    }

  /// Get the max_tp and max_tp2 from all groups.
  max_prob = 0;
  max_tp = 0;
  //Find the lowest supported group.
  uint32_t k = 0;
  while (!station->m_mcsTable[k].m_supported)
    {
      k++;
    }
  index_max_prob = GetIndex (k,0);
  index_max_tp = GetIndex (k,0);
  index_max_tp2 = GetIndex (k,0);


  for (uint32_t j = 0; j < N_GROUPS; j++)
    {
      if (station->m_mcsTable[j].m_supported)
        {
          /// Go find maximum throughput, second maximum throughput and high probability of success rates.
          if (max_tp < station->m_mcsTable[j].m_minstrelTable[GetRateId (station->m_mcsTable[j].m_maxTpRate)].throughput)
            {
              index_max_tp = station->m_mcsTable[j].m_maxTpRate;
              max_tp = station->m_mcsTable[j].m_minstrelTable[GetRateId (station->m_mcsTable[j].m_maxTpRate)].throughput;
            }

          if (max_prob < station->m_mcsTable[j].m_minstrelTable[GetRateId (station->m_mcsTable[j].m_maxProbRate)].ewmaProb)
            {
              index_max_prob = station->m_mcsTable[j].m_maxProbRate;
              max_prob = station->m_mcsTable[j].m_minstrelTable[GetRateId (station->m_mcsTable[j].m_maxProbRate)].ewmaProb;
            }
        }
    }
  max_tp = 0;
  /// Find the second highest maximum throughput rate.
  for (uint32_t i = 0; i < N_GROUPS; i++)
    {
      if (station->m_mcsTable[i].m_supported)
        {
          if ((station->m_mcsTable[i].m_maxTpRate != index_max_tp) && (max_tp < station->m_mcsTable[i].m_minstrelTable[GetRateId (station->m_mcsTable[i].m_maxTpRate)].throughput))
            {
              /// Find if another group's max_tp is better than the max_tp2.
              index_max_tp2 = station->m_mcsTable[i].m_maxTpRate;
              max_tp = station->m_mcsTable[i].m_minstrelTable[GetRateId (station->m_mcsTable[i].m_maxTpRate)].throughput;
            }
          if (max_tp < station->m_mcsTable[i].m_minstrelTable[GetRateId (station->m_mcsTable[i].m_maxTpRate2)].throughput)
            {
              /// Find if another group's max_tp2 is better than max_tp2.
              index_max_tp2 = station->m_mcsTable[i].m_maxTpRate2;
              max_tp = station->m_mcsTable[i].m_minstrelTable[GetRateId (station->m_mcsTable[i].m_maxTpRate2)].throughput;
            }
        }
    }

  station->m_maxTpRate = index_max_tp;
  station->m_maxTpRate2 = index_max_tp2;
  station->m_maxProbRate = index_max_prob;

  /// If the max_tp rate is bigger than the current rate and uses the same number of streams.
  if ((index_max_tp > station->m_txRate) && (m_minstrelGroups[GetGroupId (index_max_tp)].streams >= m_minstrelGroups[GetGroupId (station->m_txRate)].streams) )
    {
      station->m_txRate = index_max_tp;
    }

  NS_LOG_DEBUG ("max tp=" << index_max_tp << "\nmax tp2=" << index_max_tp2 << "\nmax prob=" << index_max_prob);
}

void
MinstrelHtWifiManager::RateInit (MinstrelHtWifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
  NS_LOG_DEBUG ("RateInit=" << station);

  station->m_mcsTable = McsGroupData (N_GROUPS);

  NS_LOG_DEBUG ("Supported groups by station:");
  for (uint32_t j = 0; j < N_GROUPS; j++)
    {
      /// Check if the group is supported
      station->m_mcsTable[j].m_supported = false;
      if (!(!GetPhy()->GetGuardInterval() && m_minstrelGroups[j].sgi)          ///Is SGI supported by the transmitter?
          && (GetPhy()->GetChannelWidth() >= m_minstrelGroups[j].chWidth)         ///Is channel width supported by the transmitter?
          && (GetPhy()->GetNumberOfTransmitAntennas () >= m_minstrelGroups[j].streams)) ///Are streams supported by the transmitter? FIXME Is this the correct way to check the number of streams?
        {
          if (!(!GetShortGuardInterval (station) && m_minstrelGroups[j].sgi)          ///Is SGI supported by the receiver?
              && (GetChannelWidth (station) >= m_minstrelGroups[j].chWidth)         ///Is channel width supported by the receiver?
              && (GetNumberOfReceiveAntennas (station) >= m_minstrelGroups[j].streams)) ///Are streams supported by the receiver? FIXME Is this the correct way to check the number of streams?
            {
              NS_LOG_DEBUG ("Group " << j << ": (" << (uint32_t)m_minstrelGroups[j].streams << "," << (uint32_t)m_minstrelGroups[j].sgi << "," << m_minstrelGroups[j].chWidth << ")");
              station->m_mcsTable[j].m_supported = true;

              station->m_mcsTable[j].m_minstrelTable = HtMinstrelRate (station->m_nSupportedMcs);
              station->m_mcsTable[j].m_col = 0;
              station->m_mcsTable[j].m_index = 0;
              for (uint32_t i = 0; i < station->m_nSupportedMcs; i++)
                {
                  station->m_mcsTable[j].m_minstrelTable[i].numRateAttempt = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].numRateSuccess = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].prob = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].ewmaProb = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].prevNumRateAttempt = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].prevNumRateSuccess = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].numSamplesSkipped = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].successHist = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].attemptHist = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].throughput = 0;
                  station->m_mcsTable[j].m_minstrelTable[i].perfectTxTime = GetCalcTxTime (j, GetPhy ()->GetMcs (i));
                  station->m_mcsTable[j].m_minstrelTable[i].retryCount = 1;
                  station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount = 1;
                  //Emulating minstrel.c::ath_rate_ctl_reset
                  //We only check from 2 to 10 retries. This guarantee that
                  //at least one retry is permitted.
                  Time totalTxTimeWithGivenRetries = Seconds (0.0); //tx_time in minstrel.c
                  NS_LOG_DEBUG (" Calculating the number of retries");
                  for (uint32_t retries = 2; retries < 11; retries++)
                    {
                      NS_LOG_DEBUG ("  Checking " << retries << " retries");
                      totalTxTimeWithGivenRetries = CalculateTimeUnicastPacket (station->m_mcsTable[j].m_minstrelTable[i].perfectTxTime, 0, retries);
                      NS_LOG_DEBUG ("   totalTxTimeWithGivenRetries = " << totalTxTimeWithGivenRetries);
                      if (totalTxTimeWithGivenRetries > MilliSeconds (6))
                        {
                          break;
                        }
                      station->m_mcsTable[j].m_minstrelTable[i].retryCount = retries;
                      station->m_mcsTable[j].m_minstrelTable[i].adjustedRetryCount = retries;
                    }
                }
            }
        }
    }
  SetNextSample (station);                 /// Select the initial sample index.
  UpdateStats (station);                   /// Calculate the initial high throughput rates.
  station->m_txRate = FindRate (station); /// Select the rate to use.
}

Time
MinstrelHtWifiManager::CalculateTimeUnicastPacket (Time dataTransmissionTime, uint32_t shortRetries, uint32_t longRetries)
{
  NS_LOG_FUNCTION (this << dataTransmissionTime << shortRetries << longRetries);
  //See rc80211_minstrel.c

  //First transmission (DATA + ACK timeout)
  Time tt = dataTransmissionTime + GetMac ()->GetAckTimeout ();

  uint32_t cwMax = 1023;
  uint32_t cw = 31;
  for (uint32_t retry = 0; retry < longRetries; retry++)
    {
      //Add one re-transmission (DATA + ACK timeout)
      tt += dataTransmissionTime + GetMac ()->GetAckTimeout ();

      //Add average back off (half the current contention window)
      tt += NanoSeconds ((cw / 2) * GetMac ()->GetSlot ());

      //Update contention window
      cw = std::min (cwMax, (cw + 1) * 2);
    }

  return tt;
}

void
MinstrelHtWifiManager::InitSampleTable (MinstrelHtWifiRemoteStation *station)
{
  NS_LOG_FUNCTION (this << station);
  NS_LOG_DEBUG ("InitSampleTable=" << this);

  station->m_sampleTable = HtSampleRate (station->m_nSupportedMcs, std::vector<uint32_t> (m_nSampleCol));
  station->m_col = station->m_index = 0;

  /// for off-seting to make rates fall between 0 and numrates
  uint32_t numSampleRates = station->m_nSupportedMcs;

  uint32_t newIndex;
  for (uint32_t col = 0; col < m_nSampleCol; col++)
    {
      for (uint32_t i = 0; i < numSampleRates; i++ )
        {

          /**
           * The next two lines basically tries to generate a random number
           * between 0 and the number of available rates
           */
          newIndex = m_uniformRandomVariable->GetInteger (0, numSampleRates) % numSampleRates;

          /// this loop is used for filling in other uninitilized places
          while (station->m_sampleTable[newIndex][col] != 0)
            {
              newIndex = (newIndex + 1) % numSampleRates;
            }

          station->m_sampleTable[newIndex][col] = i;
        }
    }
}

void
MinstrelHtWifiManager::PrintSampleTable (MinstrelHtWifiRemoteStation *station, std::ostream &os)
{
  NS_LOG_FUNCTION (this << station);
  NS_LOG_DEBUG ("PrintSampleTable=" << station);

  uint32_t numSampleRates = station->m_nSupportedMcs;
  for (uint32_t i = 0; i < numSampleRates; i++)
    {
      for (uint32_t j = 0; j < m_nSampleCol; j++)
        {
          os << station->m_sampleTable[i][j] << "\t";
        }
      os << std::endl;
    }
}

void
MinstrelHtWifiManager::PrintTable (MinstrelHtWifiRemoteStation *station, std::ostream &os)
{
  NS_LOG_FUNCTION (this << station);
  NS_LOG_DEBUG ("PrintTable=" << station);
  uint32_t numRates = station->m_nSupportedMcs;
  for (uint32_t j = 0; j < N_GROUPS; j++)
    {
      for (uint32_t i = 0; i < numRates; i++)
        {
          if (station->m_mcsTable[j].m_supported)
            os << "index(" << i << ") = " << station->m_mcsTable[j].m_minstrelTable[i].perfectTxTime << "\n";
        }
    }
}

uint32_t
MinstrelHtWifiManager::GetRateId (uint32_t index)
{
  NS_LOG_FUNCTION (this << index);

  uint32_t id;
  id = index % MAX_GROUP_RATES;
  return id;
}

uint32_t
MinstrelHtWifiManager::GetGroupId (uint32_t index)
{
  NS_LOG_FUNCTION (this << index);

  return index / MAX_GROUP_RATES;
}

uint32_t
MinstrelHtWifiManager::GetGroupId (uint8_t txstreams, uint8_t sgi, uint8_t ht40)
{
  NS_LOG_FUNCTION (this << (int)txstreams << (int)sgi << (int)ht40);

  return MAX_SUPPORTED_STREAMS * 2 * ht40 + MAX_SUPPORTED_STREAMS * sgi + txstreams - 1;
}

} // namespace ns3





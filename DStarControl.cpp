/*
 *	Copyright (C) 2015-2019,2021,2023,2025 Jonathan Naylor, G4KLX
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; version 2 of the License.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 */

#include "DStarControl.h"
#include "Utils.h"
#include "Sync.h"
#include "Log.h"

#if defined(USE_DSTAR)

#include <cstdio>
#include <cassert>
#include <ctime>
#include <algorithm>
#include <functional>

const unsigned int MAX_SYNC_BIT_ERRORS = 2U;

const unsigned int RSSI_COUNT = 3U * 21U;	// 3 * 420ms = 1260ms
const unsigned int BER_COUNT  = 50U * 48U;	// 50 * 20ms = 1000ms

bool CallsignCompare(const std::string& arg, const unsigned char* my)
{
	for (unsigned int i = 0U; i < (DSTAR_LONG_CALLSIGN_LENGTH - 1U); i++) {
		if (arg.at(i) != my[i])
			return false;
	}

	return true;
}

CDStarControl::CDStarControl(const std::string& callsign, const std::string& module, bool selfOnly, bool ackReply, unsigned int ackTime, DSTAR_ACK_MESSAGE ackMessage, bool errorReply, const std::vector<std::string>& blackList, const std::vector<std::string>& whiteList, CDStarNetwork* network, unsigned int timeout, bool duplex, bool remoteGateway, CRSSIInterpolator* rssiMapper) :
m_callsign(NULL),
m_gateway(NULL),
m_selfOnly(selfOnly),
m_ackReply(ackReply),
m_ackMessage(ackMessage),
m_errorReply(errorReply),
m_remoteGateway(remoteGateway),
m_blackList(blackList),
m_whiteList(whiteList),
m_network(network),
m_duplex(duplex),
m_queue(5000U, "D-Star Control"),
m_rfHeader(),
m_netHeader(),
m_rfState(RS_RF_LISTENING),
m_netState(RS_NET_IDLE),
m_net(false),
m_rfSlowData(),
m_netSlowData(),
m_rfN(0U),
m_netN(0U),
m_networkWatchdog(1000U, 0U, 1500U),
m_rfTimeoutTimer(1000U, timeout),
m_netTimeoutTimer(1000U, timeout),
m_packetTimer(1000U, 0U, 300U),
m_ackTimer(1000U, 0U, ackTime),
m_errTimer(1000U, 0U, ackTime),
m_interval(),
m_elapsed(),
m_rfFrames(0U),
m_netFrames(0U),
m_netLost(0U),
m_fec(),
m_rfBits(1U),
m_netBits(1U),
m_rfErrs(0U),
m_lastFrame(NULL),
m_lastFrameValid(false),
m_rssiMapper(rssiMapper),
m_rssi(0),
m_maxRSSI(0),
m_minRSSI(0),
m_aveRSSI(0),
m_rssiCountTotal(0U),
m_rssiAccum(0),
m_rssiCount(0U),
m_bitErrsAccum(0U),
m_bitsCount(0U),
m_enabled(true)
{
	assert(rssiMapper != NULL);

	m_callsign = new unsigned char[DSTAR_LONG_CALLSIGN_LENGTH];
	m_gateway  = new unsigned char[DSTAR_LONG_CALLSIGN_LENGTH];

	m_lastFrame = new unsigned char[DSTAR_FRAME_LENGTH_BYTES + 1U];

	std::string call = callsign;
	call.resize(DSTAR_LONG_CALLSIGN_LENGTH - 1U, ' ');
	std::string mod = module;
	mod.resize(1U, ' ');
	call.append(mod);
	
	std::string gate = callsign;
	gate.resize(DSTAR_LONG_CALLSIGN_LENGTH - 1U, ' ');
	gate.append("G");

	for (unsigned int i = 0U; i < DSTAR_LONG_CALLSIGN_LENGTH; i++) {
		m_callsign[i] = call.at(i);
		m_gateway[i]  = gate.at(i);
	}

	m_interval.start();
}

CDStarControl::~CDStarControl()
{
	delete[] m_callsign;
	delete[] m_gateway;
	delete[] m_lastFrame;
}

bool CDStarControl::writeModem(unsigned char *data, unsigned int len)
{
	assert(data != NULL);

	if (!m_enabled)
		return false;

	unsigned char type = data[0U];

	if (type == TAG_LOST && m_rfState == RS_RF_AUDIO) {
		unsigned char my1[DSTAR_LONG_CALLSIGN_LENGTH];
		unsigned char my2[DSTAR_SHORT_CALLSIGN_LENGTH];
		unsigned char your[DSTAR_LONG_CALLSIGN_LENGTH];
		m_rfHeader.getMyCall1(my1);
		m_rfHeader.getMyCall2(my2);
		m_rfHeader.getYourCall(your);

		if (m_rssi != 0) {
			LogMessage("D-Star, transmission lost from %8.8s/%4.4s to %8.8s, %.1f seconds, BER: %.1f%%, RSSI: %d/%d/%d dBm", my1, my2, your, float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits), m_minRSSI, m_maxRSSI, m_aveRSSI / int(m_rssiCountTotal));
			writeJSONRF("lost", float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits), m_minRSSI, m_maxRSSI, m_aveRSSI / int(m_rssiCountTotal));
		} else {
			LogMessage("D-Star, transmission lost from %8.8s/%4.4s to %8.8s, %.1f seconds, BER: %.1f%%", my1, my2, your, float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits));
			writeJSONRF("lost", float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits));
		}
		writeEndRF();
		return false;
	}

	if (type == TAG_LOST && m_rfState == RS_RF_INVALID) {
		m_rfState = RS_RF_LISTENING;

		if (m_netState == RS_NET_IDLE) {
			if (m_errorReply)
				m_errTimer.start();

			if (m_network != NULL)
				m_network->reset();
		}

		return false;
	}

	if (type == TAG_LOST) {
		m_rfState = RS_RF_LISTENING;
		return false;
	}

	// Have we got RSSI bytes on the end of a D-Star header?
	if (len == (DSTAR_HEADER_LENGTH_BYTES + 3U)) {
		uint16_t raw = 0U;
		raw |= (data[42U] << 8) & 0xFF00U;
		raw |= (data[43U] << 0) & 0x00FFU;

		// Convert the raw RSSI to dBm
		int m_rssi = m_rssiMapper->interpolate(raw);
		if (m_rssi != 0)
			LogDebug("D-Star, raw RSSI: %u, reported RSSI: %d dBm", raw, m_rssi);

		if (m_rssi < m_minRSSI)
			m_minRSSI = m_rssi;
		if (m_rssi > m_maxRSSI)
			m_maxRSSI = m_rssi;

		m_aveRSSI += m_rssi;
		m_rssiCountTotal++;

		m_rssiAccum += m_rssi;
		m_rssiCountTotal++;
	}

	// Have we got RSSI bytes on the end of D-Star data?
	if (len == (DSTAR_FRAME_LENGTH_BYTES + 3U)) {
		uint16_t raw = 0U;
		raw |= (data[13U] << 8) & 0xFF00U;
		raw |= (data[14U] << 0) & 0x00FFU;

		// Convert the raw RSSI to dBm
		int m_rssi = m_rssiMapper->interpolate(raw);
		if (m_rssi != 0)
			LogDebug("D-Star, raw RSSI: %u, reported RSSI: %d dBm", raw, m_rssi);

		if (m_rssi < m_minRSSI)
			m_minRSSI = m_rssi;
		if (m_rssi > m_maxRSSI)
			m_maxRSSI = m_rssi;

		m_aveRSSI += m_rssi;
		m_rssiCountTotal++;

		m_rssiAccum += m_rssi;
		m_rssiCountTotal++;
	}

	if (type == TAG_HEADER) {
		CDStarHeader header(data + 1U);
		m_rfHeader = header;

		unsigned char my1[DSTAR_LONG_CALLSIGN_LENGTH];
		header.getMyCall1(my1);

		unsigned char gateway[DSTAR_LONG_CALLSIGN_LENGTH];
		header.getRPTCall2(gateway);

		unsigned char my2[DSTAR_SHORT_CALLSIGN_LENGTH];
		header.getMyCall2(my2);

		unsigned char your[DSTAR_LONG_CALLSIGN_LENGTH];
		header.getYourCall(your);

		unsigned char rpt1[DSTAR_LONG_CALLSIGN_LENGTH];
		header.getRPTCall1(rpt1);

		// Is this a transmission destined for a repeater?
		if (!header.isRepeater()) {
			LogMessage("D-Star, non repeater RF header received from %8.8s", my1);
			m_rfState = RS_RF_INVALID;
			writeJSONRF("invalid", my1, my2, your);
			return true;
		}

		// Is it for us?
		if (::memcmp(rpt1, m_callsign, DSTAR_LONG_CALLSIGN_LENGTH) != 0) {
			LogMessage("D-Star, received RF header for wrong repeater (%8.8s) from %8.8s", rpt1, my1);
			m_rfState = RS_RF_INVALID;
			writeJSONRF("invalid", my1, my2, your);
			return true;
		}

		if (m_selfOnly && ::memcmp(my1, m_callsign, DSTAR_LONG_CALLSIGN_LENGTH - 1U) != 0 && !(std::find_if(m_whiteList.begin(), m_whiteList.end(), std::bind(CallsignCompare, std::placeholders::_1, my1)) != m_whiteList.end())) {
			LogMessage("D-Star, invalid access attempt from %8.8s", my1);
			m_rfState = RS_RF_REJECTED;
			writeJSONRF("rejected", my1, my2, your);
			return true;
		}

		if (!m_selfOnly && std::find_if(m_blackList.begin(), m_blackList.end(), std::bind(CallsignCompare, std::placeholders::_1, my1)) != m_blackList.end()) {
			LogMessage("D-Star, invalid access attempt from %8.8s", my1);
			m_rfState = RS_RF_REJECTED;
			writeJSONRF("rejected", my1, my2, your);
			return true;
		}

		m_net = ::memcmp(gateway, m_gateway, DSTAR_LONG_CALLSIGN_LENGTH) == 0;

		// Only start the timeout if not already running
		if (!m_rfTimeoutTimer.isRunning())
			m_rfTimeoutTimer.start();

		m_ackTimer.stop();
		m_errTimer.stop();

		m_rfBits = 1U;
		m_rfErrs = 0U;

		m_rfFrames = 1U;
		m_rfN = 0U;

		m_minRSSI = m_rssi;
		m_maxRSSI = m_rssi;
		m_aveRSSI = m_rssi;
		m_rssiCountTotal = 1U;

		m_rssiAccum = m_rssi;
		m_rssiCount = 1U;

		m_rfSlowData.start();

		if (m_duplex) {
			// Modify the header
			header.setRepeater(false);
			header.setRPTCall1(m_callsign);
			header.setRPTCall2(m_callsign);
			header.get(data + 1U);

			writeQueueHeaderRF(data);
		}

		if (m_net) {
			// Modify the header
			header.setRepeater(false);
			header.setRPTCall1(m_callsign);
			header.setRPTCall2(m_gateway);
			header.get(data + 1U);

			writeNetworkHeaderRF(data);
		}

		m_rfState = RS_RF_AUDIO;

		if (m_netState == RS_NET_IDLE)
			writeJSONRSSI();

		LogMessage("D-Star, received RF header from %8.8s/%4.4s to %8.8s", my1, my2, your);
		writeJSONRF("start", my1, my2, your);
	} else if (type == TAG_EOT) {
		if (m_rfState == RS_RF_REJECTED) {
			m_rfState = RS_RF_LISTENING;
		} else if (m_rfState == RS_RF_INVALID) {
			m_rfState = RS_RF_LISTENING;

			if (m_netState == RS_NET_IDLE) {
				if (m_errorReply)
					m_errTimer.start();

				if (m_network != NULL)
					m_network->reset();
			}

			return false;
		} else if (m_rfState == RS_RF_AUDIO) {
			if (m_net)
				writeNetworkDataRF(DSTAR_END_PATTERN_BYTES, 0U, true);

			if (m_duplex)
				writeQueueEOTRF();

			unsigned char my1[DSTAR_LONG_CALLSIGN_LENGTH];
			unsigned char my2[DSTAR_SHORT_CALLSIGN_LENGTH];
			unsigned char your[DSTAR_LONG_CALLSIGN_LENGTH];
			m_rfHeader.getMyCall1(my1);
			m_rfHeader.getMyCall2(my2);
			m_rfHeader.getYourCall(your);

			if (m_rssi != 0) {
				LogMessage("D-Star, received RF end of transmission from %8.8s/%4.4s to %8.8s, %.1f seconds, BER: %.1f%%, RSSI: %d/%d/%d dBm", my1, my2, your, float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits), m_minRSSI, m_maxRSSI, m_aveRSSI / int(m_rssiCountTotal));
				writeJSONRF("end", float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits), m_minRSSI, m_maxRSSI, m_aveRSSI / int(m_rssiCountTotal));
			} else {
				LogMessage("D-Star, received RF end of transmission from %8.8s/%4.4s to %8.8s, %.1f seconds, BER: %.1f%%", my1, my2, your, float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits));
				writeJSONRF("end", float(m_rfFrames) / 50.0F, float(m_rfErrs * 100U) / float(m_rfBits));
			}

			writeEndRF();
		}

		return false;
	} else if (type == TAG_DATA) {
		if (m_rfState == RS_RF_REJECTED)
			return true;

		if (m_rfState == RS_RF_INVALID)
			return true;

		if (m_rfState == RS_RF_LISTENING) {
			// The sync is regenerated by the modem so can do exact match
			if (::memcmp(data + 1U + DSTAR_VOICE_FRAME_LENGTH_BYTES, DSTAR_SYNC_BYTES, DSTAR_DATA_FRAME_LENGTH_BYTES) == 0) {
				m_rfSlowData.start();
				m_rfState = RS_RF_LATE_ENTRY;
			}

			return false;
		}

		if (m_rfState == RS_RF_AUDIO) {
			// The sync is regenerated by the modem so can do exact match
			if (::memcmp(data + 1U + DSTAR_VOICE_FRAME_LENGTH_BYTES, DSTAR_SYNC_BYTES, DSTAR_DATA_FRAME_LENGTH_BYTES) == 0) {
				m_rfSlowData.start();
				m_rfN = 0U;
			}

			// Regenerate the sync and send the RSSI data to the display
			if (m_rfN == 0U) {
				CSync::addDStarSync(data + 1U);
				writeJSONRSSI();
			}

			unsigned int errors = 0U;
			if (!m_rfHeader.isDataPacket()) {
				errors = m_fec.regenerateDStar(data + 1U);
				m_bitErrsAccum += errors;
				m_rfErrs       += errors;
				writeJSONBER();
			}

			m_bitsCount += 48U;
			m_rfBits    += 48U;
			m_rfFrames++;

			const unsigned char* text = m_rfSlowData.addText(data + 1U);
			if (text != NULL) {
				LogMessage("D-Star, slow data text = \"%s\"", text);
				writeJSONText(text);
			}

			if (m_net)
				writeNetworkDataRF(data, errors, false);

			if (m_duplex) {
				blankDTMF(data + 1U);
				writeQueueDataRF(data);
			}

			m_rfN = (m_rfN + 1U) % 21U;
		} else if (m_rfState == RS_RF_LATE_ENTRY) {
			// The sync is regenerated by the modem so can do exact match
			if (::memcmp(data + 1U + DSTAR_VOICE_FRAME_LENGTH_BYTES, DSTAR_SYNC_BYTES, DSTAR_DATA_FRAME_LENGTH_BYTES) == 0) {
				m_rfSlowData.reset();
				return false;
			} else {
				CDStarHeader* header = m_rfSlowData.addHeader(data + 1U);
				if (header == NULL)
					return false;

				m_rfHeader = *header;
				delete header;
			}

			unsigned char my1[DSTAR_LONG_CALLSIGN_LENGTH];
			m_rfHeader.getMyCall1(my1);

			unsigned char my2[DSTAR_SHORT_CALLSIGN_LENGTH];
			m_rfHeader.getMyCall2(my2);

			unsigned char your[DSTAR_LONG_CALLSIGN_LENGTH];
			m_rfHeader.getYourCall(your);

			unsigned char rpt1[DSTAR_LONG_CALLSIGN_LENGTH];
			m_rfHeader.getRPTCall1(rpt1);

			// Is this a transmission destined for a repeater?
			if (!m_rfHeader.isRepeater()) {
				LogMessage("D-Star, non repeater RF header received from %8.8s", my1);
				m_rfState = RS_RF_INVALID;
				writeJSONRF("invalid", my1, my2, your);
				return true;
			}

			// Is it for us?
			if (::memcmp(rpt1, m_callsign, DSTAR_LONG_CALLSIGN_LENGTH) != 0) {
				LogMessage("D-Star, received RF header for wrong repeater (%8.8s) from %8.8s", rpt1, my1);
				m_rfState = RS_RF_INVALID;
				writeJSONRF("invalid", my1, my2, your);
				return true;
			}

			if (m_selfOnly && ::memcmp(my1, m_callsign, DSTAR_LONG_CALLSIGN_LENGTH - 1U) != 0 && !(std::find_if(m_whiteList.begin(), m_whiteList.end(), std::bind(CallsignCompare, std::placeholders::_1, my1)) != m_whiteList.end())) {
				LogMessage("D-Star, invalid access attempt from %8.8s", my1);
				m_rfState = RS_RF_REJECTED;
				writeJSONRF("rejected", my1, my2, your);
				return true;
			}

			if (!m_selfOnly && std::find_if(m_blackList.begin(), m_blackList.end(), std::bind(CallsignCompare, std::placeholders::_1, my1)) != m_blackList.end()) {
				LogMessage("D-Star, invalid access attempt from %8.8s", my1);
				m_rfState = RS_RF_REJECTED;
				writeJSONRF("rejected", my1, my2, your);
				return true;
			}

			unsigned char gateway[DSTAR_LONG_CALLSIGN_LENGTH];
			m_rfHeader.getRPTCall2(gateway);

			m_net = ::memcmp(gateway, m_gateway, DSTAR_LONG_CALLSIGN_LENGTH) == 0;

			// Only reset the timeout if the timeout is not running
			if (!m_rfTimeoutTimer.isRunning())
				m_rfTimeoutTimer.start();

			// Create a dummy start frame to replace the received frame
			m_ackTimer.stop();
			m_errTimer.stop();

			m_rfBits = 1U;
			m_rfErrs = 0U;

			m_rfN = 0U;
			m_rfFrames = 1U;

			m_minRSSI = m_rssi;
			m_maxRSSI = m_rssi;
			m_aveRSSI = m_rssi;
			m_rssiCountTotal = 1U;

			if (m_duplex) {
				unsigned char start[DSTAR_HEADER_LENGTH_BYTES + 1U];
				start[0U] = TAG_HEADER;

				// Modify the header
				CDStarHeader header(m_rfHeader);
				header.setRepeater(false);
				header.setRPTCall1(m_callsign);
				header.setRPTCall2(m_callsign);
				header.get(start + 1U);

				writeQueueHeaderRF(start);
			}

			if (m_net) {
				unsigned char start[DSTAR_HEADER_LENGTH_BYTES + 1U];
				start[0U] = TAG_HEADER;

				// Modify the header
				CDStarHeader header(m_rfHeader);
				header.setRepeater(false);
				header.setRPTCall1(m_callsign);
				header.setRPTCall2(m_gateway);
				header.get(start + 1U);

				writeNetworkHeaderRF(start);
			}

			unsigned int errors = 0U;
			if (!m_rfHeader.isDataPacket()) {
				errors = m_fec.regenerateDStar(data + 1U);
				LogDebug("D-Star, audio sequence no. %u, errs: %u/48 (%.1f%%)", m_rfN, errors, float(errors) / 0.48F);
				m_bitErrsAccum += errors;
				m_rfErrs += errors;
			}

			m_bitsCount += 48U;
			m_rfBits    += 48U;

			if (m_net)
				writeNetworkDataRF(data, errors, false);

			if (m_duplex) {
				blankDTMF(data + 1U);
				writeQueueDataRF(data);
			}

			m_rfState = RS_RF_AUDIO;

			m_rfN = (m_rfN + 1U) % 21U;

			if (m_netState == RS_NET_IDLE) {
				writeJSONRSSI();
				writeJSONBER();
			}

			LogMessage("D-Star, received RF late entry from %8.8s/%4.4s to %8.8s", my1, my2, your);
			writeJSONRF("late_entry", my1, my2, your);
		}
	} else {
		CUtils::dump("D-Star, unknown data from modem", data, DSTAR_FRAME_LENGTH_BYTES + 1U);
	}

	return true;
}

unsigned int CDStarControl::readModem(unsigned char* data)
{
	assert(data != NULL);

	if (m_queue.isEmpty())
		return 0U;

	unsigned char len = 0U;
	m_queue.getData(&len, 1U);

	m_queue.getData(data, len);

	return len;
}

void CDStarControl::writeEndRF()
{
	m_rfState = RS_RF_LISTENING;

	m_rfTimeoutTimer.stop();

	if (m_netState == RS_NET_IDLE) {
		m_ackTimer.start();

		if (m_network != NULL)
			m_network->reset();
	}
}

void CDStarControl::writeEndNet()
{
	m_netState = RS_NET_IDLE;

	m_lastFrameValid = false;

	m_netTimeoutTimer.stop();
	m_networkWatchdog.stop();
	m_packetTimer.stop();

	if (m_network != NULL)
		m_network->reset();
}

void CDStarControl::writeNetwork()
{
	assert(m_network != NULL);

	unsigned char data[DSTAR_HEADER_LENGTH_BYTES + 2U];
	unsigned int length = m_network->read(data, DSTAR_HEADER_LENGTH_BYTES + 2U);
	if (length == 0U)
		return;

	if (!m_enabled)
		return;

	if (m_rfState == RS_RF_AUDIO && m_netState == RS_NET_IDLE)
		return;

	m_networkWatchdog.start();

	unsigned char type = data[0U];

	if (type == TAG_HEADER) {
		if (m_netState != RS_NET_IDLE)
			return;

		CDStarHeader header(data + 1U);

		unsigned char my1[DSTAR_LONG_CALLSIGN_LENGTH];
		header.getMyCall1(my1);

		unsigned char my2[DSTAR_SHORT_CALLSIGN_LENGTH];
		header.getMyCall2(my2);

		unsigned char your[DSTAR_LONG_CALLSIGN_LENGTH];
		header.getYourCall(your);

		m_netHeader = header;

		m_netTimeoutTimer.start();
		m_packetTimer.start();
		m_ackTimer.stop();
		m_errTimer.stop();

		m_lastFrameValid = false;

		m_netFrames = 0U;
		m_netLost   = 0U;

		m_netN      = 20U;

		m_netBits   = 1U;

		if (m_remoteGateway) {
			header.setRepeater(true);
			header.setRPTCall1(m_callsign);
			header.setRPTCall2(m_callsign);
			header.get(data + 1U);
		}

		writeQueueHeaderNet(data);

		m_netState = RS_NET_AUDIO;

		LINK_STATUS status = LS_NONE;
		unsigned char reflector[DSTAR_LONG_CALLSIGN_LENGTH];
		m_network->getStatus(status, reflector);
		if (status == LS_LINKED_DEXTRA || status == LS_LINKED_DPLUS || status == LS_LINKED_DCS || status == LS_LINKED_CCS || status == LS_LINKED_LOOPBACK) {
			LogMessage("D-Star, received network header from %8.8s/%4.4s to %8.8s via %8.8s", my1, my2, your, reflector);
			writeJSONNet("start", my1, my2, your, reflector);
		} else {
			LogMessage("D-Star, received network header from %8.8s/%4.4s to %8.8s", my1, my2, your);
			writeJSONNet("start", my1, my2, your);
		}	

		// Something just above here introduces a large delay forcing erroneous(?) insertion of silence packets.
		// Starting the elapsed timer here instead of the commented out position above solves that.
		m_elapsed.start();

	} else if (type == TAG_EOT) {
		if (m_netState != RS_NET_AUDIO)
			return;

		writeQueueEOTNet();

		data[1U] = TAG_EOT;

		unsigned char my1[DSTAR_LONG_CALLSIGN_LENGTH];
		unsigned char my2[DSTAR_SHORT_CALLSIGN_LENGTH];
		unsigned char your[DSTAR_LONG_CALLSIGN_LENGTH];
		m_netHeader.getMyCall1(my1);
		m_netHeader.getMyCall2(my2);
		m_netHeader.getYourCall(your);

		// We've received the header and EOT haven't we?
		m_netFrames += 2U;
		LogMessage("D-Star, received network end of transmission from %8.8s/%4.4s to %8.8s, %.1f seconds, %u%% packet loss", my1, my2, your, float(m_netFrames) / 50.0F, (m_netLost * 100U) / m_netFrames);
		writeJSONNet("end", float(m_netFrames) / 50.0F, float(m_netLost * 100U) / float(m_netFrames));

		writeEndNet();
	} else if (type == TAG_DATA) {
		if (m_netState != RS_NET_AUDIO)
			return;

		unsigned int errors = 0U;
		unsigned char n = data[1U];

		if (!m_netHeader.isDataPacket())
			errors = m_fec.regenerateDStar(data + 2U);

		blankDTMF(data + 2U);

		data[1U] = TAG_DATA;

		// Insert silence and reject if in the past
		bool ret = insertSilence(data + 1U, n);
		if (!ret)
			return;

		m_netBits += 48U;

		m_netN = n;

		// Regenerate the sync
		if (n == 0U) {
			CSync::addDStarSync(data + 2U);
			m_netSlowData.start();
		}

		const unsigned char* text = m_netSlowData.addText(data + 2U);
		if (text != NULL) {
			LogMessage("D-Star, slow data text = \"%s\"", text);
			writeJSONText(text);
		}

		m_packetTimer.start();
		m_netFrames++;

		writeQueueDataNet(data + 1U);
	} else {
		CUtils::dump("D-Star, unknown data from network", data, DSTAR_FRAME_LENGTH_BYTES + 1U);
	}
}

void CDStarControl::clock()
{
	unsigned int ms = m_interval.elapsed();
	m_interval.start();

	if (m_network != NULL)
		writeNetwork();

	m_ackTimer.clock(ms);
	if (m_ackTimer.isRunning() && m_ackTimer.hasExpired()) {
		sendAck();
		m_ackTimer.stop();
	}

	m_errTimer.clock(ms);
	if (m_errTimer.isRunning() && m_errTimer.hasExpired()) {
		sendError();
		m_errTimer.stop();
	}

	m_rfTimeoutTimer.clock(ms);
	m_netTimeoutTimer.clock(ms);

	if (m_netState == RS_NET_AUDIO) {
		m_networkWatchdog.clock(ms);

		if (m_networkWatchdog.hasExpired()) {
			// We're received the header haven't we?
			unsigned char my1[DSTAR_LONG_CALLSIGN_LENGTH];
			unsigned char my2[DSTAR_SHORT_CALLSIGN_LENGTH];
			unsigned char your[DSTAR_LONG_CALLSIGN_LENGTH];
			m_netHeader.getMyCall1(my1);
			m_netHeader.getMyCall2(my2);
			m_netHeader.getYourCall(your);
			m_netFrames += 1U;

			LogMessage("D-Star, network watchdog has expired, %.1f seconds,  %u%% packet loss", float(m_netFrames) / 50.0F, (m_netLost * 100U) / m_netFrames);
			writeJSONNet("lost", float(m_netFrames) / 50.0F, float(m_netLost * 100U) / float(m_netFrames));
			writeEndNet();
		}
	}

	// Only insert silence on audio data
	if (m_netState == RS_NET_AUDIO) {
		m_packetTimer.clock(ms);

		if (m_packetTimer.isRunning() && m_packetTimer.hasExpired()) {
			unsigned int elapsed = m_elapsed.elapsed();
			unsigned int frames = elapsed / DSTAR_FRAME_TIME;

			if (frames > m_netFrames) {
				unsigned int count = frames - m_netFrames;
				if (count > 15U) {
					LogDebug("D-Star, lost audio for 300ms filling in, elapsed: %ums, expected: %u, received: %u", elapsed, frames, m_netFrames);
					insertSilence(count - 2U);
				}
			}

			m_packetTimer.start();
		}
	}
}

void CDStarControl::writeQueueHeaderRF(const unsigned char *data)
{
	assert(data != NULL);

	if (m_netState != RS_NET_IDLE)
		return;

	if (m_rfTimeoutTimer.isRunning() && m_rfTimeoutTimer.hasExpired())
		return;

	unsigned char len = DSTAR_HEADER_LENGTH_BYTES + 1U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("D-Star, overflow in the D-Star RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	m_queue.addData(data, len);
}

void CDStarControl::writeQueueDataRF(const unsigned char *data)
{
	assert(data != NULL);

	if (m_netState != RS_NET_IDLE)
		return;

	if (m_rfTimeoutTimer.isRunning() && m_rfTimeoutTimer.hasExpired())
		return;

	unsigned char len = DSTAR_FRAME_LENGTH_BYTES + 1U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("D-Star, overflow in the D-Star RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	m_queue.addData(data, len);
}

void CDStarControl::writeQueueEOTRF()
{
	if (m_netState != RS_NET_IDLE)
		return;

	if (m_rfTimeoutTimer.isRunning() && m_rfTimeoutTimer.hasExpired())
		return;

	unsigned char len = 1U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("D-Star, overflow in the D-Star RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	unsigned char data = TAG_EOT;
	m_queue.addData(&data, len);
}

void CDStarControl::writeQueueHeaderNet(const unsigned char *data)
{
	assert(data != NULL);

	if (m_netTimeoutTimer.isRunning() && m_netTimeoutTimer.hasExpired())
		return;

	unsigned char len = DSTAR_HEADER_LENGTH_BYTES + 1U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("D-Star, overflow in the D-Star RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	m_queue.addData(data, len);
}

void CDStarControl::writeQueueDataNet(const unsigned char *data)
{
	assert(data != NULL);

	if (m_netTimeoutTimer.isRunning() && m_netTimeoutTimer.hasExpired())
		return;

	unsigned char len = DSTAR_FRAME_LENGTH_BYTES + 1U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("D-Star, overflow in the D-Star RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	m_queue.addData(data, len);
}

void CDStarControl::writeQueueEOTNet()
{
	if (m_netTimeoutTimer.isRunning() && m_netTimeoutTimer.hasExpired())
		return;

	unsigned char len = 1U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("D-Star, overflow in the D-Star RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	unsigned char data = TAG_EOT;
	m_queue.addData(&data, len);
}

void CDStarControl::writeNetworkHeaderRF(const unsigned char* data)
{
	assert(data != NULL);

	if (m_network == NULL)
		return;

	// Don't send to the network if the timeout has expired
	if (m_rfTimeoutTimer.isRunning() && m_rfTimeoutTimer.hasExpired())
		return;

	m_network->writeHeader(data + 1U, DSTAR_HEADER_LENGTH_BYTES, m_netState != RS_NET_IDLE);
}

void CDStarControl::writeNetworkDataRF(const unsigned char* data, unsigned int errors, bool end)
{
	assert(data != NULL);

	if (m_network == NULL)
		return;

	// Don't send to the network if the timeout has expired
	if (m_rfTimeoutTimer.isRunning() && m_rfTimeoutTimer.hasExpired())
		return;

	m_network->writeData(data + 1U, DSTAR_FRAME_LENGTH_BYTES, errors, end, m_netState != RS_NET_IDLE);
}

bool CDStarControl::insertSilence(const unsigned char* data, unsigned char seqNo)
{
	assert(data != NULL);

	// Check to see if we have any spaces to fill?
	unsigned int oldSeqNo = (m_netN + 1U) % 21U;
	if (oldSeqNo == seqNo) {
		// Just copy the data, nothing else to do here
		::memcpy(m_lastFrame, data, DSTAR_FRAME_LENGTH_BYTES + 1U);
		m_lastFrameValid = true;
		return true;
	}

	unsigned int count;
	if (seqNo > oldSeqNo)
		count = seqNo - oldSeqNo;
	else
		count = (21U + seqNo) - oldSeqNo;

	if (count >= 10U)
		return false;

	insertSilence(count);

	::memcpy(m_lastFrame, data, DSTAR_FRAME_LENGTH_BYTES + 1U);
	m_lastFrameValid = true;

	return true;
}

void CDStarControl::insertSilence(unsigned int count)
{
	unsigned char n = (m_netN + 1U) % 21U;

	for (unsigned int i = 0U; i < count; i++) {
		if (i < 3U && m_lastFrameValid) {
			if (n == 0U) {
				::memcpy(m_lastFrame + DSTAR_VOICE_FRAME_LENGTH_BYTES + 1U, DSTAR_NULL_SLOW_SYNC_BYTES, DSTAR_DATA_FRAME_LENGTH_BYTES);
				writeQueueDataNet(m_lastFrame);
			} else {
				::memcpy(m_lastFrame + DSTAR_VOICE_FRAME_LENGTH_BYTES + 1U, DSTAR_NULL_SLOW_DATA_BYTES, DSTAR_DATA_FRAME_LENGTH_BYTES);
				writeQueueDataNet(m_lastFrame);
			}
		} else {
			m_lastFrameValid = false;

			if (n == 0U)
				writeQueueDataNet(DSTAR_NULL_FRAME_SYNC_BYTES);
			else
				writeQueueDataNet(DSTAR_NULL_FRAME_DATA_BYTES);
		}

		m_netN = n;

		m_netFrames++;
		m_netLost++;

		n = (n + 1U) % 21U;
	}
}

void CDStarControl::blankDTMF(unsigned char* data) const
{
	assert(data != NULL);

	// DTMF begins with these byte values
	if ((data[0] & DSTAR_DTMF_MASK[0]) == DSTAR_DTMF_SIG[0] && (data[1] & DSTAR_DTMF_MASK[1]) == DSTAR_DTMF_SIG[1] &&
		(data[2] & DSTAR_DTMF_MASK[2]) == DSTAR_DTMF_SIG[2] && (data[3] & DSTAR_DTMF_MASK[3]) == DSTAR_DTMF_SIG[3] &&
		(data[4] & DSTAR_DTMF_MASK[4]) == DSTAR_DTMF_SIG[4] && (data[5] & DSTAR_DTMF_MASK[5]) == DSTAR_DTMF_SIG[5] &&
		(data[6] & DSTAR_DTMF_MASK[6]) == DSTAR_DTMF_SIG[6] && (data[7] & DSTAR_DTMF_MASK[7]) == DSTAR_DTMF_SIG[7] &&
		(data[8] & DSTAR_DTMF_MASK[8]) == DSTAR_DTMF_SIG[8])
		::memcpy(data, DSTAR_NULL_AMBE_DATA_BYTES, DSTAR_VOICE_FRAME_LENGTH_BYTES);
}

void CDStarControl::sendAck()
{
	m_rfTimeoutTimer.stop();

	if (!m_ackReply)
		return;

	unsigned char user[DSTAR_LONG_CALLSIGN_LENGTH];
	m_rfHeader.getMyCall1(user);

	CDStarHeader header;
	header.setUnavailable(true);
	header.setMyCall1(m_callsign);
	header.setYourCall(user);
	header.setRPTCall1(m_gateway);
	header.setRPTCall2(m_callsign);

	unsigned char data[DSTAR_HEADER_LENGTH_BYTES + 1U];
	header.get(data + 1U);
	data[0U] = TAG_HEADER;

	writeQueueHeaderRF(data);

	writeQueueDataRF(DSTAR_NULL_FRAME_SYNC_BYTES);

	LINK_STATUS status = LS_NONE;
	unsigned char reflector[DSTAR_LONG_CALLSIGN_LENGTH];
	if (m_network != NULL)
		m_network->getStatus(status, reflector);

	char text[40U];
	if (m_ackMessage == DSTAR_ACK_RSSI && m_rssi != 0) {
		if (status == LS_LINKED_DEXTRA || status == LS_LINKED_DPLUS || status == LS_LINKED_DCS || status == LS_LINKED_CCS || status == LS_LINKED_LOOPBACK) {
			CUtils::removeChar(reflector, ' ');//remove space from reflector so all nicely fits onto 20 chars in case rssi < 99dBm
			::sprintf(text, "%-8.8s %.1f%% %ddBm        ", reflector, float(m_rfErrs * 100U) / float(m_rfBits), m_aveRSSI / int(m_rssiCountTotal));
		} else {
			::sprintf(text, "BER:%.1f%% %ddBm           ", float(m_rfErrs * 100U) / float(m_rfBits), m_aveRSSI / int(m_rssiCountTotal));
		}
	} else if (m_ackMessage == DSTAR_ACK_SMETER && m_rssi != 0) {
		const int RSSI_S1 = -141;
		const int RSSI_S9 = -93;

		int signal = 0;
		int plus   = 0;
		int rssi   = m_aveRSSI / int(m_rssiCountTotal);

		if (rssi < RSSI_S1) {
			plus   = RSSI_S1 - rssi;
		} else if (rssi < RSSI_S9 && rssi >= RSSI_S1) {
			signal = ((rssi - RSSI_S1) / 6) + 1;
			plus   = (rssi - RSSI_S1) % 6;
		} else {
			plus   = rssi - RSSI_S9;
		}

		char signalText[15U];
		if (plus != 0)
			::sprintf(signalText, "S%d+%02d", signal, plus);
		else
			::sprintf(signalText, "S%d", signal);

		if (status == LS_LINKED_DEXTRA || status == LS_LINKED_DPLUS || status == LS_LINKED_DCS || status == LS_LINKED_CCS || status == LS_LINKED_LOOPBACK)
			::sprintf(text, "%-8.8s %.1f%% %s           ", reflector, float(m_rfErrs * 100U) / float(m_rfBits), signalText);
		else
			::sprintf(text, "BER:%.1f%% %s             ", float(m_rfErrs * 100U) / float(m_rfBits), signalText);
	} else {
		if (status == LS_LINKED_DEXTRA || status == LS_LINKED_DPLUS || status == LS_LINKED_DCS || status == LS_LINKED_CCS || status == LS_LINKED_LOOPBACK)
			::sprintf(text, "%-8.8s  BER: %.1f%%         ", reflector, float(m_rfErrs * 100U) / float(m_rfBits));
		else
			::sprintf(text, "BER: %.1f%%                 ", float(m_rfErrs * 100U) / float(m_rfBits));
	}

	m_rfSlowData.setText(text);

	::memcpy(data, DSTAR_NULL_FRAME_DATA_BYTES, DSTAR_FRAME_LENGTH_BYTES + 1U);

	for (unsigned int i = 0U; i < 19U; i++) {
		m_rfSlowData.getSlowData(data + 1U + DSTAR_VOICE_FRAME_LENGTH_BYTES);
		writeQueueDataRF(data);
	}

	writeQueueEOTRF();
}

void CDStarControl::sendError()
{
	unsigned char user[DSTAR_LONG_CALLSIGN_LENGTH];
	m_rfHeader.getMyCall1(user);

	CDStarHeader header;
	header.setUnavailable(true);
	header.setMyCall1(m_callsign);
	header.setYourCall(user);
	header.setRPTCall1(m_callsign);
	header.setRPTCall2(m_callsign);

	unsigned char data[DSTAR_HEADER_LENGTH_BYTES + 1U];
	header.get(data + 1U);
	data[0U] = TAG_HEADER;

	writeQueueHeaderRF(data);

	writeQueueDataRF(DSTAR_NULL_FRAME_SYNC_BYTES);

	LINK_STATUS status = LS_NONE;
	unsigned char reflector[DSTAR_LONG_CALLSIGN_LENGTH];
	if (m_network != NULL)
		m_network->getStatus(status, reflector);

	char text[40U];
	if (m_ackMessage == DSTAR_ACK_RSSI && m_rssi != 0) {
		if (status == LS_LINKED_DEXTRA || status == LS_LINKED_DPLUS || status == LS_LINKED_DCS || status == LS_LINKED_CCS || status == LS_LINKED_LOOPBACK) {
			CUtils::removeChar(reflector, ' ');//remove space from reflector so all nicely fits onto 20 chars in case rssi < 99dBm
			::sprintf(text, "%-8.8s %.1f%% %ddBm        ", reflector, float(m_rfErrs * 100U) / float(m_rfBits), m_aveRSSI / int(m_rssiCountTotal));
		} else {
			::sprintf(text, "BER:%.1f%% %ddBm           ", float(m_rfErrs * 100U) / float(m_rfBits), m_aveRSSI / int(m_rssiCountTotal));
		}
	} else if (m_ackMessage == DSTAR_ACK_SMETER && m_rssi != 0) {
		const int RSSI_S1 = -141;
		const int RSSI_S9 = -93;

		int signal = 0;
		int plus   = 0;
		int rssi   = m_aveRSSI / int(m_rssiCountTotal);

		if (rssi < RSSI_S1) {
			plus   = RSSI_S1 - rssi;
		} else if (rssi < RSSI_S9 && rssi >= RSSI_S1) {
			signal = ((rssi - RSSI_S1) / 6) + 1;
			plus   = (rssi - RSSI_S1) % 6;
		} else {
			plus   = rssi - RSSI_S9;
		}

		char signalText[15U];
		if (plus != 0U)
			::sprintf(signalText, "S%d+%02d", signal, plus);
		else
			::sprintf(signalText, "S%d", signal);

		if (status == LS_LINKED_DEXTRA || status == LS_LINKED_DPLUS || status == LS_LINKED_DCS || status == LS_LINKED_CCS || status == LS_LINKED_LOOPBACK)
			::sprintf(text, "%-8.8s %.1f%% %s           ", reflector, float(m_rfErrs * 100U) / float(m_rfBits), signalText);
		else
			::sprintf(text, "BER:%.1f%% %s             ", float(m_rfErrs * 100U) / float(m_rfBits), signalText);
	} else {
		if (status == LS_LINKED_DEXTRA || status == LS_LINKED_DPLUS || status == LS_LINKED_DCS || status == LS_LINKED_CCS || status == LS_LINKED_LOOPBACK)
			::sprintf(text, "%-8.8s  BER: %.1f%%         ", reflector, float(m_rfErrs * 100U) / float(m_rfBits));
		else
			::sprintf(text, "BER: %.1f%%                 ", float(m_rfErrs * 100U) / float(m_rfBits));
	}

	m_rfSlowData.setText(text);

	::memcpy(data, DSTAR_NULL_FRAME_DATA_BYTES, DSTAR_FRAME_LENGTH_BYTES + 1U);

	for (unsigned int i = 0U; i < 19U; i++) {
		m_rfSlowData.getSlowData(data + 1U + DSTAR_VOICE_FRAME_LENGTH_BYTES);
		writeQueueDataRF(data);
	}

	writeQueueEOTRF();
}

bool CDStarControl::isBusy() const
{
	return m_rfState != RS_RF_LISTENING || m_netState != RS_NET_IDLE;
}

void CDStarControl::enable(bool enabled)
{
	if (!enabled && m_enabled) {
		m_queue.clear();

		// Reset the RF section
		m_rfState = RS_RF_LISTENING;

		m_rfTimeoutTimer.stop();

		// Reset the networking section
		m_netState = RS_NET_IDLE;

		m_lastFrameValid = false;

		m_netTimeoutTimer.stop();
		m_networkWatchdog.stop();
		m_packetTimer.stop();
	}

	m_enabled = enabled;
}

void CDStarControl::writeJSONRSSI()
{
	if (m_rssi == 0)
		return;

	if (m_rssiCountTotal >= RSSI_COUNT) {
		nlohmann::json json;

		json["timestamp"] = CUtils::createTimestamp();
		json["mode"]      = "D-Star";

		json["value"]     = m_rssiAccum / int(m_rssiCountTotal);

		WriteJSON("RSSI", json);

		m_rssiAccum      = 0;
		m_rssiCountTotal = 0U;
	}
}

void CDStarControl::writeJSONBER()
{
	if (m_bitsCount >= BER_COUNT) {
		nlohmann::json json;

		json["timestamp"] = CUtils::createTimestamp();
		json["mode"]      = "D-Star";

		json["value"]     = float(m_bitErrsAccum * 100U) / float(m_bitsCount);

		WriteJSON("BER", json);

		m_bitErrsAccum = 0U;
		m_bitsCount    = 1U;
	}
}

void CDStarControl::writeJSONText(const unsigned char* text)
{
	assert(text != NULL);

	nlohmann::json json;

	json["timestamp"] = CUtils::createTimestamp();
	json["mode"]      = "D-Star";

	json["value"]     = std::string((char*)text);

	WriteJSON("Text", json);
}

void CDStarControl::writeJSONRF(const char* action, const unsigned char* my1, const unsigned char* my2, const unsigned char* your)
{
	assert(action != NULL);
	assert(my1 != NULL);
	assert(my2 != NULL);
	assert(your != NULL);

	nlohmann::json json;

	json["timestamp"]      = CUtils::createTimestamp();

	json["source_cs"]      = convertBuffer(my1, DSTAR_LONG_CALLSIGN_LENGTH);
	json["source_ext"]     = convertBuffer(my2, DSTAR_SHORT_CALLSIGN_LENGTH);
	json["destination_cs"] = convertBuffer(your, DSTAR_LONG_CALLSIGN_LENGTH);

	json["source"] = "rf";
	json["action"] = action;

	WriteJSON("D-Star", json);
}

void CDStarControl::writeJSONRF(const char* action, float duration, float ber)
{
	assert(action != NULL);

	nlohmann::json json;

	writeJSONRF(json, action, duration, ber);

	WriteJSON("D-Star", json);
}

void CDStarControl::writeJSONRF(const char* action, float duration, float ber, int minRSSI, int maxRSSI, int aveRSSI)
{
	assert(action != NULL);

	nlohmann::json json;

	writeJSONRF(json, action, duration, ber);

	nlohmann::json rssi;
	rssi["min"] = minRSSI;
	rssi["max"] = maxRSSI;
	rssi["ave"] = aveRSSI;

	json["rssi"] = rssi;

	WriteJSON("D-Star", json);
}

void CDStarControl::writeJSONNet(const char* action, const unsigned char* my1, const unsigned char* my2, const unsigned char* your, const unsigned char* reflector)
{
	assert(action != NULL);
	assert(my1 != NULL);
	assert(my2 != NULL);
	assert(your != NULL);

	nlohmann::json json;

	json["timestamp"]      = CUtils::createTimestamp();

	json["source_cs"]      = convertBuffer(my1, DSTAR_LONG_CALLSIGN_LENGTH);
	json["source_ext"]     = convertBuffer(my2, DSTAR_SHORT_CALLSIGN_LENGTH);
	json["destination_cs"] = convertBuffer(your, DSTAR_LONG_CALLSIGN_LENGTH);

	json["source"] = "network";
	json["action"] = action;

	if (reflector != NULL)
		json["reflector"] = convertBuffer(reflector, DSTAR_LONG_CALLSIGN_LENGTH);

	WriteJSON("D-Star", json);
}

void CDStarControl::writeJSONNet(const char* action, float duration, float loss)
{
	assert(action != NULL);

	nlohmann::json json;

	json["timestamp"] = CUtils::createTimestamp();

	json["duration"] = duration;
	json["loss"]     = loss;

	json["action"] = action;

	WriteJSON("D-Star", json);
}

void CDStarControl::writeJSONRF(nlohmann::json& json, const char* action, float duration, float ber)
{
	assert(action != NULL);

	json["timestamp"] = CUtils::createTimestamp();

	json["duration"] = duration;
	json["ber"]      = ber;

	json["action"] = action;
}

std::string CDStarControl::convertBuffer(const unsigned char* buffer, unsigned int length) const
{
	assert(buffer != NULL);

	std::string callsign((char*)buffer, length);

	size_t pos = callsign.find_first_of(' ');
	if (pos != std::string::npos)
		callsign = callsign.substr(0U, pos);

	return callsign;
}

#endif

/*
*   Copyright (C) 2010,2012,2013,2014,2018 by Jonathan Naylor G4KLX
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "MobileGPS.h"
#include "SerialPort.h"
#include "UDPSocket.h"
#include "StopWatch.h"
#include "Version.h"
#include "Thread.h"
#include "Timer.h"
#include "Utils.h"
#include "Conf.h"
#include "Log.h"

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "MobileGPS.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/MobileGPS.ini";
#endif

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <ctime>
#include <cstring>


int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;
	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "MobileGPS version %s\n", VERSION);
				return 0;
			} else if (arg.substr(0, 1) == "-") {
				::fprintf(stderr, "Usage: MobileGPS [-v|--version] [filename]\n");
				return 1;
			} else {
				iniFile = argv[currentArg];
			}
		}
	}

	CMobileGPS* gateway = new CMobileGPS(std::string(iniFile));
	gateway->run();
	delete gateway;

	return 0;
}

CMobileGPS::CMobileGPS(const std::string& file) :
m_file(file),
m_debug(false),
m_data(NULL),
m_offset(0U),
m_collect(false),
m_latitude(),
m_latitudeNS(),
m_longitude(),
m_longitudeEW(),
m_altitude(0.0F),
m_speed(0.0F),
m_bearing(0.0F)
{
	m_data = new unsigned char[1000U];

	m_latitude   = new char[20U];
	m_latitudeNS = new char[5U];

	m_longitude   = new char[20U];
	m_longitudeEW = new char[5U];
}

CMobileGPS::~CMobileGPS()
{
	delete[] m_latitude;
	delete[] m_latitudeNS;

	delete[] m_longitude;
	delete[] m_longitudeEW;

	delete[] m_data;
}

void CMobileGPS::run()
{
	CConf conf(m_file);

	bool ret = conf.read();
	if (!ret) {
		::fprintf(stderr, "MobileGPS: cannot read the .ini file\n");
		return;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::fprintf(stderr, "Couldn't fork() , exiting\n");
			return;
		} else if (pid != 0) {
			exit(EXIT_SUCCESS);
		}

		// Create new session and process group
		if (::setsid() == -1) {
			::fprintf(stderr, "Couldn't setsid(), exiting\n");
			return;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1) {
			::fprintf(stderr, "Couldn't cd /, exiting\n");
			return;
		}

		// If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::fprintf(stderr, "Could not get the mmdvm user, exiting\n");
				return;
			}

			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			// Set user and group ID's to mmdvm:mmdvm
			if (setgid(mmdvm_gid) != 0) {
				::fprintf(stderr, "Could not set mmdvm GID, exiting\n");
				return;
			}

			if (setuid(mmdvm_uid) != 0) {
				::fprintf(stderr, "Could not set mmdvm UID, exiting\n");
				return;
			}

			// Double check it worked (AKA Paranoia) 
			if (setuid(0) != -1) {
				::fprintf(stderr, "It's possible to regain root - something is wrong!, exiting\n");
				return;
			}
		}
	}
#endif

	ret = ::LogInitialise(conf.getLogFilePath(), conf.getLogFileRoot(), 1U, 1U);
	if (!ret) {
		::fprintf(stderr, "MobileGPS: unable to open the log file\n");
		return;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	if (m_daemon) {
		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
	}
#endif

	m_debug = conf.getDebug();

	std::string gpsPort = conf.getGPSPort();
	unsigned int gpsSpeed = conf.getGPSSpeed();
	bool gpsDebug = conf.getGPSDebug();
	
	CSerialPort gps(gpsPort, SERIAL_SPEED(gpsSpeed));
	ret = gps.open();
	if (!ret) {
		::LogFinalise();
		return;
	}

	unsigned int port = conf.getNetworkPort();
	bool networkDebug = conf.getNetworkDebug();

	CUDPSocket network(port);
	ret = network.open();
	if (!ret) {
		gps.close();
		::LogFinalise();
		return;
	}

	CTimer minTimer(1000U, conf.getMinTime());
	CTimer maxTimer(1000U, conf.getMaxTime());

	CStopWatch stopWatch;
	stopWatch.start();

	LogMessage("Starting MobileGPS-%s", VERSION);

	for (;;) {
		unsigned char buffer[200U];
		int len = gps.read(buffer, 200U);
		if (len > 0) {
			if (gpsDebug)
				CUtils::dump("GPS Data", buffer, len);
				
			interpret(buffer, len);
		}

		in_addr address;
		unsigned int port;
		len = network.read(buffer, 200U, address, port);
		if (len > 0) {
			if (networkDebug) {
				// Display address
				CUtils::dump("Data", buffer, len);
			}
		}
	}

	network.close();

	gps.close();

	::LogFinalise();
}

void CMobileGPS::interpret(const unsigned char* data, unsigned int length)
{
	for (unsigned int i = 0U; i < length; i++) {
		char c = data[i];
		if (c == '$')
			m_collect = true;

		if (m_collect) {
			m_data[m_offset++] = data[i];

			if (c == '\x0A') {
				if (m_debug)
					CUtils::dump("NMEA Data", (unsigned char*)m_data, m_offset);

				bool ret = checkXOR(m_data + 1U, m_offset - 1U);
				if (ret) {
					if (::memcmp(m_data + 3U, "GGA", 3U) == 0)
						processGGA();
					else if (::memcmp(m_data + 3U, "RMC", 3U) == 0)
						processRMC();
				}

				m_offset = 0U;
				m_collect = false;
			}
		}
	}
}

bool CMobileGPS::checkXOR(const unsigned char* data, unsigned int length) const
{
	unsigned int posStar = 0U;
	for (unsigned int i = length - 1U; i > 0U; i--) {
		if (data[i] == '*') {
			posStar = i;
			break;
		}
	}

	if (posStar == 0U)
		return false;

	unsigned char csum = calcXOR(data, posStar);

	char buffer[10U];
	::sprintf(buffer, "%02X", csum);

	return ::memcmp(buffer, data + posStar + 1U, 2U) == 0;
}

unsigned char CMobileGPS::calcXOR(const unsigned char* buffer, unsigned int length) const
{ 
	unsigned char res = 0U;

	for (unsigned int i = 0U; i < length; i++)
		res ^= buffer[i];

	return res;
}

void CMobileGPS::processGGA()
{
	// Parse the $GPGGA string into tokens
	char* pGGA[20U];
	::memset(pGGA, 0x00U, 20U * sizeof(char*));
	unsigned int nGGA = 0U;

	char* str = (char*)m_data;
	for (;;) {
		char* p = mystrsep(&str, ",\r\n");

		pGGA[nGGA++] = p;
		if (p == NULL)
			break;
	}

	// Is there any position data?
	if (pGGA[2U] == NULL || pGGA[3U] == NULL || pGGA[4U] == NULL || pGGA[5U] == NULL || ::strlen(pGGA[2U]) == 0U || ::strlen(pGGA[3U]) == 0U || ::strlen(pGGA[4U]) == 0 || ::strlen(pGGA[5U]) == 0)
		return;

	// Is it a valid GPS fix?
	if (::strcmp(pGGA[6U], "0") == 0)
		return;

	::strcpy(m_latitude, pGGA[2U]);
	::strcpy(m_latitudeNS, pGGA[3U]);
	
	::strcpy(m_longitude, pGGA[4U]);
	::strcpy(m_longitudeEW, pGGA[5U]);

	if (pGGA[9U] != NULL && ::strlen(pGGA[9U]) > 0U) {
		// Convert altitude from metres to feet
		m_altitude = ::atof(pGGA[9U]) * 3.28F;
	}
}

void CMobileGPS::processRMC()
{
	// Parse the $GPRMC string into tokens
	char* pRMC[20U];
	::memset(pRMC, 0x00U, 20U * sizeof(char*));
	unsigned int nRMC = 0U;

	char* str = (char*)m_data;
	for (;;) {
		char* p = mystrsep(&str, ",\r\n");

		pRMC[nRMC++] = p;
		if (p == NULL)
			break;
	}

	// Is there any position data?
	if (pRMC[3U] == NULL || pRMC[4U] == NULL || pRMC[5U] == NULL || pRMC[6U] == NULL || ::strlen(pRMC[3U]) == 0U || ::strlen(pRMC[4U]) == 0U || ::strlen(pRMC[5U]) == 0 || ::strlen(pRMC[6U]) == 0)
		return;

	// Is it a valid GPS fix?
	if (::strcmp(pRMC[2U], "A") != 0)
		return;

	::strcpy(m_latitude, pRMC[3U]);
	::strcpy(m_latitudeNS, pRMC[4U]);
	
	::strcpy(m_longitude, pRMC[5U]);
	::strcpy(m_longitudeEW, pRMC[6U]);

	if (pRMC[7U] != NULL && pRMC[8U] != NULL && ::strlen(pRMC[7U]) > 0U && ::strlen(pRMC[8U]) > 0U) {
		m_bearing = ::atof(pRMC[8U]);
		m_speed   = ::atof(pRMC[7U]);
	}
}

// Source found at <http://unixpapa.com/incnote/string.html>
char* CMobileGPS::mystrsep(char** sp, const char* sep) const
{
	if (sp == NULL || *sp == NULL || **sp == '\0')
		return NULL;

	char* s = *sp;
	char* p = s + ::strcspn(s, sep);

	if (*p != '\0')
		*p++ = '\0';

	*sp = p;

	return s;
}
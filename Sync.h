/*
 *   Copyright (C) 2015,2016,2018,2020,2021,2023 by Jonathan Naylor G4KLX
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

#if !defined(SYNC_H)
#define	SYNC_H

#include "Defines.h"

class CSync
{
public:
#if defined(USE_DSTAR)
	static void addDStarSync(unsigned char* data);
#endif

#if defined(USE_DMR)
	static void addDMRDataSync(unsigned char* data, bool duplex);
	static void addDMRAudioSync(unsigned char* data, bool duplex);
#endif

#if defined(USE_YSF)
	static void addYSFSync(unsigned char* data);
#endif

#if defined(USE_P25)
	static void addP25Sync(unsigned char* data);
#endif

#if defined(USE_NXDN)
	static void addNXDNSync(unsigned char* data);
#endif

private:
};

#endif

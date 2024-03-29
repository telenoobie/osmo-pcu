/*
 * Copyright (C) 2012 Ivan Klyuchnikov
 * Copyright (C) 2012 Andreas Eversberg <jolly@eversberg.eu>
 * Copyright (C) 2013 by Holger Hans Peter Freyther
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#pragma once

extern "C" {
#include <osmocom/core/linuxlist.h>
}

#include <stdint.h>

class TimingAdvance {
public:
	TimingAdvance();

	int update(uint32_t old_tlli, uint32_t new_tlli, uint8_t ta);
	int remember(uint32_t tlli, uint8_t ta);
	int recall(uint32_t tlli);
	int flush();

private:
	size_t m_ta_len;
	llist_head m_ta_list;	
};

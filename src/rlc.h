/* rlc header descriptions
 *
 * Copyright (C) 2012 Ivan Klyuchnikov
 * Copyright (C) 2012 Andreas Eversberg <jolly@eversberg.eu>
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

#include <stdint.h>

#define RLC_MAX_SNS 128 /* GPRS, must be power of 2 */
#define RLC_MAX_WS  64 /* max window size */
#define RLC_MAX_LEN 54 /* CS-4 including spare bits */

struct BTS;
struct gprs_rlc_v_n;

/* The state of a BSN in the send/receive window */
enum gprs_rlc_ul_bsn_state {
	GPRS_RLC_UL_BSN_INVALID,
	GPRS_RLC_UL_BSN_RECEIVED,
	GPRS_RLC_UL_BSN_MISSING,
	GPRS_RLC_UL_BSN_MAX,
};

enum gprs_rlc_dl_bsn_state {
	GPRS_RLC_DL_BSN_INVALID,
	GPRS_RLC_DL_BSN_NACKED,
	GPRS_RLC_DL_BSN_ACKED,
	GPRS_RLC_DL_BSN_UNACKED,
	GPRS_RLC_DL_BSN_RESEND,
	GPRS_RLC_DL_BSN_MAX,
};


static inline uint16_t mod_sns_half()
{
	return (RLC_MAX_SNS / 2) - 1;
}

struct gprs_rlc_data {
	uint8_t *prepare(size_t block_data_length);
	void put_data(const uint8_t *data, size_t len);

	/* block history */
	uint8_t block[RLC_MAX_LEN];
	/* block len of history */
	uint8_t len;
};

/*
 * I hold the currently transferred blocks and will provide
 * the routines to manipulate these arrays.
 */
struct gprs_rlc {
	gprs_rlc_data *block(int bsn);
	gprs_rlc_data m_blocks[RLC_MAX_SNS/2];
};

/**
 * TODO: for GPRS/EDGE maybe make sns a template parameter
 * so we create specialized versions...
 */
struct gprs_rlc_v_b {
	/* Check for an individual frame */
	bool is_unacked(int bsn) const;
	bool is_nacked(int bsn) const;
	bool is_acked(int bsn) const;
	bool is_resend(int bsn) const;
	bool is_invalid(int bsn) const;
	gprs_rlc_dl_bsn_state get_state(int bsn) const;

	/* Mark a RLC frame for something */
	void mark_unacked(int bsn);
	void mark_nacked(int bsn);
	void mark_acked(int bsn);
	void mark_resend(int bsn);
	void mark_invalid(int bsn);

	void reset();


private:
	bool is_state(int bsn, const gprs_rlc_dl_bsn_state state) const;
	void mark(int bsn, const gprs_rlc_dl_bsn_state state);

	gprs_rlc_dl_bsn_state m_v_b[RLC_MAX_SNS/2]; /* acknowledge state array */
};


/**
 * TODO: The UL/DL code could/should share a baseclass but
 * we are using llist_for_each_entry for the TBF which
 * requires everything which creates a requirement for a POD
 * type and in < C++11 something that is using even if the
 * most simple form of inheritance is not a POD anymore.
 */
struct gprs_rlc_dl_window {
	void reset();
	const uint16_t mod_sns() const;
	const uint16_t sns() const;
	const uint16_t ws() const;

	bool window_stalled() const;
	bool window_empty() const;

	void increment_send();
	void raise(int moves);

	const uint16_t v_s() const;
	const uint16_t v_s_mod(int offset) const;
	const uint16_t v_a() const;
	const int16_t distance() const;

	/* Methods to manage reception */
	int resend_needed();
	int mark_for_resend();
	void update(BTS *bts, char *show_rbb, uint8_t ssn,
			uint16_t *lost, uint16_t *received);
	int move_window();
	void show_state(char *show_rbb);
	int count_unacked();

	uint16_t m_v_s;	/* send state */
	uint16_t m_v_a;	/* ack state */

	gprs_rlc_v_b m_v_b;
};

struct gprs_rlc_v_n {
	void reset();

	void mark_received(int bsn);
	void mark_missing(int bsn);

	bool is_received(int bsn) const;

	gprs_rlc_ul_bsn_state state(int bsn) const;
private:
	bool is_state(int bsn, const gprs_rlc_ul_bsn_state state) const;
	void mark(int bsn, const gprs_rlc_ul_bsn_state state);
	gprs_rlc_ul_bsn_state m_v_n[RLC_MAX_SNS/2]; /* receive state array */
};

struct gprs_rlc_ul_window {
	const uint16_t mod_sns() const;
	const uint16_t sns() const;
	const uint16_t ws() const;

	const uint16_t v_r() const;
	const uint16_t v_q() const;

	const uint16_t ssn() const;

	bool is_in_window(uint8_t bsn) const;

	void update_rbb(char *rbb);
	void raise_v_r_to(int moves);
	void raise_v_r(const uint16_t bsn);
	uint16_t raise_v_q();

	void raise_v_q(int);

	uint16_t receive_bsn(const uint16_t bsn);

	uint16_t m_v_r;	/* receive state */
	uint16_t m_v_q;	/* receive window state */

	gprs_rlc_v_n m_v_n;
};

extern "C" {
/* TS 04.60  10.2.2 */
struct rlc_ul_header {
	uint8_t	r:1,
		 si:1,
		 cv:4,
		 pt:2;
	uint8_t	ti:1,
		 tfi:5,
		 pi:1,
		 spare:1;
	uint8_t	e:1,
		 bsn:7;
} __attribute__ ((packed));

struct rlc_dl_header {
	uint8_t	usf:3,
		 s_p:1,
		 rrbp:2,
		 pt:2;
	uint8_t	fbi:1,
		 tfi:5,
		 pr:2;
	uint8_t	e:1,
		 bsn:7;
} __attribute__ ((packed));

struct rlc_li_field {
	uint8_t	e:1,
		 m:1,
		 li:6;
} __attribute__ ((packed));
}

inline bool gprs_rlc_v_b::is_state(int bsn, const gprs_rlc_dl_bsn_state type) const
{
	return m_v_b[bsn & mod_sns_half()] == type;
}

inline void gprs_rlc_v_b::mark(int bsn, const gprs_rlc_dl_bsn_state type)
{
	m_v_b[bsn & mod_sns_half()] = type;
}

inline bool gprs_rlc_v_b::is_nacked(int bsn) const
{
	return is_state(bsn, GPRS_RLC_DL_BSN_NACKED);
}

inline bool gprs_rlc_v_b::is_acked(int bsn) const
{
	return is_state(bsn, GPRS_RLC_DL_BSN_ACKED);
}

inline bool gprs_rlc_v_b::is_unacked(int bsn) const
{
	return is_state(bsn, GPRS_RLC_DL_BSN_UNACKED);
}

inline bool gprs_rlc_v_b::is_resend(int bsn) const
{
	return is_state(bsn, GPRS_RLC_DL_BSN_RESEND);
}

inline bool gprs_rlc_v_b::is_invalid(int bsn) const
{
	return is_state(bsn, GPRS_RLC_DL_BSN_INVALID);
}

inline gprs_rlc_dl_bsn_state gprs_rlc_v_b::get_state(int bsn) const
{
	return m_v_b[bsn & mod_sns_half()];
}

inline void gprs_rlc_v_b::mark_resend(int bsn)
{
	return mark(bsn, GPRS_RLC_DL_BSN_RESEND);
}

inline void gprs_rlc_v_b::mark_unacked(int bsn)
{
	return mark(bsn, GPRS_RLC_DL_BSN_UNACKED);
}

inline void gprs_rlc_v_b::mark_acked(int bsn)
{
	return mark(bsn, GPRS_RLC_DL_BSN_ACKED);
}

inline void gprs_rlc_v_b::mark_nacked(int bsn)
{
	return mark(bsn, GPRS_RLC_DL_BSN_NACKED);
}

inline void gprs_rlc_v_b::mark_invalid(int bsn)
{
	return mark(bsn, GPRS_RLC_DL_BSN_INVALID);
}

inline const uint16_t gprs_rlc_dl_window::sns() const
{
	return RLC_MAX_SNS;
}

inline const uint16_t gprs_rlc_dl_window::ws() const
{
	return RLC_MAX_WS;
}

inline const uint16_t gprs_rlc_dl_window::mod_sns() const
{
	return sns() - 1;
}

inline const uint16_t gprs_rlc_dl_window::v_s() const
{
	return m_v_s;
}

inline const uint16_t gprs_rlc_dl_window::v_s_mod(int offset) const
{
	return (m_v_s + offset) & mod_sns();
}

inline const uint16_t gprs_rlc_dl_window::v_a() const
{
	return m_v_a;
}

inline bool gprs_rlc_dl_window::window_stalled() const
{
	return ((m_v_s - m_v_a) & mod_sns()) == ws();
}

inline bool gprs_rlc_dl_window::window_empty() const
{
	return m_v_s == m_v_a;
}

inline void gprs_rlc_dl_window::increment_send()
{
	m_v_s = (m_v_s + 1) & mod_sns();
}

inline void gprs_rlc_dl_window::raise(int moves)
{
	m_v_a = (m_v_a + moves) & mod_sns();
}

inline const int16_t gprs_rlc_dl_window::distance() const
{
	return (m_v_s - m_v_a) & mod_sns();
}

inline bool gprs_rlc_ul_window::is_in_window(uint8_t bsn) const
{
	uint16_t offset_v_q;

	/* current block relative to lowest unreceived block */
	offset_v_q = (bsn - m_v_q) & mod_sns();
	/* If out of window (may happen if blocks below V(Q) are received
	 * again. */
	return offset_v_q < ws();
}

inline const uint16_t gprs_rlc_ul_window::sns() const
{
	return RLC_MAX_SNS;
}

inline const uint16_t gprs_rlc_ul_window::ws() const
{
	return RLC_MAX_WS;
}

inline const uint16_t gprs_rlc_ul_window::mod_sns() const
{
	return sns() - 1;
}

inline const uint16_t gprs_rlc_ul_window::v_r() const
{
	return m_v_r;
}

inline const uint16_t gprs_rlc_ul_window::v_q() const
{
	return m_v_q;
}

inline const uint16_t gprs_rlc_ul_window::ssn() const
{
	return m_v_r;
}

inline void gprs_rlc_ul_window::raise_v_r_to(int moves)
{
	m_v_r = (m_v_r + moves) & mod_sns();
}

inline void gprs_rlc_ul_window::raise_v_q(int incr)
{
	m_v_q = (m_v_q + incr) & mod_sns();
}

inline void gprs_rlc_v_n::mark_received(int bsn)
{
	return mark(bsn, GPRS_RLC_UL_BSN_RECEIVED);
}

inline void gprs_rlc_v_n::mark_missing(int bsn)
{
	return mark(bsn, GPRS_RLC_UL_BSN_MISSING);
}

inline bool gprs_rlc_v_n::is_received(int bsn) const
{
	return is_state(bsn, GPRS_RLC_UL_BSN_RECEIVED);
}

inline bool gprs_rlc_v_n::is_state(int bsn, gprs_rlc_ul_bsn_state type) const
{
	return m_v_n[bsn & mod_sns_half()] == type;
}

inline void gprs_rlc_v_n::mark(int bsn, gprs_rlc_ul_bsn_state type)
{
	m_v_n[bsn & mod_sns_half()] = type;
}

inline gprs_rlc_ul_bsn_state gprs_rlc_v_n::state(int bsn) const
{
	return m_v_n[bsn & mod_sns_half()];
}

inline gprs_rlc_data *gprs_rlc::block(int bsn)
{
	return &m_blocks[bsn & mod_sns_half()];
}

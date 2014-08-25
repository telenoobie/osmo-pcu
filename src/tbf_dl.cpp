/* Copied from tbf.cpp
 *
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

#include <bts.h>
#include <tbf.h>
#include <rlc.h>
#include <gprs_rlcmac.h>
#include <gprs_debug.h>
#include <gprs_bssgp_pcu.h>
#include <decoding.h>

extern "C" {
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
}

#include <errno.h>
#include <string.h>

/* After sending these frames, we poll for ack/nack. */
#define POLL_ACK_AFTER_FRAMES 20


static const struct gprs_rlcmac_cs gprs_rlcmac_cs[] = {
/*	frame length	data block	max payload */
	{ 0,		0,		0  },
	{ 23,		23,		20 }, /* CS-1 */
	{ 34,		33,		30 }, /* CS-2 */
	{ 40,		39,		36 }, /* CS-3 */
	{ 54,		53,		50 }, /* CS-4 */
};

extern "C" {
int bssgp_tx_llc_discarded(struct bssgp_bvc_ctx *bctx, uint32_t tlli,
                           uint8_t num_frames, uint32_t num_octets);
}

static inline void tbf_update_ms_class(struct gprs_rlcmac_tbf *tbf,
					const uint8_t ms_class)
{
	if (!tbf->ms_class && ms_class)
		tbf->ms_class = ms_class;
}

int gprs_rlcmac_dl_tbf::append_data(const uint8_t ms_class,
				const uint16_t pdu_delay_csec,
				const uint8_t *data, const uint16_t len)
{
	LOGP(DRLCMAC, LOGL_INFO, "%s append\n", tbf_name(this));
	if (state_is(GPRS_RLCMAC_WAIT_RELEASE)) {
		LOGP(DRLCMAC, LOGL_DEBUG,
			"%s in WAIT RELEASE state "
			"(T3193), so reuse TBF\n", tbf_name(this));
		tbf_update_ms_class(this, ms_class);
		reuse_tbf(data, len);
	} else {
		/* the TBF exists, so we must write it in the queue
		 * we prepend lifetime in front of PDU */
		struct timeval *tv;
		struct msgb *llc_msg = msgb_alloc(len + sizeof(*tv) * 2,
			"llc_pdu_queue");
		if (!llc_msg)
			return -ENOMEM;
		tv = (struct timeval *)msgb_put(llc_msg, sizeof(*tv));
		gprs_llc::calc_pdu_lifetime(bts, pdu_delay_csec, tv);
		tv = (struct timeval *)msgb_put(llc_msg, sizeof(*tv));
		gettimeofday(tv, NULL);
		memcpy(msgb_put(llc_msg, len), data, len);
		m_llc.enqueue(llc_msg);
		tbf_update_ms_class(this, ms_class);
	}

	return 0;
}

static struct gprs_rlcmac_dl_tbf *tbf_lookup_dl(BTS *bts,
					const uint32_t tlli, const char *imsi)
{
	/* TODO: look up by IMSI first, then tlli, then old_tlli */
	return bts->dl_tbf_by_tlli(tlli);
}

static int tbf_new_dl_assignment(struct gprs_rlcmac_bts *bts,
				const char *imsi,
				const uint32_t tlli, const uint8_t ms_class,
				const uint8_t *data, const uint16_t len)
{
	uint8_t trx, ta, ss;
	int8_t use_trx;
	struct gprs_rlcmac_ul_tbf *ul_tbf, *old_ul_tbf;
	struct gprs_rlcmac_dl_tbf *dl_tbf;
	int8_t tfi; /* must be signed */
	int rc;

	/* check for uplink data, so we copy our informations */
#warning "Do the same look up for IMSI, TLLI and OLD_TLLI"
#warning "Refactor the below lines... into a new method"
	ul_tbf = bts->bts->ul_tbf_by_tlli(tlli);
	if (ul_tbf && ul_tbf->m_contention_resolution_done
	 && !ul_tbf->m_final_ack_sent) {
		use_trx = ul_tbf->trx->trx_no;
		ta = ul_tbf->ta;
		ss = 0;
		old_ul_tbf = ul_tbf;
	} else {
		use_trx = -1;
		/* we already have an uplink TBF, so we use that TA */
		if (ul_tbf)
			ta = ul_tbf->ta;
		else {
			/* recall TA */
			rc = bts->bts->timing_advance()->recall(tlli);
			if (rc < 0) {
				LOGP(DRLCMAC, LOGL_NOTICE, "TA unknown"
					", assuming 0\n");
				ta = 0;
			} else
				ta = rc;
		}
		ss = 1; /* PCH assignment only allows one timeslot */
		old_ul_tbf = NULL;
	}

	// Create new TBF (any TRX)
#warning "Copy and paste with alloc_ul_tbf"
	tfi = bts->bts->tfi_find_free(GPRS_RLCMAC_DL_TBF, &trx, use_trx);
	if (tfi < 0) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return -EBUSY;
	}
	/* set number of downlink slots according to multislot class */
	dl_tbf = tbf_alloc_dl_tbf(bts, ul_tbf, tfi, trx, ms_class, ss);
	if (!dl_tbf) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return -EBUSY;
	}
	dl_tbf->m_tlli = tlli;
	dl_tbf->m_tlli_valid = 1;
	dl_tbf->ta = ta;

	LOGP(DRLCMAC, LOGL_DEBUG, "%s [DOWNLINK] START\n", tbf_name(dl_tbf));

	/* new TBF, so put first frame */
	dl_tbf->m_llc.put_frame(data, len);
	dl_tbf->bts->llc_frame_sched();

	/* Store IMSI for later look-up and PCH retransmission */
	dl_tbf->assign_imsi(imsi);

	/* trigger downlink assignment and set state to ASSIGN.
	 * we don't use old_downlink, so the possible uplink is used
	 * to trigger downlink assignment. if there is no uplink,
	 * AGCH is used. */
	dl_tbf->bts->trigger_dl_ass(dl_tbf, old_ul_tbf, imsi);
	return 0;
}

/**
 * TODO: split into unit test-able parts...
 */
int gprs_rlcmac_dl_tbf::handle(struct gprs_rlcmac_bts *bts,
		const uint32_t tlli, const char *imsi,
		const uint8_t ms_class, const uint16_t delay_csec,
		const uint8_t *data, const uint16_t len)
{
	struct gprs_rlcmac_dl_tbf *dl_tbf;

	/* check for existing TBF */
	dl_tbf = tbf_lookup_dl(bts->bts, tlli, imsi);
	if (dl_tbf) {
		int rc = dl_tbf->append_data(ms_class, delay_csec, data, len);
		if (rc >= 0)
			dl_tbf->assign_imsi(imsi);
		return rc;
	} 

	return tbf_new_dl_assignment(bts, imsi, tlli, ms_class, data, len);
}

struct msgb *gprs_rlcmac_dl_tbf::llc_dequeue(bssgp_bvc_ctx *bctx)
{
	struct msgb *msg;
	struct timeval *tv, tv_now;
	uint32_t octets = 0, frames = 0;

	gettimeofday(&tv_now, NULL);

	while ((msg = m_llc.dequeue())) {
		tv = (struct timeval *)msg->data;
		msgb_pull(msg, sizeof(*tv));
		msgb_pull(msg, sizeof(*tv));

		if (gprs_llc::is_frame_expired(&tv_now, tv)) {
			LOGP(DRLCMACDL, LOGL_NOTICE, "%s Discarding LLC PDU "
				"because lifetime limit reached. Queue size %zu\n",
				tbf_name(this), m_llc.m_queue_size);
			bts->llc_timedout_frame();
			frames++;
			octets += msg->len;
			msgb_free(msg);
			continue;
		}
		break;
	}

	if (frames) {
		if (frames > 0xff)
			frames = 0xff;
		if (octets > 0xffffff)
			octets = 0xffffff;
		bssgp_tx_llc_discarded(bctx, m_tlli, frames, octets);
	}

	return msg;
}

/*
 * Create DL data block
 * The messages are fragmented and forwarded as data blocks.
 */
struct msgb *gprs_rlcmac_dl_tbf::create_dl_acked_block(uint32_t fn, uint8_t ts)
{
	LOGP(DRLCMACDL, LOGL_DEBUG, "%s downlink (V(A)==%d .. "
		"V(S)==%d)\n", tbf_name(this),
		m_window.v_a(), m_window.v_s());

do_resend:
	/* check if there is a block with negative acknowledgement */
	int resend_bsn = m_window.resend_needed();
	if (resend_bsn >= 0) {
		LOGP(DRLCMACDL, LOGL_DEBUG, "- Resending BSN %d\n", resend_bsn);
		/* re-send block with negative aknowlegement */
		m_window.m_v_b.mark_unacked(resend_bsn);
		bts->rlc_resent();
		return create_dl_acked_block(fn, ts, resend_bsn, false);
	}

	/* if the window has stalled, or transfer is complete,
	 * send an unacknowledged block */
	if (state_is(GPRS_RLCMAC_FINISHED) || dl_window_stalled()) {
		if (state_is(GPRS_RLCMAC_FINISHED)) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- Restarting at BSN %d, "
				"because all blocks have been transmitted.\n",
					m_window.v_a());
			bts->rlc_restarted();
		} else {
			LOGP(DRLCMACDL, LOGL_NOTICE, "- Restarting at BSN %d, "
				"because all window is stalled.\n",
					m_window.v_a());
			bts->rlc_stalled();
		}
		/* If V(S) == V(A) and finished state, we would have received
		 * acknowledgement of all transmitted block. In this case we
		 * would have transmitted the final block, and received ack
		 * from MS. But in this case we did not receive the final ack
		 * indication from MS. This should never happen if MS works
		 * correctly. */
		if (m_window.window_empty()) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- MS acked all blocks, "
				"so we re-transmit final block!\n");
			/* we just send final block again */
			int16_t index = m_window.v_s_mod(-1);
			bts->rlc_resent();
			return create_dl_acked_block(fn, ts, index, false);
		}
		
		/* cycle through all unacked blocks */
		int resend = m_window.mark_for_resend();

		/* At this point there should be at least one unacked block
		 * to be resent. If not, this is an software error. */
		if (resend == 0) {
			LOGP(DRLCMACDL, LOGL_ERROR, "Software error: "
				"There are no unacknowledged blocks, but V(A) "
				" != V(S). PLEASE FIX!\n");
			/* we just send final block again */
			int16_t index = m_window.v_s_mod(-1);
			return create_dl_acked_block(fn, ts, index, false);
		}
		goto do_resend;
	}

	return create_new_bsn(fn, ts);
}

struct msgb *gprs_rlcmac_dl_tbf::create_new_bsn(const uint32_t fn, const uint8_t ts)
{
	struct rlc_dl_header *rh;
	struct rlc_li_field *li;
	struct msgb *msg;
	uint8_t *delimiter, *data, *e_pointer;
	uint16_t space, chunk;
	gprs_rlc_data *rlc_data;
	bool first_fin_ack = false;
	const uint16_t bsn = m_window.v_s();

	LOGP(DRLCMACDL, LOGL_DEBUG, "- Sending new block at BSN %d\n",
		m_window.v_s());

#warning "Selection of the CS doesn't belong here"
	if (cs == 0) {
		cs = bts_data()->initial_cs_dl;
		if (cs < 1 || cs > 4)
			cs = 1;
	}
	/* total length of block, including spare bits */
	const uint8_t block_length = gprs_rlcmac_cs[cs].block_length;
	/* length of usable data of block, w/o spare bits, inc. MAC */
	const uint8_t block_data_len = gprs_rlcmac_cs[cs].block_data;

	/* now we still have untransmitted LLC data, so we fill mac block */
	rlc_data = m_rlc.block(bsn);
	data = rlc_data->prepare(block_data_len);

	rh = (struct rlc_dl_header *)data;
	rh->pt = 0; /* Data Block */
	rh->rrbp = rh->s_p = 0; /* Polling, set later, if required */
	rh->usf = 7; /* will be set at scheduler */
	rh->pr = 0; /* FIXME: power reduction */
	rh->tfi = m_tfi; /* TFI */
	rh->fbi = 0; /* Final Block Indicator, set late, if true */
	rh->bsn = bsn; /* Block Sequence Number */
	rh->e = 0; /* Extension bit, maybe set later */
	e_pointer = data + 2; /* points to E of current chunk */
	data += sizeof(*rh);
	delimiter = data; /* where next length header would be stored */
	space = block_data_len - sizeof(*rh);
	while (1) {
		chunk = m_llc.chunk_size();
		/* if chunk will exceed block limit */
		if (chunk > space) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d "
				"larger than space (%d) left in block: copy "
				"only remaining space, and we are done\n",
				chunk, space);
			/* block is filled, so there is no extension */
			*e_pointer |= 0x01;
			/* fill only space */
			m_llc.consume(data, space);
			/* return data block as message */
			break;
		}
		/* if FINAL chunk would fit precisely in space left */
		if (chunk == space && llist_empty(&m_llc.queue)) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d "
				"would exactly fit into space (%d): because "
				"this is a final block, we don't add length "
				"header, and we are done\n", chunk, space);
			LOGP(DRLCMACDL, LOGL_INFO, "Complete DL frame for "
				"%s that fits precisely in last block: "
				"len=%d\n", tbf_name(this), m_llc.frame_length());
			gprs_rlcmac_dl_bw(this, m_llc.frame_length());
			/* block is filled, so there is no extension */
			*e_pointer |= 0x01;
			/* fill space */
			m_llc.consume(data, space);
			m_llc.reset();
			/* final block */
			rh->fbi = 1; /* we indicate final block */
			set_state(GPRS_RLCMAC_FINISHED);
			/* return data block as message */
			break;
		}
		/* if chunk would fit exactly in space left */
		if (chunk == space) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d "
				"would exactly fit into space (%d): add length "
				"header with LI=0, to make frame extend to "
				"next block, and we are done\n", chunk, space);
			/* make space for delimiter */
			if (delimiter != data)
				memmove(delimiter + 1, delimiter,
					data - delimiter);
			data++;
			space--;
			/* add LI with 0 length */
			li = (struct rlc_li_field *)delimiter;
			li->e = 1; /* not more extension */
			li->m = 0; /* shall be set to 0, in case of li = 0 */
			li->li = 0; /* chunk fills the complete space */
			// no need to set e_pointer nor increase delimiter
			/* fill only space, which is 1 octet less than chunk */
			m_llc.consume(data, space);
			/* return data block as message */
			break;
		}
		LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d is less "
			"than remaining space (%d): add length header to "
			"to delimit LLC frame\n", chunk, space);
		/* the LLC frame chunk ends in this block */
		/* make space for delimiter */
		if (delimiter != data)
			memmove(delimiter + 1, delimiter, data - delimiter);
		data++;
		space--;
		/* add LI to delimit frame */
		li = (struct rlc_li_field *)delimiter;
		li->e = 0; /* Extension bit, maybe set later */
		li->m = 0; /* will be set later, if there is more LLC data */
		li->li = chunk; /* length of chunk */
		e_pointer = delimiter; /* points to E of current delimiter */
		delimiter++;
		/* copy (rest of) LLC frame to space and reset later */
		m_llc.consume(data, chunk);
		data += chunk;
		space -= chunk;
		LOGP(DRLCMACDL, LOGL_INFO, "Complete DL frame for %s"
			"len=%d\n", tbf_name(this), m_llc.frame_length());
		gprs_rlcmac_dl_bw(this, m_llc.frame_length());
		m_llc.reset();
		/* dequeue next LLC frame, if any */
		msg = llc_dequeue(gprs_bssgp_pcu_current_bctx());
		if (msg) {
			LOGP(DRLCMACDL, LOGL_INFO, "- Dequeue next LLC for "
				"%s (len=%d)\n", tbf_name(this), msg->len);
			m_llc.put_frame(msg->data, msg->len);
			bts->llc_frame_sched();
			msgb_free(msg);
		}
		/* if we have more data and we have space left */
		if (space > 0 && m_llc.frame_length()) {
			li->m = 1; /* we indicate more frames to follow */
			continue;
		}
		/* if we don't have more LLC frames */
		if (!m_llc.frame_length()) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Final block, so we "
				"done.\n");
			li->e = 1; /* we cannot extend */
			rh->fbi = 1; /* we indicate final block */
			first_fin_ack = true;
				/* + 1 indicates: first final ack */
			set_state(GPRS_RLCMAC_FINISHED);
			break;
		}
		/* we have no space left */
		LOGP(DRLCMACDL, LOGL_DEBUG, "-- No space left, so we are "
			"done.\n");
		li->e = 1; /* we cannot extend */
		break;
	}
	LOGP(DRLCMACDL, LOGL_DEBUG, "data block: %s\n",
		osmo_hexdump(rlc_data->block, block_length));
#warning "move this up?"
	rlc_data->len = block_length;
	/* raise send state and set ack state array */
	m_window.m_v_b.mark_unacked(bsn);
	m_window.increment_send();

	return create_dl_acked_block(fn, ts, bsn, first_fin_ack);
}

struct msgb *gprs_rlcmac_dl_tbf::create_dl_acked_block(
				const uint32_t fn, const uint8_t ts,
				const int index, const bool first_fin_ack)
{
	uint8_t *data;
	struct rlc_dl_header *rh;
	struct msgb *dl_msg;
	uint8_t len;

	/* get data and header from current block */
	data = m_rlc.block(index)->block;
	len = m_rlc.block(index)->len;
	rh = (struct rlc_dl_header *)data;

	/* Clear Polling, if still set in history buffer */
	rh->s_p = 0;
		
	/* poll after POLL_ACK_AFTER_FRAMES frames, or when final block is tx.
	 */
	if (m_tx_counter >= POLL_ACK_AFTER_FRAMES || first_fin_ack) {
		if (first_fin_ack) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- Scheduling Ack/Nack "
				"polling, because first final block sent.\n");
		} else {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- Scheduling Ack/Nack "
				"polling, because %d blocks sent.\n",
				POLL_ACK_AFTER_FRAMES);
		}
		/* scheduling not possible, because: */
		if (poll_state != GPRS_RLCMAC_POLL_NONE)
			LOGP(DRLCMACDL, LOGL_DEBUG, "Polling is already "
				"sheduled for %s, so we must wait for "
				"requesting downlink ack\n", tbf_name(this));
		else if (control_ts != ts)
			LOGP(DRLCMACDL, LOGL_DEBUG, "Polling cannot be "
				"sheduled in this TS %d, waiting for "
				"TS %d\n", ts, control_ts);
#warning "What happens to the first_fin_ack in case something is already scheduled?"
		else if (bts->sba()->find(trx->trx_no, ts, (fn + 13) % 2715648))
			LOGP(DRLCMACDL, LOGL_DEBUG, "Polling cannot be "
				"sheduled, because single block alllocation "
				"already exists\n");
		else  {
			LOGP(DRLCMACDL, LOGL_DEBUG, "Polling sheduled in this "
				"TS %d\n", ts);
			m_tx_counter = 0;
			/* start timer whenever we send the final block */
			if (rh->fbi == 1)
				tbf_timer_start(this, 3191, bts_data()->t3191, 0);

			/* schedule polling */
			poll_state = GPRS_RLCMAC_POLL_SCHED;
			poll_fn = (fn + 13) % 2715648;

			/* set polling in header */
			rh->rrbp = 0; /* N+13 */
			rh->s_p = 1; /* Polling */
		}
	}

	/* return data block as message */
	dl_msg = msgb_alloc(len, "rlcmac_dl_data");
	if (!dl_msg)
		return NULL;

	/* Increment TX-counter */
	m_tx_counter++;

	memcpy(msgb_put(dl_msg, len), data, len);
	bts->rlc_sent();

	return dl_msg;
}

int gprs_rlcmac_dl_tbf::update_window(const uint8_t ssn, const uint8_t *rbb)
{
	int16_t dist; /* must be signed */
	uint16_t lost = 0, received = 0;
	char show_rbb[65];
	char show_v_b[RLC_MAX_SNS + 1];
	const uint16_t mod_sns = m_window.mod_sns();

	Decoding::extract_rbb(rbb, show_rbb);
	/* show received array in debug (bit 64..1) */
	LOGP(DRLCMACDL, LOGL_DEBUG, "- ack:  (BSN=%d)\"%s\""
		"(BSN=%d)  R=ACK I=NACK\n", (ssn - 64) & mod_sns,
		show_rbb, (ssn - 1) & mod_sns);

	/* apply received array to receive state (SSN-64..SSN-1) */
	/* calculate distance of ssn from V(S) */
	dist = (m_window.v_s() - ssn) & mod_sns;
	/* check if distance is less than distance V(A)..V(S) */
	if (dist >= m_window.distance()) {
		/* this might happpen, if the downlink assignment
		 * was not received by ms and the ack refers
		 * to previous TBF
		 * FIXME: we should implement polling for
		 * control ack!*/
		LOGP(DRLCMACDL, LOGL_NOTICE, "- ack range is out of "
			"V(A)..V(S) range %s Free TBF!\n", tbf_name(this));
		return 1; /* indicate to free TBF */
	}

	m_window.update(bts, show_rbb, ssn,
			&lost, &received);

	/* report lost and received packets */
	gprs_rlcmac_received_lost(this, received, lost);

	/* raise V(A), if possible */
	m_window.raise(m_window.move_window());

	/* show receive state array in debug (V(A)..V(S)-1) */
	m_window.show_state(show_v_b);
	LOGP(DRLCMACDL, LOGL_DEBUG, "- V(B): (V(A)=%d)\"%s\""
		"(V(S)-1=%d)  A=Acked N=Nacked U=Unacked "
		"X=Resend-Unacked I=Invalid\n",
		m_window.v_a(), show_v_b,
		m_window.v_s_mod(-1));

	if (state_is(GPRS_RLCMAC_FINISHED) && m_window.window_empty()) {
		LOGP(DRLCMACDL, LOGL_NOTICE, "Received acknowledge of "
			"all blocks, but without final ack "
			"inidcation (don't worry)\n");
	}
	return 0;
}


int gprs_rlcmac_dl_tbf::maybe_start_new_window()
{
	struct msgb *msg;
	uint16_t received;

	LOGP(DRLCMACDL, LOGL_DEBUG, "- Final ACK received.\n");
	/* range V(A)..V(S)-1 */
	received = m_window.count_unacked();

	/* report all outstanding packets as received */
	gprs_rlcmac_received_lost(this, received, 0);

	set_state(GPRS_RLCMAC_WAIT_RELEASE);

	/* check for LLC PDU in the LLC Queue */
	msg = llc_dequeue(gprs_bssgp_pcu_current_bctx());
	if (!msg) {
		/* no message, start T3193, change state to RELEASE */
		LOGP(DRLCMACDL, LOGL_DEBUG, "- No new message, so we release.\n");
		/* start T3193 */
		tbf_timer_start(this, 3193,
			bts_data()->t3193_msec / 1000,
			(bts_data()->t3193_msec % 1000) * 1000);

		return 0;
	}

	/* we have more data so we will re-use this tbf */
	reuse_tbf(msg->data, msg->len);
	msgb_free(msg);
	return 0;
}

int gprs_rlcmac_dl_tbf::rcvd_dl_ack(uint8_t final_ack, uint8_t ssn, uint8_t *rbb)
{
	LOGP(DRLCMACDL, LOGL_DEBUG, "%s downlink acknowledge\n", tbf_name(this));

	if (!final_ack)
		return update_window(ssn, rbb);
	return maybe_start_new_window();
}

void gprs_rlcmac_dl_tbf::reuse_tbf(const uint8_t *data, const uint16_t len)
{
	uint8_t trx;
	struct gprs_rlcmac_dl_tbf *new_tbf;
	int8_t tfi; /* must be signed */
	int rc;
	struct msgb *msg;

	bts->tbf_reused();

	tfi = bts->tfi_find_free(GPRS_RLCMAC_DL_TBF, &trx, this->trx->trx_no);
	if (tfi < 0) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return;
	}
	new_tbf = tbf_alloc_dl_tbf(bts->bts_data(), NULL, tfi, trx, ms_class, 0);
	if (!new_tbf) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return;
	}

	new_tbf->m_tlli = m_tlli;
	new_tbf->m_tlli_valid = m_tlli_valid;
	new_tbf->ta = ta;

	/* Copy over all data to the new TBF */
	new_tbf->m_llc.put_frame(data, len);
	bts->llc_frame_sched();

	while ((msg = m_llc.dequeue()))
		new_tbf->m_llc.enqueue(msg);

	/* reset rlc states */
	m_tx_counter = 0;
	m_wait_confirm = 0;
	m_window.reset();

	/* keep to flags */
	state_flags &= GPRS_RLCMAC_FLAG_TO_MASK;
	state_flags &= ~(1 << GPRS_RLCMAC_FLAG_CCCH);

	update();

	LOGP(DRLCMAC, LOGL_DEBUG, "%s Trigger dowlink assignment on PACCH, "
		"because another LLC PDU has arrived in between\n",
		tbf_name(this));
	bts->trigger_dl_ass(new_tbf, this, NULL);
}

bool gprs_rlcmac_dl_tbf::dl_window_stalled() const
{
	return m_window.window_stalled();
}


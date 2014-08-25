/* Copied from gprs_bssgp_pcu.cpp
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
#include <encoding.h>
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

extern void *tall_pcu_ctx;

static void tbf_timer_cb(void *_tbf);

gprs_rlcmac_bts *gprs_rlcmac_tbf::bts_data() const
{
	return bts->bts_data();
}

void gprs_rlcmac_tbf::assign_imsi(const char *imsi)
{
	strncpy(m_imsi, imsi, sizeof(m_imsi));
	m_imsi[sizeof(m_imsi) - 1] = '\0';
}

gprs_rlcmac_ul_tbf *tbf_alloc_ul(struct gprs_rlcmac_bts *bts,
	int8_t use_trx, uint8_t ms_class,
	uint32_t tlli, uint8_t ta, struct gprs_rlcmac_tbf *dl_tbf)
{
	uint8_t trx;
	struct gprs_rlcmac_ul_tbf *tbf;
	int8_t tfi; /* must be signed */

#warning "Copy and paste with tbf_new_dl_assignment"
	/* create new TBF, use same TRX as DL TBF */
	tfi = bts->bts->tfi_find_free(GPRS_RLCMAC_UL_TBF, &trx, use_trx);
	if (tfi < 0) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return NULL;
	}
	/* use multislot class of downlink TBF */
	tbf = tbf_alloc_ul_tbf(bts, dl_tbf, tfi, trx, ms_class, 0);
	if (!tbf) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return NULL;
	}
	tbf->m_tlli = tlli;
	tbf->m_tlli_valid = 1; /* no contention resolution */
	tbf->m_contention_resolution_done = 1;
	tbf->ta = ta; /* use current TA */
	tbf->set_state(GPRS_RLCMAC_ASSIGN);
	tbf->state_flags |= (1 << GPRS_RLCMAC_FLAG_PACCH);
	tbf_timer_start(tbf, 3169, bts->t3169, 0);

	return tbf;
}

static void tbf_unlink_pdch(struct gprs_rlcmac_tbf *tbf)
{
	int ts;

	if (tbf->direction == GPRS_RLCMAC_UL_TBF) {
		tbf->trx->ul_tbf[tbf->tfi()] = NULL;
		for (ts = 0; ts < 8; ts++)
			tbf->pdch[ts] = NULL;
	} else {
		tbf->trx->dl_tbf[tbf->tfi()] = NULL;
		for (ts = 0; ts < 8; ts++)
			tbf->pdch[ts] = NULL;
	}
}

void tbf_free(struct gprs_rlcmac_tbf *tbf)
{
	/* Give final measurement report */
	gprs_rlcmac_rssi_rep(tbf);
	if (tbf->direction == GPRS_RLCMAC_DL_TBF) {
		gprs_rlcmac_dl_tbf *dl_tbf = static_cast<gprs_rlcmac_dl_tbf *>(tbf);
		gprs_rlcmac_lost_rep(dl_tbf);
	}

	LOGP(DRLCMAC, LOGL_INFO, "%s free\n", tbf_name(tbf));
	if (tbf->ul_ass_state != GPRS_RLCMAC_UL_ASS_NONE)
		LOGP(DRLCMAC, LOGL_ERROR, "%s Software error: Pending uplink "
			"assignment. This may not happen, because the "
			"assignment message never gets transmitted. Please "
			"be sure not to free in this state. PLEASE FIX!\n",
			tbf_name(tbf));
	if (tbf->dl_ass_state != GPRS_RLCMAC_DL_ASS_NONE)
		LOGP(DRLCMAC, LOGL_ERROR, "%s Software error: Pending downlink "
			"assignment. This may not happen, because the "
			"assignment message never gets transmitted. Please "
			"be sure not to free in this state. PLEASE FIX!\n",
			tbf_name(tbf));
	tbf->stop_timer();
	#warning "TODO: Could/Should generate  bssgp_tx_llc_discarded"
	tbf->m_llc.clear(tbf->bts);
	tbf_unlink_pdch(tbf);
	llist_del(&tbf->list.list);

	if (tbf->direction == GPRS_RLCMAC_UL_TBF)
		tbf->bts->tbf_ul_freed();
	else
		tbf->bts->tbf_dl_freed();

	LOGP(DRLCMAC, LOGL_DEBUG, "********** TBF ends here **********\n");
	talloc_free(tbf);
}

int gprs_rlcmac_tbf::update()
{
	struct gprs_rlcmac_tbf *ul_tbf = NULL;
	struct gprs_rlcmac_bts *bts_data = bts->bts_data();
	int rc;

	LOGP(DRLCMAC, LOGL_DEBUG, "********** TBF update **********\n");

	if (direction != GPRS_RLCMAC_DL_TBF)
		return -EINVAL;

	ul_tbf = bts->ul_tbf_by_tlli(m_tlli);

	tbf_unlink_pdch(this);
	rc = bts_data->alloc_algorithm(bts_data, ul_tbf, this, bts_data->alloc_algorithm_curst, 0);
	/* if no resource */
	if (rc < 0) {
		LOGP(DRLCMAC, LOGL_ERROR, "No resource after update???\n");
		return -rc;
	}

	return 0;
}

int tbf_assign_control_ts(struct gprs_rlcmac_tbf *tbf)
{
	if (tbf->control_ts == 0xff)
		LOGP(DRLCMAC, LOGL_INFO, "- Setting Control TS %d\n",
			tbf->first_common_ts);
	else if (tbf->control_ts != tbf->first_common_ts)
		LOGP(DRLCMAC, LOGL_INFO, "- Changing Control TS %d\n",
			tbf->first_common_ts);
	tbf->control_ts = tbf->first_common_ts;

	return 0;
}

const char *gprs_rlcmac_tbf::tbf_state_name[] = {
	"NULL",
	"ASSIGN",
	"FLOW",
	"FINISHED",
	"WAIT RELEASE",
	"RELEASING",
};

void tbf_timer_start(struct gprs_rlcmac_tbf *tbf, unsigned int T,
			unsigned int seconds, unsigned int microseconds)
{
	if (!osmo_timer_pending(&tbf->timer))
		LOGP(DRLCMAC, LOGL_DEBUG, "%s starting timer %u.\n",
			tbf_name(tbf), T);
	else
		LOGP(DRLCMAC, LOGL_DEBUG, "%s restarting timer %u "
			"while old timer %u pending \n",
			tbf_name(tbf), T, tbf->T);

	tbf->T = T;
	tbf->num_T_exp = 0;

	/* Tunning timers can be safely re-scheduled. */
	tbf->timer.data = tbf;
	tbf->timer.cb = &tbf_timer_cb;

	osmo_timer_schedule(&tbf->timer, seconds, microseconds);
}

void gprs_rlcmac_tbf::stop_t3191()
{
	return stop_timer();
}

void gprs_rlcmac_tbf::stop_timer()
{
	if (osmo_timer_pending(&timer)) {
		LOGP(DRLCMAC, LOGL_DEBUG, "%s stopping timer %u.\n",
			tbf_name(this), T);
		osmo_timer_del(&timer);
	}
}

void gprs_rlcmac_tbf::poll_timeout()
{
	LOGP(DRLCMAC, LOGL_NOTICE, "%s poll timeout\n",
		tbf_name(this));

	poll_state = GPRS_RLCMAC_POLL_NONE;

	if (ul_ack_state == GPRS_RLCMAC_UL_ACK_WAIT_ACK) {
		if (!(state_flags & (1 << GPRS_RLCMAC_FLAG_TO_UL_ACK))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET CONTROL ACK for PACKET UPLINK ACK\n");
			rlcmac_diag();
			state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_UL_ACK);
		}
		ul_ack_state = GPRS_RLCMAC_UL_ACK_NONE;
		if (state_is(GPRS_RLCMAC_FINISHED)) {
			gprs_rlcmac_ul_tbf *ul_tbf = static_cast<gprs_rlcmac_ul_tbf *>(this);
			ul_tbf->m_n3103++;
			if (ul_tbf->m_n3103 == ul_tbf->bts->bts_data()->n3103) {
				LOGP(DRLCMAC, LOGL_NOTICE,
					"- N3103 exceeded\n");
				ul_tbf->set_state(GPRS_RLCMAC_RELEASING);
				tbf_timer_start(ul_tbf, 3169, ul_tbf->bts->bts_data()->t3169, 0);
				return;
			}
			/* reschedule UL ack */
			ul_tbf->ul_ack_state = GPRS_RLCMAC_UL_ACK_SEND_ACK;
		}
	} else if (ul_ass_state == GPRS_RLCMAC_UL_ASS_WAIT_ACK) {
		if (!(state_flags & (1 << GPRS_RLCMAC_FLAG_TO_UL_ASS))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET CONTROL ACK for PACKET UPLINK "
				"ASSIGNMENT.\n");
			rlcmac_diag();
			state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_UL_ASS);
		}
		ul_ass_state = GPRS_RLCMAC_UL_ASS_NONE;
		n3105++;
		if (n3105 == bts_data()->n3105) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- N3105 exceeded\n");
			set_state(GPRS_RLCMAC_RELEASING);
			tbf_timer_start(this, 3195, bts_data()->t3195, 0);
			return;
		}
		/* reschedule UL assignment */
		ul_ass_state = GPRS_RLCMAC_UL_ASS_SEND_ASS;
	} else if (dl_ass_state == GPRS_RLCMAC_DL_ASS_WAIT_ACK) {
		if (!(state_flags & (1 << GPRS_RLCMAC_FLAG_TO_DL_ASS))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET CONTROL ACK for PACKET DOWNLINK "
				"ASSIGNMENT.\n");
			rlcmac_diag();
			state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_DL_ASS);
		}
		dl_ass_state = GPRS_RLCMAC_DL_ASS_NONE;
		n3105++;
		if (n3105 == bts->bts_data()->n3105) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- N3105 exceeded\n");
			set_state(GPRS_RLCMAC_RELEASING);
			tbf_timer_start(this, 3195, bts_data()->t3195, 0);
			return;
		}
		/* reschedule DL assignment */
		dl_ass_state = GPRS_RLCMAC_DL_ASS_SEND_ASS;
	} else if (direction == GPRS_RLCMAC_DL_TBF) {
		gprs_rlcmac_dl_tbf *dl_tbf = static_cast<gprs_rlcmac_dl_tbf *>(this);

		if (!(dl_tbf->state_flags & (1 << GPRS_RLCMAC_FLAG_TO_DL_ACK))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET DOWNLINK ACK.\n");
			dl_tbf->rlcmac_diag();
			dl_tbf->state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_DL_ACK);
		}
		dl_tbf->n3105++;
		if (dl_tbf->n3105 == dl_tbf->bts->bts_data()->n3105) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- N3105 exceeded\n");
			dl_tbf->set_state(GPRS_RLCMAC_RELEASING);
			tbf_timer_start(dl_tbf, 3195, dl_tbf->bts_data()->t3195, 0);
			return;
		}
		/* resend IMM.ASS on CCCH on timeout */
		if ((dl_tbf->state_flags & (1 << GPRS_RLCMAC_FLAG_CCCH))
		 && !(dl_tbf->state_flags & (1 << GPRS_RLCMAC_FLAG_DL_ACK))) {
			LOGP(DRLCMAC, LOGL_DEBUG, "Re-send dowlink assignment "
				"for %s on PCH (IMSI=%s)\n",
				tbf_name(dl_tbf),
				m_imsi);
			/* send immediate assignment */
			dl_tbf->bts->snd_dl_ass(dl_tbf, 0, m_imsi);
			dl_tbf->m_wait_confirm = 1;
		}
	} else
		LOGP(DRLCMAC, LOGL_ERROR, "- Poll Timeout, but no event!\n");
}

static int setup_tbf(struct gprs_rlcmac_tbf *tbf, struct gprs_rlcmac_bts *bts,
	struct gprs_rlcmac_tbf *old_tbf, uint8_t tfi, uint8_t trx,
	uint8_t ms_class, uint8_t single_slot)
{
	int rc;

	if (trx >= 8 || tfi >= 32)
		return -1;

	if (!tbf)
		return -1;

	/* Back pointer for PODS llist compatibility */
	tbf->list.back = tbf;

	tbf->m_created_ts = time(NULL);
	tbf->bts = bts->bts;
	tbf->m_tfi = tfi;
	tbf->trx = &bts->trx[trx];
	tbf->ms_class = ms_class;
	/* select algorithm */
	rc = bts->alloc_algorithm(bts, old_tbf, tbf, bts->alloc_algorithm_curst,
		single_slot);
	/* if no resource */
	if (rc < 0) {
		return -1;
	}
	/* assign control ts */
	tbf->control_ts = 0xff;
	rc = tbf_assign_control_ts(tbf);
	/* if no resource */
	if (rc < 0) {
		return -1;
	}

	/* set timestamp */
	gettimeofday(&tbf->meas.rssi_tv, NULL);

	tbf->m_llc.init();
	return 0;
}


struct gprs_rlcmac_ul_tbf *tbf_alloc_ul_tbf(struct gprs_rlcmac_bts *bts,
	struct gprs_rlcmac_tbf *old_tbf, uint8_t tfi, uint8_t trx,
	uint8_t ms_class, uint8_t single_slot)
{
	struct gprs_rlcmac_ul_tbf *tbf;
	int rc;

	LOGP(DRLCMAC, LOGL_DEBUG, "********** TBF starts here **********\n");
	LOGP(DRLCMAC, LOGL_INFO, "Allocating %s TBF: TFI=%d TRX=%d "
		"MS_CLASS=%d\n", "UL", tfi, trx, ms_class);

	if (trx >= 8 || tfi >= 32)
		return NULL;

	tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_ul_tbf);

	if (!tbf)
		return NULL;

	tbf->direction = GPRS_RLCMAC_UL_TBF;
	rc = setup_tbf(tbf, bts, old_tbf, tfi, trx, ms_class, single_slot);
	/* if no resource */
	if (rc < 0) {
		talloc_free(tbf);
		return NULL;
	}

	llist_add(&tbf->list.list, &bts->ul_tbfs);
	tbf->bts->tbf_ul_created();

	return tbf;
}

struct gprs_rlcmac_dl_tbf *tbf_alloc_dl_tbf(struct gprs_rlcmac_bts *bts,
	struct gprs_rlcmac_tbf *old_tbf, uint8_t tfi, uint8_t trx,
	uint8_t ms_class, uint8_t single_slot)
{
	struct gprs_rlcmac_dl_tbf *tbf;
	int rc;

	LOGP(DRLCMAC, LOGL_DEBUG, "********** TBF starts here **********\n");
	LOGP(DRLCMAC, LOGL_INFO, "Allocating %s TBF: TFI=%d TRX=%d "
		"MS_CLASS=%d\n", "DL", tfi, trx, ms_class);

	if (trx >= 8 || tfi >= 32)
		return NULL;

	tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_dl_tbf);

	if (!tbf)
		return NULL;

	tbf->direction = GPRS_RLCMAC_DL_TBF;
	rc = setup_tbf(tbf, bts, old_tbf, tfi, trx, ms_class, single_slot);
	/* if no resource */
	if (rc < 0) {
		talloc_free(tbf);
		return NULL;
	}

	llist_add(&tbf->list.list, &bts->dl_tbfs);
	tbf->bts->tbf_dl_created();

	gettimeofday(&tbf->m_bw.dl_bw_tv, NULL);
	gettimeofday(&tbf->m_bw.dl_loss_tv, NULL);

	return tbf;
}

static void tbf_timer_cb(void *_tbf)
{
	struct gprs_rlcmac_tbf *tbf = (struct gprs_rlcmac_tbf *)_tbf;
	tbf->handle_timeout();
}

void gprs_rlcmac_tbf::handle_timeout()
{
	LOGP(DRLCMAC, LOGL_DEBUG, "%s timer %u expired.\n",
		tbf_name(this), T);

	num_T_exp++;

	switch (T) {
	case 0: /* assignment */
		if ((state_flags & (1 << GPRS_RLCMAC_FLAG_PACCH))) {
			if (state_is(GPRS_RLCMAC_ASSIGN)) {
				LOGP(DRLCMAC, LOGL_NOTICE, "%s releasing due to "
					"PACCH assignment timeout.\n", tbf_name(this));
				tbf_free(this);
				return;
			} else
				LOGP(DRLCMAC, LOGL_ERROR, "Error: %s is not "
					"in assign state\n", tbf_name(this));
		}
		if ((state_flags & (1 << GPRS_RLCMAC_FLAG_CCCH))) {
			gprs_rlcmac_dl_tbf *dl_tbf = static_cast<gprs_rlcmac_dl_tbf *>(this);
			dl_tbf->m_wait_confirm = 0;
			if (dl_tbf->state_is(GPRS_RLCMAC_ASSIGN)) {
				tbf_assign_control_ts(dl_tbf);

				if (!dl_tbf->upgrade_to_multislot) {
					/* change state to FLOW, so scheduler
					 * will start transmission */
					dl_tbf->set_state(GPRS_RLCMAC_FLOW);
					break;
				}

				/* This tbf can be upgraded to use multiple DL
				 * timeslots and now that there is already one
				 * slot assigned send another DL assignment via
				 * PDCH. */

				/* keep to flags */
				dl_tbf->state_flags &= GPRS_RLCMAC_FLAG_TO_MASK;
				dl_tbf->state_flags &= ~(1 << GPRS_RLCMAC_FLAG_CCCH);

				dl_tbf->update();

				dl_tbf->bts->trigger_dl_ass(dl_tbf, dl_tbf, NULL);
			} else
				LOGP(DRLCMAC, LOGL_NOTICE, "%s Continue flow after "
					"IMM.ASS confirm\n", tbf_name(dl_tbf));
		}
		break;
	case 3169:
	case 3191:
	case 3195:
		LOGP(DRLCMAC, LOGL_NOTICE, "%s T%d timeout during "
			"transsmission\n", tbf_name(this), T);
		rlcmac_diag();
		/* fall through */
	case 3193:
		LOGP(DRLCMAC, LOGL_DEBUG,
			"%s will be freed due to timeout\n", tbf_name(this));
		/* free TBF */
		tbf_free(this);
		return;
		break;
	default:
		LOGP(DRLCMAC, LOGL_ERROR,
			"%s timer expired in unknown mode: %u\n", tbf_name(this), T);
	}
}

int gprs_rlcmac_tbf::rlcmac_diag()
{
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_CCCH)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Assignment was on CCCH\n");
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_PACCH)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Assignment was on PACCH\n");
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_UL_DATA)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Uplink data was received\n");
	else if (direction == GPRS_RLCMAC_UL_TBF)
		LOGP(DRLCMAC, LOGL_NOTICE, "- No uplink data received yet\n");
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_DL_ACK)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Downlink ACK was received\n");
	else if (direction == GPRS_RLCMAC_DL_TBF)
		LOGP(DRLCMAC, LOGL_NOTICE, "- No downlink ACK received yet\n");

	return 0;
}

struct msgb *gprs_rlcmac_tbf::create_dl_ass(uint32_t fn)
{
	struct msgb *msg;
	struct gprs_rlcmac_dl_tbf *new_dl_tbf;
	int poll_ass_dl = 1;

	if (direction == GPRS_RLCMAC_DL_TBF && control_ts != first_common_ts) {
		LOGP(DRLCMAC, LOGL_NOTICE, "Cannot poll for downlink "
			"assigment, because MS cannot reply. (control TS=%d, "
			"first common TS=%d)\n", control_ts,
			first_common_ts);
		poll_ass_dl = 0;
	}
	if (poll_ass_dl) {
		if (poll_state != GPRS_RLCMAC_POLL_NONE) {
			LOGP(DRLCMAC, LOGL_DEBUG, "Polling is already sheduled "
				"for %s, so we must wait for downlink "
				"assignment...\n", tbf_name(this));
				return NULL;
		}
		if (bts->sba()->find(trx->trx_no, control_ts, (fn + 13) % 2715648)) {
			LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already "
				"scheduled for single block allocation...\n");
			return NULL;
		}
	}

	/* on uplink TBF we get the downlink TBF to be assigned. */
	if (direction == GPRS_RLCMAC_UL_TBF) {
		gprs_rlcmac_ul_tbf *ul_tbf = static_cast<gprs_rlcmac_ul_tbf *>(this);

		/* be sure to check first, if contention resolution is done,
		 * otherwise we cannot send the assignment yet */
		if (!ul_tbf->m_contention_resolution_done) {
			LOGP(DRLCMAC, LOGL_DEBUG, "Cannot assign DL TBF now, "
				"because contention resolution is not "
				"finished.\n");
			return NULL;
		}
	}

	new_dl_tbf = static_cast<gprs_rlcmac_dl_tbf *>(m_new_tbf);
	new_dl_tbf->was_releasing = was_releasing;
	if (!new_dl_tbf) {
		LOGP(DRLCMACDL, LOGL_ERROR, "We have a schedule for downlink "
			"assignment at uplink %s, but there is no downlink "
			"TBF\n", tbf_name(this));
		dl_ass_state = GPRS_RLCMAC_DL_ASS_NONE;
		return NULL;
	}

	msg = msgb_alloc(23, "rlcmac_dl_ass");
	if (!msg)
		return NULL;
	bitvec *ass_vec = bitvec_alloc(23);
	if (!ass_vec) {
		msgb_free(msg);
		return NULL;
	}
	bitvec_unhex(ass_vec,
		"2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b");
	LOGP(DRLCMAC, LOGL_INFO, "%s  start Packet Downlink Assignment (PACCH)\n", tbf_name(new_dl_tbf));
	RlcMacDownlink_t * mac_control_block = (RlcMacDownlink_t *)talloc_zero(tall_pcu_ctx, RlcMacDownlink_t);
	Encoding::write_packet_downlink_assignment(mac_control_block, m_tfi,
		(direction == GPRS_RLCMAC_DL_TBF), new_dl_tbf,
		poll_ass_dl, bts_data()->alpha, bts_data()->gamma, -1, 0);
	LOGP(DRLCMAC, LOGL_DEBUG, "+++++++++++++++++++++++++ TX : Packet Downlink Assignment +++++++++++++++++++++++++\n");
	encode_gsm_rlcmac_downlink(ass_vec, mac_control_block);
	LOGPC(DCSN1, LOGL_NOTICE, "\n");
	LOGP(DRLCMAC, LOGL_DEBUG, "------------------------- TX : Packet Downlink Assignment -------------------------\n");
	bitvec_pack(ass_vec, msgb_put(msg, 23));
	bitvec_free(ass_vec);
	talloc_free(mac_control_block);

	if (poll_ass_dl) {
		poll_state = GPRS_RLCMAC_POLL_SCHED;
		poll_fn = (fn + 13) % 2715648;
		dl_ass_state = GPRS_RLCMAC_DL_ASS_WAIT_ACK;
	} else {
		dl_ass_state = GPRS_RLCMAC_DL_ASS_NONE;
		new_dl_tbf->set_state(GPRS_RLCMAC_FLOW);
		tbf_assign_control_ts(new_dl_tbf);
		/* stop pending assignment timer */
		new_dl_tbf->stop_timer();

	}

	return msg;
}

struct msgb *gprs_rlcmac_tbf::create_ul_ass(uint32_t fn)
{
	struct msgb *msg;
	struct gprs_rlcmac_ul_tbf *new_tbf;

	if (poll_state != GPRS_RLCMAC_POLL_NONE) {
		LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already "
			"sheduled for %s, so we must wait for uplink "
			"assignment...\n", tbf_name(this));
			return NULL;
	}
	if (bts->sba()->find(trx->trx_no, control_ts, (fn + 13) % 2715648)) {
		LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already scheduled for "
			"single block allocation...\n");
			return NULL;
	}

	new_tbf = static_cast<gprs_rlcmac_ul_tbf *>(m_new_tbf);
	if (!new_tbf) {
		LOGP(DRLCMACUL, LOGL_ERROR, "We have a schedule for uplink "
			"assignment at downlink %s, but there is no uplink "
			"TBF\n", tbf_name(this));
		ul_ass_state = GPRS_RLCMAC_UL_ASS_NONE;
		return NULL;
	}

	msg = msgb_alloc(23, "rlcmac_ul_ass");
	if (!msg)
		return NULL;
	LOGP(DRLCMAC, LOGL_INFO, "%ss start Packet Uplink Assignment (PACCH)\n", tbf_name(new_tbf));
	bitvec *ass_vec = bitvec_alloc(23);
	if (!ass_vec) {
		msgb_free(msg);
		return NULL;
	}
	bitvec_unhex(ass_vec,
		"2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b");
	Encoding::write_packet_uplink_assignment(bts_data(), ass_vec, m_tfi,
		(direction == GPRS_RLCMAC_DL_TBF), m_tlli,
		m_tlli_valid, new_tbf, 1, bts_data()->alpha,
		bts_data()->gamma, -1);
	bitvec_pack(ass_vec, msgb_put(msg, 23));
	RlcMacDownlink_t * mac_control_block = (RlcMacDownlink_t *)talloc_zero(tall_pcu_ctx, RlcMacDownlink_t);
	LOGP(DRLCMAC, LOGL_DEBUG, "+++++++++++++++++++++++++ TX : Packet Uplink Assignment +++++++++++++++++++++++++\n");
	decode_gsm_rlcmac_downlink(ass_vec, mac_control_block);
	LOGPC(DCSN1, LOGL_NOTICE, "\n");
	LOGP(DRLCMAC, LOGL_DEBUG, "------------------------- TX : Packet Uplink Assignment -------------------------\n");
	bitvec_free(ass_vec);
	talloc_free(mac_control_block);

	poll_state = GPRS_RLCMAC_POLL_SCHED;
	poll_fn = (fn + 13) % 2715648;
	ul_ass_state = GPRS_RLCMAC_UL_ASS_WAIT_ACK;

	return msg;
}

void gprs_rlcmac_tbf::free_all(struct gprs_rlcmac_trx *trx)
{
	for (uint8_t tfi = 0; tfi < 32; tfi++) {
		struct gprs_rlcmac_tbf *tbf;

		tbf = trx->ul_tbf[tfi];
		if (tbf)
			tbf_free(tbf);
		tbf = trx->dl_tbf[tfi];
		if (tbf)
			tbf_free(tbf);
	}
}

void gprs_rlcmac_tbf::free_all(struct gprs_rlcmac_pdch *pdch)
{
	for (uint8_t tfi = 0; tfi < 32; tfi++) {
		struct gprs_rlcmac_tbf *tbf;

		tbf = pdch->ul_tbf_by_tfi(tfi);
		if (tbf)
			tbf_free(tbf);
		tbf = pdch->dl_tbf_by_tfi(tfi);
		if (tbf)
			tbf_free(tbf);
	}
}

void gprs_rlcmac_tbf::tlli_mark_valid()
{
	m_tlli_valid = true;
}

void gprs_rlcmac_tbf::update_tlli(uint32_t tlli)
{
	if (tlli == m_tlli)
		return;

	bool changedUl = false;

	/*
	 * During a Routing Area Update (due the assignment of a new
	 * P-TMSI) the tlli can change. We notice this when receiving
	 * a PACKET CONTROL ACK.
	 * When we get a TLLI change on the DL we will look if there
	 * is a UL TBF and change the tlli there as well.
	 *
	 * TODO: There could be multiple DL and UL TBFs and we should
	 * have a proper way to link all the related TBFs so we can do
	 * a group update.
	 */
	if (m_tlli_valid && direction == GPRS_RLCMAC_DL_TBF) {
		gprs_rlcmac_tbf *ul_tbf;
		ul_tbf = bts->ul_tbf_by_tlli(m_tlli);

		if (ul_tbf) {
			ul_tbf->m_tlli = tlli;
			changedUl = true;
		}
	}

	/* update the timing advance for the new tlli */
	bts->timing_advance()->update(m_tlli, tlli, ta);

	LOGP(DRLCMAC, LOGL_NOTICE,
		"%s changing tlli from TLLI=0x%08x TLLI=0x%08x ul_changed=%d\n",
		tbf_name(this), m_tlli, tlli, changedUl);
	m_tlli = tlli;
}

int gprs_rlcmac_tbf::extract_tlli(const uint8_t *data, const size_t len)
{
	struct gprs_rlcmac_tbf *dl_tbf, *ul_tbf;
	struct rlc_ul_header *rh = (struct rlc_ul_header *)data;
	uint32_t new_tlli;
	int rc;

	/* no TLLI yet */
	if (!rh->ti) {
		LOGP(DRLCMACUL, LOGL_NOTICE, "UL DATA TFI=%d without "
			"TLLI, but no TLLI received yet\n", rh->tfi);
		return 0;
	}
	rc = Decoding::tlli_from_ul_data(data, len, &new_tlli);
	if (rc) {
		bts->decode_error();
		LOGP(DRLCMACUL, LOGL_NOTICE, "Failed to decode TLLI "
		"of UL DATA TFI=%d.\n", rh->tfi);
		return 0;
	}
	update_tlli(new_tlli);
	LOGP(DRLCMACUL, LOGL_INFO, "Decoded premier TLLI=0x%08x of "
		"UL DATA TFI=%d.\n", tlli(), rh->tfi);
	if ((dl_tbf = bts->dl_tbf_by_tlli(tlli()))) {
		LOGP(DRLCMACUL, LOGL_NOTICE, "Got RACH from "
			"TLLI=0x%08x while %s still exists. "
			"Killing pending DL TBF\n", tlli(),
			tbf_name(dl_tbf));
		tbf_free(dl_tbf);
		dl_tbf = NULL;
	}
	/* ul_tbf_by_tlli will not find your TLLI, because it is not
	 * yet marked valid */
	if ((ul_tbf = bts->ul_tbf_by_tlli(tlli()))) {
		LOGP(DRLCMACUL, LOGL_NOTICE, "Got RACH from "
			"TLLI=0x%08x while %s still exists. "
			"Killing pending UL TBF\n", tlli(),
			tbf_name(ul_tbf));
		tbf_free(ul_tbf);
		ul_tbf = NULL;
	}
	/* mark TLLI valid now */
	tlli_mark_valid();
	/* store current timing advance */
	bts->timing_advance()->remember(tlli(), ta);
	return 1;
}

const char *tbf_name(gprs_rlcmac_tbf *tbf)
{
	static char buf[60];
	snprintf(buf, sizeof(buf), "TBF(TFI=%d TLLI=0x%08x DIR=%s STATE=%s)",
			tbf->m_tfi, tbf->m_tlli,
			tbf->direction == GPRS_RLCMAC_UL_TBF ? "UL" : "DL",
			tbf->state_name()
			);
	buf[sizeof(buf) - 1] = '\0';
	return buf;
}

void gprs_rlcmac_tbf::rotate_in_list()
{
	llist_del(&list.list);
	if (direction == GPRS_RLCMAC_UL_TBF)
		llist_add(&list.list, &bts->bts_data()->ul_tbfs);
	else
		llist_add(&list.list, &bts->bts_data()->dl_tbfs);
}

uint8_t gprs_rlcmac_tbf::tsc() const
{
	return trx->pdch[first_ts].tsc;
}

void tbf_print_vty_info(struct vty *vty, llist_head *ltbf)
{
	gprs_rlcmac_tbf *tbf = llist_pods_entry(ltbf, gprs_rlcmac_tbf);

	vty_out(vty, "TBF: TFI=%d TLLI=0x%08x (%s) DIR=%s IMSI=%s%s", tbf->tfi(),
			tbf->tlli(), tbf->is_tlli_valid() ? "valid" : "invalid",
			tbf->direction == GPRS_RLCMAC_UL_TBF ? "UL" : "DL",
			tbf->imsi(), VTY_NEWLINE);
	vty_out(vty, " created=%lu state=%08x 1st_TS=%d 1st_cTS=%d ctrl_TS=%d "
			"MS_CLASS=%d%s",
			tbf->created_ts(), tbf->state_flags, tbf->first_ts,
			tbf->first_common_ts, tbf->control_ts, tbf->ms_class,
			VTY_NEWLINE);
	vty_out(vty, " TS_alloc=");
	for (int i = 0; i < 8; i++) {
		if (tbf->pdch[i])
			vty_out(vty, "%d ", i);
	}
	vty_out(vty, " CS=%d%s%s", tbf->cs, VTY_NEWLINE, VTY_NEWLINE);
}

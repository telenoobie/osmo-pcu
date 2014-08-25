/* poll_controller.h
 *
 * Copyright (C) 2012 Andreas Eversberg <jolly@eversberg.eu>
 * Copyright (C) 2013 by Holger Hans Peter Freyther
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <poll_controller.h>
#include <bts.h>
#include <tbf.h>

PollController::PollController(BTS& bts)
	: m_bts(bts)
{}

void PollController::expireTimedout(int frame_number)
{
	struct gprs_rlcmac_bts *bts = m_bts.bts_data();
	struct gprs_rlcmac_dl_tbf *dl_tbf;
	struct gprs_rlcmac_ul_tbf *ul_tbf;
	struct gprs_rlcmac_sba *sba, *sba2;
	struct llist_pods *lpods;
	uint32_t elapsed;

	/* check for poll timeout
	 * The UL frame numbers lag 3 behind the DL frames and the data
	 * indication is only sent after all 4 frames of the block have been
	 * received. Sometimes there is an idle frame between the end of one
	 * and start of another frame (every 3 blocks).  So the timeout should
	 * definitely be there if we're more than 8 frames past poll_fn. Let's
	 * stay on the safe side and say 13 or more. */
	llist_pods_for_each_entry(ul_tbf, &bts->ul_tbfs, list, lpods) {
		if (ul_tbf->poll_state == GPRS_RLCMAC_POLL_SCHED) {
			elapsed = (frame_number + 2715648 - ul_tbf->poll_fn)
								% 2715648;
			if (elapsed >= 13 && elapsed < 2715400)
				ul_tbf->poll_timeout();
		}
	}
	llist_pods_for_each_entry(dl_tbf, &bts->dl_tbfs, list, lpods) {
		if (dl_tbf->poll_state == GPRS_RLCMAC_POLL_SCHED) {
			elapsed = (frame_number + 2715648 - dl_tbf->poll_fn)
								% 2715648;
			if (elapsed >= 13 && elapsed < 2715400)
				dl_tbf->poll_timeout();
		}
	}
	llist_for_each_entry_safe(sba, sba2, &m_bts.sba()->m_sbas, list) {
		elapsed = (frame_number + 2715648 - sba->fn) % 2715648;
		if (elapsed >= 13 && elapsed < 2715400) {
			/* sba will be freed here */
			m_bts.sba()->timeout(sba);
		}
	}

}

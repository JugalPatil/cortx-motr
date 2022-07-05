/* -*- C -*- */
/*
 * Copyright (c) 2013-2021 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#include "lib/vec.h"
#include "lib/locality.h"
#include "lib/finject.h"
#include "ioservice/io_service.h"
#include "motr/setup.h"
#include "sns/cm/rebalance/xform.c"
#include "sns/cm/file.h"
#include "sns/cm/cm.c"
#include "sns/cm/rebalance/ag.h"
#include "sns/cm/rebalance/ag.c"
#include "sns/cm/repair/ut/cp_common.h"
#include "ioservice/fid_convert.h"	/* m0_fid_convert_cob2stob */
#include "ut/stob.h"			/* m0_ut_stob_create_by_stob_id */
#include "ut/ut.h"
#include "module/instance.h"            /* m0_get */

enum {
	SEG_NR                  = 16,
	SEG_SIZE                = 4096,
	BUF_NR                  = 2,
	DATA_NR                 = 5,
    PARITY_NR               = 2,
	CP_SINGLE               = 1,
	SINGLE_FAILURE          = 1,
	MULTI_FAILURES          = 2,
	SINGLE_FAIL_MULTI_CP_NR = 512,
	MULTI_FAIL_MULTI_CP_NR  = 5,
};

static struct m0_fid gob_fid;
static struct m0_fid cob_fid;

static struct m0_reqh            *reqh;
struct m0_pdclust_layout         *reb_pdlay;
static struct m0_cm              *cm;
static struct m0_sns_cm          *scm;
static struct m0_reqh_service    *scm_service;
static struct m0_cob_domain      *cdom;
static struct m0_semaphore        sem;
static struct m0_net_buffer_pool  nbp;
static struct m0_motr sctxx;
static const struct m0_fid      M0_SNS_CM_REPAIR_UT_PVER = M0_FID_TINIT('v', 1, 8);

/* Global structures for single copy packet test. */
static struct m0_sns_cm_rebalance_ag             s_rag;
static struct m0_sns_cm_cp                    s_cp, tgt_cp;
static struct m0_net_buffer                   s_buf;
static struct m0_net_buffer                   s_acc_buf;

/*
 * Global structures for multiple copy packet test comprising of single
 * failure.
 */

M0_INTERNAL void cob_create(struct m0_reqh *reqh, struct m0_cob_domain *cdom,
			    struct m0_be_domain *bedom,
                            uint64_t cont, struct m0_fid *gfid,
			    uint32_t cob_idx);
M0_INTERNAL void cob_delete(struct m0_cob_domain *cdom,
			    struct m0_be_domain *bedom,
			    uint64_t cont, const struct m0_fid *gfid);

static uint64_t cp_single_get(const struct m0_cm_aggr_group *ag)
{
	return CP_SINGLE;
}

static const struct m0_cm_aggr_group_ops group_single_ops = {
	.cago_local_cp_nr = &cp_single_get,
};

static size_t dummy_fom_locality(const struct m0_fom *fom)
{
	/* By default, use locality0. */
	return 0;
}

/* Dummy fom state routine to emulate only selective copy packet states. */
static int dummy_fom_tick(struct m0_fom *fom)
{
	struct m0_cm_cp *cp = container_of(fom, struct m0_cm_cp, c_fom);
	printf("Jugal : dummy_fom_tick \n");

	switch (m0_fom_phase(fom)) {
	case M0_FOM_PHASE_INIT:
		m0_fom_phase_set(fom, M0_CCP_XFORM);
		m0_semaphore_up(&sem);
		printf("Jugal : dummy_fom_tick calling xform \n");

		return cp->c_ops->co_action[M0_CCP_XFORM](cp);
	case M0_CCP_WRITE:
		m0_fom_phase_set(fom, M0_CCP_IO_WAIT);
		return M0_FSO_AGAIN;
	case M0_CCP_IO_WAIT:
		m0_fom_phase_set(fom, M0_CCP_FINI);
		return M0_FSO_WAIT;
	default:
		M0_IMPOSSIBLE("Bad State");
		printf("Jugal : dummy_fom_tick bad done \n");

		return 0;
	}

}

static int dummy_acc_cp_fom_tick(struct m0_fom *fom)
{
	printf("Jugal : dummy_acc_fom_tick \n");

	switch (m0_fom_phase(fom)) {
	case M0_FOM_PHASE_INIT:
		m0_fom_phase_set(fom, M0_CCP_WRITE);
		m0_semaphore_up(&sem);
		printf("Jugal : dummy_acc_fom_tick : phase_init \n");

		return M0_FSO_AGAIN;
	case M0_CCP_WRITE:
		m0_fom_phase_set(fom, M0_CCP_IO_WAIT);
		printf("Jugal : dummy_acc_fom_tick : CCP_WRITE \n");

		return M0_FSO_AGAIN;
	case M0_CCP_IO_WAIT:
		m0_fom_phase_set(fom, M0_CCP_FINI);
		printf("Jugal : dummy_acc_fom_tick : CCP_IO_WAIT \n");

		return M0_FSO_WAIT;
	default:
		M0_IMPOSSIBLE("Bad State");
		return 0;
	}

}

static void single_cp_fom_fini(struct m0_fom *fom)
{
	struct m0_cm_cp *cp = container_of(fom, struct m0_cm_cp, c_fom);
	printf("Jugal : single_cp_fom_fini \n");

	m0_cm_cp_fini(cp);
	printf("Jugal : single_cp_fom_fini done \n");

}

/* Over-ridden copy packet FOM ops. */
static struct m0_fom_ops single_cp_fom_ops = {
	.fo_fini          = single_cp_fom_fini,
	.fo_tick          = dummy_fom_tick,
	.fo_home_locality = dummy_fom_locality
};

static struct m0_fom_ops acc_cp_fom_ops = {
	.fo_fini          = single_cp_fom_fini,
	.fo_tick          = dummy_acc_cp_fom_tick,
	.fo_home_locality = dummy_fom_locality
};

const struct m0_sns_cm_helpers xform_ut_rebalance_helpers = {
};

static void cp_buf_free(struct m0_sns_cm_ag *sag)
{
	struct m0_cm_cp            *acc_cp;
	struct m0_net_buffer       *nbuf;
	int                         i;

	printf("Jugal : cp_buf_free \n");

	for (i = 0; i < sag->sag_fnr; ++i) {
		acc_cp = &tgt_cp.sc_base;
		m0_tl_for(cp_data_buf, &acc_cp->c_buffers, nbuf) {
			m0_bufvec_free(&nbuf->nb_buffer);
		} m0_tl_endfor;
	}

	printf("Jugal : cp_buf_free done \n");

}

static void tgt_fid_cob_create(struct m0_reqh *reqh)
{
	struct m0_stob_id stob_id;
        int		  rc;

	printf("Jugal : tgt_fid_cob_create \n");

	m0_ios_cdom_get(reqh, &cdom);
        cob_create(reqh, cdom, reqh->rh_beseg->bs_domain, 0, &gob_fid,
		   m0_fid_cob_device_id(&cob_fid));
	m0_fid_convert_cob2stob(&cob_fid, &stob_id);
	rc = m0_ut_stob_create_by_stob_id(&stob_id, NULL);
	M0_ASSERT(rc == 0);

	printf("Jugal : tgt_fid_cob_create done \n");

}

static void ag_init(struct m0_sns_cm_rebalance_ag *rag)
{
	struct m0_cm_aggr_group *ag = &rag->rag_base.sag_base;

	printf("Jugal : ag_init \n");

	/* Workaround to avoid lock of uninitialised mutex */
	m0_mutex_init(&ag->cag_mutex);
	printf("Jugal : ag_init done \n");

}

static void ag_fini(struct m0_sns_cm_rebalance_ag *rag)
{
	struct m0_cm_aggr_group *ag = &rag->rag_base.sag_base;
	printf("Jugal : ag_fini  \n");

	m0_mutex_fini(&ag->cag_mutex);
	printf("Jugal : ag_fini done \n");

}

static void ag_prepare(struct m0_sns_cm_rebalance_ag *rag, int failure_nr,
		       const struct m0_cm_aggr_group_ops *ag_ops,
		       struct m0_sns_cm_cp *cp)
{
	struct m0_sns_cm_cp *scp;
	struct m0_sns_cm_ag *sag;
	struct m0_cm_ag_id id;

	printf("Jugal : ag_prepare \n");


	sag = &rag->rag_base;
	sag->sag_base.cag_transformed_cp_nr = 0;
	sag->sag_fnr = failure_nr;
	sag->sag_base.cag_ops = ag_ops;
	sag->sag_base.cag_cp_local_nr =
		sag->sag_base.cag_ops->cago_local_cp_nr(&sag->sag_base);
	sag->sag_base.cag_cp_global_nr = sag->sag_base.cag_cp_local_nr +
					 failure_nr;
	id.ai_hi.u_hi = 0;
	id.ai_hi.u_lo = 0;
	id.ai_lo.u_hi = 0;
	id.ai_lo.u_lo = 0;
	sag->sag_base.cag_id = id;
	// sag->sag_base.cag_mutex = PTHREAD_MUTEX_INITIALIZER;
	scp = cp2snscp(&cp->sc_base);
	scp->sc_cobfid = cob_fid;

	printf("Jugal : ag_prepare done \n");

}

/*
 * Test to check that single copy packet is treated as passthrough by the
 * transformation function.
 */

void reb_cp_prepare(struct m0_cm_cp *cp, struct m0_net_buffer *buf,
		uint32_t bv_seg_nr, uint32_t bv_seg_size,
		struct m0_sns_cm_ag *sns_ag,
		char data, struct m0_fom_ops *cp_fom_ops,
		struct m0_reqh *reqh, uint64_t cp_ag_idx, bool is_acc_cp,
		struct m0_cm *cm)
{
	struct m0_reqh_service *service;
	struct m0_sns_cm       *scm;

	printf("Jugal : reb_cp_prepare \n");

	M0_UT_ASSERT(cp != NULL);
	M0_UT_ASSERT(buf != NULL);
	M0_UT_ASSERT(sns_ag != NULL);

	if (buf->nb_buffer.ov_buf == NULL)
		bv_alloc_populate(&buf->nb_buffer, data, bv_seg_nr, bv_seg_size);
	else
		bv_populate(&buf->nb_buffer, data, bv_seg_nr, bv_seg_size);
	cp->c_ag = &sns_ag->sag_base;
	if (cm == NULL) {
		service = m0_reqh_service_find(&sns_rebalance_cmt.ct_stype, reqh);
		M0_UT_ASSERT(service != NULL);
		cm = container_of(service, struct m0_cm, cm_service);
		M0_UT_ASSERT(cm != NULL);
		scm = cm2sns(cm);
		m0_ios_cdom_get(reqh, &scm->sc_cob_dom);
	}
	cp->c_ag->cag_cm = cm;
	cp->c_ops = &m0_sns_cm_rebalance_cp_ops;
	m0_cm_cp_fom_init(cm, cp, NULL, NULL);
	m0_cm_cp_buf_add(cp, buf);
	cp->c_data_seg_nr = bv_seg_nr;
	buf->nb_pool->nbp_seg_nr = bv_seg_nr;
	buf->nb_pool->nbp_seg_size = bv_seg_size;
	cp->c_fom.fo_ops = cp_fom_ops;
	cp->c_ag_cp_idx = cp_ag_idx;

	printf("Jugal : reb_cp_prepare done \n");

}

static void test_single_cp(void)
{
	printf("Jugal : test_single_cp \n");
	struct m0_sns_cm_ag       *sag;
	struct m0_cm_cp           *cp = &s_cp.sc_base;
	struct m0_sns_cm_file_ctx  fctx;
	struct m0_pool_version *pver;
	struct m0_pdclust_instance pi;

	m0_semaphore_init(&sem, 0);
	ag_prepare(&s_rag, SINGLE_FAILURE, &group_single_ops, &tgt_cp);
	s_acc_buf.nb_pool = &nbp;
	s_buf.nb_pool = &nbp;
	pver = m0_pool_version_find(&sctxx.cc_pools_common, &M0_SNS_CM_REPAIR_UT_PVER);
	pi.pi_base.li_l = m0_alloc(sizeof pi.pi_base.li_l[0]);
	pi.pi_base.li_l->l_pver = pver;
	fctx.sf_pi = &pi;
	fctx.sf_lock.m_addb2 = NULL;
	fctx.sf_lock.m_owner = NULL;
	fctx.sf_lock.m_arch.m_impl = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	fctx.sf_layout = m0_pdl_to_layout(reb_pdlay);
	sag = &s_rag.rag_base;
	sag->sag_fctx = &fctx;

	reb_cp_prepare(&tgt_cp.sc_base, &s_buf, SEG_NR, SEG_SIZE, sag, 'e',
		   &single_cp_fom_ops, reqh, 0, false, NULL);
	reb_cp_prepare(cp, &s_acc_buf, SEG_NR,
		   SEG_SIZE, sag, 0, &acc_cp_fom_ops, reqh, 0, true, NULL);
	m0_bitmap_init(&tgt_cp.sc_base.c_xform_cp_indices,
		       sag->sag_base.cag_cp_global_nr);
	s_cp.sc_is_local = true;
	m0_fom_queue(&tgt_cp.sc_base.c_fom);

	/* Wait till ast gets posted. */
	m0_semaphore_down(&sem);
	m0_reqh_idle_wait(reqh);

	/*
	 * These asserts ensure that the single copy packet has been treated
	 * as passthrough.
	 */
	m0_semaphore_fini(&sem);
	bv_free(&s_buf.nb_buffer);
	cp_buf_free(sag);
	printf("Jugal : test_single_cp done \n");

}

/*
 * Initialises the request handler since copy packet fom has to be tested using
 * request handler infrastructure.
 */
static int xform_init(void)
{
	int rc;
	printf("Jugal : xform_init \n");

	M0_SET0(&gob_fid);
	M0_SET0(&cob_fid);
	M0_SET0(&sctxx);

	reqh = NULL;
	reb_pdlay = NULL;
	cm = NULL;
	scm = NULL;
	scm_service = NULL;
	cdom = NULL;
	M0_SET0(&sem);
	M0_SET0(&nbp);

	/* Global structures for single copy packet test. */
	M0_SET0(&s_rag);
	// M0_SET_ARR0(s_fc);
	M0_SET0(&s_cp);
	M0_SET0(&tgt_cp);
	M0_SET0(&s_buf);
	M0_SET0(&s_acc_buf);

	rc = cs_init(&sctxx);
	M0_ASSERT(rc == 0);

	m0_fid_gob_make(&gob_fid, 0, 4);
	m0_fid_convert_gob2cob(&gob_fid, &cob_fid, 1);

	reqh = m0_cs_reqh_get(&sctxx);
	layout_gen(&reb_pdlay, reqh);
	tgt_fid_cob_create(reqh);

	scm_service = m0_reqh_service_find(
		m0_reqh_service_type_find("M0_CST_SNS_REB"), reqh);
        M0_ASSERT(scm_service != NULL);
		
        cm = container_of(scm_service, struct m0_cm, cm_service);
        M0_ASSERT(cm != NULL);

        scm = cm2sns(cm);
	scm->sc_cob_dom = cdom;
	scm->sc_helpers = &xform_ut_rebalance_helpers;

	ag_init(&s_rag);
	printf("Jugal : xform_init done \n");

	return 0;
}

static int xform_fini(void)
{
	struct m0_cob_domain *cdom;
	struct m0_stob_id     stob_id;
	int                   rc;
	printf("Jugal : xform_fini \n");

	ag_fini(&s_rag);

	m0_fid_convert_cob2stob(&cob_fid, &stob_id);
	rc = m0_ut_stob_destroy_by_stob_id(&stob_id);
	M0_UT_ASSERT(rc == 0);
	m0_ios_cdom_get(reqh, &cdom);
	cob_delete(cdom, reqh->rh_beseg->bs_domain, 0, &gob_fid);
	layout_destroy(reb_pdlay);
        cs_fini(&sctxx);

	printf("Jugal : xform_fini done \n");

        return 0;
}

struct m0_ut_suite snscm_reb_xform_ut = {
	.ts_name = "snscm_reb_xform_ut",
	.ts_init = &xform_init,
	.ts_fini = &xform_fini,
	.ts_tests = {
		{ "single_cp_xform", test_single_cp },
		{ NULL, NULL }
	}
};

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */

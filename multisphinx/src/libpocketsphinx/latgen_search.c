/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 2010 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * This work was supported in part by funding from the Defense Advanced 
 * Research Projects Agency and the National Science Foundation of the 
 * United States of America, and the CMU Sphinx Speech Consortium.
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

/**
 * @file latgen_search.c Lattice generation (as a search pass).
 */

#include <sphinxbase/garray.h>
#include "ps_search.h"
#include "arc_buffer.h"
#include "nodeid_map.h"
#include "ms_lattice.h"

static int latgen_search_decode(ps_search_t *base);
static int latgen_search_free(ps_search_t *base);
static char const *latgen_search_hyp(ps_search_t *base, int32 *out_score);
static int32 latgen_search_prob(ps_search_t *base);
static ps_seg_t *latgen_search_seg_iter(ps_search_t *base, int32 *out_score);

static ps_searchfuncs_t latgen_funcs = {
    /* name: */   "latgen",
    /* free: */   latgen_search_free,
    /* decode: */ latgen_search_decode,
    /* hyp: */      latgen_search_hyp,
    /* prob: */     latgen_search_prob,
    /* seg_iter: */ latgen_search_seg_iter,
};

typedef struct latgen_search_s {
    ps_search_t base;
    ngram_model_t *lm;
    dict2pid_t *d2p;
    logmath_t *lmath;
    arc_buffer_t *input_arcs;
    ms_lattice_t *output_lattice;
    /** Storage for language model state components. */
    int32 *lmhist;
    /** Allocation size of @a lmhist. */
    int max_n_hist;
    /** List of active node IDs at current frame. */
    garray_t *active_nodes;
    /** Right context ID for all links in current lattice. */
    garray_t *link_rcid;
    /**
     * Original word ID corresponding to each link.
     *
     * We need to maintain this for building the lattice, because
     * links contain base word IDs (or rather language model word IDs)
     * but we need the correct word ID in order to find the correct
     * right context mapping.
     */
    garray_t *link_altwid;
    /** Raw path score for all links in current lattice. */
    garray_t *link_score;
} latgen_search_t;

ps_search_t *
latgen_init(cmd_ln_t *config,
	    dict2pid_t *d2p,
            ngram_model_t *lm,
	    arc_buffer_t *input_arcs)
{
    latgen_search_t *latgen;

    latgen = ckd_calloc(1, sizeof(*latgen));
    ps_search_init(&latgen->base, &latgen_funcs,
                   config, NULL, d2p->dict, d2p);
    latgen->d2p = dict2pid_retain(d2p);
    latgen->input_arcs = arc_buffer_retain(input_arcs);
    latgen->lmath = logmath_retain(ngram_model_get_lmath(lm));
    latgen->lm = ngram_model_retain(lm);

    latgen->max_n_hist = ngram_model_get_size(lm) - 1;
    latgen->lmhist = ckd_calloc(latgen->max_n_hist,
                                sizeof(*latgen->lmhist));
    latgen->active_nodes = garray_init(0, sizeof(int32));
    latgen->link_rcid = garray_init(0, sizeof(uint8));
    latgen->link_altwid = garray_init(0, sizeof(int32));
    latgen->link_score = garray_init(0, sizeof(int32));
	
    return &latgen->base;
}

/**
 * Construct the list of nodes active at this frame.
 */
static int
get_frame_active_nodes(ms_lattice_t *l, garray_t *out_active_nodes,
                       int32 frame_idx)
{
    ms_latnode_iter_t *node_itor;

    garray_reset(out_active_nodes);
    for (node_itor = ms_lattice_traverse_frame(l, frame_idx); node_itor;
         node_itor = ms_latnode_iter_next(node_itor)) {
        int32 node_idx = ms_latnode_iter_get_idx(node_itor);
        garray_append(out_active_nodes, &node_idx);
    }
    return garray_size(out_active_nodes);
}

/**
 * Create the appropriate (backed-off) language model state for a node.
 *
 * FIXME: Duplicates code in ms_lattice.c, should be refactored -
 * unfortunately there is the issue of word ID mapping there which
 * does not exist here.
 */
static int32
get_backoff_lmstate(ms_lattice_t *l, ngram_model_t *lm,
                    int32 headwid, int32 *lmhist, int n_hist,
                    int32 *out_lscr, int32 *out_bowt)
{
    int32 lmstate;

    lmstate = -1;
    *out_lscr = *out_bowt = 0;
    while (n_hist > 0) {
        ngram_iter_t *ni;
        if ((ni = ngram_ng_iter(lm, headwid,
                                lmhist, n_hist)) != NULL) {
            /* Create or find the relevant lmstate. */
            if ((lmstate = ms_lattice_get_lmstate_idx
                 (l, lmhist[0], lmhist + 1, n_hist - 1)) == -1)
                lmstate = ms_lattice_lmstate_init
                    (l, lmhist[0], lmhist + 1, n_hist - 1);
            ngram_iter_get(ni, out_lscr, NULL);
            ngram_iter_free(ni);
            break;
        }
        else {
            /* Back off and update the backoff weight. */
            assert(n_hist > 0);
            if ((ni = ngram_ng_iter
                 (lm, lmhist[0], lmhist + 1, n_hist - 1)) != NULL) {
                ngram_iter_get(ni, NULL, out_bowt);
                ngram_iter_free(ni);
            }
            else
                *out_bowt = 0;
            if (--n_hist == 0)
                lmstate = -1; /* epsilon, which is okay. */
        }
    }
    return lmstate;
}

/**
 * Create a new link in the output lattice.
 */
static ms_latlink_t *
create_new_link(latgen_search_t *latgen, ms_latnode_t *src,
                ms_latnode_t *dest, ms_latlink_t *incoming_link,
                int32 wid, int32 altwid, int32 score, int32 rc)
{
    ms_latlink_t *link;
    int32 linkid, ascr;

    /* Calculate the acoustic score for this link. */
    /* FIXME: Need lscr too (should be in arc buffer) */
    linkid = ms_lattice_get_idx_link(latgen->output_lattice,
                                     incoming_link);
    ascr = score - garray_ent(latgen->link_score, int32, linkid);

    /* Create the new link. */
    /* FIXME: A matching link may already exist (with a different
     * alternate word ID) in which case we should just take the best
     * ascr. */
    link = ms_lattice_link(latgen->output_lattice,
                           src, dest, wid, ascr);

    /* Record useful facts about this link for its successors. */
    linkid = ms_lattice_get_idx_link(latgen->output_lattice, link);
    garray_expand(latgen->link_rcid, linkid + 1);
    garray_ent(latgen->link_rcid, uint8, linkid) = rc;
    garray_expand(latgen->link_altwid, linkid + 1);
    garray_ent(latgen->link_altwid, int32, linkid) = altwid;
    garray_expand(latgen->link_score, linkid + 1);
    garray_ent(latgen->link_score, int32, linkid) = score;

    return link;
}

/**
 * Create lattice links for a given node and arc.
 *
 * 1) Find the incoming arc corresponding to the initial phone of
 *    this arc's word.
 * 2) Record the starting path score.
 * 3) Find the language model state for this arc's target.
 * 4) Find or create a node for that lmstate in the target frame.
 * 5) Create links to that node for all right contexts of this arc.
 */

static int
create_outgoing_links_one(latgen_search_t *latgen,
                          ms_latnode_t *node,
                          bitvec_t *active_incoming_links,
                          sarc_t *arc)
{
    ms_latlink_t *incoming_link;
    ms_latnode_t *dest;
    int32 lmstate, headwid, lscr, bowt;
    int ciphone, n_hist, i;
    xwdssid_t *rssid;
    int n_links = 0;

    /* Find incoming link (to get starting path score) */
    /* FIXME: Actually there are probably multiple incoming links, and
     * we want to take the best scoring one. */
    ciphone = dict_first_phone(latgen->d2p->dict, arc->arc.wid);
    incoming_link = NULL;
    for (i = 0; i < ms_latnode_n_entries(node); ++i) {
        int32 linkid = ms_latnode_get_entry_idx
            (latgen->output_lattice, node, i);
        int rcid = garray_ent(latgen->link_rcid, uint8, linkid);
        /* No multiple right contexts: everything matches. */
        if (rcid == NO_RC) {
            incoming_link = ms_lattice_get_link_idx
                (latgen->output_lattice, linkid);
            break;
        }
        /* Try to match ciphone against the right context ID. */
        else {
            int32 linkwid = garray_ent(latgen->link_altwid, int32, linkid);
            rssid = dict2pid_rssid
                (latgen->d2p,
                 dict_last_phone(latgen->d2p->dict, linkwid),
                 dict_second_last_phone(latgen->d2p->dict, linkwid));
            if (rssid->cimap[ciphone] == rcid) {
                incoming_link = ms_lattice_get_link_idx
                    (latgen->output_lattice, linkid);
                break;
            }
        }
    }
    /* FIXME: This is almost certainly going to fail.  Fuck. */
    assert(node->id.sf == 0 || incoming_link != NULL);
    /* Now mark this incoming link as active. */
    if (i < ms_latnode_n_entries(node))
        bitvec_set(active_incoming_links, i);

    /* Create new language model state */
    n_hist =
        ms_lattice_get_lmstate_wids(latgen->output_lattice,
                                    node->id.lmstate,
                                    &headwid, latgen->lmhist);
    rotate_lmstate(headwid, latgen->lmhist, n_hist,
                   latgen->max_n_hist);
    /* headwid + latgen->lmhist are now the raw lmstate. */
    headwid = dict_basewid(latgen->d2p->dict, arc->arc.wid);
    /* Get the appropriate backed-off lmstate. */
    lmstate = get_backoff_lmstate
        (latgen->output_lattice,
         latgen->lm, headwid, latgen->lmhist,
         n_hist, &lscr, &bowt);

    /* FIXME: Not exactly sure where/how to apply backoff weights,
     * hopefully it'll come to me.  Actually the way we are creating
     * nodes is a bit wrong - we still need to do the duplication of
     * nodes and creation of backoff nodes like standalone expansion
     * does.  Basically the function above needs to create a backoff
     * node if it can't find a language model state for the arc under
     * consideration.  Then we duplicate all incoming arcs and add the
     * backoff weight to them - actually though since we are doing
     * this incrementally all we need to do is look for a backoff node
     * and add the backoff weight to each incoming arc as we copy it -
     * this has the side effect of only preserving relevant arcs. */

    /* Get or create a node for that lmstate/frame. */
    if ((dest = ms_lattice_get_node_id
         /* NOTE: bptbl indices are inclusive, ours are not. */
         (latgen->output_lattice, lmstate, arc->arc.dest + 1)) == NULL) {
        dest = ms_lattice_node_init
            (latgen->output_lattice, arc->arc.dest + 1, lmstate);
    }

    /* For all right contexts create a link to dest. */
    if (dict_pronlen(latgen->d2p->dict, arc->arc.wid) == 1) {
        ms_latlink_t *link;
        link = create_new_link(latgen, node, dest, incoming_link,
                               headwid, arc->arc.wid, arc->score,
                               NO_RC);
        link->lscr = lscr; /* FIXME: See above re. bowt */
        ++n_links;
    }
    else {
        for (i = 0; i < arc_buffer_max_n_rc(latgen->input_arcs); ++i) {
            if (bitvec_is_set(arc->rc_bits, i)) {
                ms_latlink_t *link;
                link = create_new_link
                    (latgen, node, dest, incoming_link,
                     headwid, arc->arc.wid, 
                     arc_buffer_get_rcscore(latgen->input_arcs, arc, i), i);
                link->lscr = lscr; /* FIXME: See above re. bowt */
                ++n_links;
            }
        }
    }

    return n_links;
}

/**
 * Create lattice links for a given arc.
 */
static int
create_outgoing_links(latgen_search_t *latgen,
                      sarc_t *arc)
{
    int n_links = 0;
    int i;

    for (i = 0; i < garray_size(latgen->active_nodes); ++i) {
        int32 nodeidx = garray_ent(latgen->active_nodes, int32, i);
        ms_latnode_t *node = ms_lattice_get_node_idx
            (latgen->output_lattice, nodeidx);
        bitvec_t *active_links;
        int node_n_links;

        /* FIXME: Should allocate this in latgen and grow as needed. */
        active_links = bitvec_alloc(ms_latnode_n_entries(node));
        node_n_links = create_outgoing_links_one(latgen, node,
                                                 active_links, arc);
        if (node_n_links == 0) {
            /* This node is a goner, prune it. */
            /* FIXME: Actually it's not clear this will happen. */
        }
        else {
            /* Prune dangling incoming links (with no active right
             * context) */
            /* FIXME: This is a kind of annoying way to have to do
             * this... overzealous encapsulation perhaps? */
            glist_t deadlinks;
            gnode_t *gn;
            int j;

            deadlinks = NULL;
            for (j = 0; j < ms_latnode_n_entries(node); ++j) {
                if (bitvec_is_set(active_links, j))
                    continue;
                deadlinks = glist_add_ptr
                    (deadlinks, ms_latnode_get_entry
                     (latgen->output_lattice, node, j));
            }
            for (gn = deadlinks; gn; gn = gnode_next(gn))
                ms_latlink_unlink(latgen->output_lattice,
                                  (ms_latlink_t *)gnode_ptr(gn));
            glist_free(deadlinks);
        }
        bitvec_free(active_links);
        n_links += node_n_links;
    }

    return n_links;
}

static int
latgen_search_process_arcs(latgen_search_t *latgen,
                           sarc_t *itor, int32 frame_idx)
{
    int n_arc;

    /* Get source nodes for these arcs. */
    if (get_frame_active_nodes(latgen->output_lattice,
                               latgen->active_nodes, frame_idx) == 0)
        return 0;

    /* Iterate over all arcs exiting in this frame */
    for (n_arc = 0; itor; itor = (sarc_t *)arc_buffer_iter_next
             (latgen->input_arcs, &itor->arc)) {
        /* See note in arc_buffer.h... */
        if (itor->arc.src != frame_idx)
            break;

        /* Create new outgoing links for each source node. */
        n_arc += create_outgoing_links(latgen, itor);
    }

    return n_arc;
}

static int
latgen_search_decode(ps_search_t *base)
{
    latgen_search_t *latgen = (latgen_search_t *)base;
    int frame_idx;

    frame_idx = 0;
    E_INFO("waiting for arc buffer start\n");
    if (arc_buffer_consumer_start_utt(latgen->input_arcs, -1) < 0)
        return -1;

    /* Create lattice and initial epsilon node. */
    latgen->output_lattice = ms_lattice_init(latgen->lmath,
                                             ps_search_dict(base));
    ms_lattice_node_init(latgen->output_lattice, 0, -1);

    /* Reset some internal arrays. */
    garray_reset(latgen->link_rcid);
    garray_reset(latgen->link_altwid);
    garray_reset(latgen->link_score);

    /* Process frames full of arcs. */
    while (arc_buffer_consumer_wait(latgen->input_arcs, -1) >= 0) {
        ptmr_start(&base->t);
        while (1) {
            arc_t *itor;
            int n_arc;

            arc_buffer_lock(latgen->input_arcs);
            itor = arc_buffer_iter(latgen->input_arcs, frame_idx);
            if (itor == NULL) {
                arc_buffer_unlock(latgen->input_arcs);
                break;
            }
            n_arc = latgen_search_process_arcs(latgen, (sarc_t *)itor, frame_idx);
            E_INFO("Added %d links leaving frame %d\n", n_arc, frame_idx);
            arc_buffer_unlock(latgen->input_arcs);
            ++frame_idx;
        }
        ptmr_stop(&base->t);
        if (arc_buffer_eou(latgen->input_arcs)) {
            E_INFO("latgen: got EOU\n");
            /* Clean up the output lattice. */
            arc_buffer_consumer_end_utt(latgen->input_arcs);
            return frame_idx;
        }
    }
    return -1;
}

static int
latgen_search_free(ps_search_t *base)
{
    latgen_search_t *latgen = (latgen_search_t *)base;

    arc_buffer_free(latgen->input_arcs);
    logmath_free(latgen->lmath);
    dict2pid_free(latgen->d2p);
    ngram_model_free(latgen->lm);
    ckd_free(latgen->lmhist);
    garray_free(latgen->active_nodes);
    return 0;
}

/**
 * Bestpath search over the lattice.
 */
static char const *
latgen_search_hyp(ps_search_t *base, int32 *out_score)
{
    return NULL;
}

/**
 * Bestpath search over the lattice.
 */
static ps_seg_t *
latgen_search_seg_iter(ps_search_t *base, int32 *out_score)
{
    return NULL;
}

/**
 * Forward-backward calculation over the lattice.
 */
static int32
latgen_search_prob(ps_search_t *base)
{
    return 0;
}
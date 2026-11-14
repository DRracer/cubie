// SPDX-License-Identifier: MIT
// Copyright (c) 2026 D.R.racer <drracer@seznam.cz>
/*
 * H.264: VDPAU picture info + slice NAL units -> V4L2 stateless controls.
 *
 * VDPAU passes the PPS/SPS subset and the reference frames; slice headers are
 * parsed here (ITU-T H.264 7.3.3), reference lists are built per 8.2.4.
 * Control semantics follow GStreamer's v4l2slh264dec. Scope:
 * 4:2:0 8-bit frame pictures (no PAFF/MBAFF fields, no FMO, no MVC).
 */
#include <stdlib.h>
#include <string.h>

#include "vdpau_private.h"

#define MAX_REFS	16
#define MAX_LIST	32
#define NO_REF		0xff
#define HEADER_MAX	2048	/* de-escaped bytes kept for header parsing */

enum { SLICE_P = 0, SLICE_B = 1, SLICE_I = 2, SLICE_SP = 3, SLICE_SI = 4 };

/* --- bit reader on the RBSP (emulation prevention removed) --- */

struct bits {
	const uint8_t *data;
	size_t size, pos;	/* pos in bits */
	bool overrun;
};

static unsigned int get_bit(struct bits *b)
{
	unsigned int v;

	if (b->pos >= b->size * 8) {
		b->overrun = true;
		return 0;
	}
	v = (b->data[b->pos / 8] >> (7 - b->pos % 8)) & 1;
	b->pos++;
	return v;
}

static uint32_t get_u(struct bits *b, unsigned int n)
{
	uint32_t v = 0;

	while (n--)
		v = (v << 1) | get_bit(b);
	return v;
}

static uint32_t get_ue(struct bits *b)
{
	unsigned int zeros = 0;

	while (!get_bit(b) && !b->overrun && zeros < 31)
		zeros++;
	return ((1U << zeros) - 1) + get_u(b, zeros);
}

static int32_t get_se(struct bits *b)
{
	uint32_t v = get_ue(b);

	return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

/* Copy up to `max` RBSP bytes of a NAL unit (drops 0x03 after 00 00). */
static size_t nal_to_rbsp(const uint8_t *src, size_t size, uint8_t *dst, size_t max)
{
	size_t n = 0;
	unsigned int zeros = 0;

	for (size_t i = 0; i < size && n < max; i++) {
		if (zeros >= 2 && src[i] == 0x03) {
			zeros = 0;
			continue;
		}
		zeros = src[i] ? 0 : zeros + 1;
		dst[n++] = src[i];
	}
	return n;
}

/* --- slice header --- */

struct ref_mod {
	unsigned int idc;
	unsigned int value;	/* abs_diff_pic_num_minus1 or long_term_pic_num */
};

struct slice_header {
	unsigned int nal_ref_idc, nal_unit_type;
	unsigned int first_mb_in_slice, slice_type;
	unsigned int frame_num;
	bool field_pic, bottom_field;
	unsigned int idr_pic_id;
	unsigned int pic_order_cnt_lsb;
	int32_t delta_pic_order_cnt_bottom, delta_pic_order_cnt[2];
	unsigned int pic_order_cnt_bit_size;
	unsigned int redundant_pic_cnt;
	bool direct_spatial_mv_pred, sp_for_switch;
	unsigned int num_ref_idx_active_minus1[2];
	struct ref_mod mods[2][MAX_LIST + 1];
	unsigned int num_mods[2];
	struct v4l2_ctrl_h264_pred_weights pred;
	bool has_pred_weights;
	unsigned int dec_ref_pic_marking_bit_size;
	unsigned int cabac_init_idc;
	int32_t slice_qp_delta, slice_qs_delta;
	unsigned int disable_deblocking_filter_idc;
	int32_t slice_alpha_c0_offset_div2, slice_beta_offset_div2;
	unsigned int header_bit_size;	/* incl. the NAL header byte */
};

static void parse_pred_weight_table(struct bits *b, struct slice_header *sh)
{
	struct v4l2_ctrl_h264_pred_weights *p = &sh->pred;
	unsigned int lists = sh->slice_type == SLICE_B ? 2 : 1;

	p->luma_log2_weight_denom = get_ue(b);
	p->chroma_log2_weight_denom = get_ue(b);	/* ChromaArrayType 1 */

	for (unsigned int l = 0; l < lists; l++) {
		struct v4l2_h264_weight_factors *f = &p->weight_factors[l];

		for (unsigned int i = 0; i <= sh->num_ref_idx_active_minus1[l]; i++) {
			f->luma_weight[i] = 1 << p->luma_log2_weight_denom;
			f->luma_offset[i] = 0;
			if (get_bit(b)) {
				f->luma_weight[i] = get_se(b);
				f->luma_offset[i] = get_se(b);
			}
			for (unsigned int j = 0; j < 2; j++) {
				f->chroma_weight[i][j] = 1 << p->chroma_log2_weight_denom;
				f->chroma_offset[i][j] = 0;
			}
			if (get_bit(b)) {
				for (unsigned int j = 0; j < 2; j++) {
					f->chroma_weight[i][j] = get_se(b);
					f->chroma_offset[i][j] = get_se(b);
				}
			}
		}
	}
}

static void parse_dec_ref_pic_marking(struct bits *b, struct slice_header *sh)
{
	size_t start = b->pos;

	if (sh->nal_unit_type == 5) {
		get_bit(b);	/* no_output_of_prior_pics_flag */
		get_bit(b);	/* long_term_reference_flag */
	} else if (get_bit(b)) {	/* adaptive_ref_pic_marking_mode_flag */
		unsigned int mmco;

		do {
			mmco = get_ue(b);
			if (mmco == 1 || mmco == 3)
				get_ue(b);	/* difference_of_pic_nums_minus1 */
			if (mmco == 2)
				get_ue(b);	/* long_term_pic_num */
			if (mmco == 3 || mmco == 6)
				get_ue(b);	/* long_term_frame_idx */
			if (mmco == 4)
				get_ue(b);	/* max_long_term_frame_idx_plus1 */
		} while (mmco && !b->overrun);
	}
	sh->dec_ref_pic_marking_bit_size = b->pos - start;
}

static int parse_slice_header(const uint8_t *nal, size_t size, const VdpPictureInfoH264 *info,
			      struct slice_header *sh)
{
	uint8_t rbsp[HEADER_MAX];
	struct bits b = { .data = rbsp };
	size_t poc_start;

	memset(sh, 0, sizeof(*sh));
	b.size = nal_to_rbsp(nal, size, rbsp, sizeof(rbsp));
	if (b.size < 2)
		return -1;

	get_bit(&b);	/* forbidden_zero_bit */
	sh->nal_ref_idc = get_u(&b, 2);
	sh->nal_unit_type = get_u(&b, 5);
	if (sh->nal_unit_type != 1 && sh->nal_unit_type != 5) {
		ERR("unsupported NAL unit type %u\n", sh->nal_unit_type);
		return -1;
	}

	sh->first_mb_in_slice = get_ue(&b);
	sh->slice_type = get_ue(&b) % 5;
	get_ue(&b);	/* pic_parameter_set_id */
	sh->frame_num = get_u(&b, info->log2_max_frame_num_minus4 + 4);
	if (!info->frame_mbs_only_flag) {
		sh->field_pic = get_bit(&b);
		if (sh->field_pic)
			sh->bottom_field = get_bit(&b);
	}
	if (sh->field_pic) {
		ERR("field pictures are not supported yet\n");
		return -1;
	}
	if (sh->nal_unit_type == 5)
		sh->idr_pic_id = get_ue(&b);

	poc_start = b.pos;
	if (info->pic_order_cnt_type == 0) {
		sh->pic_order_cnt_lsb = get_u(&b, info->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (info->pic_order_present_flag && !sh->field_pic)
			sh->delta_pic_order_cnt_bottom = get_se(&b);
	}
	if (info->pic_order_cnt_type == 1 && !info->delta_pic_order_always_zero_flag) {
		sh->delta_pic_order_cnt[0] = get_se(&b);
		if (info->pic_order_present_flag && !sh->field_pic)
			sh->delta_pic_order_cnt[1] = get_se(&b);
	}
	sh->pic_order_cnt_bit_size = b.pos - poc_start;

	if (info->redundant_pic_cnt_present_flag)
		sh->redundant_pic_cnt = get_ue(&b);
	if (sh->slice_type == SLICE_B)
		sh->direct_spatial_mv_pred = get_bit(&b);

	sh->num_ref_idx_active_minus1[0] = info->num_ref_idx_l0_active_minus1;
	sh->num_ref_idx_active_minus1[1] = info->num_ref_idx_l1_active_minus1;
	if (sh->slice_type == SLICE_P || sh->slice_type == SLICE_SP || sh->slice_type == SLICE_B) {
		if (get_bit(&b)) {	/* num_ref_idx_active_override_flag */
			sh->num_ref_idx_active_minus1[0] = get_ue(&b);
			if (sh->slice_type == SLICE_B)
				sh->num_ref_idx_active_minus1[1] = get_ue(&b);
		}
	}
	if (sh->num_ref_idx_active_minus1[0] >= MAX_LIST ||
	    sh->num_ref_idx_active_minus1[1] >= MAX_LIST)
		return -1;

	/* ref_pic_list_modification() */
	for (unsigned int l = 0; l < 2; l++) {
		if (sh->slice_type == SLICE_I || sh->slice_type == SLICE_SI ||
		    (l == 1 && sh->slice_type != SLICE_B))
			continue;
		if (!get_bit(&b))	/* ref_pic_list_modification_flag_lX */
			continue;
		for (;;) {
			unsigned int idc = get_ue(&b);

			if (idc == 3 || b.overrun)
				break;
			if (idc > 2 || sh->num_mods[l] > MAX_LIST)
				return -1;
			sh->mods[l][sh->num_mods[l]].idc = idc;
			sh->mods[l][sh->num_mods[l]].value = get_ue(&b);
			sh->num_mods[l]++;
		}
	}

	if ((info->weighted_pred_flag && (sh->slice_type == SLICE_P || sh->slice_type == SLICE_SP)) ||
	    (info->weighted_bipred_idc == 1 && sh->slice_type == SLICE_B)) {
		parse_pred_weight_table(&b, sh);
		sh->has_pred_weights = true;
	}

	if (sh->nal_ref_idc)
		parse_dec_ref_pic_marking(&b, sh);

	if (info->entropy_coding_mode_flag && sh->slice_type != SLICE_I && sh->slice_type != SLICE_SI)
		sh->cabac_init_idc = get_ue(&b);
	sh->slice_qp_delta = get_se(&b);
	if (sh->slice_type == SLICE_SP || sh->slice_type == SLICE_SI) {
		if (sh->slice_type == SLICE_SP)
			sh->sp_for_switch = get_bit(&b);
		sh->slice_qs_delta = get_se(&b);
	}
	if (info->deblocking_filter_control_present_flag) {
		sh->disable_deblocking_filter_idc = get_ue(&b);
		if (sh->disable_deblocking_filter_idc != 1) {
			sh->slice_alpha_c0_offset_div2 = get_se(&b);
			sh->slice_beta_offset_div2 = get_se(&b);
		}
	}
	/* slice_group_change_cycle (FMO) is not supported */

	if (b.overrun) {
		ERR("slice header longer than %d bytes\n", HEADER_MAX);
		return -1;
	}
	sh->header_bit_size = b.pos;	/* the NAL header byte is included */
	return 0;
}

/* --- reference frames and lists (frame pictures, 8.2.4) --- */

struct ref {
	int dpb;		/* index into decode_params.dpb */
	bool long_term;
	int32_t pic_num;	/* PicNum (FrameNumWrap) or LongTermPicNum */
	int32_t poc;
};

struct refs {
	struct ref r[MAX_REFS];
	unsigned int n;
};

static int cmp_pic_num_desc(const void *a, const void *b)
{
	return ((const struct ref *)b)->pic_num - ((const struct ref *)a)->pic_num;
}

static int cmp_pic_num_asc(const void *a, const void *b)
{
	return ((const struct ref *)a)->pic_num - ((const struct ref *)b)->pic_num;
}

static int cmp_poc_desc(const void *a, const void *b)
{
	return ((const struct ref *)b)->poc - ((const struct ref *)a)->poc;
}

static int cmp_poc_asc(const void *a, const void *b)
{
	return ((const struct ref *)a)->poc - ((const struct ref *)b)->poc;
}

/* Append refs matching a filter, sorted; returns the new length. */
static unsigned int append(const struct refs *refs, int list[], unsigned int len, bool long_term,
			   int poc_sel, int32_t cur_poc,
			   int (*cmp)(const void *, const void *))
{
	struct ref tmp[MAX_REFS];
	unsigned int n = 0;

	for (unsigned int i = 0; i < refs->n; i++) {
		const struct ref *r = &refs->r[i];

		if (r->long_term != long_term)
			continue;
		if (poc_sel < 0 && r->poc >= cur_poc)
			continue;
		if (poc_sel > 0 && r->poc <= cur_poc)
			continue;
		tmp[n++] = *r;
	}
	qsort(tmp, n, sizeof(tmp[0]), cmp);
	for (unsigned int i = 0; i < n; i++) {
		/* store the position in refs->r */
		for (unsigned int j = 0; j < refs->n; j++)
			if (refs->r[j].dpb == tmp[i].dpb)
				list[len++] = j;
	}
	return len;
}

static unsigned int init_list(const struct refs *refs, unsigned int slice_type, unsigned int l,
			      int32_t cur_poc, int list[])
{
	unsigned int len = 0;

	if (slice_type == SLICE_P || slice_type == SLICE_SP) {
		len = append(refs, list, len, false, 0, cur_poc, cmp_pic_num_desc);
		len = append(refs, list, len, true, 0, cur_poc, cmp_pic_num_asc);
	} else if (l == 0) {
		len = append(refs, list, len, false, -1, cur_poc, cmp_poc_desc);
		len = append(refs, list, len, false, 1, cur_poc, cmp_poc_asc);
		len = append(refs, list, len, true, 0, cur_poc, cmp_pic_num_asc);
	} else {
		len = append(refs, list, len, false, 1, cur_poc, cmp_poc_asc);
		len = append(refs, list, len, false, -1, cur_poc, cmp_poc_desc);
		len = append(refs, list, len, true, 0, cur_poc, cmp_pic_num_asc);
	}
	return len;
}

static int find_ref(const struct refs *refs, bool long_term, int32_t pic_num)
{
	for (unsigned int i = 0; i < refs->n; i++)
		if (refs->r[i].long_term == long_term && refs->r[i].pic_num == pic_num)
			return i;
	return -1;
}

/* 8.2.4.3: apply ref_pic_list_modification to list[0..num_active-1]. */
static void modify_list(const struct refs *refs, const struct slice_header *sh, unsigned int l,
			int32_t max_pic_num, int list[], unsigned int num_active)
{
	int32_t cur_pic_num = sh->frame_num;
	int32_t pred = cur_pic_num;
	unsigned int ref_idx = 0;

	for (unsigned int m = 0; m < sh->num_mods[l] && ref_idx < num_active; m++) {
		const struct ref_mod *mod = &sh->mods[l][m];
		int target;
		unsigned int n;

		if (mod->idc < 2) {
			int32_t diff = mod->value + 1;
			int32_t no_wrap;

			if (mod->idc == 0)
				no_wrap = pred - diff < 0 ? pred - diff + max_pic_num : pred - diff;
			else
				no_wrap = pred + diff >= max_pic_num ? pred + diff - max_pic_num : pred + diff;
			pred = no_wrap;
			target = find_ref(refs, false, no_wrap > cur_pic_num ? no_wrap - max_pic_num : no_wrap);
		} else {
			target = find_ref(refs, true, mod->value);
		}
		if (target < 0) {
			DBG("list %u modification: reference not in the DPB\n", l);
			continue;	/* keep the list as it is */
		}

		/* shift right, insert at ref_idx, drop later duplicates (8-37, 8-38) */
		for (unsigned int i = num_active; i > ref_idx; i--)
			list[i] = list[i - 1];
		list[ref_idx++] = target;
		n = ref_idx;
		for (unsigned int i = ref_idx; i <= num_active; i++)
			if (list[i] != target)
				list[n++] = list[i];
	}
}

/* --- per-picture state --- */

typedef struct {
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_ctrl_h264_pps pps;
	struct v4l2_ctrl_h264_scaling_matrix scaling;
	struct v4l2_ctrl_h264_decode_params decode;
	struct v4l2_ctrl_h264_slice_params slice;
	struct v4l2_ctrl_h264_pred_weights pred;
	uint8_t *bitstream;
	size_t bitstream_size;
} h264_ctx_t;

int h264_init(decoder_ctx_t *dec)
{
	dec->codec_priv = calloc(1, sizeof(h264_ctx_t));
	return dec->codec_priv ? 0 : -1;
}

void h264_free(decoder_ctx_t *dec)
{
	h264_ctx_t *c = dec->codec_priv;

	if (c)
		free(c->bitstream);
	free(c);
	dec->codec_priv = NULL;
}

static bool scaling_lists_flat(const VdpPictureInfoH264 *info)
{
	for (unsigned int i = 0; i < 6; i++)
		for (unsigned int j = 0; j < 16; j++)
			if (info->scaling_lists_4x4[i][j] != 16)
				return false;
	for (unsigned int i = 0; i < 2; i++)
		for (unsigned int j = 0; j < 64; j++)
			if (info->scaling_lists_8x8[i][j] != 16)
				return false;
	return true;
}

static void fill_sps_pps(h264_ctx_t *c, const decoder_ctx_t *dec, const VdpPictureInfoH264 *info)
{
	struct v4l2_ctrl_h264_sps *sps = &c->sps;
	struct v4l2_ctrl_h264_pps *pps = &c->pps;
	unsigned int mbs_h = (dec->height + 15) / 16;

	memset(sps, 0, sizeof(*sps));
	sps->profile_idc = dec->profile == VDP_DECODER_PROFILE_H264_HIGH ? 100 :
			   dec->profile == VDP_DECODER_PROFILE_H264_MAIN ? 77 : 66;
	sps->level_idc = 51;
	sps->chroma_format_idc = 1;
	sps->log2_max_frame_num_minus4 = info->log2_max_frame_num_minus4;
	sps->pic_order_cnt_type = info->pic_order_cnt_type;
	sps->log2_max_pic_order_cnt_lsb_minus4 = info->log2_max_pic_order_cnt_lsb_minus4;
	sps->max_num_ref_frames = info->num_ref_frames;
	sps->pic_width_in_mbs_minus1 = (dec->width + 15) / 16 - 1;
	sps->pic_height_in_map_units_minus1 =
		(info->frame_mbs_only_flag ? mbs_h : (mbs_h + 1) / 2) - 1;
	if (info->frame_mbs_only_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (info->mb_adaptive_frame_field_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	if (info->direct_8x8_inference_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	if (info->delta_pic_order_always_zero_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;

	memset(pps, 0, sizeof(*pps));
	pps->num_ref_idx_l0_default_active_minus1 = info->num_ref_idx_l0_active_minus1;
	pps->num_ref_idx_l1_default_active_minus1 = info->num_ref_idx_l1_active_minus1;
	pps->weighted_bipred_idc = info->weighted_bipred_idc;
	pps->pic_init_qp_minus26 = info->pic_init_qp_minus26;
	pps->chroma_qp_index_offset = info->chroma_qp_index_offset;
	pps->second_chroma_qp_index_offset = info->second_chroma_qp_index_offset;
	if (info->entropy_coding_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (info->pic_order_present_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
	if (info->weighted_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
	if (info->deblocking_filter_control_present_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	if (info->constrained_intra_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (info->redundant_pic_cnt_present_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
	if (info->transform_8x8_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

	/* VDPAU passes the effective (raster order) lists; flat ones = default. */
	memset(&c->scaling, 16, sizeof(c->scaling));
	if (!scaling_lists_flat(info)) {
		pps->flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
		memcpy(c->scaling.scaling_list_4x4, info->scaling_lists_4x4,
		       sizeof(info->scaling_lists_4x4));
		memcpy(c->scaling.scaling_list_8x8[0], info->scaling_lists_8x8[0], 64);
		memcpy(c->scaling.scaling_list_8x8[1], info->scaling_lists_8x8[1], 64);
	}
}

/* DPB from VDPAU's reference frames; also fills `refs` for list building. */
static void fill_dpb(h264_ctx_t *c, decoder_ctx_t *dec, const VdpPictureInfoH264 *info,
		     const struct slice_header *sh, struct refs *refs)
{
	int32_t max_frame_num = 1 << (info->log2_max_frame_num_minus4 + 4);
	unsigned int n = 0;

	refs->n = 0;
	for (unsigned int i = 0; i < 16; i++) {
		const VdpReferenceFrameH264 *rf = &info->referenceFrames[i];
		video_surface_ctx_t *s;
		struct v4l2_h264_dpb_entry *e;
		int32_t pic_num;

		if (rf->surface == VDP_INVALID_HANDLE)
			continue;
		s = handle_get(rf->surface);
		if (!s || decoder_surface_picture(dec, s) < 0) {
			DBG("reference %u: surface %u not decoded\n", i, rf->surface);
			continue;
		}

		if (rf->is_long_term)
			pic_num = rf->frame_idx;	/* LongTermPicNum */
		else
			pic_num = (int32_t)rf->frame_idx > (int32_t)sh->frame_num ?
				  (int32_t)rf->frame_idx - max_frame_num : (int32_t)rf->frame_idx;

		e = &c->decode.dpb[n];
		e->reference_ts = dec->capture[s->index].timestamp;
		e->pic_num = pic_num;
		e->frame_num = rf->frame_idx;
		e->fields = (rf->top_is_reference ? V4L2_H264_TOP_FIELD_REF : 0) |
			    (rf->bottom_is_reference ? V4L2_H264_BOTTOM_FIELD_REF : 0);
		if (!e->fields)
			e->fields = V4L2_H264_FRAME_REF;
		e->top_field_order_cnt = rf->field_order_cnt[0];
		e->bottom_field_order_cnt = rf->field_order_cnt[1];
		e->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |
			   (rf->is_long_term ? V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM : 0);

		refs->r[refs->n++] = (struct ref){
			.dpb = n,
			.long_term = rf->is_long_term,
			.pic_num = pic_num,
			.poc = rf->field_order_cnt[0] < rf->field_order_cnt[1] ?
			       rf->field_order_cnt[0] : rf->field_order_cnt[1],
		};
		n++;
	}
}

static void fill_ref_lists(h264_ctx_t *c, const VdpPictureInfoH264 *info,
			   const struct slice_header *sh, const struct refs *refs)
{
	int32_t cur_poc = info->field_order_cnt[0] < info->field_order_cnt[1] ?
			  info->field_order_cnt[0] : info->field_order_cnt[1];
	int32_t max_pic_num = 1 << (info->log2_max_frame_num_minus4 + 4);
	unsigned int lists = sh->slice_type == SLICE_B ? 2 : 1;
	int list[2][MAX_LIST + 1];
	unsigned int len[2];

	memset(c->slice.ref_pic_list0, NO_REF, sizeof(c->slice.ref_pic_list0));
	memset(c->slice.ref_pic_list1, NO_REF, sizeof(c->slice.ref_pic_list1));
	if (sh->slice_type == SLICE_I || sh->slice_type == SLICE_SI)
		return;

	/* 8.2.4.2: initial lists */
	for (unsigned int l = 0; l < lists; l++) {
		len[l] = init_list(refs, sh->slice_type, l, cur_poc, list[l]);
		for (unsigned int i = len[l]; i <= MAX_LIST; i++)
			list[l][i] = -1;
	}
	/* 8.2.4.2.3: RefPicList1 identical to RefPicList0 with > 1 entry -> swap */
	if (lists == 2 && len[1] > 1 && len[0] == len[1] &&
	    !memcmp(list[0], list[1], len[1] * sizeof(int))) {
		int t = list[1][0];

		list[1][0] = list[1][1];
		list[1][1] = t;
	}

	/* 8.2.4.3: modifications, then the V4L2 lists (DPB indices) */
	for (unsigned int l = 0; l < lists; l++) {
		unsigned int num_active = sh->num_ref_idx_active_minus1[l] + 1;
		struct v4l2_h264_reference *out = l ? c->slice.ref_pic_list1 : c->slice.ref_pic_list0;

		modify_list(refs, sh, l, max_pic_num, list[l], num_active);
		for (unsigned int i = 0; i < num_active; i++) {
			if (list[l][i] < 0)
				continue;	/* missing reference: index stays 0xff */
			out[i].index = refs->r[list[l][i]].dpb;
			out[i].fields = V4L2_H264_FRAME_REF;
		}
	}
}

/* Split the concatenated buffers at Annex-B start codes. */
static size_t next_nal(const uint8_t *data, size_t size, size_t pos, size_t *nal_start)
{
	size_t i = pos;

	while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1))
		i++;
	if (i + 3 > size)
		return 0;
	*nal_start = i + 3;
	for (i = *nal_start; i + 3 <= size; i++)
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
			break;
	if (i + 3 > size)
		i = size;
	/* trailing zero bytes belong to the next (4-byte) start code */
	while (i > *nal_start && data[i - 1] == 0)
		i--;
	return i - *nal_start;
}

VdpStatus h264_decode(decoder_ctx_t *dec, video_surface_ctx_t *target,
		      const VdpPictureInfoH264 *info,
		      uint32_t buffer_count, const VdpBitstreamBuffer *buffers)
{
	h264_ctx_t *c = dec->codec_priv;
	size_t total = 0, pos = 0, nal_start, nal_size;
	unsigned int slice_index = 0;
	struct refs refs = { .n = 0 };
	VdpStatus status = VDP_STATUS_OK;

	for (uint32_t i = 0; i < buffer_count; i++)
		total += buffers[i].bitstream_bytes;
	if (total > c->bitstream_size) {
		uint8_t *p = realloc(c->bitstream, total);

		if (!p)
			return VDP_STATUS_RESOURCES;
		c->bitstream = p;
		c->bitstream_size = total;
	}
	for (uint32_t i = 0; i < buffer_count; i++) {
		memcpy(c->bitstream + pos, buffers[i].bitstream, buffers[i].bitstream_bytes);
		pos += buffers[i].bitstream_bytes;
	}

	fill_sps_pps(c, dec, info);
	dec->capture[target->index].decoded = false;

	pos = 0;
	while ((nal_size = next_nal(c->bitstream, total, pos, &nal_start)) > 0) {
		size_t next_start;
		struct slice_header sh;
		struct v4l2_slice_job job = {
			.data = c->bitstream + nal_start,
			.size = nal_size,
			.first_slice = slice_index == 0,
			/* look ahead rather than trust slice_count: a wrong guess holds the buffer */
			.last_slice = !next_nal(c->bitstream, total, nal_start + nal_size, &next_start),
		};
		struct v4l2_ext_control ctrls[6];
		unsigned int n = 0;

		pos = nal_start + nal_size;
		if (parse_slice_header(job.data, job.size, info, &sh)) {
			status = VDP_STATUS_ERROR;
			if (slice_index == 0)
				break;	/* nothing queued yet */
			/* finish the picture with what we have (releases the CAPTURE buffer) */
			continue;
		}

		if (slice_index == 0) {
			memset(&c->decode, 0, sizeof(c->decode));
			fill_dpb(c, dec, info, &sh, &refs);
			c->decode.nal_ref_idc = sh.nal_ref_idc;
			c->decode.frame_num = sh.frame_num;
			c->decode.top_field_order_cnt = info->field_order_cnt[0];
			c->decode.bottom_field_order_cnt = info->field_order_cnt[1];
			c->decode.idr_pic_id = sh.idr_pic_id;
			c->decode.pic_order_cnt_lsb = sh.pic_order_cnt_lsb;
			c->decode.delta_pic_order_cnt_bottom = sh.delta_pic_order_cnt_bottom;
			c->decode.delta_pic_order_cnt0 = sh.delta_pic_order_cnt[0];
			c->decode.delta_pic_order_cnt1 = sh.delta_pic_order_cnt[1];
			c->decode.dec_ref_pic_marking_bit_size = sh.dec_ref_pic_marking_bit_size;
			c->decode.pic_order_cnt_bit_size = sh.pic_order_cnt_bit_size;
			if (sh.nal_unit_type == 5)
				c->decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
		}
		if (sh.slice_type == SLICE_P || sh.slice_type == SLICE_SP)
			c->decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
		if (sh.slice_type == SLICE_B)
			c->decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;

		memset(&c->slice, 0, sizeof(c->slice));
		c->slice.header_bit_size = sh.header_bit_size;
		c->slice.first_mb_in_slice = sh.first_mb_in_slice;
		c->slice.slice_type = sh.slice_type;
		c->slice.redundant_pic_cnt = sh.redundant_pic_cnt;
		c->slice.cabac_init_idc = sh.cabac_init_idc;
		c->slice.slice_qp_delta = sh.slice_qp_delta;
		c->slice.slice_qs_delta = sh.slice_qs_delta;
		c->slice.disable_deblocking_filter_idc = sh.disable_deblocking_filter_idc;
		c->slice.slice_alpha_c0_offset_div2 = sh.slice_alpha_c0_offset_div2;
		c->slice.slice_beta_offset_div2 = sh.slice_beta_offset_div2;
		c->slice.num_ref_idx_l0_active_minus1 = sh.num_ref_idx_active_minus1[0];
		c->slice.num_ref_idx_l1_active_minus1 = sh.num_ref_idx_active_minus1[1];
		if (sh.direct_spatial_mv_pred)
			c->slice.flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
		if (sh.sp_for_switch)
			c->slice.flags |= V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH;
		fill_ref_lists(c, info, &sh, &refs);
		c->pred = sh.pred;

		ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_SPS,
			.size = sizeof(c->sps), .ptr = &c->sps };
		ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_PPS,
			.size = sizeof(c->pps), .ptr = &c->pps };
		ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
			.size = sizeof(c->scaling), .ptr = &c->scaling };
		ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
			.size = sizeof(c->decode), .ptr = &c->decode };
		ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
			.size = sizeof(c->slice), .ptr = &c->slice };
		if (sh.has_pred_weights)
			ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS,
				.size = sizeof(c->pred), .ptr = &c->pred };

		if (decoder_submit_slice(dec, target, ctrls, n, &job)) {
			status = VDP_STATUS_ERROR;
			if (slice_index == 0)
				break;	/* the CAPTURE buffer may not even be queued */
		}
		slice_index++;
	}

	if (slice_index != info->slice_count) {
		ERR("found %u slices, VDPAU announced %u\n", slice_index, info->slice_count);
		status = VDP_STATUS_ERROR;
	}
	DBG("picture frame_num %u poc %d/%d slices %u refs %u -> %s\n", info->frame_num,
	    info->field_order_cnt[0], info->field_order_cnt[1], slice_index, refs.n,
	    status == VDP_STATUS_OK ? "ok" : "error");
	return status;
}

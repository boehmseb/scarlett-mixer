/* scarlett mixer GUI
 *
 * Copyright 2015,2016 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <alsa/asoundlib.h>

#define RTK_URI "http://gareus.org/oss/scarlettmixer#"
#define RTK_GUI "ui"

#define GD_WIDTH 41
#define GD_CX 20.5
#define GD_CY 15.5

/* device specifics, see also
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/sound/usb/mixer_scarlett.c#n635
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define DEVICE_NAME "Scarlett 18i6 USB"

/* scarlett matrix size */
#define SMI 18 // matrix ins
#define SMO 6  // matrix outs

/* scarlett I/O config */
#define SIN 18 // inputs (capture select)
#define SOUT 6 // outputs assigns (=?= matrix outs)
#endif

#define SMST 3 // output gain (stereo gain controls w/mute  =?= SOUT / 2)


typedef struct {
	snd_mixer_elem_t* elem;
	char* name;
} Mctrl;

typedef struct {
	RobWidget*   rw;
	RobWidget*   matrix;
	RobWidget*   output;
	RobTkSelect* mtx_sel[SMI];
	RobTkDial*   mtx_gain[SMI * SMO];
	RobTkLbl*    mtx_lbl[SMO];

	RobTkSep*    sep_h;
	RobTkSep*    sep_v;
	RobTkSep*    spc_v[2];

	RobTkLbl*    src_lbl[SIN];
	RobTkSelect* src_sel[SIN];

	RobTkSelect* out_sel[SOUT];
	RobTkLbl*    out_mst;
	RobTkLbl*    out_lbl[SMST];
	RobTkDial*   out_gain[SMST];

	RobTkDial*   mst_gain;
	RobTkCBtn*   btn_hiz[2];
	RobTkPBtn*   btn_reset;

	RobTkLbl*    heading[3];

	PangoFontDescription* font;
	cairo_surface_t*      mtx_sf[6];

	Mctrl*       ctrl;
	unsigned int ctrl_cnt;
	snd_mixer_t* mixer;

	int nfds;
	struct pollfd* pollfds;
	bool disable_signals;
} RobTkApp;


/* *****************************************************************************
 * Mapping for the 18i6
 *
 * NOTE: these are numerically hardcoded. see `amixer -D hw:2 control`
 * and #if'd "Print Controls" debug dump below
 */

/* mixer-matrix ; colums(src) x rows (dest) */
static Mctrl* matrix_ctrl_cr (RobTkApp* ui, unsigned int c, unsigned int r)
{
	/* Matrix 01 Mix A
	 *  ..
	 * Matrix 18 Mix F
	 */
	if (r >= SMI || c >= SMO) {
		return NULL;
	}
	unsigned int ctrl_id = 33 + r * 7 + c;
	return &ui->ctrl[ctrl_id];
}

/* wrapper to the above, linear lookup */
static Mctrl* matrix_ctrl_n (RobTkApp* ui, unsigned int n)
{
	unsigned c = n % SMO;
	unsigned r = n / SMO;
	return matrix_ctrl_cr (ui, c, r);
}

/* matrix input selector (per row)*/
static Mctrl* matrix_sel (RobTkApp* ui, unsigned int r)
{
	if (r >= SMI) {
		return NULL;
	}
	/* Matrix 01 Input, ENUM
	 *  ..
	 * Matrix 18 Input, ENUM
	 */
	unsigned int ctrl_id = 32 + r * 7;
	return &ui->ctrl[ctrl_id];
}

/* Input/Capture selector */
static Mctrl* src_sel (RobTkApp* ui, unsigned int r)
{
	if (r >= SIN) {
		return NULL;
	}
	/* Input Source 01, ENUM
	 *  ..
	 * Input Source 18, ENUM
	 */
	unsigned int ctrl_id = 13 + r;
	return &ui->ctrl[ctrl_id];
}

static int src_sel_default (unsigned int r, int max_values)
{
	/* 0 <= r < SIN;  return 0 .. max_values - 1 */
	return (r + 7) % max_values; // XXX hardcoded defaults. offset 7: "Analog 1"
}

/* Output Gains */
static Mctrl* out_gain (RobTkApp* ui, unsigned int c)
{
	switch (c) {
		case 0: return &ui->ctrl[1]; /* Master 1 (Monitor), PBS */
		case 1: return &ui->ctrl[4]; /* Master 2 (Headphone), PBS */
		case 2: return &ui->ctrl[7]; /* Master 3 (SPDIF), PBS */
	}
	return NULL;
}

static const char* out_gain_label (int n)
{
	switch (n) {
		case 0:
			return "Monitor";
		case 1:
			return "Phones";
		case 2:
			return "ADAT";
		default:
			return "??";
	}
}

/* Output Bus assignment (matrix-out to master) */
static Mctrl* out_sel (RobTkApp* ui, unsigned int c)
{
	switch (c) {
		case 0: return &ui->ctrl[2]; /* Master 1L (Monitor) Source, ENUM */
		case 1: return &ui->ctrl[3]; /* Master 1R (Monitor) Source, ENUM */
		case 2: return &ui->ctrl[5]; /* Master 2L (Headphone) Source, ENUM */
		case 3: return &ui->ctrl[6]; /* Master 2R (Headphone) Source, ENUM */
		case 4: return &ui->ctrl[8]; /* Master 3L (SPDIF) Source, ENUM */
		case 5: return &ui->ctrl[9]; /* Master 3R (SPDIF) Source, ENUM */
	}
	return NULL;
}

static int out_sel_default (unsigned int c)
{
	/* 0 <= c < SOUT; */
	return 25 + c; // XXX hardcoded defaults. offset 25: "Mix 1"
}

/* Hi-Z switches */
static Mctrl* hiz (RobTkApp* ui, unsigned int c)
{
	switch (c) {
		case 0: return &ui->ctrl[11]; /* Input 1 Impedance, ENUM */
		case 1: return &ui->ctrl[12]; /* Input 2 Impedance, ENUM */
	}
	return NULL;
}

/* master gain */
static Mctrl* mst_gain (RobTkApp* ui)
{
	return &ui->ctrl[0]; /* Master, PBS */
}


/* *****************************************************************************
 * *****************************************************************************
 *
 * CODE FROM HERE ON SHOULD BE GENERIC
 *
 * *****************************************************************************
 * ****************************************************************************/



/* *****************************************************************************
 * Alsa Mixer Interface
 */

static int open_mixer (RobTkApp* ui, const char* card)
{
	int err;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t *elem;
	snd_mixer_selem_id_alloca (&sid);

	snd_ctl_t *hctl;
	snd_ctl_card_info_t *card_info;
	snd_ctl_card_info_alloca (&card_info);

	if ((err = snd_ctl_open (&hctl, card, 0)) < 0) {
		fprintf (stderr, "Control device %s open error: %s\n", card, snd_strerror (err));
		return err;
	}
	if ((err = snd_ctl_card_info (hctl, card_info)) < 0) {
		fprintf (stderr, "Control device %s hw info error: %s\n", card, snd_strerror (err));
		return err;
	}
	const char* card_name = snd_ctl_card_info_get_name (card_info);
	snd_ctl_close (hctl);

	if (!card_name || strcmp (card_name, DEVICE_NAME)) {
		fprintf (stderr, "Device '%s' is a '%s' - expected '%s'\n", card, card_name ? card_name : "unknown", DEVICE_NAME);
		return -1;
	}

	if ((err = snd_mixer_open (&ui->mixer, 0)) < 0) {
		fprintf (stderr, "Mixer %s open error: %s\n", card, snd_strerror (err));
		return err;
	}
	if ((err = snd_mixer_attach (ui->mixer, card)) < 0) {
		fprintf (stderr, "Mixer attach %s error: %s\n", card, snd_strerror (err));
		snd_mixer_close (ui->mixer);
		return err;
	}
	if ((err = snd_mixer_selem_register (ui->mixer, NULL, NULL)) < 0) {
		fprintf (stderr, "Mixer register error: %s\n", snd_strerror (err));
		snd_mixer_close (ui->mixer);
		return err;
	}
	err = snd_mixer_load (ui->mixer);
	if (err < 0) {
		fprintf (stderr, "Mixer %s load error: %s\n", card, snd_strerror (err));
		snd_mixer_close (ui->mixer);
		return err;
	}

	int cnt = 0;

	for (elem = snd_mixer_first_elem (ui->mixer); elem; elem = snd_mixer_elem_next (elem)) {
		if (!snd_mixer_selem_is_active (elem)) {
			continue;
		}
		++cnt;
	}

	ui->ctrl_cnt = cnt;

	if (cnt == 0) {
		fprintf (stderr, "Mixer %s: no controls found\n", card);
		return -1;
	}

	ui->ctrl = (Mctrl*)calloc (cnt, sizeof (Mctrl));

	int i = 0;
	for (elem = snd_mixer_first_elem (ui->mixer); elem; elem = snd_mixer_elem_next (elem)) {
		if (!snd_mixer_selem_is_active (elem)) {
			continue;
		}

		Mctrl* c = &ui->ctrl[i];
		c->elem = elem;
		c->name = strdup (snd_mixer_selem_get_name (elem));

#if 0 // Print Controls
		printf ("%d %s", i, c->name);
		if (snd_mixer_selem_is_enumerated (elem)) { printf (", ENUM"); }
		if (snd_mixer_selem_has_playback_switch (elem)) { printf (", PBS"); }
		if (snd_mixer_selem_has_capture_switch (elem)) { printf (", CPS"); }
		printf ("\n");
#endif
		++i;
	}
	return 0;
}

static void close_mixer (RobTkApp* ui)
{
	for (unsigned int i = 0; i < ui->ctrl_cnt; ++i) {
		free (ui->ctrl[i].name);
	}
	free (ui->ctrl);
	snd_mixer_close (ui->mixer);
}

static void set_mute (Mctrl* c, bool muted)
{
	int v = muted ? 0 : 1;
	assert (c && snd_mixer_selem_has_playback_switch (c->elem));
	for (int chn = 0; chn <= 2; ++chn) {
		snd_mixer_selem_channel_id_t cid = (snd_mixer_selem_channel_id_t) chn;
		if (snd_mixer_selem_has_playback_channel (c->elem, cid)) {
			snd_mixer_selem_set_playback_switch (c->elem, cid, v);
		}
	}
}

static bool get_mute (Mctrl* c)
{
	int v = 0;
	assert (c && snd_mixer_selem_has_playback_switch (c->elem));
	snd_mixer_selem_get_playback_switch (c->elem, (snd_mixer_selem_channel_id_t)0, &v);
	return v == 0;
}

static float get_dB (Mctrl* c)
{
	assert (c);
	long val = 0;
	snd_mixer_selem_get_playback_dB (c->elem, (snd_mixer_selem_channel_id_t)0, &val);
	return val / 100.f;
}

static void set_dB (Mctrl* c, float dB)
{
	long val = 100.f * dB;
	for (int chn = 0; chn <= 2; ++chn) {
		snd_mixer_selem_channel_id_t cid = (snd_mixer_selem_channel_id_t) chn;
		if (snd_mixer_selem_has_playback_channel (c->elem, cid)) {
			snd_mixer_selem_set_playback_dB (c->elem, cid, val, /*playback*/0);
		}
		if (snd_mixer_selem_has_capture_channel (c->elem, cid)) {
			snd_mixer_selem_set_playback_dB (c->elem, cid, val, /*capture*/1);
		}
	}
}

static float get_dB_range (Mctrl* c, bool maximum)
{
	long min, max;
	min = max = 0;
	snd_mixer_selem_get_playback_dB_range (c->elem, &min, &max);
	if (maximum) {
		return max / 100.f;
	} else {
		return min / 100.f;
	}
}

static void set_enum (Mctrl* c, int v)
{
	assert (snd_mixer_selem_is_enumerated (c->elem));
	snd_mixer_selem_set_enum_item (c->elem, (snd_mixer_selem_channel_id_t)0, v);
}

static int get_enum (Mctrl* c)
{
	unsigned int idx = 0;
	assert (snd_mixer_selem_is_enumerated (c->elem));
	snd_mixer_selem_get_enum_item (c->elem, (snd_mixer_selem_channel_id_t)0, &idx);
	return idx;
}

/* *****************************************************************************
 * Helpers
 */

static float db_to_knob (float db)
{
	float k = (db + 128.f) / 228.75f;
  float s = k * sqrt (0.5) / (1 - k);
	return s * s;
}

static float knob_to_db (float v)
{
	// v = 0..1
	float db = sqrtf (v) / (sqrtf (0.5) + sqrtf (v)) * 228.75f - 128.f;
	if (db > 6.f) return 6.f;
	return rint (db);
}

/* *****************************************************************************
 * Callbacks
 */

static bool cb_btn_reset (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	/* toggle all values (force change) */

	for (int r = 0; r < SIN; ++r) {
		Mctrl* sctrl = src_sel (ui, r);
		int mcnt = snd_mixer_selem_get_enum_items (sctrl->elem);
		const int val = robtk_select_get_value (ui->src_sel[r]);
		set_enum (sctrl, (val + 1) % mcnt);
		set_enum (sctrl, val);
	}
	for (int r = 0; r < SMI; ++r) {
		Mctrl* sctrl = matrix_sel (ui, r);
		int mcnt = snd_mixer_selem_get_enum_items (sctrl->elem);
		const int val = robtk_select_get_value (ui->mtx_sel[r]);
		set_enum (sctrl, (val + 1) % mcnt);
		set_enum (sctrl, val);
	}
	for (unsigned int o = 0; o < SOUT; ++o) {
		Mctrl* sctrl = out_sel (ui, o);
		int mcnt = snd_mixer_selem_get_enum_items (sctrl->elem);
		const int val = robtk_select_get_value (ui->out_sel[o]);
		set_enum (sctrl, (val + 1) % mcnt);
		set_enum (sctrl, val);
	}

	for (int r = 0; r < SMI; ++r) {
		for (unsigned int c = 0; c < SMO; ++c) {
			unsigned int n = r * SMO + c;
			Mctrl* ctrl = matrix_ctrl_cr (ui, c, r);
			const float val = knob_to_db (robtk_dial_get_value (ui->mtx_gain[n]));
			if (val == -128) {
				set_dB (ctrl, 127);
			} else {
				set_dB (ctrl, -128);
			}
			set_dB (ctrl, val);
		}
	}
	for (unsigned int n = 0; n < SMST; ++n) {
		Mctrl* ctrl = out_gain (ui, n);
		const bool mute = robtk_dial_get_state (ui->out_gain[n]) == 1;
		const float val = knob_to_db (robtk_dial_get_value (ui->out_gain[n]));
		set_mute (ctrl, !mute);
		set_mute (ctrl, mute);
		if (val == -128) {
			set_dB (ctrl, 127);
		} else {
			set_dB (ctrl, -128);
		}
		set_dB (ctrl, val);
	}
	return TRUE;
}

static bool cb_set_hiz (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	if (ui->disable_signals) return TRUE;
	for (uint32_t i = 0; i < 2; ++i) {
		int val = robtk_cbtn_get_active (ui->btn_hiz[i]) ? 1 : 0;
		set_enum (hiz (ui, i), val);
	}
	return TRUE;
}

static bool cb_src_sel (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	if (ui->disable_signals) return TRUE;
	unsigned int n;
	memcpy (&n, w->name, sizeof (unsigned int));
	const float val = robtk_select_get_value (ui->src_sel[n]);
	set_enum (src_sel (ui, n), val);
	return TRUE;
}

static bool cb_mtx_src (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	if (ui->disable_signals) return TRUE;
	unsigned int n;
	memcpy (&n, w->name, sizeof (unsigned int));
	const float val = robtk_select_get_value (ui->mtx_sel[n]);
	set_enum (matrix_sel (ui, n), val);
	return TRUE;
}

static bool cb_mtx_gain (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	unsigned int n;
	memcpy (&n, w->name, sizeof (unsigned int));
	const float val = knob_to_db (robtk_dial_get_value (ui->mtx_gain[n]));
	if (val == -128) {
		ui->mtx_gain[n]->click_state = 1;
	} else if (val == 0) {
		ui->mtx_gain[n]->click_state = 2;
	} else {
		ui->mtx_gain[n]->click_state = 0;
	}
	if (ui->disable_signals) return TRUE;
	set_dB (matrix_ctrl_n (ui, n), val);
	return TRUE;
}

static bool cb_out_src (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	if (ui->disable_signals) return TRUE;
	unsigned int n;
	memcpy (&n, w->name, sizeof (unsigned int));
	const float val = robtk_select_get_value (ui->out_sel[n]);
	set_enum (out_sel (ui, n), val);
	return TRUE;
}

static bool cb_out_gain (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	if (ui->disable_signals) return TRUE;
	unsigned int n;
	memcpy (&n, w->name, sizeof (unsigned int));
	const bool mute = robtk_dial_get_state (ui->out_gain[n]) == 1;
	const float val = robtk_dial_get_value (ui->out_gain[n]);
	set_mute (out_gain (ui, n), mute);
	set_dB (out_gain (ui, n), knob_to_db (val));
	return TRUE;
}

static bool cb_mst_gain (RobWidget* w, void* handle) {
	RobTkApp* ui = (RobTkApp*)handle;
	if (ui->disable_signals) return TRUE;
	const bool mute = robtk_dial_get_state (ui->mst_gain) == 1;
	const float val = robtk_dial_get_value (ui->mst_gain);
	set_mute (mst_gain (ui), mute);
	set_dB (mst_gain (ui), knob_to_db (val));
	return TRUE;
}

/* *****************************************************************************
 * GUI Helpers
 */

static void set_select_values (RobTkSelect* s,  Mctrl* ctrl)
{
	if (!ctrl) return;
	assert (ctrl);
	int mcnt = snd_mixer_selem_get_enum_items (ctrl->elem);
	for (int i = 0; i < mcnt; ++i) {
		char name[64];
		if (snd_mixer_selem_get_enum_item_name (ctrl->elem, i, sizeof (name) - 1, name) < 0) {
			continue;
		}
		robtk_select_add_item (s, i, name);
	}
	robtk_select_set_value (s, get_enum (ctrl));
}

static void dial_annotation_db (RobTkDial* d, cairo_t* cr, void* data)
{
	RobTkApp* ui = (RobTkApp*)data;
	char txt[16];
	snprintf (txt, 16, "%+3.0fdB", knob_to_db (d->cur));

	int tw, th;
	cairo_save (cr);
	PangoLayout * pl = pango_cairo_create_layout (cr);
	pango_layout_set_font_description (pl, ui->font);
	pango_layout_set_text (pl, txt, -1);
	pango_layout_get_pixel_size (pl, &tw, &th);
	cairo_translate (cr, d->w_width / 2, d->w_height - 0);
	cairo_translate (cr, -tw / 2.0 , -th);
	cairo_set_source_rgba (cr, .0, .0, .0, .5);
	rounded_rectangle (cr, -1, -1, tw+3, th+1, 3);
	cairo_fill (cr);
	CairoSetSouerceRGBA (c_wht);
	pango_cairo_show_layout (cr, pl);
	g_object_unref (pl);
	cairo_restore (cr);
	cairo_new_path (cr);
}

static void create_faceplate (RobTkApp *ui) {
	cairo_t* cr;
	float c_bg[4]; get_color_from_theme (1, c_bg);

#define MTX_SF(SF)                                                             \
	SF = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, GD_WIDTH, GED_HEIGHT); \
	cr = cairo_create (SF);                                                      \
	cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);                              \
	cairo_rectangle (cr, 0, 0, GD_WIDTH, GED_HEIGHT);                            \
	CairoSetSouerceRGBA (c_bg);                                                  \
	cairo_fill (cr);                                                             \
	CairoSetSouerceRGBA (c_g60);                                                 \
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);                                \
	cairo_set_line_width (cr, 1.0);

#define MTX_ARROW_H               \
	cairo_move_to (cr, 5, GD_CY);   \
	cairo_rel_line_to (cr, -5, -4); \
	cairo_rel_line_to (cr, 0, 8);   \
	cairo_close_path (cr);          \
	cairo_fill (cr);

#define MTX_ARROW_V                      \
	cairo_move_to (cr, GD_CX, GED_HEIGHT); \
	cairo_rel_line_to (cr, -4, -5);        \
	cairo_rel_line_to (cr, 8, 0);          \
	cairo_close_path (cr);                 \
	cairo_fill (cr);

	MTX_SF (ui->mtx_sf[0]);
	MTX_ARROW_H;
	MTX_ARROW_V;

	cairo_move_to (cr, 0, GD_CY);
	cairo_line_to (cr, GD_WIDTH, GD_CY);
	cairo_stroke (cr);
	cairo_move_to (cr, GD_CX, 0);
	cairo_line_to (cr, GD_CX, GED_HEIGHT);
	cairo_stroke (cr);
	cairo_destroy (cr);

	// top-row
	MTX_SF (ui->mtx_sf[1]);
	MTX_ARROW_H;
	MTX_ARROW_V;

	cairo_move_to (cr, 0, GD_CY);
	cairo_line_to (cr, GD_WIDTH, GD_CY);
	cairo_stroke (cr);
	cairo_move_to (cr, GD_CX, GD_CY);
	cairo_line_to (cr, GD_CX, GED_HEIGHT);
	cairo_stroke (cr);
	cairo_destroy (cr);

	// left column
	MTX_SF (ui->mtx_sf[2]);
	MTX_ARROW_V;

	cairo_move_to (cr, 0, GD_CY);
	cairo_line_to (cr, GD_WIDTH, GD_CY);
	cairo_stroke (cr);
	cairo_move_to (cr, GD_CX, 0);
	cairo_line_to (cr, GD_CX, GED_HEIGHT);
	cairo_stroke (cr);
	cairo_destroy (cr);

	// right column
	MTX_SF (ui->mtx_sf[3]);
	MTX_ARROW_H;
	MTX_ARROW_V;

	cairo_move_to (cr, 0, GD_CY);
	cairo_line_to (cr, GD_CX, GD_CY);
	cairo_stroke (cr);
	cairo_move_to (cr, GD_CX, 0);
	cairo_line_to (cr, GD_CX, GED_HEIGHT);
	cairo_stroke (cr);
	cairo_destroy (cr);

	// top-left
	MTX_SF (ui->mtx_sf[4]);
	MTX_ARROW_V;

	cairo_move_to (cr, 0, GD_CY);
	cairo_line_to (cr, GD_WIDTH, GD_CY);
	cairo_stroke (cr);
	cairo_move_to (cr, GD_CX, GD_CY);
	cairo_line_to (cr, GD_CX, GED_HEIGHT);
	cairo_stroke (cr);
	cairo_destroy (cr);

	// top-right
	MTX_SF (ui->mtx_sf[5]);
	MTX_ARROW_H;
	MTX_ARROW_V;

	cairo_move_to (cr, 0, GD_CY);
	cairo_line_to (cr, GD_CX, GD_CY);
	cairo_stroke (cr);
	cairo_move_to (cr, GD_CX, GD_CY);
	cairo_line_to (cr, GD_CX, GED_HEIGHT);
	cairo_stroke (cr);
	cairo_destroy (cr);
}

static RobWidget* robtk_dial_mouse_intercept (RobWidget* handle, RobTkBtnEvent *ev) {
	RobTkDial* d = (RobTkDial *)GET_HANDLE (handle);
	RobTkApp* ui = (RobTkApp*)d->handle;
	if (!d->sensitive) { return NULL; }

	if (ev->button == 2) {
		/* middle-click exclusively assign output */
		unsigned int n;
		memcpy (&n, d->rw->name, sizeof (unsigned int));

		unsigned c = n % SMO;
		unsigned r = n / SMO;
		for (uint32_t i = 0; i < SMO; ++i) {
			unsigned int nn = r * SMO + i;
			if (i == c) {
				if (d->cur == 0) {
					robtk_dial_set_value (ui->mtx_gain[nn], db_to_knob (0));
				} else {
					robtk_dial_set_value (ui->mtx_gain[nn], 0);
				}
			} else {
				robtk_dial_set_value (ui->mtx_gain[nn], 0);
			}
		}
		return handle;
	}
	return robtk_dial_mousedown (handle, ev);
}

/* *****************************************************************************
 * GUI
 */

static RobWidget* toplevel (RobTkApp* ui, void* const top) {
	ui->rw = rob_vbox_new (FALSE, 2);
	robwidget_make_toplevel (ui->rw, top);

	create_faceplate (ui);
	ui->font = pango_font_description_from_string ("Mono 9px");

	const int c0 = 4; // matrix column offset
	const int rb = 2 + SMI; // matrix bottom

	/* table layout. NB: these are min sizes, table grows if needed */
	ui->matrix = rob_table_new (/*rows*/rb, /*cols*/ 5 + SMO, FALSE);
	ui->output = rob_table_new (/*rows*/3,  /*cols*/ 2 + 3 * SMST, FALSE);

	/* headings */
	ui->heading[0]  = robtk_lbl_new ("Capture");
	rob_table_attach (ui->matrix, robtk_lbl_widget (ui->heading[0]), 2, 3, 0, 1, 2, 6, RTK_EXANDF, RTK_SHRINK);
	ui->heading[1]  = robtk_lbl_new ("Source");
	rob_table_attach (ui->matrix, robtk_lbl_widget (ui->heading[1]), c0, c0 + 1, 0, 1, 2, 6, RTK_SHRINK, RTK_SHRINK);
	ui->heading[2]  = robtk_lbl_new ("Matrix Mixer");
	rob_table_attach (ui->matrix, robtk_lbl_widget (ui->heading[2]), c0 + 1, c0 + 1 + SMO, 0, 1, 2, 6, RTK_SHRINK, RTK_SHRINK);

	/* input selectors */
	unsigned int r;
	for (r = 0; r < SIN; ++r) {
		char txt[8];
		sprintf (txt, "%d", r + 1);
		ui->src_lbl[r] = robtk_lbl_new (txt);
		rob_table_attach (ui->matrix, robtk_lbl_widget (ui->src_lbl[r]), 1, 2, r + 1, r + 2, 2, 2, RTK_SHRINK, RTK_SHRINK);

		ui->src_sel[r] = robtk_select_new ();
		Mctrl* sctrl = src_sel (ui, r);
		int mcnt = snd_mixer_selem_get_enum_items (sctrl->elem);
		set_select_values (ui->src_sel[r], sctrl);
		robtk_select_set_default_item (ui->src_sel[r], src_sel_default (r, mcnt));
		robtk_select_set_callback (ui->src_sel[r], cb_src_sel, ui);

		rob_table_attach (ui->matrix, robtk_select_widget (ui->src_sel[r]), 2, 3, r + 1, r + 2, 2, 2, RTK_SHRINK, RTK_SHRINK);
		// hack alert, abusing the name filed -- should add a .data field to Robwidget
		memcpy (ui->src_sel[r]->rw->name, &r, sizeof (unsigned int));
	}

	/* hidden spacers left/right */
	ui->spc_v[0] = robtk_sep_new (FALSE);
	robtk_sep_set_linewidth (ui->spc_v[0], 0);
	rob_table_attach (ui->matrix, robtk_sep_widget (ui->spc_v[0]), 0, 1, 0, rb, 0, 0, RTK_EXANDF, RTK_FILL);
	ui->spc_v[1] = robtk_sep_new (FALSE);
	robtk_sep_set_linewidth (ui->spc_v[1], 0);
	rob_table_attach (ui->matrix, robtk_sep_widget (ui->spc_v[1]), c0 + 1 + SMO, c0 + 2 + SMO, 0, rb, 0, 0, RTK_EXANDF, RTK_FILL);

	/* vertical separator line between inputs and matrix (c0-1 .. c0)*/
	ui->sep_v = robtk_sep_new (FALSE);
	rob_table_attach (ui->matrix, robtk_sep_widget (ui->sep_v), 3, 4, 0, rb, 10, 0, RTK_SHRINK, RTK_FILL);

	/* matrix */
	for (r = 0; r < SMI; ++r) {
		ui->mtx_sel[r] = robtk_select_new ();

		Mctrl* sctrl = matrix_sel (ui, r);
		set_select_values (ui->mtx_sel[r], sctrl);
		robtk_select_set_default_item (ui->mtx_sel[r], 1 + r); // XXX defaults (0 == off)
		robtk_select_set_callback (ui->mtx_sel[r], cb_mtx_src, ui);

		rob_table_attach (ui->matrix, robtk_select_widget (ui->mtx_sel[r]), c0, c0 + 1, r + 1, r + 2, 2, 2, RTK_SHRINK, RTK_SHRINK);
		memcpy (ui->mtx_sel[r]->rw->name, &r, sizeof (unsigned int));

		for (unsigned int c = 0; c < SMO; ++c) {
			unsigned int n = r * SMO + c;
			Mctrl* ctrl = matrix_ctrl_cr (ui, c, r);
			assert (ctrl);
			ui->mtx_gain[n] = robtk_dial_new_with_size (
					0, 1, 1.f / 80.f,
					GD_WIDTH, GED_HEIGHT, GD_CX, GD_CY, GED_RADIUS);
			robtk_dial_set_default (ui->mtx_gain[n], db_to_knob (0));
			robtk_dial_set_value (ui->mtx_gain[n], db_to_knob (get_dB (ctrl)));
			robtk_dial_set_callback (ui->mtx_gain[n], cb_mtx_gain, ui);
			robtk_dial_annotation_callback (ui->mtx_gain[n], dial_annotation_db, ui);
			robwidget_set_mousedown (ui->mtx_gain[n]->rw, robtk_dial_mouse_intercept);
			ui->mtx_gain[n]->displaymode = 3;
			if (0 == robtk_dial_get_value (ui->mtx_gain[n])) {
				ui->mtx_gain[n]->click_state = 1;
			}
			else if (0 == knob_to_db (robtk_dial_get_value (ui->mtx_gain[n]))) {
				ui->mtx_gain[n]->click_state = 2;
			}

			if (c == (SMO - 1) && r == 0) {
				robtk_dial_set_surface (ui->mtx_gain[n], ui->mtx_sf[5]);
			}
			else if (c == 0 && r == 0) {
				robtk_dial_set_surface (ui->mtx_gain[n], ui->mtx_sf[4]);
			}
			else if (c == (SMO - 1)) {
				robtk_dial_set_surface (ui->mtx_gain[n], ui->mtx_sf[3]);
			}
			else if (c == 0) {
				robtk_dial_set_surface (ui->mtx_gain[n], ui->mtx_sf[2]);
			}
			else if (r == 0) {
				robtk_dial_set_surface (ui->mtx_gain[n], ui->mtx_sf[1]);
			}
			else {
				robtk_dial_set_surface (ui->mtx_gain[n], ui->mtx_sf[0]);
			}

			rob_table_attach (ui->matrix, robtk_dial_widget (ui->mtx_gain[n]), c0 + c + 1, c0 + c + 2, r + 1, r + 2, 0, 0, RTK_SHRINK, RTK_SHRINK);

			memcpy (ui->mtx_gain[n]->rw->name, &n, sizeof (unsigned int));
		}
	}

	/* matrix out labels */
	for (unsigned int c = 0; c < SMO; ++c) {
		char txt[8];
		sprintf (txt, "Mix %c", 'A' + c);
		ui->mtx_lbl[c]  = robtk_lbl_new (txt);
		rob_table_attach (ui->matrix, robtk_lbl_widget (ui->mtx_lbl[c]), c0 + c + 1, c0 + c + 2, r + 1, r + 2, 2, 2, RTK_SHRINK, RTK_SHRINK);
	}


	/*** output Table ***/

	/* master level */
	ui->out_mst = robtk_lbl_new ("Master");
	rob_table_attach (ui->output, robtk_lbl_widget (ui->out_mst), 0, 2, 0, 1, 2, 2, RTK_SHRINK, RTK_SHRINK);
	{
		Mctrl* ctrl = mst_gain (ui);
		ui->mst_gain = robtk_dial_new_with_size (
				0, 1, 1.f / 80.f,
				75, 50, 37.5, 22.5, 20);

		robtk_dial_enable_states (ui->mst_gain, 1);
		robtk_dial_set_state_color (ui->mst_gain, 1, .5, .2, .2, 1.0);

		robtk_dial_set_default (ui->mst_gain, db_to_knob (0));
		robtk_dial_set_default_state (ui->mst_gain, 0);

		robtk_dial_set_value (ui->mst_gain, db_to_knob (get_dB (ctrl)));
		robtk_dial_set_state (ui->mst_gain, get_mute (ctrl) ? 1 : 0);
		robtk_dial_set_callback (ui->mst_gain, cb_mst_gain, ui);
		robtk_dial_annotation_callback (ui->mst_gain, dial_annotation_db, ui);
		rob_table_attach (ui->output, robtk_dial_widget (ui->mst_gain), 0, 2, 1, 3, 2, 0, RTK_SHRINK, RTK_SHRINK);
	}

	/* output level + labels */
	for (unsigned int o = 0; o < SMST; ++o) {
		ui->out_lbl[o]  = robtk_lbl_new (out_gain_label (o));
		rob_table_attach (ui->output, robtk_lbl_widget (ui->out_lbl[o]), 3 * o + 2, 3 * o + 5, 0, 1, 2, 2, RTK_SHRINK, RTK_SHRINK);

		Mctrl* ctrl = out_gain (ui, o);
		ui->out_gain[o] = robtk_dial_new_with_size (
				0, 1, 1.f / 80.f,
				65, 40, 32.5, 17.5, 15);

		robtk_dial_enable_states (ui->out_gain[o], 1);
		robtk_dial_set_state_color (ui->out_gain[o], 1, .5, .3, .1, 1.0);

		robtk_dial_set_default (ui->out_gain[o], db_to_knob (0));
		robtk_dial_set_default_state (ui->out_gain[o], 0);

		robtk_dial_set_value (ui->out_gain[o], db_to_knob (get_dB (ctrl)));
		robtk_dial_set_state (ui->out_gain[o], get_mute (ctrl) ? 1 : 0);
		robtk_dial_set_callback (ui->out_gain[o], cb_out_gain, ui);
		robtk_dial_annotation_callback (ui->out_gain[o], dial_annotation_db, ui);
		rob_table_attach (ui->output, robtk_dial_widget (ui->out_gain[o]), 3 * o + 2, 3 * o + 5, 1, 2, 2, 0, RTK_SHRINK, RTK_SHRINK);

		memcpy (ui->out_gain[o]->rw->name, &o, sizeof (unsigned int));
	}


	/* Hi-Z*/
	for (unsigned int i = 0; i < 2; ++i) {
		ui->btn_hiz[i] = robtk_cbtn_new ("HiZ", GBT_LED_LEFT, false);
		robtk_cbtn_set_active (ui->btn_hiz[i], get_enum (hiz (ui, i)) == 1);
		robtk_cbtn_set_callback (ui->btn_hiz[i], cb_set_hiz, ui);
		rob_table_attach (ui->output, robtk_cbtn_widget (ui->btn_hiz[i]),
				i, i + 1, 3, 4, 0, 0, RTK_SHRINK, RTK_SHRINK);
	}

	/* output selectors */
	for (unsigned int o = 0; o < SOUT; ++o) {
		ui->out_sel[o] = robtk_select_new ();
		Mctrl* sctrl = out_sel (ui, o);
		set_select_values (ui->out_sel[o], sctrl);
		robtk_select_set_default_item (ui->out_sel[o], out_sel_default (o));
		robtk_select_set_callback (ui->out_sel[o], cb_out_src, ui);

		memcpy (ui->out_sel[o]->rw->name, &o, sizeof (unsigned int));
		int pc = 3 * (o / 2); /* stereo-pair column */
		if (o & 1) {
			/* right channel */
			rob_table_attach (ui->output, robtk_select_widget (ui->out_sel[o]), 3 + pc, 5 + pc, 3, 4, 2, 2, RTK_SHRINK, RTK_SHRINK);
		} else {
			/* left channel */
			rob_table_attach (ui->output, robtk_select_widget (ui->out_sel[o]), 2 + pc, 4 + pc, 2, 3, 2, 2, RTK_SHRINK, RTK_SHRINK);
		}

	}

#if 1
	/* re-send */
	ui->btn_reset = robtk_pbtn_new ("R");
	rob_table_attach (ui->output, robtk_pbtn_widget (ui->btn_reset), 1 + 3 * (SOUT / 2), 2 + 3 * (SOUT / 2), 2, 3, 2, 2, RTK_SHRINK, RTK_SHRINK);
	robtk_pbtn_set_callback_up (ui->btn_reset, cb_btn_reset, ui);
#endif

	ui->sep_h = robtk_sep_new (TRUE);

	/* top-level packing */
	rob_vbox_child_pack (ui->rw, ui->matrix, TRUE, TRUE);
	rob_vbox_child_pack (ui->rw, robtk_sep_widget (ui->sep_h), TRUE, TRUE);
	rob_vbox_child_pack (ui->rw, ui->output, TRUE, TRUE);
	return ui->rw;
}

static void gui_cleanup (RobTkApp* ui) {

	close_mixer (ui);
	free (ui->pollfds);

	for (int i = 0; i < SIN; ++i) {
		robtk_select_destroy (ui->src_sel[i]);
		robtk_lbl_destroy (ui->src_lbl[i]);
	}
	for (int r = 0; r < SMI; ++r) {
		robtk_select_destroy (ui->mtx_sel[r]);
		for (int c = 0; c < SMO; ++c) {
			robtk_dial_destroy (ui->mtx_gain[r * SMO + c]);
		}
	}
	for (int i = 0; i < SMO; ++i) {
		robtk_lbl_destroy (ui->mtx_lbl[i]);
	}
	for (int i = 0; i < SOUT; ++i) {
		robtk_select_destroy (ui->out_sel[i]);
	}
	for (int i = 0; i < SMST; ++i) {
		robtk_lbl_destroy (ui->out_lbl[i]);
		robtk_dial_destroy (ui->out_gain[i]);
	}

	for (int i = 0; i < 3; ++i) {
		robtk_lbl_destroy (ui->heading[i]);
	}
	for (int i = 0; i < 6; ++i) {
		cairo_surface_destroy (ui->mtx_sf[i]);
	}

	robtk_lbl_destroy (ui->out_mst);
	robtk_dial_destroy (ui->mst_gain);
	robtk_cbtn_destroy (ui->btn_hiz[0]);
	robtk_cbtn_destroy (ui->btn_hiz[1]);

	robtk_sep_destroy (ui->sep_v);
	robtk_sep_destroy (ui->sep_h);
	robtk_sep_destroy (ui->spc_v[0]);
	robtk_sep_destroy (ui->spc_v[1]);

	rob_table_destroy (ui->output);
	rob_table_destroy (ui->matrix);
	rob_box_destroy (ui->rw);

	pango_font_description_free (ui->font);
}

/* *****************************************************************************
 * RobTk + LV2
 */

#define LVGL_RESIZEABLE

static void ui_enable (LV2UI_Handle handle) { }
static void ui_disable (LV2UI_Handle handle) { }

static LV2UI_Handle
instantiate (
		void* const               ui_toplevel,
		const LV2UI_Descriptor*   descriptor,
		const char*               plugin_uri,
		const char*               bundle_path,
		LV2UI_Write_Function      write_function,
		LV2UI_Controller          controller,
		RobWidget**               widget,
		const LV2_Feature* const* features)
{
	RobTkApp* ui = (RobTkApp*) calloc (1,sizeof (RobTkApp));
	char const* card = "hw:2";

	struct _rtkargv { int argc; char **argv; };
	struct _rtkargv* rtkargv = NULL;

	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, "http://gareus.org/oss/lv2/robtk#argv")) {
			rtkargv = (struct _rtkargv*)features[i]->data;
		}
	}

	if (rtkargv && rtkargv->argc > 1) {
		card = rtkargv->argv[1];
	}

	// TODO probe all devices find the first matching DEVICE_NAME

	if (open_mixer (ui, card)) {
		free (ui);
		return 0;
	}

	// TODO check if device is an 18i6

	ui->disable_signals = true;
	*widget = toplevel (ui, ui_toplevel);
	ui->disable_signals = false;
	return ui;
}

static enum LVGLResize
plugin_scale_mode (LV2UI_Handle handle)
{
	return LVGL_LAYOUT_TO_FIT;
}

static void
cleanup (LV2UI_Handle handle)
{
	RobTkApp* ui = (RobTkApp*)handle;
	gui_cleanup (ui);
	free (ui);
}

static const void*
extension_data (const char* uri)
{
	return NULL;
}

static void
port_event (LV2UI_Handle handle,
            uint32_t     port_index,
            uint32_t     buffer_size,
            uint32_t     format,
            const void*  buffer)
{
	RobTkApp* ui = (RobTkApp*)handle;
	assert (ui->mixer);

	int n = snd_mixer_poll_descriptors_count (ui->mixer);
	unsigned short revents;

	if (n != ui->nfds) {
		free (ui->pollfds);
		ui->nfds = n;
		ui->pollfds = (struct pollfd*)calloc (n, sizeof (struct pollfd));
	}
	if (snd_mixer_poll_descriptors (ui->mixer, ui->pollfds, n) < 0) {
		return;
	}
	n = poll (ui->pollfds, ui->nfds, 0);
	if (n <= 0) {
		return;
	}

	if (snd_mixer_poll_descriptors_revents (ui->mixer, ui->pollfds, n, &revents) < 0) {
		fprintf (stderr, "cannot get poll events\n");
		robtk_close_self (ui->rw->top);
	}
	if (revents & (POLLERR | POLLNVAL)) {
		fprintf (stderr, "Poll error\n");
		robtk_close_self (ui->rw->top);
	}
	else if (revents & POLLIN) {
		snd_mixer_handle_events (ui->mixer);
	}

	/* simply update the complete GUI (on any change) */

	ui->disable_signals = true;
	Mctrl* ctrl;

	for (unsigned int r = 0; r < SIN; ++r) {
		ctrl = src_sel (ui, r);
		robtk_select_set_value (ui->src_sel[r], get_enum (ctrl));
	}

	for (unsigned int r = 0; r < SMI; ++r) {
		ctrl = matrix_sel (ui, r);
		robtk_select_set_value (ui->mtx_sel[r], get_enum (ctrl));

		for (unsigned int c = 0; c < SMO; ++c) {
			unsigned int n = r * SMO + c;
			ctrl = matrix_ctrl_cr (ui, c, r);
			robtk_dial_set_value (ui->mtx_gain[n], db_to_knob (get_dB (ctrl)));
		}
	}

	for (unsigned int o = 0; o < SMST; ++o) {
		ctrl = out_gain (ui, o);
		robtk_dial_set_value (ui->out_gain[o], db_to_knob (get_dB (ctrl)));
		robtk_dial_set_state (ui->out_gain[o], get_mute (ctrl) ? 1 : 0);
	}

	ctrl = mst_gain (ui);
	robtk_dial_set_value (ui->mst_gain, db_to_knob (get_dB (ctrl)));
	robtk_dial_set_state (ui->mst_gain, get_mute (ctrl) ? 1 : 0);

	for (unsigned int i = 0; i < 2; ++i) {
		robtk_cbtn_set_active (ui->btn_hiz[i], get_enum (hiz (ui, i)) == 1);
	}

	for (unsigned int o = 0; o < SOUT; ++o) {
		ctrl = out_sel (ui, o);
		robtk_select_set_value (ui->out_sel[o], get_enum (ctrl));
	}

	ui->disable_signals = false;
}
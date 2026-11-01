// SPDX-License-Identifier: GPL-2.0+
/*
 * hakmenu: HakiOS's start screen on a phone. A title and two big buttons drawn straight into the framebuffer:
 * the first (red-orange, the default) boots iOS, the second reboots. Volume up/down move, power selects
 * (CONFIG_BUTTON_REMAP_PHONE_KEYS), and on a keyboard the arrows and Enter do the same.
 *
 *   hakmenu "<title>" "<subtitle>" "<label 1>" "<command 1>" "<label 2>" "<command 2>"
 *
 * Returns after running the chosen command (the caller loops).
 */

#include <command.h>
#include <cli.h>
#include <dm.h>
#include <menu.h>
#include <video.h>
#include <video_font.h>
#include <linux/delay.h>
#include <watchdog.h>
#include <stdio.h>

#define C_BG		0xff101114
#define C_TITLE		0xffffffff
#define C_SUB		0xffb8bcc8
#define C_LINE		0xff3a3d46
#define C_SEL		0xffff4a1c	/* red-orange, the button that boots iOS when selected */
#define C_SEL_EDGE	0xffff8a5c
#define C_OFF		0xff4a4e58
#define C_TXT		0xffffffff
#define C_HINT		0xff8e93a0

struct canvas {
	u32 *fb;
	int w, h, stride;	/* stride in pixels */
	const struct video_fontdata *font;
};

static void fill(struct canvas *c, int x, int y, int w, int h, u32 col)
{
	for (int j = max(y, 0); j < min(y + h, c->h); j++) {
		u32 *row = c->fb + (ulong)j * c->stride;

		for (int i = max(x, 0); i < min(x + w, c->w); i++)
			row[i] = col;
	}
}

/* a rectangle with its four corners cut round (radius r) */
static void round_rect(struct canvas *c, int x, int y, int w, int h, int r, u32 col)
{
	for (int j = 0; j < h; j++) {
		int inset = 0;
		int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);

		if (dy) {
			while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r)
				inset++;
		}
		fill(c, x + inset, y + j, w - 2 * inset, 1, col);
	}
}

static void glyph(struct canvas *c, int x, int y, int scale, unsigned char ch, u32 col)
{
	const struct video_fontdata *f = c->font;
	const u8 *bits = f->video_fontdata + (ulong)ch * f->char_pixel_bytes;

	for (int row = 0; row < f->height; row++) {
		for (int bit = 0; bit < f->width; bit++) {
			u8 b = bits[row * f->byte_width + bit / 8];

			if (b & (0x80 >> (bit % 8)))
				fill(c, x + bit * scale, y + row * scale, scale, scale, col);
		}
	}
}

static int text_width(struct canvas *c, const char *s, int scale)
{
	return strlen(s) * c->font->width * scale;
}

static void text_center(struct canvas *c, int cx, int y, int scale, const char *s, u32 col)
{
	int x = cx - text_width(c, s, scale) / 2;

	for (; *s; s++, x += c->font->width * scale)
		glyph(c, x, y, scale, (unsigned char)*s, col);
}

static void text_at(struct canvas *c, int x, int y, int scale, const char *s, u32 col)
{
	for (; *s; s++, x += c->font->width * scale)
		glyph(c, x, y, scale, (unsigned char)*s, col);
}

static void draw(struct udevice *vid, struct canvas *c, char *const argv[], int sel)
{
	int cx = c->w / 2, bw = c->w * 82 / 100, bh = c->h / 14, bx = (c->w - bw) / 2;
	int y = c->h / 5;
	int fh = c->font->height;

	fill(c, 0, 0, c->w, c->h, C_BG);
	text_center(c, cx, y, 4, argv[1], C_TITLE);
	y += fh * 4 + fh / 2;
	text_center(c, cx, y, 2, argv[2], C_SUB);
	y += fh * 2 + fh;
	fill(c, bx, y, bw, 3, C_LINE);
	y += fh * 2;

	for (int i = 0; i < 2; i++) {
		int on = (sel == i);
		char num[2] = { '1' + i, 0 };
		int ty = y + (bh - fh * 2) / 2;

		if (on)
			round_rect(c, bx - 4, y - 4, bw + 8, bh + 8, 22, C_SEL_EDGE);
		round_rect(c, bx, y, bw, bh, 18, on ? C_SEL : C_OFF);
		text_at(c, bx + fh, ty, 2, num, C_TXT);
		text_center(c, cx + fh, ty, 2, argv[3 + 2 * i], C_TXT);
		y += bh + fh * 2;
	}

	text_center(c, cx, y + fh, 2, "VOL KEYS MOVE, POWER SELECTS", C_HINT);
	video_sync(vid, true);
}

static int do_hakmenu(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct cli_ch_state s_cch, *cch = &s_cch;
	struct video_priv *priv;
	struct udevice *vid;
	struct canvas c;
	int sel = 0;

	if (argc != 7)
		return CMD_RET_USAGE;
	if (uclass_first_device_err(UCLASS_VIDEO, &vid))
		return CMD_RET_FAILURE;
	priv = dev_get_uclass_priv(vid);
	if (priv->bpix != VIDEO_BPP32)
		return CMD_RET_FAILURE;

	c.fb = priv->fb;
	c.w = priv->xsize;
	c.h = priv->ysize;
	c.stride = priv->line_length / 4;
	c.font = &fonts[0];

	cli_ch_init(cch);
	for (;;) {
		enum bootmenu_key key;
		int ch, err = 0;

		draw(vid, &c, argv, sel);

		ch = cli_ch_process(cch, 0);
		while (!ch && !tstc()) {
			schedule();
			mdelay(10);
			ch = cli_ch_process(cch, err);
			err = -ETIMEDOUT;
		}
		if (!ch)
			ch = cli_ch_process(cch, getchar());
		key = bootmenu_conv_key(ch);

		if (key == BKEY_UP || key == BKEY_DOWN)
			sel ^= 1;
		else if (ch == '1' || ch == '2')
			sel = ch - '1';

		if (key == BKEY_SELECT || ch == '\r' || ch == '1' || ch == '2') {
			fill(&c, 0, 0, c.w, c.h, 0xff000000);
			video_sync(vid, true);
			return run_command(argv[4 + 2 * sel], 0) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
		}
	}
}

U_BOOT_CMD(hakmenu, 7, 0, do_hakmenu,
	   "HakiOS start screen: two buttons on the framebuffer",
	   "\"<title>\" \"<subtitle>\" \"<label 1>\" \"<command 1>\" \"<label 2>\" \"<command 2>\"");

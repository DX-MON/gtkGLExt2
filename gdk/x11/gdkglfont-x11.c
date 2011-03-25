/* GdkGLExt - OpenGL Extension to GDK
 * Copyright (C) 2002-2004	Naofumi Yasufuku
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307	USA.
 */

#include <string.h>
#define __USE_ISOC99
#include <math.h>

#include <pango/pango.h>
#include <pango/pangoft2.h>

#include "gdkglx.h"
#include "gdkglprivate-x11.h"
#include "gdkglfont.h"

#ifdef GDKGLEXT_MULTIHEAD_SUPPORT
#include <gdk/gdkdisplay.h>
#endif /* GDKGLEXT_MULTIHEAD_SUPPORT */

// Use of uint64_t to future-proof the code in case someone wants to call with a ridiculously big font-size..
typedef struct _BitStream
{
	uint8_t *Data;
	uint64_t NumBits;
	uint64_t Size;
	uint64_t CurrentBit;
} BitStream;

BitStream *OpenBitStream(uint64_t size, uint8_t *buffer)
{
	BitStream *BS = malloc(sizeof(BitStream));
	BS->Size = size;
	BS->NumBits = size * 8;
	BS->CurrentBit = 0;
	BS->Data = buffer;
	return BS;
}

void CloseBitStream(BitStream *BS)
{
	free(BS);
}

void PutBit(BitStream *BS, uint8_t Bit)
{
	uint64_t Byte;
	if (BS->CurrentBit == BS->NumBits)
		return; // Protect against buffer overruns/overflows..
	Byte = BS->CurrentBit / 8;
	BS->Data[Byte] <<= 1;
	BS->Data[Byte] |= Bit;
	BS->CurrentBit++;
}

uint8_t *_flip_bitmap_y(uint8_t * const _in, uint32_t width, uint32_t height, uint32_t pitch)
{
	uint32_t i, j, height_less_one = height - 1, size = pitch * height;
	uint8_t *_out = malloc(size);

	for (i = 0; i < height; i++)
	{
		for (j = 1; j <= pitch; j++)
			_out[((height_less_one - i) * pitch) + pitch - j] = _in[(i * pitch) + pitch - j];
	}

	return _out;
}

uint8_t *_flip_bitmap_x(uint8_t * const _in, uint32_t width, uint32_t height, uint32_t pitch, uint32_t num)
{
	uint32_t i, size = pitch * height;
	uint8_t *_out = malloc(size);
	BitStream *BS = OpenBitStream(size, _out);

	for (i = 0; i < size; i++)
	{
		uint8_t j;
		for (j = 0; j < 8; j++)
		{
			uint8_t r_j = 7 - j;
			if ((_in[i] & (1 << r_j)) == 0)
				PutBit(BS, 0);
			else
				PutBit(BS, 1);
		}
	}

	CloseBitStream(BS);
	return _out;
}

static PangoFont *
gdk_gl_font_use_pango_font_common (/*PangoContext					*context,*/
									const PangoFontDescription	*font_desc,
									int							first,
									int							count,
									int							list_base)
{
	PangoContext *context = NULL;
	PangoFont *font = NULL;
	PangoFontMap *font_map = NULL;
	FT_Bitmap bmp;
	int i;
	GLint alignment;

	GDK_GL_NOTE_FUNC_PRIVATE ();

	font_map = pango_ft2_font_map_new();
	if (font_map == NULL)
	{
		g_warning("cannot create a FontMap");
		goto FAIL;
	}
	context = pango_font_map_create_context(font_map);
	if (context == NULL)
	{
		g_warning("cannot create a Context");
		goto FAIL;
	}
	font = pango_context_load_font(context, font_desc);
	if (font == NULL)
	{
		g_warning ("cannot load PangoFont");
		goto FAIL;
	}

	glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);

	for (i = 0; i < count; i++)
	{
		uint8_t *buff;
		uint32_t glyph;
		FT_Face face = pango_ft2_font_get_face(font);

		glyph = FT_Get_Char_Index(face, i + first);
		FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO);
		FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO);
		bmp = face->glyph->bitmap;

		glPixelStorei(GL_UNPACK_ALIGNMENT, bmp.pitch);
		glNewList(list_base + i, GL_COMPILE);
		buff = _flip_bitmap_x(bmp.buffer, bmp.width, bmp.rows, bmp.pitch, i);
		if (bmp.pitch > 0)
		{
			uint8_t *_buff = _flip_bitmap_y(buff, bmp.width, bmp.rows, bmp.pitch);
			free(buff);
			buff = _buff;
		}
		// face->glyph->linearHoriAdvance >> 16 and face->glyph->metrics.horiAdvance >> 6 should be the same, but I'll be trusting the former.
		// (face->glyph->metrics.height - face->glyph->metrics.horiBearingY) >> 6 and bmp.rows - face->glyph->bitmap_top are also the same, but
		// again, I'll be trusting the non-metrics values.
		glBitmap(bmp.width, bmp.rows, 0, bmp.rows - face->glyph->bitmap_top, face->glyph->linearHoriAdvance >> 16, 0, buff);
		glEndList();
		free(buff);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);

 FAIL:
	if (context != NULL)
		g_object_unref(context);

	return font;
}

/**
 * gdk_gl_font_use_pango_font:
 * @font_desc: a #PangoFontDescription describing the font to use.
 * @first: the index of the first glyph to be taken.
 * @count: the number of glyphs to be taken.
 * @list_base: the index of the first display list to be generated.
 *
 * Creates bitmap display lists from a #PangoFont.
 *
 * Return value: the #PangoFont used, or NULL if no font matched.
 **/
PangoFont *
gdk_gl_font_use_pango_font (const PangoFontDescription *font_desc,
														int												 first,
														int												 count,
														int												 list_base)
{
//	PangoContext *context;
	PangoFont *font;

	g_return_val_if_fail (font_desc != NULL, NULL);

	GDK_GL_NOTE_FUNC ();

#ifdef GDKGLEXT_MULTIHEAD_SUPPORT
//	context = gdk_pango_context_get_for_screen(gdk_display_get_default_screen(gdk_display_get_default()));
#else	/* GDKGLEXT_MULTIHEAD_SUPPORT */
//	context = gdk_pango_context_get_for_screen(gdk_display_get_default_screen(gdk_x11_lookup_xdisplay(gdk_x11_get_default_xdisplay())));
#endif /* GDKGLEXT_MULTIHEAD_SUPPORT */

	font = gdk_gl_font_use_pango_font_common (/*context, */font_desc, first, count, list_base);
//	if (context != NULL)
//		g_object_unref(context);
	return font;
}

#ifdef GDKGLEXT_MULTIHEAD_SUPPORT

/**
 * gdk_gl_font_use_pango_font_for_display:
 * @display: a #GdkDisplay.
 * @font_desc: a #PangoFontDescription describing the font to use.
 * @first: the index of the first glyph to be taken.
 * @count: the number of glyphs to be taken.
 * @list_base: the index of the first display list to be generated.
 *
 * Creates bitmap display lists from a #PangoFont.
 *
 * Return value: the #PangoFont used, or NULL if no font matched.
 **/
PangoFont *
gdk_gl_font_use_pango_font_for_display (GdkDisplay								 *display,
																				const PangoFontDescription *font_desc,
																				int												 first,
																				int												 count,
																				int												 list_base)
{
//	PangoContext *context;

	g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
	g_return_val_if_fail (font_desc != NULL, NULL);

	GDK_GL_NOTE_FUNC ();

//	context = gdk_pango_context_get_for_screen(gdk_display_get_default_screen(display));

	return gdk_gl_font_use_pango_font_common (/*context, */font_desc, first, count, list_base);
}

#endif /* GDKGLEXT_MULTIHEAD_SUPPORT */

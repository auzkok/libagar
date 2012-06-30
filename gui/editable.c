/*
 * Copyright (c) 2002-2012 Hypertriton, Inc. <http://hypertriton.com/>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Low-level single/multi-line text input widget. This is the base widget
 * used by other widgets such as AG_Textbox(3).
 */

#include <core/core.h>
#include <core/config.h>

#include <config/have_freetype.h>

#include "editable.h"
#include "text.h"
#include "window.h"

#include "ttf.h"
#include "keymap.h"
#include "primitive.h"
#include "cursors.h"
#include "menu.h"
#include "icons.h"
#include "gui_math.h"

#include <string.h>
#include <stdarg.h>
#include <ctype.h>

AG_EditableClipboard agEditableClipbrd;		/* For Copy/Cut/Paste */
AG_EditableClipboard agEditableKillring;	/* For Emacs-style Kill/Yank */

/*
 * Return a new working buffer. The variable is returned locked; the caller
 * should invoke ReleaseBuffer() after use.
 */
static __inline__ AG_EditableBuffer *
GetBuffer(AG_Editable *ed)
{
	AG_EditableBuffer *buf;

	if (ed->flags & AG_EDITABLE_EXCL) {
		buf = &ed->sBuf;
	} else {
		if ((buf = TryMalloc(sizeof(AG_EditableBuffer))) == NULL) {
			return (NULL);
		}
		buf->s = NULL;
		buf->len = 0;
		buf->maxLen = 0;
	}
	if (AG_Defined(ed, "text")) {			/* AG_Text element */
		AG_Text *txt;

		buf->var = AG_GetVariable(ed, "text", &txt);
		buf->reallocable = 1;

		AG_MutexLock(&txt->lock);

		if ((ed->flags & AG_EDITABLE_EXCL) == 0 ||
		    buf->s == NULL) {
			AG_TextEnt *te = &txt->ent[ed->lang];

			if (te->buf != NULL) {
				buf->s = AG_ImportUnicode("UTF-8", te->buf,
				    &buf->len, &buf->maxLen);
			} else {
				if ((buf->s = TryMalloc(sizeof(Uint32))) != NULL) {
					buf->s[0] = (Uint32)'\0';
				}
				buf->len = 0;
			}
			if (buf->s == NULL) {
				AG_MutexUnlock(&txt->lock);
				AG_UnlockVariable(buf->var);
				buf->var = NULL;
				goto fail;
			}
		}
	} else {					/* Fixed-size buffer */
		char *s;

		buf->var = AG_GetVariable(ed, "string", &s);
		buf->reallocable = 0;

		if ((ed->flags & AG_EDITABLE_EXCL) == 0 ||
		    buf->s == NULL) {
			buf->s = AG_ImportUnicode(ed->encoding, s, &buf->len,
			    &buf->maxLen);
			if (buf->s == NULL) {
				AG_UnlockVariable(buf->var);
				goto fail;
			}
		}
	}
	return (buf);
fail:
	if ((ed->flags & AG_EDITABLE_EXCL) == 0) { Free(buf); }
	return (NULL);
}

/* Clear a working buffer. */
static __inline__ void
ClearBuffer(AG_EditableBuffer *buf)
{
	AG_Free(buf->s);
	buf->s = NULL;
	buf->len = 0;
	buf->maxLen = 0;
}

/* Commit changes to the working buffer. */
static void
CommitBuffer(AG_Editable *ed, AG_EditableBuffer *buf)
{
	if (AG_Defined(ed, "text")) {			/* AG_Text binding */
		AG_Text *txt = buf->var->data.p;
		AG_TextEnt *te = &txt->ent[ed->lang];
		size_t lenEnc;

		if (AG_LengthUTF8FromUCS4(buf->s, &lenEnc) == -1) {
			goto fail;
		}
		lenEnc++;

		if (lenEnc > te->maxLen &&
		    AG_TextRealloc(te, lenEnc) == -1) {
			goto fail;
		}
		if (AG_ExportUnicode(ed->encoding, te->buf, buf->s,
		    te->maxLen+1) == -1) {
			goto fail;
		}
	} else {					/* C string binding */
		if (AG_ExportUnicode(ed->encoding, buf->var->data.s, buf->s,
		    buf->var->info.size) == -1)
			goto fail;
	}
	ed->flags |= AG_EDITABLE_MARKPREF;
	AG_PostEvent(NULL, ed, "editable-postchg", NULL);
	return;
fail:
	Verbose("CommitBuffer: %s; ignoring\n", AG_GetError());
}

/* Release the working buffer. */
static __inline__ void
ReleaseBuffer(AG_Editable *ed, AG_EditableBuffer *buf)
{
	if (AG_Defined(ed, "text")) {
		AG_Text *txt = buf->var->data.p;
		AG_MutexUnlock(&txt->lock);
	}
	if (buf->var != NULL) {
		AG_UnlockVariable(buf->var);
		buf->var = NULL;
	}
	if (!(ed->flags & AG_EDITABLE_EXCL)) {
		ClearBuffer(buf);
		Free(buf);
	}
}

/* Return a working (locked) buffer handle. */
AG_EditableBuffer *
AG_EditableGetBuffer(AG_Editable *ed)
{
	AG_ObjectLock(ed);
	return GetBuffer(ed);
}

/* Clear a working buffer. */
void
AG_EditableClearBuffer(AG_Editable *ed, AG_EditableBuffer *buf)
{
	return ClearBuffer(buf);
}

/* Increase the working buffer size to accomodate new characters. */
int
AG_EditableGrowBuffer(AG_Editable *ed, AG_EditableBuffer *buf, Uint32 *ins,
    size_t nIns)
{
	size_t ucsSize;		/* UCS-4 buffer size in bytes */
	size_t convLen;		/* Converted string length in bytes */
	Uint32 *sNew;

	ucsSize = (buf->len + nIns + 1)*sizeof(Uint32);

	if (Strcasecmp(ed->encoding, "UTF-8") == 0) {
		size_t sLen, insLen;

		if (AG_LengthUTF8FromUCS4(buf->s, &sLen) == -1 ||
		    AG_LengthUTF8FromUCS4(ins, &insLen) == -1) {
			return (-1);
		}
		convLen = sLen + insLen + 1;
	} else {
		/* TODO Proper estimates for other charsets */
		convLen = ucsSize;
	}
	
	if (!buf->reallocable) {
		if (convLen > buf->var->info.size) {
			AG_SetError("%u > %u bytes", (Uint)convLen, (Uint)buf->var->info.size);
			return (-1);
		}
	}
	if (ucsSize > buf->maxLen) {
		if ((sNew = TryRealloc(buf->s, ucsSize)) == NULL) {
			return (-1);
		}
		buf->s = sNew;
		buf->maxLen = ucsSize;
	}
	return (0);
}

/* Release a working buffer. */
void
AG_EditableReleaseBuffer(AG_Editable *ed, AG_EditableBuffer *buf)
{
	ReleaseBuffer(ed, buf);
	AG_ObjectUnlock(ed);
}

AG_Editable *
AG_EditableNew(void *parent, Uint flags)
{
	AG_Editable *ed;

	ed = Malloc(sizeof(AG_Editable));
	AG_ObjectInit(ed, &agEditableClass);

	if (flags & AG_EDITABLE_HFILL)
		AG_ExpandHoriz(ed);
	if (flags & AG_EDITABLE_VFILL)
		AG_ExpandVert(ed);
	if (flags & AG_EDITABLE_CATCH_TAB)
		WIDGET(ed)->flags |= AG_WIDGET_CATCH_TAB;

	ed->flags |= flags;

	/* Set exclusive mode if requested; also sets default redraw rate. */
	AG_EditableSetExcl(ed, (flags & AG_EDITABLE_EXCL));

	AG_ObjectAttach(parent, ed);
	return (ed);
}

/* Bind to a C string containing UTF-8 encoded text. */
void
AG_EditableBindUTF8(AG_Editable *ed, char *buf, size_t bufSize)
{
	AG_ObjectLock(ed);
	AG_Unset(ed, "text");
	AG_BindString(ed, "string", buf, bufSize);
	ed->encoding = "UTF-8";
	AG_ObjectUnlock(ed);
}

/* Bind to a C string containing ASCII text. */
void
AG_EditableBindASCII(AG_Editable *ed, char *buf, size_t bufSize)
{
	AG_ObjectLock(ed);
	AG_Unset(ed, "text");
	AG_BindString(ed, "string", buf, bufSize);
	ed->encoding = "US-ASCII";
	AG_ObjectUnlock(ed);
}

/* Bind to a C string containing text in specified encoding. */
void
AG_EditableBindEncoded(AG_Editable *ed, const char *encoding, char *buf,
    size_t bufSize)
{
	AG_ObjectLock(ed);
	AG_Unset(ed, "text");
	AG_BindString(ed, "string", buf, bufSize);
	ed->encoding = encoding;
	AG_ObjectUnlock(ed);
}

/* Bind to an AG_Text element. */
void
AG_EditableBindText(AG_Editable *ed, AG_Text *txt)
{
	AG_ObjectLock(ed);
	AG_Unset(ed, "string");
	AG_BindPointer(ed, "text", (void *)txt);
	ed->encoding = "UTF-8";
	AG_ObjectUnlock(ed);
}

/* Set the current language (for AG_Text bindings). */
void
AG_EditableSetLang(AG_Editable *ed, enum ag_language lang)
{
	AG_ObjectLock(ed);
	ed->lang = lang;
	ed->pos = 0;
	ed->sel = 0;
	AG_ObjectUnlock(ed);
	AG_Redraw(ed);
}

/* Enable or disable password entry mode. */
void
AG_EditableSetPassword(AG_Editable *ed, int enable)
{
	AG_ObjectLock(ed);
	AG_SETFLAGS(ed->flags, AG_EDITABLE_PASSWORD, enable);
	AG_ObjectUnlock(ed);
}

/* Enable or disable word wrapping. */
void
AG_EditableSetWordWrap(AG_Editable *ed, int enable)
{
	AG_ObjectLock(ed);
	AG_SETFLAGS(ed->flags, AG_EDITABLE_WORDWRAP, enable);
	AG_ObjectUnlock(ed);
}

/*
 * Specify whether the buffer is accessed exclusively by the widget.
 * We can disable periodic redraws in exclusive mode.
 */
void
AG_EditableSetExcl(AG_Editable *ed, int enable)
{
	AG_ObjectLock(ed);
	ClearBuffer(&ed->sBuf);

	if (enable) {
		ed->flags |= AG_EDITABLE_EXCL;
		AG_RedrawOnTick(ed, -1);
	} else {
		ed->flags &= ~(AG_EDITABLE_EXCL);
		AG_RedrawOnTick(ed, 1000);
	}
	AG_ObjectUnlock(ed);
}

/* Toggle floating-point only input */
void
AG_EditableSetFltOnly(AG_Editable *ed, int enable)
{
	AG_ObjectLock(ed);
	if (enable) {
		ed->flags |= AG_EDITABLE_FLT_ONLY;
		ed->flags &= ~(AG_EDITABLE_INT_ONLY);
	} else {
		ed->flags &= ~(AG_EDITABLE_FLT_ONLY);
	}
	AG_ObjectUnlock(ed);
}

/* Toggle integer only input */
void
AG_EditableSetIntOnly(AG_Editable *ed, int enable)
{
	AG_ObjectLock(ed);
	if (enable) {
		ed->flags |= AG_EDITABLE_INT_ONLY;
		ed->flags &= ~(AG_EDITABLE_FLT_ONLY);
	} else {
		ed->flags &= ~(AG_EDITABLE_INT_ONLY);
	}
	AG_ObjectUnlock(ed);
}

/* Set alternate font */
void
AG_EditableSetFont(AG_Editable *ed, AG_Font *font)
{
	AG_ObjectLock(ed);
	ed->font = font;
	AG_ObjectUnlock(ed);
}

/* Evaluate if a key is acceptable in integer-only mode. */
static int
KeyIsIntOnly(AG_KeySym ks)
{
	return (ks == AG_KEY_MINUS ||
	        ks == AG_KEY_PLUS ||
		isdigit((int)ks));
}

/* Evaluate if a key is acceptable in float-only mode. */
static int
KeyIsFltOnly(AG_KeySym ks, Uint32 unicode)
{
	return (ks == AG_KEY_PLUS ||
	        ks == AG_KEY_MINUS ||
	        ks == AG_KEY_PERIOD ||
	        ks == AG_KEY_E ||
	        ks == AG_KEY_I ||
	        ks == AG_KEY_N ||
	        ks == AG_KEY_F ||
	        ks == AG_KEY_A ||
	        unicode == 0x221e ||		/* Infinity */
	        isdigit((int)ks));
}

/*
 * Process a keystroke. May be invoked from the repeat timeout routine or
 * the keydown handler. If we return 1, the current delay/repeat cycle will
 * be maintained, otherwise it will be cancelled.
 */
static int
ProcessKey(AG_Editable *ed, AG_KeySym ks, AG_KeyMod kmod, Uint32 unicode)
{
	AG_EditableBuffer *buf;
	int i, rv = 0;

	if (ks == AG_KEY_ESCAPE) {
		return (0);
	}
	if (ks == AG_KEY_RETURN &&
	   (ed->flags & AG_EDITABLE_MULTILINE) == 0)
		return (0);

	if (kmod == AG_KEYMOD_NONE &&
	    isascii((int)ks) &&
	    isprint((int)ks)) {
		if ((ed->flags & AG_EDITABLE_INT_ONLY) &&
		    !KeyIsIntOnly(ks)) {
			return (0);
		} else if ((ed->flags & AG_EDITABLE_FLT_ONLY) &&
		           !KeyIsFltOnly(ks, unicode)) {
			return (0);
		}
	}

	if ((buf = GetBuffer(ed)) == NULL)
		return (0);

	if (ed->pos < 0) { ed->pos = 0; }
	if (ed->pos > buf->len) { ed->pos = buf->len; }

	for (i = 0; ; i++) {
		const struct ag_keycode *kc = &agKeymap[i];
		
		if (kc->key != AG_KEY_LAST &&
		   (kc->key != ks || kc->func == NULL)) {
			continue;
		}
		if (kc->key == AG_KEY_LAST ||
		    kc->modmask == 0 || (kmod & kc->modmask)) {
			AG_PostEvent(NULL, ed, "editable-prechg", NULL);
			rv = kc->func(ed, buf, ks, kmod, unicode);
			break;
		}
	}
	if (rv == 1) {
		CommitBuffer(ed, buf);
	}
	ReleaseBuffer(ed, buf);
	return (1);
}

static Uint32
RepeatTimeout(void *obj, Uint32 ival, void *arg)
{
	AG_Editable *ed = obj;

	if (ProcessKey(ed, ed->repeatKey, ed->repeatMod, ed->repeatUnicode)
	    == 0) {
		return (0);
	}
	AG_Redraw(ed);
	return (agKbdRepeat);
}

static Uint32
DelayTimeout(void *obj, Uint32 ival, void *arg)
{
	AG_Editable *ed = obj;

	AG_ScheduleTimeout(ed, &ed->toRepeat, agKbdRepeat);
	AG_DelTimeout(ed, &ed->toCursorBlink);
	ed->flags |= AG_EDITABLE_BLINK_ON;
	AG_Redraw(ed);
	return (0);
}

static Uint32
BlinkTimeout(void *obj, Uint32 ival, void *arg)
{
	AG_Editable *ed = obj;

	if ((ed->flags & AG_EDITABLE_CURSOR_MOVING) == 0) {
		AG_INVFLAGS(ed->flags, AG_EDITABLE_BLINK_ON);
	}
	AG_Redraw(ed);
	return (ival);
}

static void
GainedFocus(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();

	AG_LockTimeouts(ed);
	
	AG_DelTimeout(ed, &ed->toDelay);
	AG_DelTimeout(ed, &ed->toRepeat);
	AG_ScheduleTimeout(ed, &ed->toCursorBlink, agTextBlinkRate);
	ed->flags |= AG_EDITABLE_BLINK_ON;
	AG_UnlockTimeouts(ed);

	AG_Redraw(ed);
}

static void
LostFocus(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();

	AG_LockTimeouts(ed);
	AG_DelTimeout(ed, &ed->toDelay);
	AG_DelTimeout(ed, &ed->toRepeat);
	AG_DelTimeout(ed, &ed->toCursorBlink);
	ed->flags &= ~(AG_EDITABLE_BLINK_ON|AG_EDITABLE_CURSOR_MOVING);
	AG_UnlockTimeouts(ed);
	AG_CancelEvent(ed, "dblclick-expire");
	
	AG_Redraw(ed);
}

/* Evaluate word wrapping at given character. */
static __inline__ int
WrapAtChar(AG_Editable *ed, int x, Uint32 *s)
{
	AG_Driver *drv = WIDGET(ed)->drv;
	AG_Glyph *gl;
	Uint32 *t;
	int x2;

	if (!(ed->flags & AG_EDITABLE_WORDWRAP) ||
	    x == 0 || !isspace((int)*s)) {
		return (0);
	}
	for (t = &s[1], x2 = x;
	     *t != '\0';
	     t++) {
		gl = AG_TextRenderGlyph(drv, *t);
		x2 += gl->advance;
		if (isspace((int)*t) || *t == '\n') {
			if (x2 > WIDTH(ed)) {
				return (1);
			} else {
				break;
			}
		}
	}
	return (0);
}

/*
 * Map mouse coordinates to a position within the buffer.
 */
#define ON_LINE(my,y) \
	( ((my) >= (y) && (my) <= (y)+agTextFontLineSkip) || \
	  (!absflag && \
	   (((my) <= ed->y*agTextFontLineSkip && line == 0) || \
	    ((my) > (nLines*agTextFontLineSkip) && (line == nLines-1)))) )
#define ON_CHAR(mx,x,glyph) \
	((mx) >= (x) && (mx) <= (x)+(glyph)->advance)
int
AG_EditableMapPosition(AG_Editable *ed, AG_EditableBuffer *buf, int mx, int my,
    int *pos, int absflag)
{
	AG_Driver *drv = WIDGET(ed)->drv;
	AG_Font *font;
	Uint32 ch;
	int i, x, y, line = 0;
	int nLines = 1;
	int yMouse;
	
	AG_ObjectLock(ed);

	yMouse = my + ed->y*agTextFontLineSkip;
	if (yMouse < 0) {
		AG_ObjectUnlock(ed);
		return (-1);
	}
	x = 0;
	y = 0;

	if ((font = ed->font) == NULL &&
	    (font = AG_FetchFont(NULL, -1, -1)) == NULL) {
		AG_ObjectUnlock(ed);
		return (-1);
	}

 	for (i = 0; i < buf->len; i++) {
		Uint32 ch = buf->s[i];

		if (WrapAtChar(ed, x, &buf->s[i])) {
			x = 0;
			nLines++;
		}
		if (ch == '\n') {
			x = 0;
			nLines++;
		} else if (ch == '\t') {
			x += agTextTabWidth;
		} else {
			switch (font->type) {
#ifdef HAVE_FREETYPE
			case AG_FONT_VECTOR:
			{
				AG_TTFFont *ttf = font->ttf;
				AG_TTFGlyph *glyph;

				if (AG_TTFFindGlyph(ttf, ch,
				    TTF_CACHED_METRICS|TTF_CACHED_BITMAP) != 0) {
					continue;
				}
				glyph = ttf->current;
				x += glyph->advance;
				break;
			}
#endif /* HAVE_FREETYPE */
			case AG_FONT_BITMAP:
			{
				AG_Glyph *gl;
			
				gl = AG_TextRenderGlyph(drv, ch);
				x += gl->su->w;
				break;
			}
			default:
				break;
			}
		}
	}

	x = 0;
	for (i = 0; i < buf->len; i++) {
		ch = buf->s[i];
		if (mx <= 0 && ON_LINE(yMouse,y)) {
			*pos = i;
			goto in;
		}
		if (WrapAtChar(ed, x, &buf->s[i])) {
			if (ON_LINE(yMouse,y) && mx > x) {
				*pos = i;
				goto in;
			}
			y += agTextFontLineSkip;
			x = 0;
			line++;
		}
		if (ch == '\n') {
			if (ON_LINE(yMouse,y) && mx > x) {
				*pos = i;
				goto in;
			}
			y += agTextFontLineSkip;
			x = 0;
			line++;
			continue;
		} else if (ch == '\t') {
			if (ON_LINE(yMouse,y) &&
			    mx >= x && mx <= x+agTextTabWidth) {
				*pos = (mx < x + agTextTabWidth/2) ? i : i+1;
				goto in;
			}
			x += agTextTabWidth;
			continue;
		}
		
		switch (font->type) {
#ifdef HAVE_FREETYPE
		case AG_FONT_VECTOR:
		{
			AG_TTFFont *ttf = font->ttf;
			AG_TTFGlyph *glyph;

			if (AG_TTFFindGlyph(ttf, ch,
			    TTF_CACHED_METRICS|TTF_CACHED_BITMAP) != 0) {
				continue;
			}
			glyph = ttf->current;

			if (ON_LINE(yMouse,y) && ON_CHAR(mx,x,glyph)) {
				*pos = (mx < x+glyph->advance/2) ? i : i+1;
				goto in;
			}
			x += glyph->advance;
			break;
		}
#endif /* HAVE_FREETYPE */
		case AG_FONT_BITMAP:
		{
			AG_Glyph *gl;
			
			gl = AG_TextRenderGlyph(drv, ch);
			if (ON_LINE(yMouse,y) && mx >= x && mx <= x+gl->su->w) {
				*pos = i;
				goto in;
			}
			x += gl->su->w;
			break;
		}
		default:
			AG_FatalError("AG_Editable: Unknown font format");
		}
	}
	AG_ObjectUnlock(ed);
	return (1);
in:
	AG_ObjectUnlock(ed);
	return (0);
}
#undef ON_LINE
#undef ON_CHAR

/* Move cursor to the given position in pixels. */
void
AG_EditableMoveCursor(AG_Editable *ed, AG_EditableBuffer *buf, int mx, int my,
    int absflag)
{
	int rv;

	AG_ObjectLock(ed);
	rv = AG_EditableMapPosition(ed, buf, mx, my, &ed->pos, absflag);
	ed->sel = 0;
	if (rv == -1) {
		ed->pos = 0;
	} else if (rv == 1) {
		ed->pos = buf->len;
	}
	AG_ObjectUnlock(ed);
	AG_Redraw(ed);
}

/* Set the cursor position (-1 = end of the string) with bounds checking. */
int
AG_EditableSetCursorPos(AG_Editable *ed, AG_EditableBuffer *buf, int pos)
{
	int rv;

	AG_ObjectLock(ed);
	ed->pos = pos;
	ed->sel = 0;
	if (ed->pos < 0) {
		if (pos == -1 || ed->pos > buf->len)
			ed->pos = buf->len;
	}
	rv = ed->pos;
	AG_ObjectUnlock(ed);
	
	AG_Redraw(ed);
	return (rv);
}

static void
Draw(void *obj)
{
	AG_Editable *ed = obj;
	AG_Driver *drv = WIDGET(ed)->drv;
	AG_DriverClass *drvOps = WIDGET(ed)->drvOps;
	AG_EditableBuffer *buf;
	AG_Rect2 rClip;
	int i, dx, dy, x, y;

	if ((buf = GetBuffer(ed)) == NULL)
		return;

	rClip = WIDGET(ed)->rView;
	rClip.x1 -= agTextFontHeight*2;	/* XXX largest character width */
	rClip.y1 -= agTextFontLineSkip;
	rClip.x2 += agTextFontHeight*2;
	rClip.y2 += agTextFontLineSkip;

	AG_PushBlendingMode(ed, AG_ALPHA_SRC, AG_ALPHA_ONE_MINUS_SRC);
	AG_PushClipRect(ed, ed->r);

	AG_PushTextState();

	if (ed->font != NULL) { AG_TextFont(ed->font); }
	AG_TextColor(agColors[TEXTBOX_TXT_COLOR]);

	x = 0;
	y = -ed->y*agTextFontLineSkip;
	ed->xMax = 10;
	ed->yMax = 1;
	for (i = 0; i <= buf->len; i++) {
		AG_Glyph *gl;
		Uint32 c = buf->s[i];

		if (i == ed->pos) {
			if (ed->sel == 0 &&
			    (ed->flags & AG_EDITABLE_BLINK_ON) &&
			    (ed->y >= 0 && ed->y <= ed->yMax-1) &&
			    AG_WidgetIsFocused(ed)) {
				AG_DrawLineV(ed,
				    x - ed->x, (y + 1),
				    (y + agTextFontLineSkip - 1),
				    agColors[TEXTBOX_CURSOR_COLOR]);
			}
			ed->xCurs = x;
			if (ed->flags & AG_EDITABLE_MARKPREF) {
				ed->flags &= ~(AG_EDITABLE_MARKPREF);
				ed->xCursPref = x;
			}
			ed->yCurs = y/agTextFontLineSkip + ed->y;
		}

		if (i == buf->len)
			break;

		if (WrapAtChar(ed, x, &buf->s[i])) {
			y += agTextFontLineSkip;
			ed->xMax = MAX(ed->xMax, x);
			ed->yMax++;
			x = 0;
		}
		if (c == '\n') {
			y += agTextFontLineSkip;
			ed->xMax = MAX(ed->xMax, x+10);
			ed->yMax++;
			x = 0;
			continue;
		} else if (c == '\t') {
			if ((ed->sel > 0 && i >= ed->pos && i < ed->pos + ed->sel) ||
			    (ed->sel < 0 && i <  ed->pos && i > ed->pos + ed->sel - 1)) {
				AG_DrawRectFilled(ed,
				    AG_RECT(x - ed->x, y,
				            agTextTabWidth+1,
					    agTextFontLineSkip+1),
				    agColors[TEXT_SEL_COLOR]);
			}
			x += agTextTabWidth;
			continue;
		}

		c = (ed->flags & AG_EDITABLE_PASSWORD) ? '*' : c;
		gl = AG_TextRenderGlyph(drv, c);
		dx = WIDGET(ed)->rView.x1 + x - ed->x;
		dy = WIDGET(ed)->rView.y1 + y;

		if (!AG_RectInside2(&rClip, dx, dy)) {
			x += gl->advance;
			continue;
		}
		if ((ed->sel > 0 && i >= ed->pos && i < ed->pos + ed->sel) ||
		    (ed->sel < 0 && i <  ed->pos && i > ed->pos + ed->sel - 1)) {
			AG_DrawRectFilled(ed,
			    AG_RECT(x - ed->x, y, gl->su->w + 1, gl->su->h),
			    agColors[TEXT_SEL_COLOR]);
		}
		drvOps->drawGlyph(drv, gl, dx,dy);
		x += gl->advance;
	}
	if (ed->yMax == 1)
		ed->xMax = x;
	
	if ( !(ed->flags & (AG_EDITABLE_NOSCROLL|AG_EDITABLE_NOSCROLL_ONCE)) ) {
		if (ed->flags & AG_EDITABLE_MULTILINE) {
			if (ed->yCurs < ed->y) {
				ed->y = ed->yCurs;
				if (ed->y < 0) { ed->y = 0; }
			} else if (ed->yCurs > ed->y + ed->yVis - 1) {
				ed->y = ed->yCurs - ed->yVis + 1;
			}
		}
		if (!(ed->flags & AG_EDITABLE_WORDWRAP)) {
			if (ed->xCurs < ed->x) {
				ed->x = ed->xCurs;
				if (ed->x < 0) { ed->x = 0; }
			} else if (ed->xCurs > ed->x + WIDTH(ed) - 10) {
				ed->x = ed->xCurs - WIDTH(ed) + 10;
			}
		}
	} else {
		if (ed->yCurs < ed->y) {
			if (ed->flags & AG_EDITABLE_MULTILINE) {
				AG_EditableMoveCursor(ed, buf,
				    ed->xCursPref - ed->x, 1,
				    0);
			}
		} else if (ed->yCurs > ed->y + ed->yVis - 1) {
			if (ed->flags & AG_EDITABLE_MULTILINE) {
				AG_EditableMoveCursor(ed, buf,
				    ed->xCursPref - ed->x,
				    ed->yVis*agTextFontLineSkip - 1,
				    0);
			}
		} else if (ed->xCurs < ed->x+10) {
			if (!(ed->flags & AG_EDITABLE_WORDWRAP)) {
				AG_EditableMoveCursor(ed, buf,
				    ed->x+10,
				    (ed->yCurs - ed->y)*agTextFontLineSkip + 1,
				    1);
			}
		} else if (ed->xCurs > ed->x+WIDTH(ed)-10) {
			if (!(ed->flags & AG_EDITABLE_WORDWRAP)) {
				AG_EditableMoveCursor(ed, buf,
				    ed->x+WIDTH(ed)-10,
				    (ed->yCurs - ed->y)*agTextFontLineSkip + 1,
				    1);
			}
		}
	}
	ed->flags &= ~(AG_EDITABLE_NOSCROLL_ONCE);
	AG_PopTextState();

	AG_PopClipRect(ed);
	AG_PopBlendingMode(ed);

	ReleaseBuffer(ed, buf);
}

void
AG_EditableSizeHint(AG_Editable *ed, const char *text)
{
	AG_ObjectLock(ed);
	AG_TextSize(text, &ed->wPre, &ed->hPre);
	AG_ObjectUnlock(ed);
}

void
AG_EditableSizeHintPixels(AG_Editable *ed, Uint w, Uint h)
{
	AG_ObjectLock(ed);
	ed->wPre = w;
	ed->hPre = h;
	AG_ObjectUnlock(ed);
}

void
AG_EditableSizeHintLines(AG_Editable *ed, Uint nLines)
{
	AG_ObjectLock(ed);
	ed->hPre = nLines*agTextFontHeight;
	AG_ObjectUnlock(ed);
}

static void
SizeRequest(void *obj, AG_SizeReq *r)
{
	AG_Editable *ed = obj;

	r->w = ed->wPre;
	r->h = ed->hPre;
}

static int
SizeAllocate(void *obj, const AG_SizeAlloc *a)
{
	AG_Editable *ed = obj;

	if (a->w < 2 || a->h < 2) {
		if (WIDGET(ed)->window != NULL &&
		    ed->ca != NULL) {
			AG_UnmapCursor(ed, ed->ca);
			ed->ca = NULL;
		}
		return (-1);
	}
	ed->yVis = a->h/agTextFontLineSkip;
	ed->r = AG_RECT(-1, -1, a->w-1, a->h-1);

	if (WIDGET(ed)->window != NULL &&
	   !(WIDGET(ed)->window->flags & AG_WINDOW_NOCURSORCHG)) {
		AG_Rect r = AG_RECT(0, 0, a->w, a->h);

		if (ed->ca == NULL) {
			ed->ca = AG_MapStockCursor(ed, r, AG_TEXT_CURSOR);
		} else {
			ed->ca->r = r;
		}
	}
	return (0);
}

static void
KeyDown(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();
	int keysym = AG_INT(1);
	int keymod = AG_INT(2);
	Uint32 unicode = (Uint32)AG_ULONG(3);

	switch (keysym) {
	case AG_KEY_LSHIFT:
	case AG_KEY_RSHIFT:
	case AG_KEY_LALT:
	case AG_KEY_RALT:
	case AG_KEY_LMETA:
	case AG_KEY_RMETA:
	case AG_KEY_LCTRL:
	case AG_KEY_RCTRL:
		return;
	case AG_KEY_TAB:
		if (!(WIDGET(ed)->flags & AG_WIDGET_CATCH_TAB)) {
			return;
		}
		break;
	default:
		break;
	}

	ed->repeatKey = keysym;
	ed->repeatMod = keymod;
	ed->repeatUnicode = unicode;
	ed->flags |= AG_EDITABLE_BLINK_ON;

	AG_LockTimeouts(ed);
	AG_DelTimeout(ed, &ed->toRepeat);
	if (ProcessKey(ed, keysym, keymod, unicode) == 1) {
		AG_ScheduleTimeout(ed, &ed->toDelay, agKbdDelay);
	} else {
		AG_DelTimeout(ed, &ed->toDelay);
	}
	AG_UnlockTimeouts(ed);

	AG_Redraw(ed);
}

static void
KeyUp(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();
	int keysym = AG_INT(1);
	
	if (ed->repeatKey == keysym) {
		AG_LockTimeouts(ed);
		AG_DelTimeout(ed, &ed->toRepeat);
		AG_DelTimeout(ed, &ed->toDelay);
		AG_ScheduleTimeout(ed, &ed->toCursorBlink, agTextBlinkRate);
		AG_UnlockTimeouts(ed);
	}
	if (keysym == AG_KEY_RETURN &&
	   (ed->flags & AG_EDITABLE_MULTILINE) == 0) {
		if (ed->flags & AG_EDITABLE_ABANDON_FOCUS) {
			AG_WidgetUnfocus(ed);
		}
		AG_PostEvent(NULL, ed, "editable-return", NULL);
	}
	
	AG_Redraw(ed);
}

static void
MouseDoubleClick(AG_Editable *ed)
{
	AG_EditableBuffer *buf;
	Uint32 *c;

	AG_CancelEvent(ed, "dblclick-expire");
	ed->selDblClick = -1;
	ed->flags |= AG_EDITABLE_WORDSELECT;

	if ((buf = GetBuffer(ed)) == NULL) {
		return;
	}
	if (ed->pos >= 0 && ed->pos < buf->len) {
		ed->sel = 0;

		c = &buf->s[ed->pos];
		if (*c == (Uint32)('\n')) {
			goto out;
		}
		for (;
		     ed->pos > 0;
		     ed->pos--, c--) {
			if (isspace((char)*c) && ed->pos < buf->len) {
				c++;
				ed->pos++;
				break;
			}
		}
		while ((ed->pos + ed->sel) < buf->len &&
		    !isspace((char)*c)) {
			c++;
			ed->sel++;
		}
	}
out:
	ReleaseBuffer(ed, buf);
}

/* Ensure selection offset is positive. */
static __inline__ void
NormSelection(AG_Editable *ed)
{
	if (ed->sel < 0) {
		ed->pos += ed->sel;
		ed->sel = -(ed->sel);
	}
}

/* Copy specified range to specified clipboard. */
void
AG_EditableCopyChunk(AG_Editable *ed, AG_EditableClipboard *cb,
    Uint32 *s, size_t len)
{
	Uint32 *sNew;

	AG_MutexLock(&cb->lock);
	sNew = TryRealloc(cb->s, (len+1)*sizeof(Uint32));
	if (sNew != NULL) {
		cb->s = sNew;
		memcpy(cb->s, s, len*sizeof(Uint32));
		cb->s[len] = '\0';
		cb->len = len;
	}
	Strlcpy(cb->encoding, ed->encoding, sizeof(cb->encoding));
	AG_MutexUnlock(&cb->lock);
}

/* Perform Cut action on buffer. */
int
AG_EditableCut(AG_Editable *ed, AG_EditableBuffer *buf, AG_EditableClipboard *cb)
{
	if (AG_EditableReadOnly(ed) || ed->sel == 0) {
		return (0);
	}
	NormSelection(ed);
	AG_EditableCopyChunk(ed, cb, &buf->s[ed->pos], ed->sel);
	AG_EditableDelete(ed, buf);
	return (1);
}

/* Perform Copy action on standard clipboard. */
int
AG_EditableCopy(AG_Editable *ed, AG_EditableBuffer *buf, AG_EditableClipboard *cb)
{
	if (ed->sel == 0) {
		return (0);
	}
	NormSelection(ed);
	AG_EditableCopyChunk(ed, cb, &buf->s[ed->pos], ed->sel);
	return (1);
}
	
/* Perform Paste action on buffer. */
int
AG_EditablePaste(AG_Editable *ed, AG_EditableBuffer *buf,
    AG_EditableClipboard *cb)
{
	if (AG_EditableReadOnly(ed))
		return (0);

	if (ed->sel != 0)
		AG_EditableDelete(ed, buf);

	AG_MutexLock(&cb->lock);

	if (cb->s == NULL)
		goto out;

	if (strcmp(ed->encoding, cb->encoding) != 0) {
		/* TODO: charset conversion */
		goto fail;
	}
	if (AG_EditableGrowBuffer(ed, buf, cb->s, cb->len) == -1) {
		Verbose("Paste Failed: %s\n", AG_GetError());
		goto out;
	}
	if (ed->pos < buf->len) {
		memmove(&buf->s[ed->pos + cb->len], &buf->s[ed->pos],
		    (buf->len - ed->pos)*sizeof(Uint32));
	}
	memcpy(&buf->s[ed->pos], cb->s, cb->len*sizeof(Uint32));
	buf->len += cb->len;
	buf->s[buf->len] = '\0';
	ed->pos += cb->len;
out:
	AG_MutexUnlock(&cb->lock);
	return (1);
fail:
	AG_MutexUnlock(&cb->lock);
	return (0);
}

/* Delete the current selection. */
int
AG_EditableDelete(AG_Editable *ed, AG_EditableBuffer *buf)
{
	if (AG_EditableReadOnly(ed) || ed->sel == 0)
		return (0);

	NormSelection(ed);
	if (ed->pos + ed->sel == buf->len) {
		buf->s[ed->pos] = '\0';
	} else {
		memmove(&buf->s[ed->pos], &buf->s[ed->pos + ed->sel],
		    (buf->len - ed->sel + 1 - ed->pos)*sizeof(Uint32));
	}
	buf->len -= ed->sel;
	ed->sel = 0;
	return (1);
}

/* Perform "Select All" on buffer. */
void
AG_EditableSelectAll(AG_Editable *ed, AG_EditableBuffer *buf)
{
	ed->pos = 0;
	ed->sel = buf->len;
}

/*
 * Right-click popup menu actions.
 */
static void
MenuCut(AG_Event *event)
{
	AG_Editable *ed = AG_PTR(1);
	AG_EditableBuffer *buf;
	
	if ((buf = GetBuffer(ed)) != NULL) {
		if (AG_EditableCut(ed, buf, &agEditableClipbrd)) {
			CommitBuffer(ed, buf);
		}
		ReleaseBuffer(ed, buf);
	}
}
static void
MenuCopy(AG_Event *event)
{
	AG_Editable *ed = AG_PTR(1);
	AG_EditableBuffer *buf;
	
	if ((buf = GetBuffer(ed)) != NULL) {
		AG_EditableCopy(ed, buf, &agEditableClipbrd);
		ReleaseBuffer(ed, buf);
	}
}
static void
MenuPaste(AG_Event *event)
{
	AG_Editable *ed = AG_PTR(1);
	AG_EditableBuffer *buf;
	
	if ((buf = GetBuffer(ed)) != NULL) {
		if (AG_EditablePaste(ed, buf, &agEditableClipbrd)) {
			CommitBuffer(ed, buf);
		}
		ReleaseBuffer(ed, buf);
	}
}
static void
MenuDelete(AG_Event *event)
{
	AG_Editable *ed = AG_PTR(1);
	AG_EditableBuffer *buf;
	
	if ((buf = GetBuffer(ed)) != NULL) {
		if (AG_EditableDelete(ed, buf)) {
			CommitBuffer(ed, buf);
		}
		ReleaseBuffer(ed, buf);
	}
}
static void
MenuSelectAll(AG_Event *event)
{
	AG_Editable *ed = AG_PTR(1);
	AG_EditableBuffer *buf;
	
	if ((buf = GetBuffer(ed)) != NULL) {
		AG_EditableSelectAll(ed, buf);
		ReleaseBuffer(ed, buf);
	}
}
static void
MenuSetLang(AG_Event *event)
{
	AG_Editable *ed = AG_PTR(1);
	enum ag_language lang = (enum ag_language)AG_INT(2);

	AG_EditableSetLang(ed, lang);
}
static AG_PopupMenu *
PopupMenu(AG_Editable *ed)
{
	AG_PopupMenu *pm;
	AG_MenuItem *m;
	AG_Variable *vText;
	AG_Text *txt;

	if ((pm = AG_PopupNew(ed)) == NULL) {
		return (NULL);
	}
	m = AG_MenuAction(pm->item, _("Cut"), NULL, MenuCut, "%p", ed);
	m->state = (!AG_EditableReadOnly(ed) && ed->sel != 0);
	m = AG_MenuAction(pm->item, _("Copy"), NULL, MenuCopy, "%p", ed);
	m->state = (ed->sel != 0);
	m = AG_MenuAction(pm->item, _("Paste"), NULL, MenuPaste, "%p", ed);
	m->state = (!AG_EditableReadOnly(ed) && agEditableClipbrd.len > 0);
	m = AG_MenuAction(pm->item, _("Delete"), NULL, MenuDelete, "%p", ed);
	m->state = (!AG_EditableReadOnly(ed) && ed->sel != 0);
	AG_MenuSeparator(pm->item);
	AG_MenuAction(pm->item, _("Select All"), NULL, MenuSelectAll, "%p", ed);
	if ((ed->flags & AG_EDITABLE_MULTILINGUAL) &&
	    AG_Defined(ed, "text") &&
	    (vText = AG_GetVariable(ed, "text", &txt)) != NULL) {
		AG_MenuItem *mLang;
		int i;
		
		AG_MenuSeparator(pm->item);
		mLang = AG_MenuNode(pm->item, _("Select Language"), NULL);
		for (i = 0; i < AG_LANG_LAST; i++) {
			AG_TextEnt *te = &txt->ent[i];

			if (te->len == 0)
				continue;

			AG_MenuAction(mLang, _(agLanguageNames[i]),
			    NULL, MenuSetLang, "%p,%i", ed, i);
		}
		AG_UnlockVariable(vText);
	}
	return (pm);
}

static void
MouseButtonDown(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();
	int btn = AG_INT(1);
	int mx = AG_INT(2);
	int my = AG_INT(3);
	AG_EditableBuffer *buf;
	
	AG_WidgetFocus(ed);

	switch (btn) {
	case AG_MOUSE_LEFT:
		if (ed->pm != NULL) {
			AG_PopupDestroy(ed, ed->pm);
			ed->pm = NULL;
		}
		ed->flags |= AG_EDITABLE_CURSOR_MOVING|AG_EDITABLE_BLINK_ON;
		mx += ed->x;
		if ((buf = GetBuffer(ed)) == NULL) {
			return;
		}
		AG_EditableMoveCursor(ed, buf, mx, my, 0);
		ReleaseBuffer(ed, buf);
		ed->flags |= AG_EDITABLE_MARKPREF;

		if (ed->selDblClick != -1 &&
		    Fabs(ed->selDblClick - ed->pos) <= 1) {
			MouseDoubleClick(ed);
		} else {
			ed->selDblClick = ed->pos;
			AG_SchedEvent(NULL, ed, agMouseDblclickDelay,
			    "dblclick-expire", NULL);
		}
		break;
	case AG_MOUSE_RIGHT:
		if ((ed->flags & AG_EDITABLE_NOPOPUP) == 0) {
			if (ed->pm != NULL) {
				AG_PopupDestroy(ed, ed->pm);
			}
			if ((ed->pm = PopupMenu(ed)) != NULL)
				AG_PopupShowAt(ed->pm, mx,my);
		}
		break;
	case AG_MOUSE_WHEELUP:
		if (ed->flags & AG_EDITABLE_MULTILINE) {
			ed->flags |= AG_EDITABLE_NOSCROLL_ONCE;
			ed->y -= AG_WidgetScrollDelta(&ed->wheelTicks);
			if (ed->y < 0) { ed->y = 0; }
		}
		break;
	case AG_MOUSE_WHEELDOWN:
		if (ed->flags & AG_EDITABLE_MULTILINE) {
			ed->flags |= AG_EDITABLE_NOSCROLL_ONCE;
			ed->y += AG_WidgetScrollDelta(&ed->wheelTicks);
			ed->y = MIN(ed->y, ed->yMax - ed->yVis);
		}
		break;
	default:
		break;
	}
	
	AG_Redraw(ed);
}

static void
MouseButtonUp(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();
	int btn = AG_INT(1);

	switch (btn) {
	case AG_MOUSE_LEFT:
		ed->flags &= ~(AG_EDITABLE_CURSOR_MOVING);
		ed->flags &= ~(AG_EDITABLE_WORDSELECT);
		AG_Redraw(ed);
		break;
	default:
		break;
	}
}

static void
MouseMotion(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();
	AG_EditableBuffer *buf;
	int mx = AG_INT(1);
	int my = AG_INT(2);
	int newPos;

	if (!AG_WidgetIsFocused(ed))
		return;
	if ((ed->flags & AG_EDITABLE_CURSOR_MOVING) == 0)
		return;

	if ((buf = GetBuffer(ed)) == NULL)
		return;
	
	if (AG_EditableMapPosition(ed, buf, mx, my, &newPos, 0) != 0) {
		ReleaseBuffer(ed, buf);
		return;
	}

	if (ed->flags & AG_EDITABLE_WORDSELECT) {
		Uint32 *c;

		c = &buf->s[newPos];
		if (*c == (Uint32)('\n')) {
			goto out;
		}
		if (newPos > ed->pos) {
			if (ed->sel < 0) {
				ed->pos += ed->sel;
				ed->sel = -(ed->sel);
			}
			ed->sel = newPos - ed->pos;
			while (c < &buf->s[buf->len] &&
			    !isspace((char)*c) && *c != (Uint32)'\n') {
				c++;
				ed->sel++;
			}
		} else if (newPos < ed->pos) {
			if (ed->sel > 0) {
				ed->pos += ed->sel;
				ed->sel = -(ed->sel);
			}
			ed->sel = newPos - ed->pos;
			while (c > &buf->s[0] &&
			    !isspace((char)*c) && *c != (Uint32)'\n') {
				c--;
				ed->sel--;
			}
			if (isspace((char)buf->s[ed->pos + ed->sel])) {
				ed->sel++;
			}
		}
	} else {
		ed->sel = newPos - ed->pos;
	}
out:
	ReleaseBuffer(ed, buf);
	AG_Redraw(ed);
}

/*
 * Overwrite the contents of the text buffer with the given string
 * (supplied in UTF-8).
 */
void
AG_EditableSetString(AG_Editable *ed, const char *text)
{
	AG_EditableBuffer *buf;

	AG_ObjectLock(ed);
	if ((buf = GetBuffer(ed)) == NULL) {
		goto out;
	}
	if (text != NULL) {
		Free(buf->s);
		buf->s = AG_ImportUnicode("UTF-8", text, &buf->len, &buf->maxLen);
		if (buf->s != NULL) {
			ed->pos = buf->len;
		} else {
			buf->len = 0;
		}
	} else {
		if (buf->maxLen >= sizeof(Uint32)) {
			buf->s[0] = '\0';
			ed->pos = 0;
			buf->len = 0;
		}
	}
	ed->sel = 0;
	CommitBuffer(ed, buf);
	ReleaseBuffer(ed, buf);
out:
	AG_ObjectUnlock(ed);
	AG_Redraw(ed);
}

/*
 * Overwrite the contents of the text buffer with the given formatted
 * C string (in UTF-8 encoding).
 */
void
AG_EditablePrintf(void *obj, const char *fmt, ...)
{
	AG_Editable *ed = obj;
	AG_EditableBuffer *buf;
	va_list ap;
	char *s;

#ifdef AG_DEBUG
	if (!AG_OfClass(obj, "AG_Widget:AG_Editable:*") &&
	    !AG_OfClass(obj, "AG_Widget:AG_Textbox:*"))
		AG_FatalError(NULL);
#endif

	AG_ObjectLock(ed);
	if ((buf = GetBuffer(ed)) == NULL) {
		goto out;
	}
	if (fmt != NULL && fmt[0] != '\0') {
		va_start(ap, fmt);
		Vasprintf(&s, fmt, ap);
		va_end(ap);

		Free(buf->s);
		buf->s = AG_ImportUnicode("UTF-8", s, &buf->len, &buf->maxLen);
		if (buf->s != NULL) {
			ed->pos = buf->len;
		} else {
			buf->len = 0;
		}
	} else {
		s[0] = '\0';
		ed->pos = 0;
	}
	ed->sel = 0;
	CommitBuffer(ed, buf);
	ReleaseBuffer(ed, buf);
out:
	AG_ObjectUnlock(ed);
	AG_Redraw(ed);
}

/* Return a duplicate of the current string. */
char *
AG_EditableDupString(AG_Editable *ed)
{
	AG_Variable *var;
	char *sDup, *s;

	if (AG_Defined(ed, "text")) {
		AG_Text *txt;
		var = AG_GetVariable(ed, "text", &txt);
		s = txt->ent[ed->lang].buf;
		sDup = TryStrdup((s != NULL) ? s : "");
	} else {
		var = AG_GetVariable(ed, "string", &s);
		sDup = TryStrdup(s);
	}
	AG_UnlockVariable(var);
	return (sDup);
}

/* Copy text to a fixed-size buffer and always NUL-terminate. */
size_t
AG_EditableCopyString(AG_Editable *ed, char *dst, size_t dst_size)
{
	AG_Variable *var;
	size_t rv;
	char *s;

	AG_ObjectLock(ed);
	if (AG_Defined(ed, "text")) {
		AG_Text *txt;
		var = AG_GetVariable(ed, "text", &txt);
		s = txt->ent[ed->lang].buf;
		rv = Strlcpy(dst, (s != NULL) ? s : "", dst_size);
	} else {
		var = AG_GetVariable(ed, "string", &s);
		rv = Strlcpy(dst, s, dst_size);
	}
	AG_UnlockVariable(var);
	AG_ObjectUnlock(ed);
	return (rv);
}

/* Perform trivial conversion from string to int. */
int
AG_EditableInt(AG_Editable *ed)
{
	char abuf[32];
	AG_EditableBuffer *buf;
	int i;

	AG_ObjectLock(ed);
	if ((buf = GetBuffer(ed)) == NULL) {
		AG_FatalError(NULL);
	}
	AG_ExportUnicode("UTF-8", abuf, buf->s, sizeof(abuf));
	i = atoi(abuf);
	ReleaseBuffer(ed, buf);
	AG_ObjectUnlock(ed);
	return (i);
}

/* Perform trivial conversion from string to float . */
float
AG_EditableFlt(AG_Editable *ed)
{
	char abuf[32];
	AG_EditableBuffer *buf;
	float flt;

	AG_ObjectLock(ed);
	if ((buf = GetBuffer(ed)) == NULL) {
		AG_FatalError(NULL);
	}
	AG_ExportUnicode("UTF-8", abuf, buf->s, sizeof(abuf));
	flt = (float)strtod(abuf, NULL);
	ReleaseBuffer(ed, buf);
	AG_ObjectUnlock(ed);
	return (flt);
}

/* Perform trivial conversion from string to double. */
double
AG_EditableDbl(AG_Editable *ed)
{
	char abuf[32];
	AG_EditableBuffer *buf;
	double flt;

	AG_ObjectLock(ed);
	if ((buf = GetBuffer(ed)) == NULL) {
		AG_FatalError(NULL);
	}
	AG_ExportUnicode("UTF-8", abuf, buf->s, sizeof(abuf));
	flt = strtod(abuf, NULL);
	ReleaseBuffer(ed, buf);
	AG_ObjectUnlock(ed);
	return (flt);
}

static void
DoubleClickTimeout(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();
	ed->selDblClick = -1;
}

static void
Bound(AG_Event *event)
{
	AG_Editable *ed = AG_SELF();
	AG_Variable *binding = AG_PTR(1);

	if (strcmp(binding->name, "string") == 0) {
		AG_Unset(ed, "text");
	} else if (strcmp(binding->name, "text") == 0) {
		AG_Unset(ed, "string");
	}
}

static void
Init(void *obj)
{
	AG_Editable *ed = obj;

	WIDGET(ed)->flags |= AG_WIDGET_FOCUSABLE|
	                     AG_WIDGET_UNFOCUSED_MOTION|
			     AG_WIDGET_TABLE_EMBEDDABLE;

	ed->encoding = "UTF-8";

	if ((ed->text = AG_TextNewS(NULL)) == NULL)
		AG_FatalError(NULL);

	ed->flags = AG_EDITABLE_BLINK_ON|AG_EDITABLE_MARKPREF;
	ed->pos = 0;
	ed->sel = 0;
	ed->selDblClick = -1;
	ed->compose = 0;
	ed->wPre = 0;
	ed->hPre = agTextFontLineSkip + 2;
	ed->xCurs = 0;
	ed->yCurs = 0;
	ed->xCursPref = 0;

	ed->x = 0;
	ed->xMax = 10;
	ed->y = 0;
	ed->yMax = 1;
	ed->yVis = 1;
	ed->wheelTicks = 0;
	ed->repeatKey = 0;
	ed->repeatMod = AG_KEYMOD_NONE;
	ed->repeatUnicode = 0;
	ed->r = AG_RECT(0,0,0,0);
	ed->ca = NULL;
	ed->font = NULL;
	ed->pm = NULL;
	ed->lang = AG_LANG_NONE;

	ed->sBuf.var = NULL;
	ed->sBuf.s = NULL;
	ed->sBuf.len = 0;
	ed->sBuf.maxLen = 0;
	ed->sBuf.reallocable = 0;

	AG_SetEvent(ed, "key-down", KeyDown, NULL);
	AG_SetEvent(ed, "key-up", KeyUp, NULL);
	AG_SetEvent(ed, "mouse-button-down", MouseButtonDown, NULL);
	AG_SetEvent(ed, "mouse-button-up", MouseButtonUp, NULL);
	AG_SetEvent(ed, "mouse-motion", MouseMotion, NULL);
	AG_SetEvent(ed, "widget-gainfocus", GainedFocus, NULL);
	AG_SetEvent(ed, "widget-lostfocus", LostFocus, NULL);
	AG_AddEvent(ed, "widget-hidden", LostFocus, NULL);
	AG_SetEvent(ed, "dblclick-expire", DoubleClickTimeout, NULL);
	AG_SetEvent(ed, "bound", Bound, NULL);

	AG_SetTimeout(&ed->toRepeat, RepeatTimeout, NULL, 0);
	AG_SetTimeout(&ed->toDelay, DelayTimeout, NULL, 0);
	AG_SetTimeout(&ed->toCursorBlink, BlinkTimeout, NULL, 0);

	AG_BindPointer(ed, "text", (void *)ed->text);

	AG_RedrawOnTick(ed, 1000);

#ifdef AG_DEBUG
	AG_BindInt(ed, "pos", &ed->pos);
	AG_BindInt(ed, "sel", &ed->sel);
	AG_BindInt(ed, "xCurs", &ed->xCurs);
	AG_BindInt(ed, "yCurs", &ed->yCurs);
	AG_BindInt(ed, "xCursPref", &ed->xCursPref);
	AG_BindInt(ed, "x", &ed->x);
	AG_BindInt(ed, "xMax", &ed->xMax);
	AG_BindInt(ed, "y", &ed->y);
	AG_BindInt(ed, "yMax", &ed->yMax);
	AG_BindInt(ed, "yVis", &ed->yVis);
#endif /* AG_DEBUG */
}

static void
Destroy(void *obj)
{
	AG_Editable *ed = obj;

	if (ed->flags & AG_EDITABLE_EXCL)
		Free(ed->sBuf.s);
	
	AG_TextFree(ed->text);
}

/* Initialize/release the global clipboards. */
static void
InitClipboard(AG_EditableClipboard *cb)
{
	AG_MutexInit(&cb->lock);
	Strlcpy(cb->encoding, "UTF-8", sizeof(cb->encoding));
	cb->s = NULL;
	cb->len = 0;
}
static void
FreeClipboard(AG_EditableClipboard *cb)
{
	Free(cb->s);
	AG_MutexDestroy(&cb->lock);
}
void
AG_EditableInitClipboards(void)
{
	InitClipboard(&agEditableClipbrd);
	InitClipboard(&agEditableKillring);
}
void
AG_EditableDestroyClipboards(void)
{
	FreeClipboard(&agEditableClipbrd);
	FreeClipboard(&agEditableKillring);
}

AG_WidgetClass agEditableClass = {
	{
		"Agar(Widget:Editable)",
		sizeof(AG_Editable),
		{ 0,0 },
		Init,
		NULL,		/* free */
		Destroy,
		NULL,		/* load */
		NULL,		/* save */
		NULL		/* edit */
	},
	Draw,
	SizeRequest,
	SizeAllocate
};

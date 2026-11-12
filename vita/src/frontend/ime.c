/*  The system's on-screen keyboard (IME dialog) for a line of text: the game list's search.
    From ReBOX. GPL-2.0-or-later.
*/
#include "frontend/ime.h"
#include "video/video.h"
#include "platform/log.h"

#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <string.h>

int rs12_ime_input(const char *title, const char *initial, char *out, int out_size)
{
	static SceWChar16 wtitle[64], winit[64], wtext[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
	int i;
	for (i = 0; title[i] && i < 63; i++)
		wtitle[i] = (unsigned char)title[i];
	wtitle[i] = 0;
	for (i = 0; initial && initial[i] && i < 63; i++)
		winit[i] = (unsigned char)initial[i];
	winit[i] = 0;
	memset(wtext, 0, sizeof(wtext));

	SceImeDialogParam p;
	sceImeDialogParamInit(&p);
	p.supportedLanguages = 0;
	p.languagesForced = SCE_FALSE;
	p.type = SCE_IME_TYPE_BASIC_LATIN;
	p.option = 0;
	p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
	p.title = wtitle;
	p.maxTextLength = out_size - 1 < SCE_IME_DIALOG_MAX_TEXT_LENGTH ? (SceUInt32)(out_size - 1) : SCE_IME_DIALOG_MAX_TEXT_LENGTH;
	p.initialText = winit;
	p.inputTextBuffer = wtext;
	const int rc = sceImeDialogInit(&p);
	RS12_LOG("ime: dialog init %08x", (unsigned)rc);
	if (rc < 0)
		return 0;
	int entered = 0;
	for (;;) {   // (the dialog draws over the last picture: the screen kept presented while it runs)
		const SceCommonDialogStatus st = sceImeDialogGetStatus();
		if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
			SceImeDialogResult r;
			memset(&r, 0, sizeof(r));
			sceImeDialogGetResult(&r);
			entered = r.button == SCE_IME_DIALOG_BUTTON_ENTER;
			break;
		}
		if (st == SCE_COMMON_DIALOG_STATUS_NONE)
			break;
		rs12_video_present_dialog();
	}
	sceImeDialogTerm();
	if (!entered)
		return 0;
	int n = 0;   // (Latin input: the code points below 128 kept)
	for (i = 0; wtext[i] && n < out_size - 1; i++)
		if (wtext[i] < 128)
			out[n++] = (char)wtext[i];
	out[n] = 0;
	return 1;
}

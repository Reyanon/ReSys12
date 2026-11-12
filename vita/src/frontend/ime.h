// The system's on-screen keyboard (IME dialog). From ReBOX. GPL-2.0-or-later.
#ifndef RS12_IME_H
#define RS12_IME_H

// a line of text from the on-screen keyboard (title, the text it starts with): 1 = entered (out holds it), 0 = cancelled
int rs12_ime_input(const char *title, const char *initial, char *out, int out_size);

#endif

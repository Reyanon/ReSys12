// Update check and install from the GitHub releases of Reyanon/ReSys12 (same as ReST-V)
#ifndef RS12_UPDATE_H
#define RS12_UPDATE_H

// start the check on its own thread (network, GitHub API); nothing when the measuring tools started the app
void rs12_update_check_start(void);
// 1 once a release newer than this build (and not skipped) is known
int rs12_update_ready(void);
const char *rs12_update_version(void); // its tag, e.g. "v0.1.2"
const char *rs12_update_notes(void);   // the notes of every newer release, newest first (plain text, one line per '\n')
void rs12_update_skip(void);           // do not offer this version again
// download, unpack and hand over to the kernel helper (does not return then). progress(stage, percent) is called
// while it runs (percent -1: unknown, -2: the stage text alone). Returns a message on failure
const char *rs12_update_install(void (*progress)(const char *stage, int percent));

#endif

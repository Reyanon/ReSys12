// RePad: Bluetooth controllers for the PS Vita (kernel plugin). What apps can read from it.
//
// Ports: port 0 is the Vita's own controls, ports 1-4 the controllers RePad connected (in the order they came).
// sceCtrlGetControllerPortInfo reports them as DualShock 4 (SCE_CTRL_TYPE_DS4) so that apps accept them; the real
// kind of ports 1-4 is in the spare bytes of the port info: unk[0..3], after the marker REPAD_MARKER in unk[4..7].
// On a handheld Vita, controller 1 is added to port 0 (so every app sees it) unless an app reads ports 1-4 itself:
// then each controller stays on its own port (a multi-player app: the Vita 1P, a controller 2P).
#ifndef REPAD_H
#define REPAD_H

#define REPAD_MARKER "RPAD"
#define REPAD_SLOTS 4

// port info unk[8]: RePad's state. REPAD_FLAG_ON: it serves the controllers (else it stays loaded but leaves the
// controls as they are, as if it were not there)
#define REPAD_FLAG_ON 0x01

// commands from apps: sceCtrlPeekBufferPositive2(REPAD_CMD_PORT, &data, command) (data is not touched). RePad answers
// it itself (returns 0), without RePad the call fails as for any port that does not exist
#define REPAD_CMD_PORT 0x5250
enum { REPAD_CMD_OFF = 1, REPAD_CMD_ON = 2 };

enum {
	REPAD_KIND_NONE,
	REPAD_KIND_DS3,
	REPAD_KIND_DS4,
	REPAD_KIND_DUALSENSE,
	REPAD_KIND_XBOX_ONE,
	REPAD_KIND_SWITCH_PRO,
};

#endif

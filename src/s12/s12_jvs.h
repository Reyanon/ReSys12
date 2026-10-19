// ReSys12: JVS I/O boards on the H8's SCI0 (high-level model). Behaviour follows MAME src/devices/bus/jvs/jvs.cpp,
// jvshle.cpp and namcoio.cpp (BSD-3-Clause, smf).
#ifndef S12_JVS_H
#define S12_JVS_H

#include <stdint.h>

#define S12_JVS_NODES 2
#define S12_JVS_BUF 0x102

typedef struct {
	uint8_t address;     // 0xff = not set
	uint8_t initialized; // answered SetAddress (its sense line went to Initialized)
	uint16_t coin[2];    // coin counters
	uint8_t coin_prev[2];
} S12JvsNode;

typedef struct {
	int nodes;                    // 0 = nothing on the port
	S12JvsNode node[S12_JVS_NODES]; // node 0 is the one next to the host
	// host -> devices
	uint8_t rx[S12_JVS_BUF];
	int rx_len, rx_state;
	// devices -> host (escaped bytes on the wire)
	uint8_t tx[S12_JVS_BUF * 2 + 2];
	int tx_len, tx_pos;
	uint64_t tx_next; // H8 time of the next byte
} S12Jvs;

// attach the boards of the current set (MAME default JVS options)
void s12_jvs_reset(S12Jvs *j);
// host sent a byte (SCI0)
void s12_jvs_host_byte(S12Jvs *j, uint8_t b, uint64_t now);
// feed pending reply bytes to SCI0, one per byte time; call between H8 slices
void s12_jvs_update(S12Jvs *j, uint64_t now);
// sense line seen by the host: 0 = None (no device), 1 = Uninitialized, 2 = Initialized
int s12_jvs_sense(const S12Jvs *j);
// 120 Hz coin sampling (MAME coin timer); called once per frame is enough for the coin pulses the frontend makes
void s12_jvs_frame(S12Jvs *j);

#endif

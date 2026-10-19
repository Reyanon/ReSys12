// ReSys12: JVS I/O boards on the H8's SCI0, as a high-level model of the board firmware.
// Behaviour follows MAME src/devices/bus/jvs/jvs.cpp (port, sense line, daisy chain), jvshle.cpp (packet framing,
// standard commands) and namcoio.cpp (Namco boards, EM Pri1-01 printer) (BSD-3-Clause, smf).
//
// MAME runs the boards' own H8/3334 firmware (technodr EM I/O1-02, aplarail ASCA-1); here the board answers the same
// commands directly: identification string (read from the set's I/O ROM when present), features (8 analog channels,
// 1 player x 19 switches, 2 coin slots, outputs), switches, coins, analog inputs (10 bits, left-justified as the
// board's ADC), outputs (ignored). Bytes go through SCI0 at the JVS rate (115200 baud, one byte per 10 bits).
#include "s12_jvs.h"

#include <string.h>
#ifdef RS12_HARNESS
#include <stdio.h>
#include <stdlib.h>
// diagnostics: RS12_JVSLOG=1 prints every packet (host > / board <)
static void jvs_log(const char *dir, const uint8_t *b, int n)
{
	static int on = -1;
	if (on < 0)
		on = getenv("RS12_JVSLOG") != NULL;
	if (!on)
		return;
	fprintf(stderr, "jvs %s", dir);
	for (int i = 0; i < n; i++)
		fprintf(stderr, " %02x", b[i]);
	fprintf(stderr, "\n");
}
#else
#define jvs_log(dir, b, n) ((void)0)
#endif

#include "h8.h"
#include "s12.h"

enum { SYNC = 0xe0, ESCAPE = 0xd0 };
enum { RX_SYNC, RX_DATA, RX_ESCAPED };
// one byte on the wire: 10 bits x 16 ticks of the 1.8432 MHz clock, in H8 states (16.9344 MHz)
#define BYTE_STATES 1470
enum { NODE_IO, NODE_PRINTER };

static int node_kind(int n)
{
	return (n == 1 && s12.game && s12.game->machine == S12_M_TECHNODR) ? NODE_PRINTER : NODE_IO;
}

void s12_jvs_reset(S12Jvs *j)
{
	memset(j, 0, sizeof(*j));
	const S12Game *g = s12.game;
	// MAME default JVS options: aplarail (ASCA-1 / Cyber Lead), technodr (EM I/O1-02 + EM Pri1-01 printer), questff
	if (g && (g->machine == S12_M_APLARAIL || g->machine == S12_M_QUESTFF))
		j->nodes = 1;
	else if (g && g->machine == S12_M_TECHNODR)
		j->nodes = 2;
	for (int n = 0; n < S12_JVS_NODES; n++) {
		j->node[n].address = 0xff;
		j->node[n].coin_prev[0] = j->node[n].coin_prev[1] = 1;
	}
}

int s12_jvs_sense(const S12Jvs *j)
{
	if (!j->nodes)
		return 0;
	return j->node[0].initialized ? 2 : 1;
}

// ---- identification ----
// the board's own string from its program ROM (no ROM bytes in this source); a plain one if the set has no I/O ROM.
// The 8 bytes before it are the firmware date the Namco command 0x70/02 returns (*date, NULL without the ROM).
static const char *io_id_date(const uint8_t **date)
{
	static char id[100];
	const S12Rom *r = s12.rom;
	*date = NULL;
	if (r && r->rgn[S12_RGN_IOCPU]) {
		const uint8_t *p = r->rgn[S12_RGN_IOCPU];
		const uint32_t n = r->rgn_size[S12_RGN_IOCPU];
		static const char key[] = "namco ltd.;";
		for (uint32_t i = 0; i + sizeof(key) < n; i++) {
			if (memcmp(p + i, key, sizeof(key) - 1))
				continue;
			uint32_t k = 0;
			while (i + k < n && p[i + k] && k < sizeof(id) - 1) {
				id[k] = (char)p[i + k];
				k++;
			}
			id[k] = 0;
			if (i >= 8)
				*date = p + i - 8;
			return id;
		}
	}
	return "namco ltd.;I/O PCB;Ver1.00;JPN";
}

static const char *io_id(void)
{
	const uint8_t *date;
	return io_id_date(&date);
}

// ---- inputs (MAME JVS_PLAYER layout: bit 7 start, 6 service, 5 up, 4 down, 3 left, 2 right, 1 B1, 0 B2, 15 B3,
// 14 B4, 13 B5, 12 B6, 11 B7, 10 B8) ----
static uint32_t player_bits(int p)
{
	const uint32_t in = s12.in.player[p & 1];
	uint32_t v = 0;
	if (in & S12_IN_START) v |= 0x80;
	if (in & S12_IN_SERVICE) v |= 0x40;
	if (in & S12_IN_UP) v |= 0x20;
	if (in & S12_IN_DOWN) v |= 0x10;
	if (in & S12_IN_LEFT) v |= 0x08;
	if (in & S12_IN_RIGHT) v |= 0x04;
	if (in & S12_IN_B1) v |= 0x02;
	if (in & S12_IN_B2) v |= 0x01;
	if (in & S12_IN_B3) v |= 0x8000;
	if (in & S12_IN_B4) v |= 0x4000;
	if (in & S12_IN_B5) v |= 0x2000;
	if (in & S12_IN_B6) v |= 0x1000;
	if (in & S12_IN_B7) v |= 0x0800;
	if (in & S12_IN_B8) v |= 0x0400;
	return v;
}

void s12_jvs_frame(S12Jvs *j)
{
	for (int n = 0; n < j->nodes; n++) {
		if (node_kind(n) != NODE_IO)
			continue;
		S12JvsNode *d = &j->node[n];
		for (int c = 0; c < 2; c++) {
			const uint8_t now = (s12.in.system & (c ? S12_SYS_COIN2 : S12_SYS_COIN1)) ? 1 : 0;
			if (now && !d->coin_prev[c])
				d->coin[c] = (uint16_t)((d->coin[c] + 1) & 0x3fff);
			d->coin_prev[c] = now;
		}
	}
}

// ---- packet handling (MAME jvs_hle_device::packet_received / execute) ----
typedef struct {
	const uint8_t *in;
	int in_pos, in_len; // parameters: in[in_pos .. in_len - 2] (the last byte is the checksum)
	uint8_t out[S12_JVS_BUF];
	int out_len;
} Packet;

static int consume(Packet *pk, int n, const uint8_t **par)
{
	if (pk->in_pos + n < pk->in_len) {
		*par = pk->in + pk->in_pos;
		pk->in_pos += n;
		return 1;
	}
	if (pk->out[2] == 0x01 && pk->out_len + 1 < S12_JVS_BUF)
		pk->out[pk->out_len++] = 0x02; // report: incorrect number of parameters
	pk->in_pos = pk->in_len - 1;
	return 0;
}

static uint8_t *produce(Packet *pk, int n)
{
	if (pk->out_len + n >= S12_JVS_BUF) {
		pk->out[2] = 0x04; // status: acknowledge overflow
		return NULL;
	}
	if (pk->out[2] != 0x01)
		return NULL;
	uint8_t *r = pk->out + pk->out_len;
	pk->out_len += n;
	return r;
}

static void put_report(Packet *pk, uint8_t report)
{
	uint8_t *r = produce(pk, 1);
	if (r)
		r[0] = report;
}

static void execute(S12Jvs *j, int n, Packet *pk, uint8_t cmd)
{
	S12JvsNode *d = &j->node[n];
	const int io = node_kind(n) == NODE_IO;
	const uint8_t *par;
	uint8_t *r;
	switch (cmd) {
	case 0xf0: // reset
		if (consume(pk, 1, &par) && par[0] == 0xd9) {
			d->address = 0xff;
			d->initialized = 0;
		}
		break;
	case 0xf1: // set address: the last unaddressed node of the chain takes it (its downstream sense is not Uninitialized)
		if (consume(pk, 1, &par) && d->address == 0xff && (n + 1 >= j->nodes || j->node[n + 1].initialized) &&
			(r = produce(pk, 1)) != NULL) {
			d->address = par[0];
			r[0] = 0x01;
		}
		break;
	case 0x10: { // I/O identify
		const char *id = io ? io_id() : "namco ltd.;EM Pri1-01;Ver2.00;JPN&EXP,Techno-Drive PRN";
		const int len = (int)strlen(id) + 1;
		if ((r = produce(pk, 1 + len)) != NULL) {
			r[0] = 0x01;
			memcpy(r + 1, id, (size_t)len);
		}
		break;
	}
	case 0x11: // command revision
	case 0x12: // JVS revision
	case 0x13: // communication version
		if ((r = produce(pk, 2)) != NULL) { // the Namco boards' firmware answers 1.1 / 2.0 / 1.0
			r[0] = 0x01;
			if (io)
				r[1] = cmd == 0x11 ? 0x11 : cmd == 0x12 ? 0x20 : 0x10;
			else
				r[1] = cmd == 0x11 ? 0x13 : cmd == 0x12 ? 0x30 : 0x10;
		}
		break;
	case 0x70: // Namco: board information (EM I/O1-02 / ASCA-1 firmware, subcommand & 0x3f)
		if (!io) {
			pk->out[2] = 0x02;
			break;
		}
		if (!consume(pk, 1, &par))
			break;
		switch (par[0] & 0x3f) {
		case 0x01: // 8 bytes of board memory at the given address: not modelled, zeros
			if (consume(pk, 2, &par) && (r = produce(pk, 9)) != NULL) {
				memset(r, 0, 9);
				r[0] = 0x01;
			}
			break;
		case 0x02: { // firmware date
			const uint8_t *date;
			io_id_date(&date);
			if ((r = produce(pk, 9)) != NULL) {
				r[0] = 0x01;
				if (date)
					memcpy(r + 1, date, 8);
				else
					memset(r + 1, 0, 8);
			}
			break;
		}
		case 0x03: // board DIP switches: all off
			if ((r = produce(pk, 2)) != NULL) {
				r[0] = 0x01;
				r[1] = 0x00;
			}
			break;
		case 0x04: // RAM self test at power on: 0xffff = passed
			if ((r = produce(pk, 3)) != NULL) {
				r[0] = 0x01;
				r[1] = 0xff;
				r[2] = 0xff;
			}
			break;
		default:
			put_report(pk, 0x01);
			break;
		}
		break;
	case 0x14: // feature check
		put_report(pk, 0x01);
		if (io) {
			// switches: 1 player x 19, coins: 2 slots, analog: 8 channels, general purpose outputs: EM I/O1-02 24,
			// ASCA-1 13
			const uint8_t outputs = (s12.game && s12.game->machine == S12_M_TECHNODR) ? 24 : 13;
			const uint8_t feat[] = { 0x01, 1, 19, 0, 0x02, 2, 0, 0, 0x03, 8, 0, 0, 0x12, outputs, 0, 0 };
			if ((r = produce(pk, sizeof(feat))) != NULL)
				memcpy(r, feat, sizeof(feat));
		}
		put_report(pk, 0x00);
		break;
	case 0x15: { // main board id: a NUL-terminated string
		if (!consume(pk, 1, &par))
			break;
		int len = 0;
		while (pk->in_pos + len < pk->in_len - 1 && par[len])
			len++;
		consume(pk, len, &par);
		put_report(pk, 0x01);
		break;
	}
	case 0x20: // switches (players, bytes)
		if (consume(pk, 2, &par)) {
			const int players = par[0], bytes = par[1];
			if (io && players <= 1 && bytes <= 3) {
				if ((r = produce(pk, 2 + players * bytes)) != NULL) {
					r[0] = 0x01;
					r[1] = (s12.in.system & S12_SYS_TEST) ? 0x80 : 0x00;
					const uint32_t v = player_bits(0);
					for (int p = 0; p < players; p++)
						for (int k = 0; k < bytes; k++)
							r[2 + p * bytes + k] = (uint8_t)(v >> (k * 8));
				}
			} else
				put_report(pk, 0x03);
		}
		break;
	case 0x21: // coins (slots)
		if (consume(pk, 1, &par)) {
			const int count = par[0];
			if (io && count <= 2) {
				if ((r = produce(pk, 1 + count * 2)) != NULL) {
					r[0] = 0x01;
					for (int c = 0; c < count; c++) {
						r[1 + c * 2] = (uint8_t)((d->coin[c] >> 8) & 0x3f);
						r[2 + c * 2] = (uint8_t)d->coin[c];
					}
				}
			} else
				put_report(pk, 0x03);
		}
		break;
	case 0x22: // analog (channels): 10-bit ADC values, left-justified
		if (consume(pk, 1, &par)) {
			const int count = par[0];
			if (io && count <= 8) {
				if ((r = produce(pk, 1 + count * 2)) != NULL) {
					r[0] = 0x01;
					for (int c = 0; c < count; c++) {
						const uint16_t v = (uint16_t)(s12.in.jvs_analog[c] & 0xffc0);
						r[1 + c * 2] = (uint8_t)(v >> 8);
						r[2 + c * 2] = (uint8_t)v;
					}
				}
			} else
				put_report(pk, 0x03);
		}
		break;
	case 0x2f: // retransmit: not kept, answer normally (the host retries the command)
		break;
	case 0x30: // coin decrease (slot, amount)
	case 0x35: // coin increase
		if (consume(pk, 3, &par)) {
			const int c = par[0] - 1;
			const int amount = par[1] << 8 | par[2];
			if (io && c >= 0 && c < 2) {
				if (cmd == 0x30)
					d->coin[c] = (uint16_t)(d->coin[c] < amount ? 0 : d->coin[c] - amount);
				else
					d->coin[c] = (uint16_t)((d->coin[c] + amount) & 0x3fff);
				put_report(pk, 0x01);
			} else
				put_report(pk, 0x03);
		}
		break;
	case 0x32: // general purpose output 1 (bytes, data): lamps, ignored
		if (consume(pk, 1, &par) && consume(pk, par[0], &par))
			put_report(pk, io ? 0x01 : 0x03);
		break;
	case 0x33: // analog output (channels, 2 bytes each)
		if (consume(pk, 1, &par) && consume(pk, par[0] * 2, &par))
			put_report(pk, 0x01);
		break;
	case 0x37: // general purpose output 2 (index, data)
	case 0x38: // general purpose output 3 (index, data)
		if (consume(pk, 2, &par))
			put_report(pk, 0x01);
		break;
	case 0x72: // EM Pri1-01 printer (MAME namco_em_pri1_01_device): always ready, prints nothing
		if (io) {
			pk->out[2] = 0x02;
			break;
		}
		if (!consume(pk, 1, &par))
			break;
		switch (par[0]) {
		case 0x01: case 0x02: case 0x04:
			put_report(pk, 0x01);
			break;
		case 0x03: // status: 1 = ok
			if ((r = produce(pk, 2)) != NULL) {
				r[0] = 0x01;
				r[1] = 0x01;
			}
			break;
		case 0x05:
			if (consume(pk, 1, &par))
				put_report(pk, par[0] == 1 ? 0x01 : 0x03);
			break;
		case 0x07:
			if (consume(pk, 1, &par) && consume(pk, par[0], &par))
				put_report(pk, 0x01);
			break;
		default:
			pk->out[2] = 0x02; // status: unknown command
			break;
		}
		break;
	default:
		pk->out[2] = 0x02; // status: unknown command
		break;
	}
}

static void queue_reply(S12Jvs *j, const Packet *pk, uint64_t now)
{
	uint8_t sum = 0;
	j->tx_len = 0;
	j->tx_pos = 0;
	j->tx[j->tx_len++] = SYNC;
	for (int i = 0; i <= pk->out_len; i++) {
		uint8_t b;
		if (i < pk->out_len) {
			b = pk->out[i];
			sum = (uint8_t)(sum + b);
		} else
			b = sum;
		if (b == SYNC || b == ESCAPE) {
			j->tx[j->tx_len++] = ESCAPE;
			b--;
		}
		j->tx[j->tx_len++] = b;
	}
	jvs_log("<", j->tx, j->tx_len);
	// the host's last byte is still being shifted out, then the line turns around
	j->tx_next = now + 3 * BYTE_STATES;
}

static void packet_received(S12Jvs *j, uint64_t now)
{
	const uint8_t *in = j->rx;
	const int len = j->rx_len;
	jvs_log(">", in, len);
	if (len < 4)
		return;
	uint8_t sum = 0;
	for (int i = 0; i < len - 1; i++)
		sum = (uint8_t)(sum + in[i]);
	for (int n = j->nodes - 1; n >= 0; n--) {
		S12JvsNode *d = &j->node[n];
		if (in[0] != 0xff && in[0] != d->address)
			continue;
		Packet pk;
		memset(&pk, 0, sizeof(pk));
		pk.in = in;
		pk.in_len = len;
		pk.out[0] = 0x00; // to the host
		pk.out[2] = 0x01; // status: normal
		pk.out_len = 3;
		if (in[len - 1] != sum) {
			if (in[0] == 0xff)
				continue;
			pk.out[2] = 0x03; // status: checksum error
		} else {
			pk.in_pos = 2;
			const uint8_t was = d->address;
			while (pk.out[2] == 0x01 && pk.in_pos < len - 1)
				execute(j, n, &pk, in[pk.in_pos++]);
			if (pk.out_len == 3 && pk.out[2] == 0x01)
				continue; // nothing to say (reset, or a broadcast another node took)
			if (was == 0xff && d->address != 0xff)
				d->initialized = 1; // MAME: sense goes to Initialized once the reply is out
		}
		pk.out[1] = (uint8_t)(pk.out_len - 1); // length: status + reports + checksum
		queue_reply(j, &pk, now);
		return; // one node answers a packet
	}
}

void s12_jvs_host_byte(S12Jvs *j, uint8_t b, uint64_t now)
{
	if (!j->nodes)
		return;
	if (b == SYNC) {
		j->rx_len = 0;
		j->rx_state = RX_DATA;
		return;
	}
	if (j->rx_state == RX_SYNC)
		return;
	if (b == ESCAPE) {
		j->rx_state = RX_ESCAPED;
		return;
	}
	if (j->rx_state == RX_ESCAPED) {
		b++;
		j->rx_state = RX_DATA;
	}
	if (j->rx_len >= S12_JVS_BUF) {
		j->rx_len = 0;
		j->rx_state = RX_SYNC;
		return;
	}
	j->rx[j->rx_len++] = b;
	if (j->rx_len >= 2 && j->rx_len == j->rx[1] + 2) {
		packet_received(j, now);
		j->rx_len = 0;
		j->rx_state = RX_SYNC;
	}
}

void s12_jvs_update(S12Jvs *j, uint64_t now)
{
	while (j->tx_pos < j->tx_len && now >= j->tx_next) {
		if (!h8_sci_rx_byte(0, j->tx[j->tx_pos]))
			return; // receiver off or the last byte not read yet: wait
		j->tx_pos++;
		j->tx_next += BYTE_STATES;
	}
}

#ifndef GPIB_SIM_H_
#define GPIB_SIM_H_

/* Simulated instruments on a simulated GPIB bus, at the level of the handshake lines. Time advances 1 us per line
 * read or board_millis() call, plus the delays. Host tests and vxi11_host, and a board's SIM=1 build (no GPIB plug). */

#include <stdint.h>
#include <stdbool.h>

typedef struct sim_dev {
	/* configuration */
	uint8_t pad;
	int8_t  sad;      /* secondary address, -1 = none */
	int     slow;     /* line reads per handshake transition */
	void  (*on_message)(struct sim_dev *); /* a received message ended with EOI */
	/* talker side */
	uint8_t  out[256];
	uint16_t out_len, out_pos;
	bool     out_eoi;    /* EOI with the last byte */
	int      out_delay;  /* line reads before the answer is ready */
	uint8_t  status;
	bool     srq;
	/* listener side */
	uint8_t  in[4096];
	bool     in_eoi[4096];
	uint16_t in_len;
	int      messages;
	/* every command byte seen, and counters */
	uint8_t  cmds[256];
	uint16_t ncmds;
	int      sdc, get, gtl, llo, ifc;
	/* state */
	bool listener, talker, spe, lpas, tpas, status_sent, ifc_low;
	enum { AH_READY, AH_ACCEPTED } ah;
	enum { SH_IDLE, SH_WAIT_NRFD, SH_WAIT_NDAC } sh;
	int      wait;
	uint8_t  cur;
	bool     cur_eoi, cur_status;
	uint32_t lines;
} sim_dev_t;

#ifndef SIM_MAX_DEVS
#define SIM_MAX_DEVS 3 /* a board build takes 1: each is ~8.7 KB of RAM */
#endif

extern uint64_t sim_now_us;
extern bool     sim_powered;
extern int      sim_led_errors;

void       sim_reset(void);                  /* no instruments, bus powered */
sim_dev_t *sim_add(uint8_t pad, int8_t sad);
void       sim_answer(sim_dev_t *d, const char *s, bool eoi);

#endif /* GPIB_SIM_H_ */

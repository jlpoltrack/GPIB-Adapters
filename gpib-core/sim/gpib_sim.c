#include <string.h>
#include "gpib_sim.h"
#include "gpib_sim_pins.h"
#include "board.h"

uint32_t sim_ctrl = L_ALL;
uint64_t sim_now_us;
bool     sim_powered = true;
int      sim_led_errors; /* board_led_error() on the host */

static sim_dev_t s_devs[SIM_MAX_DEVS];
static int       s_ndevs;

void sim_pins_init(void)
{
	sim_ctrl = (L_ALL & ~L_REN) | (sim_ctrl & L_REN); /* everything released, REN kept (phy_init() sets it) */
}

void sim_reset(void)
{
	memset(s_devs, 0, sizeof(s_devs));
	s_ndevs = 0;
	sim_powered = true;
	sim_led_errors = 0;
	sim_ctrl = L_ALL;
}

sim_dev_t *sim_add(uint8_t pad, int8_t sad)
{
	sim_dev_t *d = &s_devs[s_ndevs++];
	d->pad = pad;
	d->sad = sad;
	d->lines = L_ALL;
	return d;
}

void sim_answer(sim_dev_t *d, const char *s, bool eoi)
{
	d->out_len = (uint16_t)strlen(s);
	memcpy(d->out, s, d->out_len);
	d->out_pos = 0;
	d->out_eoi = eoi;
}

static uint32_t combine(void)
{
	uint32_t v = sim_ctrl;
	for (int i = 0; i < s_ndevs; i++)
		v &= s_devs[i].lines;
	return sim_powered ? v : 0;
}

static void command(sim_dev_t *d, uint8_t b)
{
	bool lpas = d->lpas, tpas = d->tpas;

	if (d->ncmds < sizeof(d->cmds))
		d->cmds[d->ncmds++] = b;
	d->lpas = d->tpas = false;

	if (b == 0x3F) {
		d->listener = false;
	} else if (b == 0x5F) {
		d->talker = false;
	} else if ((b >= 0x20) && (b <= 0x3E)) {
		if ((b & 0x1F) == d->pad) {
			if (d->sad < 0)
				d->listener = true;
			else
				d->lpas = true;
		}
	} else if ((b >= 0x40) && (b <= 0x5E)) {
		if ((b & 0x1F) == d->pad) {
			if (d->sad < 0)
				d->talker = true;
			else
				d->tpas = true;
		} else {
			d->talker = false; /* only one talker */
		}
	} else if ((b >= 0x60) && (b <= 0x7E)) {
		if (lpas)
			d->listener = ((b & 0x1F) == d->sad);
		if (tpas)
			d->talker = ((b & 0x1F) == d->sad);
	} else if (b == 0x18) {
		d->spe = true;
		d->status_sent = false;
	} else if (b == 0x19) {
		d->spe = false;
	} else if ((b == 0x04) && d->listener) {
		d->sdc++;
		d->out_len = d->out_pos = 0;
	} else if ((b == 0x08) && d->listener) {
		d->get++;
	} else if ((b == 0x01) && d->listener) {
		d->gtl++;
	} else if (b == 0x11) {
		d->llo++;
	}
}

static void data(sim_dev_t *d, uint8_t b, bool eoi)
{
	if (d->in_len < sizeof(d->in)) {
		d->in_eoi[d->in_len] = eoi;
		d->in[d->in_len++] = b;
	}
	if (eoi) {
		d->messages++;
		if (d->on_message)
			d->on_message(d);
	}
}

/* true once the device's handshake delay has passed */
static bool waited(sim_dev_t *d)
{
	if (d->wait < d->slow) {
		d->wait++;
		return false;
	}
	d->wait = 0;
	return true;
}

static void step(sim_dev_t *d)
{
	uint32_t b = combine();
	bool atn = !(b & L_ATN);
	uint32_t out = L_ALL;

	if (!(b & L_IFC)) {
		if (!d->ifc_low)
			d->ifc++;
		d->ifc_low = true;
		d->listener = d->talker = d->spe = d->lpas = d->tpas = false;
		d->ah = AH_READY;
		d->sh = SH_IDLE;
		d->lines = L_ALL;
		return;
	}
	d->ifc_low = false;
	if (d->srq)
		out &= ~L_SRQ;
	if (d->out_delay > 0)
		d->out_delay--;

	/* acceptor: every device during ATN, the listeners otherwise */
	if (atn || d->listener) {
		if ((d->ah == AH_READY) && !(b & L_DAV) && waited(d)) {
			uint8_t byte = (uint8_t)~b;
			if (atn)
				command(d, byte);
			else
				data(d, byte, !(b & L_EOI));
			d->ah = AH_ACCEPTED;
		} else if ((d->ah == AH_ACCEPTED) && (b & L_DAV) && waited(d)) {
			d->ah = AH_READY;
		}
		if (d->ah == AH_READY)
			out &= ~L_NDAC; /* ready: NRFD released */
		else
			out &= ~L_NRFD; /* accepted: NDAC released */
	} else {
		d->ah = AH_READY;
	}

	/* source */
	if (d->talker && !atn) {
		if (d->sh == SH_IDLE) {
			if (d->spe && !d->status_sent) {
				d->cur = (uint8_t)(d->status | (d->srq ? 0x40 : 0));
				d->cur_eoi = false;
				d->cur_status = true;
				d->sh = SH_WAIT_NRFD;
			} else if (!d->spe && (d->out_pos < d->out_len) && (d->out_delay == 0)) {
				d->cur = d->out[d->out_pos];
				d->cur_eoi = d->out_eoi && (d->out_pos + 1 == d->out_len);
				d->cur_status = false;
				d->sh = SH_WAIT_NRFD;
			}
		}
		if ((d->sh == SH_WAIT_NRFD) && (b & L_NRFD) && !(b & L_NDAC) && waited(d))
			d->sh = SH_WAIT_NDAC;
		if ((d->sh == SH_WAIT_NDAC) && (b & L_NDAC)) {
			d->sh = SH_IDLE;
			if (d->cur_status) {
				d->status_sent = true;
				d->srq = false;
			} else {
				d->out_pos++;
			}
		}
		if (d->sh != SH_IDLE) {
			out &= ~(uint32_t)d->cur;
			if (d->cur_eoi)
				out &= ~L_EOI;
			if (d->sh == SH_WAIT_NDAC)
				out &= ~L_DAV;
		}
	} else {
		d->sh = SH_IDLE; /* ATN took over: the byte is sent again later */
	}
	d->lines = out;
}

uint32_t sim_bus(void)
{
	sim_now_us++;
	for (int i = 0; i < s_ndevs; i++)
		step(&s_devs[i]);
	return combine();
}

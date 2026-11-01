/*
 * The touch screen of the Galaxy A72 (a72q): an STMicroelectronics FTS5CU56A on I2C, as a source of
 * touches for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg touchscreen=1.
 *
 * STATUS: written, never run. Nothing here has been seen working on a phone. Every value marked
 * "verify" comes from a public source and has to be checked against the phone's own boot log.
 *
 * Where the values come from (see devices/a72q/HARDWARE.md):
 *   bus      QUP wrap 1, serial engine 1 ("i2c7" in the mainline tree), at 0x00a84000, 400 kHz,
 *            pins GPIO 6 (SDA) and 7 (SCL)                       sc7180.dtsi / sm7125-samsung-common.dtsi
 *   chip     I2C address 0x49; interrupt line TLMM GPIO 9        sm7125-samsung-common.dtsi
 *   supplies vdd = LDO10A 1.8 V, avdd = LDO7C 3.0 V              sm7125-samsung-common.dtsi
 *   protocol device ID read 0x22 (answers 0x48 0x36 ...), sense on 0x10, sense off 0x11,
 *            read one event 0x60, clear all events 0x62, 16-byte events; a coordinate event has
 *            event id 0 in bits [1:0] of byte 0, the finger in [5:2], the action in [7:6]
 *            (1 press, 2 move, 3 release), X = byte1 << 4 | byte3 >> 4, Y = byte2 << 4 | byte3 & 0xf
 *                                                                Samsung's fts5cu56a driver header (GPL),
 *                                                                read for the interface only
 *
 * What this first version assumes: the boot firmware left the I2C engine's clocks on and its pins
 * muxed, and the touch chip's two supplies up (the stock boot draws the Samsung logo with the panel
 * on, so this is likely but not certain). The driver checks the engine is there and is an I2C one
 * before it touches anything, and stops with a log line if not. Turning the supplies and clocks on
 * itself (RPMh and GCC, the way hakios.goodix.c does on the POCO) is step two, once a boot log
 * says whether it is needed.
 *
 * No interrupt: a thread asks the chip for one event at a time, 125 times a second
 * (boot-arg touch_poll_us=N to change), the same polling the POCO's driver does.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define TAG "hakios-stmfts: "

static vm_offset_t se;

static inline uint32_t rd(vm_offset_t a) { return *(volatile uint32_t *)a; }
static inline void wr(vm_offset_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile ("dsb sy" ::: "memory"); }
static void sleep_us(uint32_t us) { delay(us); }

/* ---- the serial engine, as an I2C master in FIFO mode ------------------------------------------------- */

#define SE_PHYS                 0x00a84000UL    /* verify: wrap 1, engine 1 */
#define SE_SIZE                 0x4000UL
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 3 = I2C */
#define I2C_TX_TRANS_LEN        (se + 0x26c)
#define I2C_RX_TRANS_LEN        (se + 0x270)
#define I2C_SCL_COUNTERS        (se + 0x278)    /* [29:20] high, [19:10] low, [9:0] cycle */
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation, [26:0] parameters */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* [24:0] words waiting */

#define OP_WRITE                1
#define OP_READ                 2
#define PARAM_ADDR(a)           ((uint32_t)(a) << 9)
#define PARAM_STOP_STRETCH      (1u << 2)       /* no STOP after this transfer: a repeated START follows */
#define IRQ_CMD_DONE            (1u << 0)
#define IRQ_NACK_MASK           (0x3fu << 6)    /* verify: the "general purpose" bits the engine sets on NACK/arbitration */

#define CHIP_ADDR               0x49

static bool
i2c_wait_done(const char *what)
{
	for (int i = 0; i < 2000; i++) {                        /* 20 ms */
		uint32_t s = rd(GENI_M_IRQ_STATUS);
		if (s & IRQ_NACK_MASK) {
			wr(GENI_M_IRQ_CLEAR, s);
			wr(GENI_M_CMD_CTRL, 1u << 1);           /* abort */
			printf(TAG "%s: no answer (irq 0x%x)\n", what, s);
			return false;
		}
		if (s & IRQ_CMD_DONE) {
			wr(GENI_M_IRQ_CLEAR, s);
			return true;
		}
		sleep_us(10);
	}
	wr(GENI_M_CMD_CTRL, 1u << 2);                           /* cancel */
	printf(TAG "%s: timed out\n", what);
	return false;
}

/* write `len` bytes, then (if rlen) read `rlen` bytes after a repeated START */
static bool
i2c_xfer(const uint8_t *out, uint32_t len, uint8_t *in, uint32_t rlen)
{
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(I2C_TX_TRANS_LEN, len);
	wr(GENI_M_CMD0, (OP_WRITE << 27) | PARAM_ADDR(CHIP_ADDR) | (rlen ? PARAM_STOP_STRETCH : 0));
	for (uint32_t i = 0; i < len; i += 4) {
		uint32_t w = 0;
		for (uint32_t b = 0; b < 4 && i + b < len; b++) {
			w |= (uint32_t)out[i + b] << (8 * b);
		}
		wr(GENI_TX_FIFO, w);
	}
	if (!i2c_wait_done("write")) {
		return false;
	}
	if (!rlen) {
		return true;
	}
	wr(I2C_RX_TRANS_LEN, rlen);
	wr(GENI_M_CMD0, (OP_READ << 27) | PARAM_ADDR(CHIP_ADDR));
	uint32_t got = 0;
	for (int i = 0; i < 2000 && got < rlen; i++) {
		while ((rd(GENI_RX_FIFO_STATUS) & 0x1ffffff) && got < rlen) {
			uint32_t w = rd(GENI_RX_FIFO);
			for (uint32_t b = 0; b < 4 && got < rlen; b++) {
				in[got++] = (uint8_t)(w >> (8 * b));
			}
		}
		if (got < rlen) {
			sleep_us(10);
		}
	}
	return i2c_wait_done("read") && got == rlen;
}

static bool
i2c_setup(void)
{
	se = ml_io_map(SE_PHYS, SE_SIZE);
	uint32_t fw = rd(GENI_FW_REVISION_RO);
	uint32_t proto = (fw >> 8) & 0xff;
	printf(TAG "serial engine at 0x%lx: firmware 0x%x, protocol %u\n", SE_PHYS, fw, proto);
	if (proto != 3) {
		printf(TAG "not an I2C engine (or its clocks are off): stopping, nothing touched\n");
		return false;
	}
	/* 400 kHz from the 19.2 MHz crystal: divider 2, high 5, low 11, cycle 22 (verify against the boot log) */
	wr(GENI_SER_M_CLK_CFG, (2u << 4) | 1);
	wr(I2C_SCL_COUNTERS, (5u << 20) | (11u << 10) | 22u);
	return true;
}

/* ---- the chip ------------------------------------------------------------------------------------------ */

#define CMD_SENSE_ON            0x10
#define CMD_READ_ID             0x22
#define CMD_READ_ONE_EVENT      0x60
#define CMD_CLEAR_ALL_EVENT     0x62
#define EVENT_SIZE              16
#define EID_COORDINATE          0
#define ACTION_PRESS            1
#define ACTION_MOVE             2
#define ACTION_RELEASE          3
#define FINGERS                 10

static uint32_t poll_us = 8000;
static uint32_t flip;                                   /* boot-arg touch_flip: bit 0 X, bit 1 Y, bit 2 swap */
static uint32_t max_x = 1080, max_y = 2400;             /* the A72's panel, in the chip's units (verify) */
static struct { bool down; uint32_t x, y; } contact[FINGERS];

static void
report(const uint8_t *e)
{
	uint32_t eid = e[0] & 3, finger = (e[0] >> 2) & 0xf, action = e[0] >> 6;
	if (eid != EID_COORDINATE || finger >= FINGERS) {
		return;
	}
	uint32_t x = ((uint32_t)e[1] << 4) | (e[3] >> 4);
	uint32_t y = ((uint32_t)e[2] << 4) | (e[3] & 0xf);
	if (flip & 4) { uint32_t t = x; x = y; y = t; }
	if ((flip & 1) && x < max_x) x = max_x - 1 - x;
	if ((flip & 2) && y < max_y) y = max_y - 1 - y;

	if (action == ACTION_RELEASE) {
		if (contact[finger].down) {
			contact[finger].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, finger, contact[finger].x, contact[finger].y);
		}
		return;
	}
	if (action != ACTION_PRESS && action != ACTION_MOVE) {
		return;
	}
	hakios_touch_post(contact[finger].down ? HAKIOS_TOUCH_MOVE : HAKIOS_TOUCH_DOWN, finger, x, y);
	contact[finger].down = true;
	contact[finger].x = x;
	contact[finger].y = y;
}

static void
stmfts_thread(void *arg __unused, wait_result_t wr_unused __unused)
{
	if (!i2c_setup()) {
		return;
	}
	uint8_t cmd = CMD_READ_ID, id[3] = { 0 };
	if (!i2c_xfer(&cmd, 1, id, sizeof(id))) {
		printf(TAG "the chip did not answer at 0x%x: supplies off, pins not muxed, or another address\n", CHIP_ADDR);
		return;
	}
	printf(TAG "device ID %02x %02x %02x (an FTS5 answers 48 36 ...)\n", id[0], id[1], id[2]);

	cmd = CMD_CLEAR_ALL_EVENT;
	i2c_xfer(&cmd, 1, NULL, 0);
	cmd = CMD_SENSE_ON;
	if (!i2c_xfer(&cmd, 1, NULL, 0)) {
		printf(TAG "sense on was refused\n");
		return;
	}
	printf(TAG "scanning; polling every %u us\n", poll_us);

	uint32_t errors = 0;
	for (;;) {
		uint8_t e[EVENT_SIZE];
		cmd = CMD_READ_ONE_EVENT;
		if (i2c_xfer(&cmd, 1, e, sizeof(e))) {
			errors = 0;
			if (e[0] != 0 || e[1] != 0 || e[2] != 0) {      /* an empty queue reads as zeros */
				report(e);
				continue;                               /* more may be queued: read again at once */
			}
		} else if (++errors == 50) {
			printf(TAG "50 failed reads in a row: stopping\n");
			return;
		}
		delay(poll_us);
	}
}

void
hakios_stmfts_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (kernel_thread_start(stmfts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

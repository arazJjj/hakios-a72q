/*
 * The touch screen of the Galaxy A72 (a72q): an STMicroelectronics FTS5CU56A on I2C, as a source of
 * touches for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg touchscreen=1.
 *
 * STATUS: v2, compiled against the interfaces only, never run on the phone. v1 could not have worked:
 *   - it was never started (no STARTUP line, and hakios.input.c does not call it), and the device's
 *     config did not build it (CONFIG_HAKIOS_STMFTS, CONFIG_TOUCHSCREEN);
 *   - it treated bit 7 of M_IRQ_STATUS (M_RX_IRQ, set on every read) as a NACK, so every read failed;
 *   - it never enabled M_IRQ_EN, and the status bits it polls only follow their enables, so CMD_DONE
 *     never showed and every transfer timed out;
 *   - it read the serial engine's registers before turning its clocks on, which can hang the bus;
 *   - it set no FIFO packing, FIFO mode or pin mux, and left the touch chip's supplies to chance.
 * v2 does what Linux's i2c-qcom-geni.c and qcom-geni-se.c do, the way hakios.goodix.c does it for SPI.
 *
 * Where the values come from:
 *   bus      QUP wrap 1, serial engine 1 ("i2c7"), 0x00a84000, clock GCC_QUPV3_WRAP1_S1_CLK  sc7180.dtsi
 *            pins GPIO 6 (SDA), 7 (SCL), north tile 0x03900000, function 1 "qup11_i2c"   pinctrl-sc7180.c
 *   clocks   vote register 0x52008: wrap1 core 2x bit 18, core 19, m-ahb 20, s-ahb 21, s1 23;
 *            S1 RCG at 0x18148 (DFS), branch 0x18144; m-ahb 0x1800c, s-ahb 0x18010            gcc-sc7180.c
 *   chip     address 0x49, interrupt TLMM GPIO 9 (active low)                  sm7125-samsung-common.dtsi
 *   supplies vdd = LDO10A 1.8 V ("ldoa10"), avdd = LDO7C 3.0 V ("ldoc7"), through RPMh: cmd-db at
 *            0x80820000, the apps RSC's DRV 2 at 0x18220000, TCS offset 0xd00               sc7180.dtsi
 *   I2C      400 kHz from 19.2 MHz: divider 2, high 5, low 11, cycle 22; errors on GP_IRQ 0/1/3/4  i2c-qcom-geni.c
 *   protocol device ID 0x22, sense on 0x10, read one event 0x60, clear events 0x62, 16-byte events;
 *            coordinate event: id [1:0], finger [5:2], action [7:6] (1 press, 2 move, 3 release),
 *            X = b1 << 4 | b3 >> 4, Y = b2 << 4 | b3 & 0xf      Samsung's stm_ts driver header (GPL), interface only
 *
 * Writes outside the engine: the GCC votes, two TLMM pin configurations, and one RPMh request per supply
 * (both are the supplies Android itself keeps on for this chip). Nothing is written to storage.
 * Boot-args: touchscreen=1 (start), touch_poll_us=N, touch_flip=N (bit 0 X, bit 1 Y, bit 2 swap),
 * touch_supplies=0 (leave the supplies as the boot firmware left them).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern lck_mtx_t hakios_rsc_lock;

#define TAG "hakios-stmfts: "

static vm_offset_t se, gcc, tlmm_north, rsc, cmd_db;

static inline uint32_t rd(vm_offset_t a) { return *(volatile uint32_t *)a; }
static inline void wr(vm_offset_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile ("dsb sy" ::: "memory"); }
static void sleep_us(uint32_t us) { delay(us); }

/* ---- pins ---------------------------------------------------------------------------------------- */

#define TLMM_NORTH_PHYS         0x03900000UL
#define TLMM_NORTH_SIZE         0x300000UL
#define GPIO_CFG(n)             (tlmm_north + 0x1000UL * (n))   /* [1:0] pull, [5:2] function, [8:6] drive */
#define GPIO_IO(n)              (tlmm_north + 0x1000UL * (n) + 4)
#define PIN_SDA                 6
#define PIN_SCL                 7
#define PIN_INT                 9
#define FUNC_QUP11_I2C          1
#define PULL_UP                 3

static void
pins_on(void)
{
	/* function qup11_i2c, pull-up, 2 mA: what Linux's qup_i2c7 default state is */
	wr(GPIO_CFG(PIN_SDA), PULL_UP | FUNC_QUP11_I2C << 2);
	wr(GPIO_CFG(PIN_SCL), PULL_UP | FUNC_QUP11_I2C << 2);
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)
#define VOTE_WRAP1_CORE_2X      (1u << 18)
#define VOTE_WRAP1_CORE         (1u << 19)
#define VOTE_WRAP1_M_AHB        (1u << 20)
#define VOTE_WRAP1_S_AHB        (1u << 21)
#define VOTE_WRAP1_S1           (1u << 23)
#define GCC_WRAP1_M_AHB_CBCR    (gcc + 0x1800c)
#define GCC_WRAP1_S_AHB_CBCR    (gcc + 0x18010)
#define GCC_WRAP1_S1_CBCR       (gcc + 0x18144)
#define GCC_WRAP1_S1_CMD        (gcc + 0x18148)
#define GCC_WRAP1_S1_CFG        (gcc + 0x1814c)
#define GCC_WRAP1_S1_DFSR       (gcc + 0x1815c)
#define GCC_WRAP1_S1_PERF_CFG(l) (gcc + 0x18164 + 4 * (l))
#define GCC_WRAP1_S1_PERF_M(l)  (gcc + 0x181a4 + 4 * (l))
#define GCC_WRAP1_S1_PERF_N(l)  (gcc + 0x181e4 + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_WRAP1_CORE_2X | VOTE_WRAP1_CORE | VOTE_WRAP1_M_AHB | VOTE_WRAP1_S_AHB);
	if (rd(GCC_WRAP1_S1_DFSR) & 1) {
		/* preset rates: the I2C timings below are for exactly 19.2 MHz */
		bool found = false;

		for (uint32_t level = 0; level < 8 && !found; level++) {
			if (rcg_rate(rd(GCC_WRAP1_S1_PERF_CFG(level)), rd(GCC_WRAP1_S1_PERF_M(level)),
			    rd(GCC_WRAP1_S1_PERF_N(level))) == XO_HZ) {
				se_clock_select = level;
				found = true;
			}
		}
		if (!found) {
			printf(TAG "no 19.2 MHz preset rate for the serial engine\n");
			return false;
		}
	} else {
		/* the crystal, undivided */
		wr(GCC_WRAP1_S1_CFG, (rd(GCC_WRAP1_S1_CFG) & ~0x371fu) | 1);
		wr(GCC_WRAP1_S1_CMD, rd(GCC_WRAP1_S1_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_WRAP1_S1_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_WRAP1_S1_CMD) & 1) {
			printf(TAG "the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_WRAP1_S1_CMD));
			return false;
		}
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_WRAP1_S1);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_WRAP1_M_AHB_CBCR) | rd(GCC_WRAP1_S_AHB_CBCR) | rd(GCC_WRAP1_S1_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf(TAG "clocks stay off: %#x %#x %#x\n", rd(GCC_WRAP1_M_AHB_CBCR), rd(GCC_WRAP1_S_AHB_CBCR),
	    rd(GCC_WRAP1_S1_CBCR));
	return false;
}

/* ---- the chip's supplies (RPMh) ------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80820000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's DRV 2 */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf(TAG "no command database at %#lx\n", CMD_DB_PHYS);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done = false;

	lck_mtx_lock(&hakios_rsc_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf(TAG "RPMh: the command slots stay busy\n");
			lck_mtx_unlock(&hakios_rsc_lock);
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	for (int i = 0; i < 2000 && !done; i++) {
		done = rd(TCS(TCS_IRQ_STATUS)) & 1;
		if (!done) {
			sleep_us(100);
		}
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(&hakios_rsc_lock);
	if (!done) {
		printf(TAG "RPMh: no answer to the request\n");
	}
	return done;
}

static bool
supply_on(const char *name, uint32_t millivolts)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf(TAG "RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf(TAG "%s on (%u mV)\n", name, millivolts);
	return true;
}

/* ---- I2C through the serial engine (GENI), FIFO mode ------------------------------------------------- */

#define SE_PHYS                 0x00a84000UL
#define SE_SIZE                 0x4000UL
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_SER_S_CLK_CFG      (se + 0x04c)
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 3 = I2C */
#define GENI_CLK_SEL            (se + 0x07c)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define I2C_TX_TRANS_LEN        (se + 0x26c)
#define I2C_RX_TRANS_LEN        (se + 0x270)
#define I2C_SCL_COUNTERS        (se + 0x278)    /* [29:20] high, [19:10] low, [9:0] cycle */
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation, [26:0] parameters */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* [24:0] words waiting */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] TX FIFO depth in words */

#define OP_WRITE                1
#define OP_READ                 2
#define PARAM_ADDR(a)           ((uint32_t)(a) << 9)
#define PARAM_STOP_STRETCH      (1u << 2)       /* no STOP: a repeated START follows */
#define M_CMD_DONE              (1u << 0)
/* overrun, illegal command, failure; GP_IRQ 0 (unknown), 1 (NACK), 3 (bus), 4 (arbitration); FIFO errors */
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 9) | (1u << 10) | (1u << 12) | \
	                         (1u << 13) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define M_IRQ_WANTED            (0x7fc0007fu | (0x3fu << 9))
#define CHIP_ADDR               0x49

static bool
i2c_wait_done(const char *what)
{
	for (int i = 0; i < 2000; i++) {                        /* 20 ms */
		uint32_t s = rd(GENI_M_IRQ_STATUS);

		if (s & M_ERRORS) {
			wr(GENI_M_IRQ_CLEAR, s);
			wr(GENI_M_CMD_CTRL, 1u << 1);           /* abort */
			if (s & (1u << 10)) {
				printf(TAG "%s: NACK from 0x%x\n", what, CHIP_ADDR);
			} else {
				printf(TAG "%s: error (irq 0x%x)\n", what, s);
			}
			return false;
		}
		if (s & M_CMD_DONE) {
			wr(GENI_M_IRQ_CLEAR, s);
			return true;
		}
		sleep_us(10);
	}
	wr(GENI_M_CMD_CTRL, 1u << 2);                           /* cancel */
	printf(TAG "%s: timed out (irq 0x%x)\n", what, rd(GENI_M_IRQ_STATUS));
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
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
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
			if (rd(GENI_M_IRQ_STATUS) & M_ERRORS) {
				break;                          /* i2c_wait_done says which */
			}
			sleep_us(10);
		}
	}
	return i2c_wait_done("read") && got == rlen;
}

/* 0 = ready; otherwise why not */
static const char *
i2c_setup(void)
{
	uint32_t depth;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 3) {
		return "the serial engine has no I2C firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled (GSI only)";
	}
	depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (depth < 2) {
		return "the serial engine reports no FIFO";
	}
	/* geni_se_init + geni_se_select_fifo_mode + geni_se_config_packing(8 bits, 4 per word, MSB first) */
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_TX_WATERMARK, 0);
	wr(GENI_RX_WATERMARK, depth - 1);
	wr(GENI_RX_RFR_WATERMARK, depth);
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | M_IRQ_WANTED);   /* the status bits we poll follow these */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	/* 400 kHz from 19.2 MHz (geni_i2c_clk_map_19p2mhz) */
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (2u << 4) | 1);
	wr(GENI_SER_S_CLK_CFG, (2u << 4) | 1);
	wr(I2C_SCL_COUNTERS, (5u << 20) | (11u << 10) | 22u);
	return NULL;
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
static uint32_t flip;
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
	uint32_t supplies = 1;
	const char *why;

	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_NORTH_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);

	PE_parse_boot_argn("touch_supplies", &supplies, sizeof(supplies));
	if (supplies) {
		cmd_db = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
		/* vdd first, then avdd, as Samsung's driver powers it; then the chip's own start-up time */
		if (!supply_on("ldoa10", 1800) || !supply_on("ldoc7", 3000)) {
			printf(TAG "going on with the supplies as the boot firmware left them\n");
		}
		delay(50000);
	}
	pins_on();
	if (!clocks_on()) {
		return;                                 /* the engine's registers are not touched with its clocks off */
	}
	if ((why = i2c_setup()) != NULL) {
		printf(TAG "%s: stopping\n", why);
		return;
	}

	uint8_t cmd = CMD_READ_ID, id[3] = { 0 };
	if (!i2c_xfer(&cmd, 1, id, sizeof(id))) {
		printf(TAG "the chip did not answer at 0x%x (int pin %u)\n", CHIP_ADDR, rd(GPIO_IO(PIN_INT)) & 1);
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
				continue;
			}
		} else if (++errors == 50) {
			printf(TAG "50 failed reads in a row: stopping\n");
			return;
		}
		delay(poll_us);
	}
}

/* EARLY_BOOT is too early for a thread of our own: a thread call starts it once the scheduler runs (as pmic does) */
static void
stmfts_start(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
	thread_t thread;

	if (kernel_thread_start(stmfts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf(TAG "no thread: stopping\n");
	}
}

static void
hakios_stmfts_init(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;                                 /* nothing is touched unless asked */
	}
	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	thread_call_enter(thread_call_allocate(stmfts_start, NULL));
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_stmfts_init);

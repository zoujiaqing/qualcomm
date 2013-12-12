/*-
 * Copyright (c) 2013 Ganbold Tsagaankhuu <ganbold@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/timeet.h>
#include <sys/timetc.h>
#include <sys/watchdog.h>
#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/intr.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <sys/kdb.h>

/*
 * Use GPT timer for event timer and
 * DGT timer for delay and timecounter.
 * Use global krait mpcore timer address.
 * There are also per core timers and that seems to be used as
 * local timers.
 *
 * Android source says:
 * 0-15:  STI/SGI (software triggered/generated interrupts)
 * 16-31: PPI (private peripheral interrupts)
 * 32+:   SPI (shared peripheral interrupts)
 *
 * #define GIC_PPI_START 16
 * #define GIC_SPI_START 32
 *
 * #define INT_DEBUG_TIMER_EXP	(GIC_PPI_START + 1)
 * #define INT_GP_TIMER_EXP	(GIC_PPI_START + 2)
 * #define INT_GP_TIMER2_EXP	(GIC_PPI_START + 3)
 *
 * So global timer irqs will be:
 * Global DGT timer irq: 17
 * Global GPT0 timer irq: 18
 * Global GPT1 timer irq: 19
 */

#define	MSM_TMR_BASE		0xf200a000
#define	MSM_GPT_BASE		0x04
#define	MSM_DGT_BASE		0x24
#define	SPSS_TIMER_STATUS	0x88

#define	GPT_REG(off)		(MSM_GPT_BASE + (off))
#define	DGT_REG(off)		(MSM_DGT_BASE + (off))

#define	GPT_MATCH_VAL		GPT_REG(0x0000)
#define	GPT_COUNT_VAL		GPT_REG(0x0004)
#define	GPT_ENABLE		GPT_REG(0x0008)
#define	GPT_CLEAR		GPT_REG(0x000C)

#define	DGT_MATCH_VAL		DGT_REG(0x0000)
#define	DGT_COUNT_VAL		DGT_REG(0x0004)
#define	DGT_ENABLE		DGT_REG(0x0008)
#define	DGT_CLEAR		DGT_REG(0x000C)
#define	DGT_CLK_CTL		DGT_REG(0x0010)

#define	GPT_ENABLE_CLR_ON_MATCH_EN	2
#define	GPT_ENABLE_EN			1
#define	DGT_ENABLE_CLR_ON_MATCH_EN	2
#define	DGT_ENABLE_EN			1

#define	SPSS_TIMER_STATUS_DGT_EN		(1 << 0)
#define	SPSS_TIMER_STATUS_DGT_CLR_ON_MTCH	(1 << 1)
#define	SPSS_TIMER_STATUS_DGT_CLR_PEND		(1 << 2)
#define	SPSS_TIMER_STATUS_DGT_WR_PEND		(1 << 3)

#define	SPSS_TIMER_STATUS_GPT_EN		(1 << 8)
#define	SPSS_TIMER_STATUS_GPT_CLR_ON_MTCH	(1 << 9)
#define	SPSS_TIMER_STATUS_GPT_CLR_PEND		(1 << 10)
#define	SPSS_TIMER_STATUS_GPT_WR_PEND		(1 << 11)

/* 
 * PXO clock source is 27MHz.
 * It is possible for the AHB clock to run at as slow as 5 MHz
 * using settings of the Global Clock Controller. The debug timer
 * counter can also run at 5 MHz (TCXO divided by 4).
 * However, due to the synchronization circuit using the edge detect,
 * the timer should always run at least 4x slower than the AHB clock.
 * Thus, if the timer's use is required by the system, the divider
 * should be set to divide by 4 and the slowest usable AHB frequency
 * is 20MHz. The timer would run at 5MHz in this case.
 */
enum {
	DGT_CLK_CTL_DIV_1 = 0,
	DGT_CLK_CTL_DIV_2 = 1,
	DGT_CLK_CTL_DIV_3 = 2,
	DGT_CLK_CTL_DIV_4 = 3,
};

/*
#define SYS_TIMER_CLKSRC		32768
#define SYS_TIMER_CLKSRC		27000000
*/

#define GPT_TIMER_CLKSRC		19200000
#define DGT_TIMER_CLKSRC		6750000

struct krait_timer_softc {
	device_t 	sc_dev;
	struct resource *res[2];
	bus_space_tag_t sc_bst;
	bus_space_handle_t sc_bsh;
	void 		*sc_ih;		/* interrupt handler */
	uint32_t 	sc_period;
	uint32_t 	timer0_freq;
	struct eventtimer et;
	int		sc_oneshot;
};

int krait_timer_get_timerfreq(struct krait_timer_softc *);

#define	timer_read_4(sc, reg)	\
	bus_space_read_4(sc->sc_bst, sc->sc_bsh, reg)
#define	timer_write_4(sc, reg, val)	\
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, reg, val)

static u_int	krait_timer_get_timecount(struct timecounter *);
static int	krait_timer_start(struct eventtimer *,
    sbintime_t first, sbintime_t period);
static int	krait_timer_stop(struct eventtimer *);

static int krait_timer_initialized = 0;
static int krait_timer_hardclock(void *);
static int krait_timer_probe(device_t);
static int krait_timer_attach(device_t);

static struct timecounter krait_timer_timecounter = {
	.tc_name           = "krait_timer timer0",
	.tc_get_timecount  = krait_timer_get_timecount,
	.tc_counter_mask   = ~0u,
	.tc_frequency      = 0,
	.tc_quality        = 1000,
};

struct krait_timer_softc *krait_timer_sc = NULL;

static struct resource_spec krait_timer_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE },
	{ -1, 0 }
};

static int
krait_timer_probe(device_t dev)
{
	struct krait_timer_softc *sc;

	sc = device_get_softc(dev);

	if (!ofw_bus_is_compatible(dev, "qcom,msm-timer"))
		return (ENXIO);

	device_set_desc(dev, "Qualcomm KRAIT timer");
	return (BUS_PROBE_DEFAULT);
}

static int
krait_timer_attach(device_t dev)
{
	struct krait_timer_softc *sc;
	int err;
	uint32_t val;

	sc = device_get_softc(dev);

	if (bus_alloc_resources(dev, krait_timer_spec, sc->res)) {
		device_printf(dev, "could not allocate resources\n");
		return (ENXIO);
	}

	sc->sc_dev = dev;
	sc->sc_bst = rman_get_bustag(sc->res[0]);
	sc->sc_bsh = rman_get_bushandle(sc->res[0]);

	/* Setup and enable the timer interrupt */
	err = bus_setup_intr(dev, sc->res[1], INTR_TYPE_CLK,
	    krait_timer_hardclock, NULL, sc, &sc->sc_ih);
	if (err != 0) {
		bus_release_resources(dev, krait_timer_spec, sc->res);
		device_printf(dev, "Unable to setup the clock irq handler, "
		    "err = %d\n", err);
		return (ENXIO);
	}

	sc->timer0_freq = DGT_TIMER_CLKSRC;

	/* Set desired frequency in event timer and timecounter */
	sc->et.et_frequency = GPT_TIMER_CLKSRC;
	sc->et.et_name = "krait_timer Eventtimer";
	sc->et.et_flags = ET_FLAGS_ONESHOT | ET_FLAGS_PERIODIC;
	sc->et.et_quality = 1000;
	sc->et.et_min_period = (0x00000005LLU << 32) / sc->et.et_frequency;
	sc->et.et_max_period = (0xfffffffeLLU << 32) / sc->et.et_frequency;
	sc->et.et_start = krait_timer_start;
	sc->et.et_stop = krait_timer_stop;
	sc->et.et_priv = sc;
	et_register(&sc->et);

	if (device_get_unit(dev) == 0)
		krait_timer_sc = sc;

	krait_timer_timecounter.tc_frequency = sc->timer0_freq;
	tc_init(&krait_timer_timecounter);

	if (bootverbose) {
		device_printf(sc->sc_dev, "clock: hz=%d stathz = %d\n",
		    hz, stathz);

		device_printf(sc->sc_dev, "event timer clock frequency %u\n",
		    sc->timer0_freq);
		device_printf(sc->sc_dev, "timecounter clock frequency %lld\n",
		    krait_timer_timecounter.tc_frequency);
	}
	/* Set clock for DGT timer */
	timer_write_4(sc, DGT_CLK_CTL, DGT_CLK_CTL_DIV_4);

	/* First disable timers at attach */
	val = timer_read_4(sc, DGT_ENABLE);
	val &= ~DGT_ENABLE_EN;
	timer_write_4(sc, DGT_ENABLE, val);

	val = timer_read_4(sc, GPT_ENABLE);
	val &= ~GPT_ENABLE_EN;
	timer_write_4(sc, GPT_ENABLE, val);

	krait_timer_initialized = 1;

	return (0);
}

static int
krait_timer_start(struct eventtimer *et, sbintime_t first,
    sbintime_t period)
{
	struct krait_timer_softc *sc;
	uint32_t count;
	uint32_t val;

	sc = (struct krait_timer_softc *)et->et_priv;

	if (period != 0)
		sc->sc_period = ((uint32_t)et->et_frequency * period) >> 32;
	else
		sc->sc_period = 0;
	if (first != 0)
		count = ((uint32_t)et->et_frequency * first) >> 32;
	else
		count = sc->sc_period;

	/*
	 * Update match value.
	 * The general purpose timer will signal interrupt
	 * when its counter value has reached the value stored
	 * in the MTCH register.
	 */
	timer_write_4(sc, GPT_MATCH_VAL, count);
	/*
	 * CLR register is a one-shot command register that,
	 * when written with any value, resets the timer to a value of 0.
	 * This occurs regardless of the state of the GPT_EN/DGT_EN register
	 */
	timer_write_4(sc, GPT_CLEAR, 0);

	/*
	 * Set timer mode and enable GPT timer.
	 * When bit GPT_ENABLE_CLR_ON_MATCH_EN is set,
	 * the timer will clear when it reaches the match value.
	 */
	val = timer_read_4(sc, GPT_ENABLE);
	if (period != 0) {
		/* periodic */
		sc->sc_oneshot = 0;
		val |= GPT_ENABLE_CLR_ON_MATCH_EN;
	} else {
		/* oneshot */
		sc->sc_oneshot = 1;
		val &= ~GPT_ENABLE_CLR_ON_MATCH_EN;
	}
	val |= GPT_ENABLE_EN;
	timer_write_4(sc, GPT_ENABLE, val);

	/*
	 * Enable DGT timer and use it in free running mode
	 * for timecounter and delay.
	 */
	val = timer_read_4(sc, DGT_ENABLE);
	val |= DGT_ENABLE_EN;
	timer_write_4(sc, DGT_ENABLE, val);

	/* Load maximum value to MTCH register */
	timer_write_4(sc, DGT_MATCH_VAL, ~0);

	/*
	 * XXX: Android/linux uses it in this order.
	 * Clear the counter.
	 */
	timer_write_4(sc, DGT_CLEAR, 0);

	return (0);
}

static int
krait_timer_stop(struct eventtimer *et)
{
	struct krait_timer_softc *sc;
	uint32_t val;

	sc = (struct krait_timer_softc *)et->et_priv;

	/* Disable GPT timer */
	val = timer_read_4(sc, GPT_ENABLE);
	val &= ~GPT_ENABLE_EN;
	timer_write_4(sc, GPT_ENABLE, val);

	/* wait until it is disabled */
	while (timer_read_4(sc, SPSS_TIMER_STATUS) & SPSS_TIMER_STATUS_GPT_EN)
		;
	/* Reset GPT timer */
        timer_write_4(sc, GPT_CLEAR, 0);

	/* XXX: wait until it is disabled */
	while (timer_read_4(sc, SPSS_TIMER_STATUS) & SPSS_TIMER_STATUS_GPT_EN)
		;
	sc->sc_period = 0;

	return (0);
}

int
krait_timer_get_timerfreq(struct krait_timer_softc *sc)
{

	return (sc->timer0_freq);
}

void
cpu_initclocks(void)
{

	cpu_initclocks_bsp();
}

static int
krait_timer_hardclock(void *arg)
{
	struct krait_timer_softc *sc;
	uint32_t val;

	sc = (struct krait_timer_softc *)arg;
	
	/*
	 * Timers don't support an interrupt enable/disable bit.
	 * Workaround the lack of an interrupt enable bit by explicitly
	 * stopping the timer in the interrupt handler when the clockevent
	 * is in ONESHOT mode. This should prevent any possibility of the
	 * timer wrapping and matching again.
	 */

	if(sc->sc_oneshot) {
		/* disable GPT timer */
		val = timer_read_4(sc, GPT_ENABLE);
		val &= ~GPT_ENABLE_EN;
		timer_write_4(sc, GPT_ENABLE, val);
	} else {

		val = timer_read_4(sc, GPT_ENABLE);
		/*
		 * Zero value for bit GPT_ENABLE_CLR_ON_MATCH_EN and
		 * sc_period > 0 means timer_start was called with non NULL
		 * first value. Now we will set periodic timer with the
		 * given period value.
		 */
		if ((val & (1<<1)) == 0 && sc->sc_period > 0) {
			/* Update timer */
			timer_write_4(sc, GPT_MATCH_VAL, sc->sc_period);

			/* Make periodic */
			sc->sc_oneshot = 0;
			val |= GPT_ENABLE_CLR_ON_MATCH_EN;
		}
		/* Enable GPT timer in any case */
		val |= GPT_ENABLE_EN;
		timer_write_4(sc, GPT_ENABLE, val);
	}
	if (sc->et.et_active)
		sc->et.et_event_cb(&sc->et, sc->et.et_arg);

	return (FILTER_HANDLED);
}

u_int
krait_timer_get_timecount(struct timecounter *tc)
{
	u_int timecount;

	if (krait_timer_sc == NULL)
		return (0);

	/* Use DGT timer as timecounter */
	timecount = timer_read_4(krait_timer_sc, DGT_COUNT_VAL);

	return (timecount);
}

static device_method_t krait_timer_methods[] = {
	DEVMETHOD(device_probe,		krait_timer_probe),
	DEVMETHOD(device_attach,	krait_timer_attach),

	DEVMETHOD_END
};

static driver_t krait_timer_driver = {
	"krait_timer",
	krait_timer_methods,
	sizeof(struct krait_timer_softc),
};

static devclass_t krait_timer_devclass;

DRIVER_MODULE(krait_timer, simplebus, krait_timer_driver,
    krait_timer_devclass, 0, 0);

void
DELAY(int usec)
{
	int32_t counts, counts_per_usec;
	uint32_t first, last;

	/*
	 * Check the timers are setup, if not just
	 * use a for loop for the meantime
	 */
	if (krait_timer_sc == NULL) {
		for (; usec > 0; usec--)
			for (counts = 200; counts > 0; counts--)
				/*
				 * Prevent gcc from optimizing
				 * out the loop   
				 */
				cpufunc_nullop();
		return;
	}

	/* Get the number of times to count */
	counts_per_usec =
	    ((krait_timer_timecounter.tc_frequency / 1000000) + 1);

	/*
	 * Clamp the timeout at a maximum value (about 32 seconds with
	 * a 66MHz clock). *Nobody* should be delay()ing for anywhere
	 * near that length of time and if they are, they should be hung
	 * out to dry.
	 */
	if (usec >= (0x80000000U / counts_per_usec))
		counts = (0x80000000U / counts_per_usec) - 1;
	else
		counts = usec * counts_per_usec;

	first = timer_read_4(krait_timer_sc, DGT_COUNT_VAL);

	while (counts > 0) {
		last = timer_read_4(krait_timer_sc, DGT_COUNT_VAL);
		counts -= (int32_t)(last - first);
		first = last;
	}
}


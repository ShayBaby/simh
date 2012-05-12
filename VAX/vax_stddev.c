/* vax_stddev.c: VAX 3900 standard I/O devices

   Copyright (c) 1998-2012, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   tti          terminal input
   tto          terminal output
   clk          100Hz and TODR clock

   18-Apr-12    RMS     Revised TTI to use clock coscheduling and
                        remove IORESET bug
   13-Jan-12    MP      Normalized the saved format of the TODR persistent 
                        file so that it may be moved around from one platform
                        to another along with other simulator state files 
                        (disk & tape images, save/restore files, etc.)
   28-Sep-11    MP      Generalized setting TODR for all OSes.  
                        Unbound the TODR value from the 100hz clock tick 
                        interrupt.  TODR now behaves like the original 
                        battery backed-up clock and runs with the wall 
                        clock, not the simulated instruction clock
                        (except when running ROM diagnostics).
                        Two operational modes are available:
                        - Default VMS mode, which is similar to the previous 
                          behavior in that without initializing the TODR it 
                          would default to the value VMS would set it to if
                          VMS knew the correct time.  This would be correct
                          almost all the time unless a VMS disk hadn't been
                          booted from for more than a year.  This mode 
                          produces strange time results for non VMS OSes on 
                          each system boot.
                        - OS Agnostic mode.  This mode behaves precisely like
                          the VAX780 TODR and works correctly for all OSes.  
                          This mode is enabled by attaching the TODR to a 
                          battery backup state file for the TOY clock 
                          (i.e. sim> attach TODR TOY_CLOCK).  When operating 
                          in OS Agnostic mode, the TODR will initially start
                          counting from 0 and be adjusted differently when an
                          OS specifically writes to the TODR.  On the first OS 
                          boot with an attached TODR VMS will prompt to set 
                          the time unless the SYSGEN parameter TIMEPROMPTWAIT 
                          is set to 0.
   05-Jan-11    MP      Added Asynch I/O support
   17-Aug-08    RMS     Resync TODR on any clock reset
   18-Jun-07    RMS     Added UNIT_IDLE flag to console input, clock
   17-Oct-06    RMS     Synced keyboard poll to real-time clock for idling
   22-Nov-05    RMS     Revised for new terminal processing routines
   09-Sep-04    RMS     Integrated powerup into RESET (with -p)
   28-May-04    RMS     Removed SET TTI CTRL-C
   29-Dec-03    RMS     Added console backpressure support
   25-Apr-03    RMS     Revised for extended file support
   02-Mar-02    RMS     Added SET TTI CTRL-C
   22-Dec-02    RMS     Added console halt capability
   01-Nov-02    RMS     Added 7B/8B capability to terminal
   12-Sep-02    RMS     Removed paper tape, added variable vector support
   30-May-02    RMS     Widened POS to 32b
   30-Apr-02    RMS     Automatically set TODR to VMS-correct value during boot
*/

#include "vax_defs.h"
#include <time.h>

#define TTICSR_IMP      (CSR_DONE + CSR_IE)             /* terminal input */
#define TTICSR_RW       (CSR_IE)
#define TTIBUF_ERR      0x8000                          /* error */
#define TTIBUF_OVR      0x4000                          /* overrun */
#define TTIBUF_FRM      0x2000                          /* framing error */
#define TTIBUF_RBR      0x0400                          /* receive break */
#define TTOCSR_IMP      (CSR_DONE + CSR_IE)             /* terminal output */
#define TTOCSR_RW       (CSR_IE)
#define CLKCSR_IMP      (CSR_IE)                        /* real-time clock */
#define CLKCSR_RW       (CSR_IE)
#define CLK_DELAY       5000                            /* 100 Hz */
#define TMXR_MULT       1                               /* 100 Hz */

extern int32 int_req[IPL_HLVL];
extern int32 hlt_pin;
extern int32 sim_switches, sim_is_running;

int32 tti_csr = 0;                                      /* control/status */
int32 tto_csr = 0;                                      /* control/status */
int32 clk_csr = 0;                                      /* control/status */
int32 clk_tps = 100;                                    /* ticks/second */
int32 todr_reg = 0;                                     /* TODR register */
int32 todr_blow = 1;                                    /* TODR battery low */
struct todr_battery_info {
    uint32 toy_gmtbase;                                 /* GMT base of set value */
    uint32 toy_gmtbasemsec;                             /* The milliseconds of the set value */
    };
typedef struct todr_battery_info TOY;
int32 tmxr_poll = CLK_DELAY * TMXR_MULT;                /* term mux poll */
int32 tmr_poll = CLK_DELAY;                             /* pgm timer poll */

t_stat tti_svc (UNIT *uptr);
t_stat tto_svc (UNIT *uptr);
t_stat clk_svc (UNIT *uptr);
t_stat tti_reset (DEVICE *dptr);
t_stat tto_reset (DEVICE *dptr);
t_stat clk_reset (DEVICE *dptr);
t_stat clk_attach (UNIT *uptr, char *cptr);
t_stat clk_detach (UNIT *uptr);
t_stat todr_resync (void);

extern int32 sysd_hlt_enb (void);
extern int32 fault_PC;

/* TTI data structures

   tti_dev      TTI device descriptor
   tti_unit     TTI unit descriptor
   tti_reg      TTI register list
*/

DIB tti_dib = { 0, 0, NULL, NULL, 1, IVCL (TTI), SCB_TTI, { NULL } };

UNIT tti_unit = { UDATA (&tti_svc, UNIT_IDLE|TT_MODE_8B, 0), 0 };

REG tti_reg[] = {
    { HRDATA (BUF, tti_unit.buf, 16) },
    { HRDATA (CSR, tti_csr, 16) },
    { FLDATA (INT, int_req[IPL_TTI], INT_V_TTI) },
    { FLDATA (DONE, tti_csr, CSR_V_DONE) },
    { FLDATA (IE, tti_csr, CSR_V_IE) },
    { DRDATA (POS, tti_unit.pos, T_ADDR_W), PV_LEFT },
    { DRDATA (TIME, tti_unit.wait, 24), PV_LEFT },
    { NULL }
    };

MTAB tti_mod[] = {
    { TT_MODE, TT_MODE_7B, "7b", "7B", NULL },
    { TT_MODE, TT_MODE_8B, "8b", "8B", NULL },
    { MTAB_XTD|MTAB_VDV, 0, "VECTOR", NULL,
      NULL, &show_vec, NULL },
    { 0 }
    };

DEVICE tti_dev = {
    "TTI", &tti_unit, tti_reg, tti_mod,
    1, 10, 31, 1, 16, 8,
    NULL, NULL, &tti_reset,
    NULL, NULL, NULL,
    &tti_dib, 0
    };

/* TTO data structures

   tto_dev      TTO device descriptor
   tto_unit     TTO unit descriptor
   tto_reg      TTO register list
*/

DIB tto_dib = { 0, 0, NULL, NULL, 1, IVCL (TTO), SCB_TTO, { NULL } };

UNIT tto_unit = { UDATA (&tto_svc, TT_MODE_8B, 0), SERIAL_OUT_WAIT };

REG tto_reg[] = {
    { HRDATA (BUF, tto_unit.buf, 8) },
    { HRDATA (CSR, tto_csr, 16) },
    { FLDATA (INT, int_req[IPL_TTO], INT_V_TTO) },
    { FLDATA (DONE, tto_csr, CSR_V_DONE) },
    { FLDATA (IE, tto_csr, CSR_V_IE) },
    { DRDATA (POS, tto_unit.pos, T_ADDR_W), PV_LEFT },
    { DRDATA (TIME, tto_unit.wait, 24), PV_LEFT },
    { NULL }
    };

MTAB tto_mod[] = {
    { TT_MODE, TT_MODE_7B, "7b", "7B", NULL },
    { TT_MODE, TT_MODE_8B, "8b", "8B", NULL },
    { TT_MODE, TT_MODE_7P, "7p", "7P", NULL },
    { MTAB_XTD|MTAB_VDV, 0, "VECTOR", NULL,     NULL, &show_vec },
    { 0 }
    };

DEVICE tto_dev = {
    "TTO", &tto_unit, tto_reg, tto_mod,
    1, 10, 31, 1, 16, 8,
    NULL, NULL, &tto_reset,
    NULL, NULL, NULL,
    &tto_dib, 0
    };

/* CLK data structures

   clk_dev      CLK device descriptor
   clk_unit     CLK unit descriptor
   clk_reg      CLK register list
*/

DIB clk_dib = { 0, 0, NULL, NULL, 1, IVCL (CLK), SCB_INTTIM, { NULL } };

UNIT clk_unit = { UDATA (&clk_svc, UNIT_IDLE+UNIT_FIX, sizeof(TOY)), CLK_DELAY };/* 100Hz */

REG clk_reg[] = {
    { HRDATA (CSR, clk_csr, 16) },
    { FLDATA (INT, int_req[IPL_CLK], INT_V_CLK) },
    { FLDATA (IE, clk_csr, CSR_V_IE) },
    { DRDATA (TODR, todr_reg, 32), PV_LEFT },
    { FLDATA (BLOW, todr_blow, 0) },
    { DRDATA (TIME, clk_unit.wait, 24), REG_NZ + PV_LEFT },
    { DRDATA (POLL, tmr_poll, 24), REG_NZ + PV_LEFT + REG_HRO },
    { DRDATA (TPS, clk_tps, 8), REG_NZ + PV_LEFT },
#if defined (SIM_ASYNCH_IO)
    { DRDATA (ASYNCH, sim_asynch_enabled, 1), PV_LEFT },
    { DRDATA (LATENCY, sim_asynch_latency, 32), PV_LEFT },
    { DRDATA (INST_LATENCY, sim_asynch_inst_latency, 32), PV_LEFT },
#endif
    { NULL }
    };

MTAB clk_mod[] = {
    { MTAB_XTD|MTAB_VDV, 0, "VECTOR", NULL,     NULL, &show_vec },
    { 0 }
    };

DEVICE clk_dev = {
    "CLK", &clk_unit, clk_reg, clk_mod,
    1, 0, 8, 4, 0, 32,
    NULL, NULL, &clk_reset,
    NULL, &clk_attach, &clk_detach,
    &clk_dib, 0
    };

/* Clock and terminal MxPR routines

   iccs_rd/wr   interval timer
   rxcs_rd/wr   input control/status
   rxdb_rd      input buffer
   txcs_rd/wr   output control/status
   txdb_wr      output buffer
*/

int32 iccs_rd (void)
{
return (clk_csr & CLKCSR_IMP);
}

int32 rxcs_rd (void)
{
return (tti_csr & TTICSR_IMP);
}

int32 rxdb_rd (void)
{
int32 t = tti_unit.buf;                                 /* char + error */

tti_csr = tti_csr & ~CSR_DONE;                          /* clr done */
tti_unit.buf = tti_unit.buf & 0377;                     /* clr errors */
CLR_INT (TTI);
return t;
}

int32 txcs_rd (void)
{
return (tto_csr & TTOCSR_IMP);
}

void iccs_wr (int32 data)
{
if ((data & CSR_IE) == 0)
    CLR_INT (CLK);
clk_csr = (clk_csr & ~CLKCSR_RW) | (data & CLKCSR_RW);
return;
}

void rxcs_wr (int32 data)
{
if ((data & CSR_IE) == 0)
    CLR_INT (TTI);
else if ((tti_csr & (CSR_DONE + CSR_IE)) == CSR_DONE)
    SET_INT (TTI);
tti_csr = (tti_csr & ~TTICSR_RW) | (data & TTICSR_RW);
return;
}

void txcs_wr (int32 data)
{
if ((data & CSR_IE) == 0)
    CLR_INT (TTO);
else if ((tto_csr & (CSR_DONE + CSR_IE)) == CSR_DONE)
    SET_INT (TTO);
tto_csr = (tto_csr & ~TTOCSR_RW) | (data & TTOCSR_RW);
return;
}

void txdb_wr (int32 data)
{
tto_unit.buf = data & 0377;
tto_csr = tto_csr & ~CSR_DONE;
CLR_INT (TTO);
sim_activate (&tto_unit, tto_unit.wait);
return;
}

/* Terminal input routines

   tti_svc      process event (character ready)
   tti_reset    process reset
*/

t_stat tti_svc (UNIT *uptr)
{
int32 c;

sim_activate (uptr, KBD_WAIT (uptr->wait, clk_cosched (tmr_poll)));
                                                        /* continue poll */
if ((c = sim_poll_kbd ()) < SCPE_KFLAG)                 /* no char or error? */
    return c;
if (c & SCPE_BREAK) {                                   /* break? */
    if (sysd_hlt_enb ())                                /* if enabled, halt */
        hlt_pin = 1;
    tti_unit.buf = TTIBUF_ERR | TTIBUF_FRM | TTIBUF_RBR;
    }
else tti_unit.buf = sim_tt_inpcvt (c, TT_GET_MODE (uptr->flags));
uptr->pos = uptr->pos + 1;
tti_csr = tti_csr | CSR_DONE;
if (tti_csr & CSR_IE)
    SET_INT (TTI);
return SCPE_OK;
}

t_stat tti_reset (DEVICE *dptr)
{
tmxr_set_console_input_unit (&tti_unit);
tti_unit.buf = 0;
tti_csr = 0;
CLR_INT (TTI);
sim_activate (&tti_unit, KBD_WAIT (tti_unit.wait, tmr_poll));
return SCPE_OK;
}

/* Terminal output routines

   tto_svc      process event (character typed)
   tto_reset    process reset
*/

t_stat tto_svc (UNIT *uptr)
{
int32 c;
t_stat r;

c = sim_tt_outcvt (tto_unit.buf, TT_GET_MODE (uptr->flags));
if (c >= 0) {
    if ((r = sim_putchar_s (c)) != SCPE_OK) {           /* output; error? */
        sim_activate (uptr, uptr->wait);                /* retry */
        return ((r == SCPE_STALL)? SCPE_OK: r);         /* !stall? report */
        }
    }
tto_csr = tto_csr | CSR_DONE;
if (tto_csr & CSR_IE)
    SET_INT (TTO);
uptr->pos = uptr->pos + 1;
return SCPE_OK;
}

t_stat tto_reset (DEVICE *dptr)
{
tto_unit.buf = 0;
tto_csr = CSR_DONE;
CLR_INT (TTO);
sim_cancel (&tto_unit);                                 /* deactivate unit */
return SCPE_OK;
}

/* Clock routines

   clk_svc      process event (clock tick)
   clk_reset    process reset
   todr_rd/wr   time of year clock
   todr_resync  powerup for TODR (get date from system)
*/

t_stat clk_svc (UNIT *uptr)
{
int32 t;

if (clk_csr & CSR_IE)
    SET_INT (CLK);
t = sim_rtcn_calb (clk_tps, TMR_CLK);                   /* calibrate clock */
sim_activate (&clk_unit, t);                            /* reactivate unit */
tmr_poll = t;                                           /* set tmr poll */
tmxr_poll = t * TMXR_MULT;                              /* set mux poll */
if (!todr_blow && todr_reg)                             /* if running? */
    todr_reg = todr_reg + 1;                            /* incr TODR */
return SCPE_OK;
}

/* Clock coscheduling routine */

int32 clk_cosched (int32 wait)
{
int32 t;

t = sim_is_active (&clk_unit);
return (t? t - 1: wait);
}

int32 todr_rd (void)
{
TOY *toy = (TOY *)clk_unit.filebuf;
struct timespec base, now, val;

if ((fault_PC&0xFFFE0000) == 0x20040000)                /* running from ROM? */
    return todr_reg;                                    /* return counted value for ROM diags */

if (0 == todr_reg)                                      /* clock running? */
    return todr_reg;

/* Maximum number of seconds which can be represented as 10ms ticks 
   in the 32bit TODR.  This is the 33bit value 0x100000000/100 to get seconds */
#define TOY_MAX_SECS (0x40000000/25)

clock_gettime(CLOCK_REALTIME, &now);                    /* get curr time */
base.tv_sec = toy->toy_gmtbase;
base.tv_nsec = toy->toy_gmtbasemsec * 1000000;
sim_timespec_diff (&val, &now, &base);

if (val.tv_sec >= TOY_MAX_SECS)                         /* todr overflowed? */
    return todr_reg = 0;                                /* stop counting */

return (int32)(val.tv_sec*100 + val.tv_nsec/10000000);  /* 100hz Clock Ticks */
}


void todr_wr (int32 data)
{
TOY *toy = (TOY *)clk_unit.filebuf;
struct timespec now, val, base;

/* Save the GMT time when set value was 0 to record the base for future 
   read operations in "battery backed-up" state */

if (-1 == clock_gettime(CLOCK_REALTIME, &now))          /* get curr time */
    return;                                             /* error? */
val.tv_sec = ((uint32)data) / 100;
val.tv_nsec = (((uint32)data) % 100) * 10000000;
sim_timespec_diff (&base, &now, &val);                  /* base = now - data */
toy->toy_gmtbase = (uint32)base.tv_sec;
toy->toy_gmtbasemsec = base.tv_nsec/1000000;
todr_reg = data;
if (data)
    todr_blow = 0;
}

/* TODR resync routine */

t_stat todr_resync (void)
{
TOY *toy = (TOY *)clk_unit.filebuf;

if (clk_unit.flags & UNIT_ATT) {                        /* Attached means behave like real VAX780 */
    if (!toy->toy_gmtbase)                              /* Never set? */
        todr_wr (0);                                    /* Start ticking from 0 */
    }
else {                                                  /* Not-Attached means */
    uint32 base;                                        /* behave like simh VMS default */
    time_t curr;
    struct tm *ctm;

    curr = time (NULL);                                 /* get curr time */
    if (curr == (time_t) -1)                            /* error? */
        return SCPE_NOFNC;
    ctm = localtime (&curr);                            /* decompose */
    if (ctm == NULL)                                    /* error? */
        return SCPE_NOFNC;
    base = (((((ctm->tm_yday * 24) +                    /* sec since 1-Jan */
            ctm->tm_hour) * 60) +
            ctm->tm_min) * 60) +
            ctm->tm_sec;
    todr_wr ((base * 100) + 0x10000000);                /* use VMS form */
    }
return SCPE_OK;
}

/* Reset routine */

t_stat clk_reset (DEVICE *dptr)
{
int32 t;

clk_csr = 0;
CLR_INT (CLK);
if (!sim_is_running) {                                  /* RESET (not IORESET)? */
    t = sim_rtcn_init (clk_unit.wait, TMR_CLK);         /* init timer */
    sim_activate (&clk_unit, t);                        /* activate unit */
    tmr_poll = t;                                       /* set tmr poll */
    tmxr_poll = t * TMXR_MULT;                          /* set mux poll */
    }
if (clk_unit.filebuf == NULL) {                         /* make sure the TODR is initialized */
    clk_unit.filebuf = calloc(sizeof(TOY), 1);
    if (clk_unit.filebuf == NULL)
        return SCPE_MEM;
    todr_resync ();
    }
return SCPE_OK;
}

/* CLK attach */

t_stat clk_attach (UNIT *uptr, char *cptr)
{
t_stat r;

uptr->flags = uptr->flags | (UNIT_ATTABLE | UNIT_BUFABLE);
memset (uptr->filebuf, 0, (size_t)uptr->capac);
r = attach_unit (uptr, cptr);
if (r != SCPE_OK)
    uptr->flags = uptr->flags & ~(UNIT_ATTABLE | UNIT_BUFABLE);
else
    uptr->hwmark = (uint32) uptr->capac;
return r;
}

/* CLK detach */

t_stat clk_detach (UNIT *uptr)
{
t_stat r;

r = detach_unit (uptr);
if ((uptr->flags & UNIT_ATT) == 0)
    uptr->flags = uptr->flags & ~(UNIT_ATTABLE | UNIT_BUFABLE);
return r;
}


/*
* UAE - The Un*x Amiga Emulator
*
* A590/A2091/A3000/CDTV SCSI expansion (DMAC/SuperDMAC + WD33C93) emulation
* Includes A590 + XT drive emulation.
* GVP Series II
*
* Copyright 2007-2014 Toni Wilen
*
*/

#define GVP_S1_DEBUG_IO 0
#define GVP_S2_DEBUG_IO 0
#define A2091_DEBUG 0
#define A2091_DEBUG_IO 0
#define XT_DEBUG 0
#define A3000_DEBUG 0
#define A3000_DEBUG_IO 0
#define WD33C93_DEBUG 0
#define WD33C93_DEBUG_PIO 0

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "memory.h"
#include "rommgr.h"
#include "custom.h"
#include "newcpu.h"
#include "debug.h"
#include "scsi.h"
#include "threaddep/thread.h"
#include "a2091.h"
#include "blkdev.h"
#include "gui.h"
#include "zfile.h"
#include "filesys.h"
#include "autoconf.h"
#include "cdtv.h"
#include "savestate.h"
#include "cpuboard.h"

#define CDMAC_ROM_VECTOR 0x2000
#define CDMAC_ROM_OFFSET 0x2000
#define GVP_ROM_OFFSET 0x8000
#define GVP_SERIES_I_RAM_OFFSET 0x4000
#define GVP_SERIES_I_RAM_MASK 16383

/* SuperDMAC CNTR bits. */
#define SCNTR_TCEN	(1<<5)
#define SCNTR_PREST	(1<<4)
#define SCNTR_PDMD	(1<<3)
#define SCNTR_INTEN	(1<<2)
#define SCNTR_DDIR	(1<<1)
#define SCNTR_IO_DX	(1<<0)
/* DMAC CNTR bits. */
#define CNTR_TCEN	(1<<7)
#define CNTR_PREST	(1<<6)
#define CNTR_PDMD	(1<<5)
#define CNTR_INTEN	(1<<4)
#define CNTR_DDIR	(1<<3)
/* ISTR bits. */
#define ISTR_INT_F	(1<<7)	/* Interrupt Follow */
#define ISTR_INTS	(1<<6)	/* SCSI or XT Peripheral Interrupt */
#define ISTR_E_INT	(1<<5)	/* End-Of-Process Interrupt */
#define ISTR_INT_P	(1<<4)	/* Interrupt Pending */
#define ISTR_UE_INT	(1<<3)	/* Under-Run FIFO Error Interrupt */
#define ISTR_OE_INT	(1<<2)	/* Over-Run FIFO Error Interrupt */
#define ISTR_FF_FLG	(1<<1)	/* FIFO-Full Flag */
#define ISTR_FE_FLG	(1<<0)	/* FIFO-Empty Flag */

/* GVP models */
#define GVP_GFORCE_040		0x20
#define GVP_GFORCE_040_SCSI	0x30
#define GVP_A1291_SCSI		0x40
#define GVP_GFORCE_030		0xa0
#define GVP_GFORCE_030_SCSI	0xb0
#define GVP_COMBO_R4		0x60
#define GVP_COMBO_R4_SCSI	0x70
#define GVP_COMBO_R3		0xe0
#define GVP_COMBO_R3_SCSI	0xf0
#define GVP_SERIESII		0xf8
#define GVP_A530			0xc0
#define GVP_A530_SCSI		0xd0

/* wd register names */
#define WD_OWN_ID		0x00
#define WD_CONTROL		0x01
#define WD_TIMEOUT_PERIOD	0x02
#define WD_CDB_1		0x03
#define WD_T_SECTORS	0x03
#define WD_CDB_2		0x04
#define WD_T_HEADS		0x04
#define WD_CDB_3		0x05
#define WD_T_CYLS_0		0x05
#define WD_CDB_4		0x06
#define WD_T_CYLS_1		0x06
#define WD_CDB_5		0x07
#define WD_L_ADDR_0		0x07
#define WD_CDB_6		0x08
#define WD_L_ADDR_1		0x08
#define WD_CDB_7		0x09
#define WD_L_ADDR_2		0x09
#define WD_CDB_8		0x0a
#define WD_L_ADDR_3		0x0a
#define WD_CDB_9		0x0b
#define WD_SECTOR		0x0b
#define WD_CDB_10		0x0c
#define WD_HEAD			0x0c
#define WD_CDB_11		0x0d
#define WD_CYL_0		0x0d
#define WD_CDB_12		0x0e
#define WD_CYL_1		0x0e
#define WD_TARGET_LUN		0x0f
#define WD_COMMAND_PHASE	0x10
#define WD_SYNCHRONOUS_TRANSFER 0x11
#define WD_TRANSFER_COUNT_MSB	0x12
#define WD_TRANSFER_COUNT	0x13
#define WD_TRANSFER_COUNT_LSB	0x14
#define WD_DESTINATION_ID	0x15
#define WD_SOURCE_ID		0x16
#define WD_SCSI_STATUS		0x17
#define WD_COMMAND		0x18
#define WD_DATA			0x19
#define WD_QUEUE_TAG		0x1a
#define WD_AUXILIARY_STATUS	0x1f
/* WD commands */
#define WD_CMD_RESET		0x00
#define WD_CMD_ABORT		0x01
#define WD_CMD_ASSERT_ATN	0x02
#define WD_CMD_NEGATE_ACK	0x03
#define WD_CMD_DISCONNECT	0x04
#define WD_CMD_RESELECT		0x05
#define WD_CMD_SEL_ATN		0x06
#define WD_CMD_SEL		0x07
#define WD_CMD_SEL_ATN_XFER	0x08
#define WD_CMD_SEL_XFER		0x09
#define WD_CMD_RESEL_RECEIVE	0x0a
#define WD_CMD_RESEL_SEND	0x0b
#define WD_CMD_WAIT_SEL_RECEIVE	0x0c
#define WD_CMD_TRANS_ADDR	0x18
#define WD_CMD_TRANS_INFO	0x20
#define WD_CMD_TRANSFER_PAD	0x21
#define WD_CMD_SBT_MODE		0x80

/* paused or aborted interrupts */
#define CSR_MSGIN			0x20
#define CSR_SDP				0x21
#define CSR_SEL_ABORT		0x22
#define CSR_RESEL_ABORT		0x25
#define CSR_RESEL_ABORT_AM	0x27
#define CSR_ABORT			0x28
/* successful completion interrupts */
#define CSR_RESELECT		0x10
#define CSR_SELECT			0x11
#define CSR_TRANS_ADDR		0x15
#define CSR_SEL_XFER_DONE	0x16
#define CSR_XFER_DONE		0x18
/* terminated interrupts */
#define CSR_INVALID			0x40
#define CSR_UNEXP_DISC		0x41
#define CSR_TIMEOUT			0x42
#define CSR_PARITY			0x43
#define CSR_PARITY_ATN		0x44
#define CSR_BAD_STATUS		0x45
#define CSR_UNEXP			0x48
/* service required interrupts */
#define CSR_RESEL			0x80
#define CSR_RESEL_AM		0x81
#define CSR_DISC			0x85
#define CSR_SRV_REQ			0x88
/* SCSI Bus Phases */
#define PHS_DATA_OUT	0x00
#define PHS_DATA_IN		0x01
#define PHS_COMMAND		0x02
#define PHS_STATUS		0x03
#define PHS_MESS_OUT	0x06
#define PHS_MESS_IN		0x07

/* Auxialiry status */
#define ASR_INT			0x80	/* Interrupt pending */
#define ASR_LCI			0x40	/* Last command ignored */
#define ASR_BSY			0x20	/* Busy, only cmd/data/asr readable */
#define ASR_CIP			0x10	/* Busy, cmd unavail also */
#define ASR_xxx			0x0c
#define ASR_PE			0x02	/* Parity error (even) */
#define ASR_DBR			0x01	/* Data Buffer Ready */
/* Status */
#define CSR_CAUSE		0xf0
#define CSR_RESET		0x00	/* chip was reset */
#define CSR_CMD_DONE	0x10	/* cmd completed */
#define CSR_CMD_STOPPED	0x20	/* interrupted or abrted*/
#define CSR_CMD_ERR		0x40	/* end with error */
#define CSR_BUS_SERVICE	0x80	/* REQ pending on the bus */
/* Control */
#define CTL_DMA			0x80	/* Single byte dma */
#define CTL_DBA_DMA		0x40	/* direct buffer access (bus master) */
#define CTL_BURST_DMA	0x20	/* continuous mode (8237) */
#define CTL_NO_DMA		0x00	/* Programmed I/O */
#define CTL_HHP			0x10	/* Halt on host parity error */
#define CTL_EDI			0x08	/* Ending disconnect interrupt */
#define CTL_IDI			0x04	/* Intermediate disconnect interrupt*/
#define CTL_HA			0x02	/* Halt on ATN */
#define CTL_HSP			0x01	/* Halt on SCSI parity error */

/* SCSI Messages */
#define MSG_COMMAND_COMPLETE 0x00
#define MSG_SAVE_DATA_POINTER 0x02
#define MSG_RESTORE_DATA_POINTERS 0x03
#define MSG_NOP 0x08
#define MSG_IDENTIFY 0x80

/* XT hard disk controller registers */
#define XD_DATA         0x00    /* data RW register */
#define XD_RESET        0x01    /* reset WO register */
#define XD_STATUS       0x01    /* status RO register */
#define XD_SELECT       0x02    /* select WO register */
#define XD_JUMPER       0x02    /* jumper RO register */
#define XD_CONTROL      0x03    /* DMAE/INTE WO register */
#define XD_RESERVED     0x03    /* reserved */

/* XT hard disk controller commands (incomplete list) */
#define XT_CMD_TESTREADY   0x00    /* test drive ready */
#define XT_CMD_RECALIBRATE 0x01    /* recalibrate drive */
#define XT_CMD_SENSE       0x03    /* request sense */
#define XT_CMD_FORMATDRV   0x04    /* format drive */
#define XT_CMD_VERIFY      0x05    /* read verify */
#define XT_CMD_FORMATTRK   0x06    /* format track */
#define XT_CMD_FORMATBAD   0x07    /* format bad track */
#define XT_CMD_READ        0x08    /* read */
#define XT_CMD_WRITE       0x0A    /* write */
#define XT_CMD_SEEK        0x0B    /* seek */
/* Controller specific commands */
#define XT_CMD_DTCSETPARAM 0x0C    /* set drive parameters (DTC 5150X & CX only?) */

/* Bits for command status byte */
#define XT_CSB_ERROR       0x02    /* error */
#define XT_CSB_LUN         0x20    /* logical Unit Number */

/* XT hard disk controller status bits */
#define XT_STAT_READY      0x01    /* controller is ready */
#define XT_STAT_INPUT      0x02    /* data flowing from controller to host */
#define XT_STAT_COMMAND    0x04    /* controller in command phase */
#define XT_STAT_SELECT     0x08    /* controller is selected */
#define XT_STAT_REQUEST    0x10    /* controller requesting data */
#define XT_STAT_INTERRUPT  0x20    /* controller requesting interrupt */

/* XT hard disk controller control bits */
#define XT_INT          0x02    /* Interrupt enable */
#define XT_DMA_MODE     0x01    /* DMA enable */

#define XT_UNIT 7
#define XT_SECTORS 17 /* hardwired */

static struct wd_state wd_a2091;
static struct wd_state wd_a2091_2;
static struct wd_state wd_a3000;
static struct wd_state wd_gvp;
static struct wd_state wd_gvp_2;
struct wd_state wd_cdtv;

static struct wd_state *wda2091[] = {
		&wd_a2091,
		&wd_a2091_2,
};

static struct wd_state *wdscsi[] = {
		&wd_a2091,
		&wd_a2091_2,
		&wd_a3000,
		&wd_cdtv,
		NULL
};

static struct wd_state *gvpscsi[] = {
	&wd_gvp,
	&wd_gvp_2,
};

static void reset_dmac(struct wd_state *wd)
{
	switch (wd->dmac_type)
	{
		case GVP_DMAC_S1:
		case GVP_DMAC_S2:
		wd->gdmac.cntr = 0;
		wd->gdmac.dma_on = 0;
		break;
		case COMMODORE_SDMAC:
		case COMMODORE_DMAC:
		wd->cdmac.dmac_dma = 0;
		wd->cdmac.dmac_istr = 0;
		wd->cdmac.dmac_cntr = 0;
		break;
	}
}

static bool isirq(struct wd_state *wd)
{
	if (!wd->enabled)
		return false;
	switch (wd->dmac_type)
	{
		case GVP_DMAC_S1:
		return wd->gdmac.cntr && (wd->wc.auxstatus & ASR_INT) != 0;
		case GVP_DMAC_S2:
		if (wd->wc.auxstatus & ASR_INT)
			wd->gdmac.cntr |= 2;
		if ((wd->gdmac.cntr & (2 | 8)) == 10)
			return true;
		break;
		case COMMODORE_SDMAC:
		if (wd->wc.auxstatus & ASR_INT)
			wd->cdmac.dmac_istr |= ISTR_INTS;
		if ((wd->cdmac.dmac_cntr & SCNTR_INTEN) && (wd->cdmac.dmac_istr & (ISTR_INTS | ISTR_E_INT)))
			return true;
		break;
		case COMMODORE_DMAC:
		if (wd->cdmac.xt_irq)
			wd->cdmac.dmac_istr |= ISTR_INTS;
		if (wd->wc.auxstatus & ASR_INT)
			wd->cdmac.dmac_istr |= ISTR_INTS;
		if ((wd->cdmac.dmac_cntr & CNTR_INTEN) && (wd->cdmac.dmac_istr & (ISTR_INTS | ISTR_E_INT)))
			return true;
		break;
	}
	return false;
}

static void set_dma_done(struct wd_state *wds)
{
	switch (wds->dmac_type)
	{
		case GVP_DMAC_S1:
		case GVP_DMAC_S2:
		wds->gdmac.dma_on = -1;
		break;
		case COMMODORE_SDMAC:
		case COMMODORE_DMAC:
		wds->cdmac.dmac_dma = -1;
		break;
	}
}

static bool is_dma_enabled(struct wd_state *wds)
{
	switch (wds->dmac_type)
	{
		case GVP_DMAC_S1:
		return true;
		case GVP_DMAC_S2:
		return wds->gdmac.dma_on > 0;
		case COMMODORE_SDMAC:
		case COMMODORE_DMAC:
		return wds->cdmac.dmac_dma > 0;
	}
	return false;	
}

void rethink_a2091 (void)
{
	if (isirq (&wd_a2091) ||isirq (&wd_a2091_2) || isirq (&wd_a3000) || isirq(&wd_gvp) || isirq(&wd_gvp_2)) {
		uae_int_requested |= 2;
#if A2091_DEBUG > 2 || A3000_DEBUG > 2
		write_log (_T("Interrupt_RETHINK\n"));
#endif
	} else {
		uae_int_requested &= ~2;
	}
}

static void dmac_scsi_int(struct wd_state *wd)
{
	if (!wd->enabled)
		return;
	if (!(wd->wc.auxstatus & ASR_INT))
		return;
	if (isirq(wd))
		uae_int_requested |= 2;
}

static void dmac_a2091_xt_int(struct wd_state *wd)
{
	if (!wd->enabled)
		return;
	wd->cdmac.xt_irq = true;
	if (isirq(wd))
		uae_int_requested |= 2;
}

void scsi_dmac_a2091_start_dma (struct wd_state *wd)
{
#if A3000_DEBUG > 0 || A2091_DEBUG > 0
	write_log (_T("DMAC DMA started, ADDR=%08X, LEN=%08X words\n"), wd->cdmac.dmac_acr, wd->cdmac.dmac_wtc);
#endif
	wd->cdmac.dmac_dma = 1;
}
void scsi_dmac_a2091_stop_dma (struct wd_state *wd)
{
	wd->cdmac.dmac_dma = 0;
	wd->cdmac.dmac_istr &= ~ISTR_E_INT;
}

static void dmac_reset (struct wd_state *wd)
{
#if WD33C93_DEBUG > 0
	if (wd->dmac_type == COMMODORE_SDMAC)
		write_log (_T("A3000 %s SCSI reset\n"), WD33C93);
	else if (wd->dmac_type == COMMODORE_DMAC)
		write_log (_T("A2091 %s SCSI reset\n"), WD33C93);
#endif
}

static void incsasr (struct wd_chip_state *wd, int w)
{
	if (wd->sasr == WD_AUXILIARY_STATUS || wd->sasr == WD_DATA || wd->sasr == WD_COMMAND)
		return;
	if (w && wd->sasr == WD_SCSI_STATUS)
		return;
	wd->sasr++;
	wd->sasr &= 0x1f;
}

static void dmac_a2091_cint (struct wd_state *wd)
{
	wd->cdmac.dmac_istr = 0;
	rethink_a2091 ();
}

static void doscsistatus(struct wd_state *wd, uae_u8 status)
{
	wd->wc.wdregs[WD_SCSI_STATUS] = status;
	wd->wc.auxstatus |= ASR_INT;
#if WD33C93_DEBUG > 1
	write_log (_T("%s STATUS=%02X\n"), WD33C93, status);
#endif
	if (!wd->enabled)
		return;
	if (wd->cdtv) {
		cdtv_scsi_int ();
		return;
	}
	dmac_scsi_int(wd);
#if WD33C93_DEBUG > 2
	write_log (_T("Interrupt\n"));
#endif
}

static void set_status (struct wd_chip_state *wd, uae_u8 status, int delay)
{
	wd->queue_index++;
	if (wd->queue_index >= WD_STATUS_QUEUE)
		wd->queue_index = 0;
	wd->scsidelay_status[wd->queue_index] = status;
	wd->scsidelay_irq[wd->queue_index] = delay == 0 ? 1 : (delay <= 2 ? 2 : delay);
}

static void set_status (struct wd_chip_state *wd, uae_u8 status)
{
	set_status (wd, status, 0);
}

static uae_u32 gettc (struct wd_chip_state *wd)
{
	return wd->wdregs[WD_TRANSFER_COUNT_LSB] | (wd->wdregs[WD_TRANSFER_COUNT] << 8) | (wd->wdregs[WD_TRANSFER_COUNT_MSB] << 16);
}
static void settc (struct wd_chip_state *wd, uae_u32 tc)
{
	wd->wdregs[WD_TRANSFER_COUNT_LSB] = tc & 0xff;
	wd->wdregs[WD_TRANSFER_COUNT] = (tc >> 8) & 0xff;
	wd->wdregs[WD_TRANSFER_COUNT_MSB] = (tc >> 16) & 0xff;
}
static bool decreasetc(struct wd_chip_state *wd)
{
	uae_u32 tc = gettc (wd);
	if (!tc)
		return true;
	tc--;
	settc (wd, tc);
	return tc == 0;
}

static bool canwddma(struct wd_state *wds)
{
	struct wd_chip_state *wd = &wds->wc;
	uae_u8 mode = wd->wdregs[WD_CONTROL] >> 5;
	switch(wds->dmac_type)
	{
		case COMMODORE_DMAC:
		case COMMODORE_SDMAC:
		case GVP_DMAC_S2:
		if (mode != 0 && mode != 4 && mode != 1) {
			write_log (_T("%s weird DMA mode %d!!\n"), WD33C93, mode);
		}
		return mode == 4 || mode == 1;
		case GVP_DMAC_S1:
		if (mode != 0 && mode != 2) {
			write_log (_T("%s weird DMA mode %d!!\n"), WD33C93, mode);
		}
		return mode == 2;
		default:
		return false;
	}
}

#if WD33C93_DEBUG > 0
static TCHAR *scsitostring (struct wd_chip_state *wd, struct scsi_data *scsi)
{
	static TCHAR buf[200];
	TCHAR *p;
	int i;

	p = buf;
	p[0] = 0;
	for (i = 0; i < scsi->offset && i < sizeof wd->wd_data; i++) {
		if (i > 0) {
			_tcscat (p, _T("."));
			p++;
		}
		_stprintf (p, _T("%02X"), wd->wd_data[i]);
		p += _tcslen (p);
	}
	return buf;
}
#endif

static void setphase(struct wd_chip_state *wd, uae_u8 phase)
{
	wd->wdregs[WD_COMMAND_PHASE] = phase;
}

static void dmacheck_a2091 (struct wd_state *wd)
{
	wd->cdmac.dmac_acr++;
	if (wd->cdmac.old_dmac && (wd->cdmac.dmac_cntr & CNTR_TCEN)) {
		if (wd->cdmac.dmac_wtc == 0)
			wd->cdmac.dmac_istr |= ISTR_E_INT;
		else
			wd->cdmac.dmac_wtc--;
	}
}

static bool do_dma_commodore(struct wd_state *wd, struct scsi_data *scsi)
{
	if (wd->cdtv)
		cdtv_getdmadata(&wd->cdmac.dmac_acr);
	if (scsi->direction < 0) {
#if WD33C93_DEBUG > 0
		uaecptr odmac_acr = wd->cdmac.dmac_acr;
#endif
		for (;;) {
			uae_u8 v;
			int status = scsi_receive_data (scsi, &v);
			put_byte(wd->cdmac.dmac_acr, v);
			if (wd->wc.wd_dataoffset < sizeof wd->wc.wd_data)
				wd->wc.wd_data[wd->wc.wd_dataoffset++] = v;
			dmacheck_a2091 (wd);
			if (decreasetc (&wd->wc))
				break;
			if (status)
				break;
		}
#if WD33C93_DEBUG > 0
		write_log (_T("%s Done DMA from WD, %d/%d %08X\n"), WD33C93, scsi->offset, scsi->data_len, odmac_acr);
#endif
		return true;
	} else if (scsi->direction > 0) {
#if WD33C93_DEBUG > 0
		uaecptr odmac_acr = wd->cdmac.dmac_acr;
#endif
		for (;;) {
			int status;
			uae_u8 v = get_byte(wd->cdmac.dmac_acr);
			if (wd->wc.wd_dataoffset < sizeof wd->wc.wd_data)
				wd->wc.wd_data[wd->wc.wd_dataoffset++] = v;
			status = scsi_send_data (scsi, v);
			dmacheck_a2091 (wd);
			if (decreasetc (&wd->wc))
				break;
			if (status)
				break;
		}
#if WD33C93_DEBUG > 0
		write_log (_T("%s Done DMA to WD, %d/%d %08x\n"), WD33C93, scsi->offset, scsi->data_len, odmac_acr);
#endif
		return true;
	}
	return false;
}

static bool do_dma_gvp_s1(struct wd_state *wd, struct scsi_data *scsi)
{
	if (scsi->direction < 0) {
		for (;;) {
			uae_u8 v;
			int status = scsi_receive_data (scsi, &v);
			wd->gdmac.buffer[wd->wc.wd_dataoffset++] = v;
			wd->wc.wd_dataoffset &= GVP_SERIES_I_RAM_MASK;
			if (decreasetc (&wd->wc))
				break;
			if (status)
				break;
		}
#if WD33C93_DEBUG > 0
		write_log (_T("%s Done DMA from WD, %d/%d\n"), WD33C93, scsi->offset, scsi->data_len);
#endif
		return true;
	} else if (scsi->direction > 0) {
		for (;;) {
			int status;
			uae_u8 v = wd->gdmac.buffer[wd->wc.wd_dataoffset++];
			wd->wc.wd_dataoffset &= GVP_SERIES_I_RAM_MASK;
			status = scsi_send_data (scsi, v);
			wd->gdmac.addr++;
			if (decreasetc (&wd->wc))
				break;
			if (status)
				break;
		}
#if WD33C93_DEBUG > 0
		write_log (_T("%s Done DMA to WD, %d/%d\n"), WD33C93, scsi->offset, scsi->data_len);
#endif
		return true;
	}
	return false;
}


static bool do_dma_gvp_s2(struct wd_state *wd, struct scsi_data *scsi)
{
#if WD33C93_DEBUG > 0
	uae_u32 dmaptr = wd->gdmac.addr;
#endif
	if (!is_dma_enabled(wd))
		return false;

	if (scsi->direction < 0) {
		if (wd->gdmac.cntr & 0x10) {
			write_log(_T("GVP DMA: mismatched direction when reading!\n"));
			return false;
		}
		for (;;) {
			uae_u8 v;
			int status = scsi_receive_data (scsi, &v);
			put_byte(wd->gdmac.addr, v);
			if (wd->wc.wd_dataoffset < sizeof wd->wc.wd_data)
				wd->wc.wd_data[wd->wc.wd_dataoffset++] = v;
			wd->gdmac.addr++;
			if (decreasetc (&wd->wc))
				break;
			if (status)
				break;
		}
#if WD33C93_DEBUG > 0
		write_log (_T("%s Done DMA from WD, %d/%d %08x\n"), WD33C93, scsi->offset, scsi->data_len, dmaptr);
#endif
		return true;
	} else if (scsi->direction > 0) {
		if (!(wd->gdmac.cntr & 0x10)) {
			write_log(_T("GVP DMA: mismatched direction when writing!\n"));
			return false;
		}
		for (;;) {
			int status;
			uae_u8 v = get_byte(wd->gdmac.addr);
			if (wd->wc.wd_dataoffset < sizeof wd->wc.wd_data)
				wd->wc.wd_data[wd->wc.wd_dataoffset++] = v;
			status = scsi_send_data (scsi, v);
			wd->gdmac.addr++;
			if (decreasetc (&wd->wc))
				break;
			if (status)
				break;
		}
#if WD33C93_DEBUG > 0
		write_log (_T("%s Done DMA to WD, %d/%d %08x\n"), WD33C93, scsi->offset, scsi->data_len, dmaptr);
#endif
		return true;
	}
	return false;
}

static bool do_dma(struct wd_state *wd)
{
	struct scsi_data *scsi = wd->wc.scsi;
	wd->wc.wd_data_avail = 0;
	if (scsi->direction == 0)
		write_log (_T("%s DMA but no data!?\n"), WD33C93);
	switch (wd->dmac_type)
	{
		case COMMODORE_DMAC:
		case COMMODORE_SDMAC:
		return do_dma_commodore(wd, scsi);
		case GVP_DMAC_S2:
		return do_dma_gvp_s2(wd, scsi);
		case GVP_DMAC_S1:
		return do_dma_gvp_s1(wd, scsi);
	}
	return false;
}

static bool wd_do_transfer_out (struct wd_chip_state *wd, struct scsi_data *scsi)
{
#if WD33C93_DEBUG > 0
	write_log (_T("%s SCSI O [%02X] %d/%d TC=%d %s\n"), WD33C93, wd->wdregs[WD_COMMAND_PHASE], scsi->offset, scsi->data_len, gettc (wd), scsitostring (wd, scsi));
#endif
	if (wd->wdregs[WD_COMMAND_PHASE] < 0x20) {
		int msg = wd->wd_data[0];
		/* message was sent */
		setphase (wd, 0x20);
		wd->wd_phase = CSR_XFER_DONE | PHS_COMMAND;
		scsi->status = 0;
		scsi_start_transfer (scsi);
#if WD33C93_DEBUG > 0
		write_log (_T("%s SCSI got MESSAGE %02X\n"), WD33C93, msg);
#endif
		scsi->message[0] = msg;
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x30) {
#if WD33C93_DEBUG > 0
		write_log (_T("%s SCSI got COMMAND %02X\n"), WD33C93, wd->wd_data[0]);
#endif
		if (scsi->offset < scsi->data_len) {
			// data missing, ask for more
			wd->wd_phase = CSR_XFER_DONE | PHS_COMMAND;
			setphase (wd, 0x30 + scsi->offset);
			set_status (wd, wd->wd_phase, 1);
			return false;
		}
		settc (wd, 0);
		scsi_start_transfer (scsi);
		scsi_emulate_analyze (scsi);
		if (scsi->direction > 0) {
			/* if write command, need to wait for data */
			if (scsi->data_len <= 0 || scsi->direction == 0) {
				// Status phase if command didn't return anything and don't want anything
				wd->wd_phase = CSR_XFER_DONE | PHS_STATUS;
				setphase (wd, 0x46);
			} else {
				wd->wd_phase = CSR_XFER_DONE | PHS_DATA_OUT;
				setphase (wd, 0x45);
			}
		} else {
			scsi_emulate_cmd (scsi);
			if (wd->scsi->data_len <= 0 || scsi->direction == 0) {
				// Status phase if command didn't return anything and don't want anything
				wd->wd_phase = CSR_XFER_DONE | PHS_STATUS;
				setphase (wd, 0x46);
			} else {
				wd->wd_phase = CSR_XFER_DONE | PHS_DATA_IN;
				setphase (wd, 0x45); // just skip all reselection and message stuff for now..
			}
		}
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x46 || wd->wdregs[WD_COMMAND_PHASE] == 0x45) {
		if (wd->scsi->offset < scsi->data_len) {
			// data missing, ask for more
			wd->wd_phase = CSR_XFER_DONE | (scsi->direction < 0 ? PHS_DATA_IN : PHS_DATA_OUT);
			set_status (wd, wd->wd_phase, 10);
			return false;
		}
		settc (wd, 0);
		if (scsi->direction > 0) {
			/* data was sent */
			scsi_emulate_cmd (scsi);
			scsi->data_len = 0;
			wd->wd_phase = CSR_XFER_DONE | PHS_STATUS;
		}
		scsi_start_transfer (scsi);
		setphase (wd, 0x47);
	}
	wd->wd_dataoffset = 0;
	set_status (wd, wd->wd_phase, scsi->direction <= 0 ? 0 : 1);
	wd->wd_busy = 0;
	return true;
}

static bool wd_do_transfer_in (struct wd_chip_state *wd, struct scsi_data *scsi, bool message_in_transfer_info)
{
#if WD33C93_DEBUG > 0
	write_log (_T("%s SCSI I [%02X] %d/%d TC=%d %s\n"), WD33C93, wd->wdregs[WD_COMMAND_PHASE], scsi->offset, scsi->data_len, gettc (wd), scsitostring (wd, scsi));
#endif
	wd->wd_dataoffset = 0;
	if (wd->wdregs[WD_COMMAND_PHASE] >= 0x36 && wd->wdregs[WD_COMMAND_PHASE] < 0x46) {
		if (scsi->offset < scsi->data_len) {
			// data missing, ask for more
			wd->wd_phase = CSR_XFER_DONE | (scsi->direction < 0 ? PHS_DATA_IN : PHS_DATA_OUT);
			set_status(wd, wd->wd_phase, 1);
			return false;
		}
		if (gettc (wd) != 0) {
			wd->wd_phase = CSR_UNEXP | PHS_STATUS;
			setphase(wd, 0x46);
		} else {
			wd->wd_phase = CSR_XFER_DONE | PHS_STATUS;
			setphase(wd, 0x46);
		}
		scsi_start_transfer(scsi);
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x46 || wd->wdregs[WD_COMMAND_PHASE] == 0x47) {
		setphase(wd, 0x50);
		wd->wd_phase = CSR_XFER_DONE | PHS_MESS_IN;
		scsi_start_transfer(scsi);
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x50) {
		// was TRANSFER INFO with message phase, wait for Negate ACK
		if (!message_in_transfer_info) {
			wd->wd_phase = CSR_DISC;
			wd->wd_selected = false;
			scsi_start_transfer(scsi);
			setphase(wd, 0x60);
		} else {
			wd->wd_phase = CSR_MSGIN;
		}
	}
	set_status(wd, wd->wd_phase, 1);
	scsi->direction = 0;
	return true;
}

static void wd_cmd_sel_xfer (struct wd_chip_state *wd, struct wd_state *wds, bool atn)
{
	int i, tmp_tc;
	int delay = 0;
	struct scsi_data *scsi;

	wd->wd_data_avail = 0;
	tmp_tc = gettc (wd);
	scsi = wd->scsi = wds->scsis[wd->wdregs[WD_DESTINATION_ID] & 7];
	if (!scsi) {
		set_status (wd, CSR_TIMEOUT, 0);
		wd->wdregs[WD_COMMAND_PHASE] = 0x00;
#if WD33C93_DEBUG > 0
		write_log (_T("* %s select and transfer%s, ID=%d: No device\n"),
			WD33C93, atn ? _T(" with atn") : _T(""), wd->wdregs[WD_DESTINATION_ID] & 0x7);
#endif
		return;
	}
	if (!wd->wd_selected) {
		scsi->message[0] = 0x80;
		wd->wd_selected = true;
		wd->wdregs[WD_COMMAND_PHASE] = 0x10;
	}
#if WD33C93_DEBUG > 0
	write_log (_T("* %s select and transfer%s, ID=%d PHASE=%02X TC=%d wddma=%d dmac=%d\n"),
		WD33C93, atn ? _T(" with atn") : _T(""), wd->wdregs[WD_DESTINATION_ID] & 0x7, wd->wdregs[WD_COMMAND_PHASE], tmp_tc, wd->wdregs[WD_CONTROL] >> 5, wds->cdmac.dmac_dma);
#endif
	if (wd->wdregs[WD_COMMAND_PHASE] <= 0x30) {
		scsi->buffer[0] = 0;
		scsi->status = 0;
		memcpy (scsi->cmd, &wd->wdregs[3], 16);
		scsi->data_len = tmp_tc;
		scsi_emulate_analyze (scsi);
		settc (wd, scsi->cmd_len);
		wd->wd_dataoffset = 0;
		scsi_start_transfer (scsi);
		scsi->direction = 2;
		scsi->data_len = scsi->cmd_len;
		for (i = 0; i < gettc (wd); i++) {
			uae_u8 b = scsi->cmd[i];
			wd->wd_data[i] = b;
			scsi_send_data (scsi, b);
			wd->wd_dataoffset++;
		}
		// 0x30 = command phase has started
		scsi->data_len = tmp_tc;
		scsi_emulate_analyze (scsi);
		wd->wdregs[WD_COMMAND_PHASE] = 0x30 + gettc (wd);
		settc (wd, 0);
#if WD33C93_DEBUG > 0
		write_log (_T("%s: Got Command %s, datalen=%d\n"), WD33C93, scsitostring (wd, scsi), scsi->data_len);
#endif
	}

	if (wd->wdregs[WD_COMMAND_PHASE] <= 0x41) {
		wd->wdregs[WD_COMMAND_PHASE] = 0x44;
#if 0
		if (wd->wdregs[WD_CONTROL] & CTL_IDI) {
			wd->wd_phase = CSR_DISC;
			set_status (wd, wd->wd_phase, delay);
			wd->wd_phase = CSR_RESEL;
			set_status (wd, wd->wd_phase, delay + 10);
			return;
		}
#endif
		wd->wdregs[WD_COMMAND_PHASE] = 0x44;
	}

	// target replied or start/continue data phase (if data available)
	if (wd->wdregs[WD_COMMAND_PHASE] == 0x44) {
		if (scsi->direction <= 0) {
			scsi_emulate_cmd (scsi);
		}
		scsi_start_transfer (scsi);
		wd->wdregs[WD_COMMAND_PHASE] = 0x45;
	}
		
	if (wd->wdregs[WD_COMMAND_PHASE] == 0x45) {
		settc (wd, tmp_tc);
		wd->wd_dataoffset = 0;
		setphase (wd, 0x45);

		if (gettc (wd) == 0) {
			if (scsi->direction != 0) {
				// TC = 0 but we may have data
				if (scsi->direction < 0) {
					if (scsi->data_len == 0) {
						// no data, continue normally to status phase
						setphase (wd, 0x46);
						goto end;
					}
				}
				wd->wd_phase = CSR_UNEXP;
				if (scsi->direction < 0)
					wd->wd_phase |= PHS_DATA_IN;
				else
					wd->wd_phase |= PHS_DATA_OUT;
				set_status (wd, wd->wd_phase, 1);
				return;
			}
		}

		if (wd->scsi->direction) {
			if (canwddma (wds)) {
				if (scsi->direction <= 0) {
					do_dma(wds);
					if (scsi->offset < scsi->data_len) {
						// buffer not completely retrieved?
						wd->wd_phase = CSR_UNEXP | PHS_DATA_IN;
						set_status (wd, wd->wd_phase, 1);
						return;
					}
					if (gettc (wd) > 0) {
						// requested more data than was available.
						wd->wd_phase = CSR_UNEXP | PHS_STATUS;
						set_status (wd, wd->wd_phase, 1);
						return;
					}
					setphase (wd, 0x46);
				} else {
					if (do_dma(wds)) {
						setphase (wd, 0x46);
						if (scsi->offset < scsi->data_len) {
							// not enough data?
							wd->wd_phase = CSR_UNEXP | PHS_DATA_OUT;
							set_status (wd, wd->wd_phase, 1);
							return;
						}
						// got all data -> execute it
						scsi_emulate_cmd (scsi);
					}
				}
			} else {
				// no dma = Service Request
				wd->wd_phase = CSR_SRV_REQ;
				if (scsi->direction < 0)
					wd->wd_phase |= PHS_DATA_IN;
				else
					wd->wd_phase |= PHS_DATA_OUT;
				set_status (wd, wd->wd_phase, 1);
				return;
			}
		} else {
			// TC > 0 but no data to transfer
			if (gettc (wd)) {
				wd->wd_phase = CSR_UNEXP | PHS_STATUS;
				set_status (wd, wd->wd_phase, 1);
				return;
			}
			wd->wdregs[WD_COMMAND_PHASE] = 0x46;
		}
	}

	end:
	if (wd->wdregs[WD_COMMAND_PHASE] == 0x46) {
		scsi->buffer[0] = 0;
		wd->wdregs[WD_COMMAND_PHASE] = 0x50;
		wd->wdregs[WD_TARGET_LUN] = scsi->status;
		scsi->buffer[0] = scsi->status;
	}

	// 0x60 = command complete
	wd->wdregs[WD_COMMAND_PHASE] = 0x60;
	if (!(wd->wdregs[WD_CONTROL] & CTL_EDI)) {
		wd->wd_phase = CSR_SEL_XFER_DONE;
		delay += 2;
		set_status (wd, wd->wd_phase, delay);
		delay += 2;
		wd->wd_phase = CSR_DISC;
		set_status (wd, wd->wd_phase, delay);
	} else {
		delay += 2;
		wd->wd_phase = CSR_SEL_XFER_DONE;
		set_status (wd, wd->wd_phase, delay);
	}
	wd->wd_selected = 0;
}

static void wd_cmd_trans_info (struct wd_state *wds, struct scsi_data *scsi)
{
	struct wd_chip_state *wd = &wds->wc;
	if (wd->wdregs[WD_COMMAND_PHASE] == 0x20) {
		wd->wdregs[WD_COMMAND_PHASE] = 0x30;
		scsi->status = 0;
	}
	wd->wd_busy = 1;
	if (wd->wdregs[WD_COMMAND] & 0x80)
		settc (wd, 1);
	if (gettc (wd) == 0)
		settc (wd, 1);
	wd->wd_dataoffset = 0;

	if (wd->wdregs[WD_COMMAND_PHASE] == 0x30) {
		scsi->direction = 2; // command
		scsi->cmd_len = scsi->data_len = gettc (wd);
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x10) {
		scsi->direction = 1; // message
		scsi->data_len = gettc (wd);
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x45) {
		scsi_emulate_analyze (scsi);
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x46 || wd->wdregs[WD_COMMAND_PHASE] == 0x47) {
		scsi->buffer[0] = scsi->status;
		wd->wdregs[WD_TARGET_LUN] = scsi->status;
		scsi->direction = -1; // status
		scsi->data_len = 1;
	} else if (wd->wdregs[WD_COMMAND_PHASE] == 0x50) {
		scsi->direction = -1;
		scsi->data_len = gettc (wd);
	}

	if (canwddma (wds)) {
		wd->wd_data_avail = -1;
	} else {
		wd->wd_data_avail = 1;
	}

#if WD33C93_DEBUG > 0
	write_log (_T("* %s transfer info phase=%02x TC=%d dir=%d data=%d/%d wddma=%d\n"),
		WD33C93, wd->wdregs[WD_COMMAND_PHASE], gettc (wd), scsi->direction, scsi->offset, scsi->data_len, wd->wdregs[WD_CONTROL] >> 5);
#endif

}

/* Weird stuff, XT driver (which has nothing to do with SCSI or WD33C93) uses this WD33C93 command! */
static void wd_cmd_trans_addr(struct wd_chip_state *wd, struct wd_state *wds)
{
	uae_u32 tcyls = (wd->wdregs[WD_T_CYLS_0] << 8) | wd->wdregs[WD_T_CYLS_1];
	uae_u32 theads = wd->wdregs[WD_T_HEADS];
	uae_u32 tsectors = wd->wdregs[WD_T_SECTORS];
	uae_u32 lba = (wd->wdregs[WD_L_ADDR_0] << 24) | (wd->wdregs[WD_L_ADDR_1] << 16) |
		(wd->wdregs[WD_L_ADDR_2] << 8) | (wd->wdregs[WD_L_ADDR_3] << 0);
	uae_u32 cyls, heads, sectors;

	cyls = lba / (theads * tsectors);
	heads = (lba - ((cyls * theads * tsectors))) / tsectors;
	sectors = (lba - ((cyls * theads * tsectors))) % tsectors;

	//write_log(_T("WD TRANS ADDR: LBA=%d TC=%d TH=%d TS=%d -> C=%d H=%d S=%d\n"), lba, tcyls, theads, tsectors, cyls, heads, sectors);

	wd->wdregs[WD_CYL_0] = cyls >> 8;
	wd->wdregs[WD_CYL_1] = cyls;
	wd->wdregs[WD_HEAD] = heads;
	wd->wdregs[WD_SECTOR] = sectors;

	if (wds) {
		// This is cheating, sector value is hardwired on MFM drives. This hack allows to mount hardfiles
		// that are created using incompatible geometry. (XT MFM/RLL drives have real physical geometry)
		if (wds->cdmac.xt_sectors != tsectors && wds->scsis[XT_UNIT]) {
			write_log(_T("XT drive sector value patched from %d to %d\n"), wds->cdmac.xt_sectors, tsectors);
			wds->cdmac.xt_sectors = tsectors;
		}
	}

	if (cyls >= tcyls)
		set_status(wd, CSR_BAD_STATUS);
	else
		set_status(wd, CSR_TRANS_ADDR);
}

static void wd_cmd_sel (struct wd_chip_state *wd, struct wd_state *wds, bool atn)
{
	struct scsi_data *scsi;
#if WD33C93_DEBUG > 0
	write_log (_T("* %s select%s, ID=%d\n"), WD33C93, atn ? _T(" with atn") : _T(""), wd->wdregs[WD_DESTINATION_ID] & 0x7);
#endif
	wd->wd_phase = 0;
	wd->wdregs[WD_COMMAND_PHASE] = 0;

	scsi = wd->scsi = wds->scsis[wd->wdregs[WD_DESTINATION_ID] & 7];
	if (!scsi || (wd->wdregs[WD_DESTINATION_ID] & 7) == 7) {
#if WD33C93_DEBUG > 0
		write_log (_T("%s no drive\n"), WD33C93);
#endif
		set_status (wd, CSR_TIMEOUT, 1000);
		return;
	}
	scsi_start_transfer (wd->scsi);
	wd->wd_selected = true;
	scsi->message[0] = 0x80;
	set_status (wd, CSR_SELECT, 2);
	if (atn) {
		wd->wdregs[WD_COMMAND_PHASE] = 0x10;
		set_status (wd, CSR_SRV_REQ | PHS_MESS_OUT, 4);
	} else {
		wd->wdregs[WD_COMMAND_PHASE] = 0x20;
		set_status (wd, CSR_SRV_REQ | PHS_COMMAND, 4);
	} 
}

static void wd_cmd_reset (struct wd_chip_state *wd, bool irq)
{
	int i;

#if WD33C93_DEBUG > 0
	if (irq)
		write_log (_T("%s reset\n"), WD33C93);
#endif
	for (i = 1; i < 0x16; i++)
		wd->wdregs[i] = 0;
	wd->wdregs[0x18] = 0;
	wd->sasr = 0;
	wd->wd_selected = false;
	wd->scsi = NULL;
	wd->scsidelay_irq[0] = 0;
	wd->scsidelay_irq[1] = 0;
	wd->auxstatus = 0;
	wd->wd_data_avail = 0;
	if (irq) {
		set_status (wd, (wd->wdregs[0] & 0x08) ? 1 : 0, 50);
	}
}

static void wd_cmd_abort (struct wd_chip_state *wd)
{
#if WD33C93_DEBUG > 0
	write_log (_T("%s abort\n"), WD33C93);
#endif
}

static void xt_command_done(struct wd_state *wd);

static void wd_check_interrupt(struct wd_state *wds)
{
	struct wd_chip_state *wd = &wds->wc;
	if (wd->auxstatus & ASR_INT)
		return;
	for (int i = 0; i < WD_STATUS_QUEUE; i++) {
		if (wd->scsidelay_irq[i] == 1) {
			wd->scsidelay_irq[i] = 0;
			doscsistatus(wds, wd->scsidelay_status[i]);
			wd->wd_busy = 0;
		} else if (wd->scsidelay_irq[i] > 1) {
			wd->scsidelay_irq[i]--;
		}
	}
}

static void scsi_hsync_check_dma(struct wd_state *wds)
{
	struct wd_chip_state *wd = &wds->wc;
	if (wd->wd_data_avail < 0 && is_dma_enabled(wds)) {
		bool v;
		do_dma(wds);
		if (wd->scsi->direction < 0) {
			v = wd_do_transfer_in (wd, wd->scsi, false);
		} else if (wd->scsi->direction > 0) {
			v = wd_do_transfer_out (wd, wd->scsi);
		} else {
			write_log (_T("%s data transfer attempt without data!\n"), WD33C93);
			v = true;
		}
		if (v) {
			wd->scsi->direction = 0;
			wd->wd_data_avail = 0;
		} else {
			set_dma_done(wds);
		}
	}
}

static void scsi_hsync2_a2091 (struct wd_state *wds)
{
	struct wd_chip_state *wd = &wds->wc;

	if (!wds->enabled)
		return;
	scsi_hsync_check_dma(wds);
	if (wds->cdmac.dmac_dma > 0 && (wds->cdmac.xt_status & (XT_STAT_INPUT | XT_STAT_REQUEST))) {
		wd->scsi = wds->scsis[XT_UNIT];
		if (do_dma(wds)) {
			xt_command_done(wds);
		}
	}
	wd_check_interrupt(wds);

}

static void scsi_hsync2_gvp (struct wd_state *wds)
{
	if (!wds->enabled)
		return;
	scsi_hsync_check_dma(wds);
	wd_check_interrupt(wds);
}

void scsi_hsync (void)
{
	scsi_hsync2_a2091(&wd_a2091);
	scsi_hsync2_a2091(&wd_a2091_2);
	scsi_hsync2_a2091(&wd_a3000);
	scsi_hsync2_a2091(&wd_cdtv);
	scsi_hsync2_gvp(&wd_gvp);
	scsi_hsync2_gvp(&wd_gvp_2);
}


static int writeonlyreg (int reg)
{
	if (reg == WD_SCSI_STATUS)
		return 1;
	return 0;
}

static uae_u32 makecmd (struct scsi_data *s, int msg, uae_u8 cmd)
{
	uae_u32 v = 0;
	if (s)
		v |= s->id << 24;
	v |= msg << 8;
	v |= cmd;
	return v;
}

static void writewdreg (struct wd_chip_state *wd, int sasr, uae_u8 val)
{
	switch (sasr)
	{
	case WD_OWN_ID:
		if (wd->wd33c93_ver == 0)
			val &= ~(0x20 | 0x08);
		else if (wd->wd33c93_ver == 1)
			val &= ~0x20;
		break;
	}
	if (sasr > WD_QUEUE_TAG && sasr < WD_AUXILIARY_STATUS)
		return;
	// queue tag is B revision only
	if (sasr == WD_QUEUE_TAG && wd->wd33c93_ver < 2)
		return;
	wd->wdregs[sasr] = val;
}

void wdscsi_put (struct wd_chip_state *wd, struct wd_state *wds, uae_u8 d)
{
#if WD33C93_DEBUG > 1
	if (WD33C93_DEBUG > 3 || wd->sasr != WD_DATA)
		write_log (_T("W %s REG %02X = %02X (%d) PC=%08X\n"), WD33C93, wd->sasr, d, d, M68K_GETPC);
#endif
	if (!writeonlyreg (wd->sasr)) {
		writewdreg (wd, wd->sasr, d);
	}
	if (!wd->wd_used) {
		wd->wd_used = 1;
		write_log (_T("%s %s in use\n"), wds->name, WD33C93);
	}
	if (wd->sasr == WD_COMMAND_PHASE) {
#if WD33C93_DEBUG > 1
		write_log (_T("%s PHASE=%02X\n"), WD33C93, d);
#endif
		;
	} else if (wd->sasr == WD_DATA) {
#if WD33C93_DEBUG_PIO
		write_log (_T("%s WD_DATA WRITE %02x %d/%d\n"), WD33C93, d, wd->scsi->offset, wd->scsi->data_len);
#endif
		if (!wd->wd_data_avail) {
			write_log (_T("%s WD_DATA WRITE without data request!?\n"), WD33C93);
			return;
		}
		if (wd->wd_dataoffset < sizeof wd->wd_data)
			wd->wd_data[wd->wd_dataoffset] = wd->wdregs[wd->sasr];
		wd->wd_dataoffset++;
		decreasetc (wd);
		wd->wd_data_avail = 1;
		if (scsi_send_data (wd->scsi, wd->wdregs[wd->sasr]) || gettc (wd) == 0) {
			wd->wd_data_avail = 0;
			write_comm_pipe_u32 (&wds->requests, makecmd (wd->scsi, 2, 0), 1);
		}
	} else if (wd->sasr == WD_COMMAND) {
		wd->wd_busy = true;
		write_comm_pipe_u32(&wds->requests, makecmd(wds->scsis[wd->wdregs[WD_DESTINATION_ID] & 7], 0, d), 1);
		if (wd->scsi && wd->scsi->cd_emu_unit >= 0)
			gui_flicker_led (LED_CD, wd->scsi->id, 1);
	}
	incsasr (wd, 1);
}

void wdscsi_sasr (struct wd_chip_state *wd, uae_u8 b)
{
	wd->sasr = b;
}
uae_u8 wdscsi_getauxstatus (struct wd_chip_state *wd)
{
	return (wd->auxstatus & ASR_INT) | (wd->wd_busy || wd->wd_data_avail < 0 ? ASR_BSY : 0) | (wd->wd_data_avail != 0 ? ASR_DBR : 0);
}

uae_u8 wdscsi_get (struct wd_chip_state *wd, struct wd_state *wds)
{
	uae_u8 v;
#if WD33C93_DEBUG > 1
	uae_u8 osasr = wd->sasr;
#endif

	v = wd->wdregs[wd->sasr];
	if (wd->sasr == WD_DATA) {
		if (!wd->wd_data_avail) {
			write_log (_T("%s WD_DATA READ without data request!?\n"), WD33C93);
			return 0;
		}
		int status = scsi_receive_data (wd->scsi, &v);
#if WD33C93_DEBUG_PIO
		write_log (_T("%s WD_DATA READ %02x %d/%d\n"), WD33C93, v, wd->scsi->offset, wd->scsi->data_len);
#endif
		if (wd->wd_dataoffset < sizeof wd->wd_data)
			wd->wd_data[wd->wd_dataoffset] = v;
		wd->wd_dataoffset++;
		decreasetc (wd);
		wd->wdregs[wd->sasr] = v;
		wd->wd_data_avail = 1;
		if (status || gettc (wd) == 0) {
			wd->wd_data_avail = 0;
			write_comm_pipe_u32 (&wds->requests, makecmd (wd->scsi, 3, 0), 1);
		}
	} else if (wd->sasr == WD_SCSI_STATUS) {
		uae_int_requested &= ~2;
		wd->auxstatus &= ~0x80;
		if (wds->cdtv)
			cdtv_scsi_clear_int ();
		wds->cdmac.dmac_istr &= ~ISTR_INTS;
#if 0
		if (wd->wdregs[WD_COMMAND_PHASE] == 0x10) {
			wd->wdregs[WD_COMMAND_PHASE] = 0x11;
			wd->wd_phase = CSR_SRV_REQ | PHS_MESS_OUT;
			set_status (wd, wd->wd_phase, 1);
		}
#endif
	} else if (wd->sasr == WD_AUXILIARY_STATUS) {
		v = wdscsi_getauxstatus (wd);
	}
	incsasr (wd, 0);
#if WD33C93_DEBUG > 1
	if (WD33C93_DEBUG > 3 || osasr != WD_DATA)
		write_log (_T("R %s REG %02X = %02X (%d) PC=%08X\n"), WD33C93, osasr, v, v, M68K_GETPC);
#endif
	return v;
}

/* XT */

static void xt_default_geometry(struct wd_state *wds)
{
	wds->cdmac.xt_cyls = wds->wc.scsi->hfd->cyls > 1023 ? 1023 : wds->wc.scsi->hfd->cyls;
	wds->cdmac.xt_heads = wds->wc.scsi->hfd->heads > 31 ? 31 : wds->wc.scsi->hfd->heads;
}


static void xt_set_status(struct wd_state *wds, uae_u8 state)
{
	wds->cdmac.xt_status = state;
	wds->cdmac.xt_status |= XT_STAT_SELECT;
	wds->cdmac.xt_status |= XT_STAT_READY;
}

static void xt_reset(struct wd_state *wds)
{
	wds->wc.scsi = wds->scsis[XT_UNIT];
	if (!wds->wc.scsi)
		return;
	wds->cdmac.xt_control = 0;
	wds->cdmac.xt_datalen = 0;
	wds->cdmac.xt_status = 0;
	xt_default_geometry(wds);
	write_log(_T("XT reset\n"));
}

static void xt_command_done(struct wd_state *wds)
{
	switch (wds->cdmac.xt_cmd[0])
	{
		case XT_CMD_DTCSETPARAM:
			wds->cdmac.xt_heads = wds->wc.scsi->buffer[2] & 0x1f;
			wds->cdmac.xt_cyls = ((wds->wc.scsi->buffer[0] & 3) << 8) | (wds->wc.scsi->buffer[1]);
			wds->cdmac.xt_sectors = XT_SECTORS;
			if (!wds->cdmac.xt_heads || !wds->cdmac.xt_cyls)
				xt_default_geometry(wds);
			write_log(_T("XT SETPARAM: cyls=%d heads=%d\n"), wds->cdmac.xt_cyls, wds->cdmac.xt_heads);
			break;
		case XT_CMD_WRITE:
			scsi_emulate_cmd(wds->wc.scsi);
			break;

	}

	xt_set_status(wds, XT_STAT_INTERRUPT);
	if (wds->cdmac.xt_control & XT_INT)
		dmac_a2091_xt_int(wds);
	wds->cdmac.xt_datalen = 0;
	wds->cdmac.xt_statusbyte = 0;
#if XT_DEBUG > 0
	write_log(_T("XT command %02x done\n"), wds->xt_cmd[0]);
#endif
}

static void xt_wait_data(struct wd_state *wds, int len)
{
	xt_set_status(wds, XT_STAT_REQUEST);
	wds->cdmac.xt_offset = 0;
	wds->cdmac.xt_datalen = len;
}

static void xt_sense(struct wd_state *wds)
{
	wds->cdmac.xt_datalen = 4;
	wds->cdmac.xt_offset = 0;
	memset(wds->wc.scsi->buffer, 0, wds->cdmac.xt_datalen);
}

static void xt_readwrite(struct wd_state *wds, int rw)
{
	struct scsi_data *scsi = wds->scsis[XT_UNIT];
	int transfer_len;
	uae_u32 lba;
	// 1 = head
	// 2 = bits 6,7: cyl high, bits 0-5: sectors
	// 3 = cyl (low)
	// 4 = transfer count
	lba = ((wds->cdmac.xt_cmd[3] | ((wds->cdmac.xt_cmd[2] << 2) & 0x300))) * (wds->cdmac.xt_heads * wds->cdmac.xt_sectors) +
		(wds->cdmac.xt_cmd[1] & 0x1f) * wds->cdmac.xt_sectors +
		(wds->cdmac.xt_cmd[2] & 0x3f);

	wds->wc.scsi = scsi;
	wds->cdmac.xt_offset = 0;
	transfer_len = wds->cdmac.xt_cmd[4] == 0 ? 256 : wds->cdmac.xt_cmd[4];
	wds->cdmac.xt_datalen = transfer_len * 512;

#if XT_DEBUG > 0
	write_log(_T("XT %s block %d, %d\n"), rw ? _T("WRITE") : _T("READ"), lba, transfer_len);
#endif

	scsi->cmd[0] = rw ? 0x0a : 0x08; /* WRITE(6) / READ (6) */
	scsi->cmd[1] = lba >> 16;
	scsi->cmd[2] = lba >> 8;
	scsi->cmd[3] = lba >> 0;
	scsi->cmd[4] = transfer_len;
	scsi->cmd[5] = 0;
	scsi_emulate_analyze(wds->wc.scsi);
	if (rw) {
		wds->wc.scsi->direction = 1;
		xt_set_status(wds, XT_STAT_REQUEST);
	} else {
		wds->wc.scsi->direction = -1;
		scsi_emulate_cmd(scsi);
		xt_set_status(wds, XT_STAT_INPUT);
	}
	scsi_start_transfer(scsi);
	settc(&wds->wc, scsi->data_len);

	if (!(wds->cdmac.xt_control & XT_DMA_MODE))
		xt_command_done(wds);
}

static void xt_command(struct wd_state *wds)
{
	wds->wc.scsi = wds->scsis[XT_UNIT];
	switch (wds->cdmac.xt_cmd[0])
	{
	case XT_CMD_READ:
		xt_readwrite(wds, 0);
		break;
	case XT_CMD_WRITE:
		xt_readwrite(wds, 1);
		break;
	case XT_CMD_SEEK:
		xt_command_done(wds);
		break;
	case XT_CMD_VERIFY:
		xt_command_done(wds);
		break;
	case XT_CMD_FORMATBAD:
	case XT_CMD_FORMATTRK:
		xt_command_done(wds);
		break;
	case XT_CMD_TESTREADY:
		xt_command_done(wds);
		break;
	case XT_CMD_RECALIBRATE:
		xt_command_done(wds);
		break;
	case XT_CMD_SENSE:
		xt_sense(wds);
		break;
	case XT_CMD_DTCSETPARAM:
		xt_wait_data(wds, 8);
		break;
	default:
		write_log(_T("XT unknown command %02X\n"), wds->cdmac.xt_cmd[0]);
		xt_command_done(wds);
		wds->cdmac.xt_status |= XT_STAT_INPUT;
		wds->cdmac.xt_datalen = 1;
		wds->cdmac.xt_statusbyte = XT_CSB_ERROR;
		break;
	}
}

static uae_u8 read_xt_reg(struct wd_state *wds, int reg)
{
	uae_u8 v = 0xff;

	wds->wc.scsi = wds->scsis[XT_UNIT];
	if (!wds->wc.scsi)
		return v;

	switch(reg)
	{
	case XD_DATA:
		if (wds->cdmac.xt_status & XT_STAT_INPUT) {
			v = wds->wc.scsi->buffer[wds->cdmac.xt_offset];
			wds->cdmac.xt_offset++;
			if (wds->cdmac.xt_offset >= wds->cdmac.xt_datalen) {
				xt_command_done(wds);
			}
		} else {
			v = wds->cdmac.xt_statusbyte;
		}
		break;
	case XD_STATUS:
		v = wds->cdmac.xt_status;
		break;
	case XD_JUMPER:
		// 20M: 0 40M: 2, xt.device checks it.
		v = wds->wc.scsi->hfd->size >= 41615 * 2 * 512 ? 2 : 0;
		break;
	case XD_RESERVED:
		break;
	}
#if XT_DEBUG > 2
	write_log(_T("XT read %d: %02X\n"), reg, v);
#endif
	return v;
}

static void write_xt_reg(struct wd_state *wds, int reg, uae_u8 v)
{
	wds->wc.scsi = wds->scsis[XT_UNIT];
	if (!wds->wc.scsi)
		return;

#if XT_DEBUG > 2
	write_log(_T("XT write %d: %02X\n"), reg, v);
#endif

	switch (reg)
	{
	case XD_DATA:
#if XT_DEBUG > 1
		write_log(_T("XT data write %02X\n"), v);
#endif
		if (!(wds->cdmac.xt_status & XT_STAT_REQUEST)) {
			wds->cdmac.xt_offset = 0;
			xt_set_status(wds, XT_STAT_COMMAND | XT_STAT_REQUEST);
		}
		if (wds->cdmac.xt_status & XT_STAT_REQUEST) {
			if (wds->cdmac.xt_status & XT_STAT_COMMAND) {
				wds->cdmac.xt_cmd[wds->cdmac.xt_offset++] = v;
				xt_set_status(wds, XT_STAT_COMMAND | XT_STAT_REQUEST);
				if (wds->cdmac.xt_offset == 6) {
					xt_command(wds);
				}
			} else {
				wds->wc.scsi->buffer[wds->cdmac.xt_offset] = v;
				wds->cdmac.xt_offset++;
				if (wds->cdmac.xt_offset >= wds->cdmac.xt_datalen) {
					xt_command_done(wds);
				}
			}
		}
		break;
	case XD_RESET:
		xt_reset(wds);
		break;
	case XD_SELECT:
#if XT_DEBUG > 1
		write_log(_T("XT select %02X\n"), v);
#endif
		xt_set_status(wds, XT_STAT_SELECT);
		break;
	case XD_CONTROL:
		wds->cdmac.xt_control = v;
		wds->cdmac.xt_irq = 0;
		break;
	}
}

/* DMAC */

static uae_u32 dmac_a2091_read_word (struct wd_state *wd, uaecptr addr)
{
	uae_u32 v = 0;

	if (addr < 0x40)
		return (wd->dmacmemory[addr] << 8) | wd->dmacmemory[addr + 1];
	if (addr >= CDMAC_ROM_OFFSET) {
		if (wd->rom) {
			int off = addr & wd->rom_mask;
			if (wd->rombankswitcher && (addr & 0xffe0) == CDMAC_ROM_OFFSET)
				wd->rombank = (addr & 0x02) >> 1;
			off += wd->rombank * wd->rom_size;
			return (wd->rom[off] << 8) | wd->rom[off + 1];
		}
		return 0;
	}

	addr &= ~1;
	switch (addr)
	{
	case 0x40:
		v = wd->cdmac.dmac_istr;
		if (v && (wd->cdmac.dmac_cntr & CNTR_INTEN))
			v |= ISTR_INT_P;
		wd->cdmac.dmac_istr &= ~0xf;
		break;
	case 0x42:
		v = wd->cdmac.dmac_cntr;
		break;
	case 0x80:
		if (wd->cdmac.old_dmac)
			v = (wd->cdmac.dmac_wtc >> 16) & 0xffff;
		break;
	case 0x82:
		if (wd->cdmac.old_dmac)
			v = wd->cdmac.dmac_wtc & 0xffff;
		break;
	case 0x90:
		v = wdscsi_getauxstatus(&wd->wc);
		break;
	case 0x92:
		v = wdscsi_get(&wd->wc, wd);
		break;
	case 0xc0:
		v = 0xf8 | (1 << 0) | (1 << 1) | (1 << 2); // bits 0-2 = dip-switches
		break;
	case 0xc2:
	case 0xc4:
	case 0xc6:
		v = 0xffff;
		break;
	case 0xe0:
		if (wd->cdmac.dmac_dma <= 0)
			scsi_dmac_a2091_start_dma (wd);
		break;
	case 0xe2:
		scsi_dmac_a2091_stop_dma (wd);
		break;
	case 0xe4:
		dmac_a2091_cint (wd);
		break;
	case 0xe8:
		/* FLUSH (new only) */
		if (!wd->cdmac.old_dmac && wd->cdmac.dmac_dma > 0)
			wd->cdmac.dmac_istr |= ISTR_FE_FLG;
		break;
	}
#if A2091_DEBUG_IO > 0
	write_log (_T("dmac_wget %04X=%04X PC=%08X\n"), addr, v, M68K_GETPC);
#endif
	return v;
}

static uae_u32 dmac_a2091_read_byte (struct wd_state *wd, uaecptr addr)
{
	uae_u32 v = 0;

	if (addr < 0x40)
		return wd->dmacmemory[addr];
	if (addr >= CDMAC_ROM_OFFSET) {
		if (wd->rom) {
			int off = addr & wd->rom_mask;
			if (wd->rombankswitcher && (addr & 0xffe0) == CDMAC_ROM_OFFSET)
				wd->rombank = (addr & 0x02) >> 1;
			off += wd->rombank * wd->rom_size;
			return wd->rom[off];
		}
		return 0;
	}

	switch (addr)
	{
	case 0x91:
		v = wdscsi_getauxstatus (&wd->wc);
		break;
	case 0x93:
		v = wdscsi_get (&wd->wc, wd);
		break;
	case 0xa1:
	case 0xa3:
	case 0xa5:
	case 0xa7:
		v = read_xt_reg(wd, (addr - 0xa0) / 2);
		break;
	default:
		v = dmac_a2091_read_word (wd, addr);
		if (!(addr & 1))
			v >>= 8;
		break;
	}
#if A2091_DEBUG_IO > 0
	write_log (_T("dmac_bget %04X=%02X PC=%08X\n"), addr, v, M68K_GETPC);
#endif
	return v;
}

static void dmac_a2091_write_word (struct wd_state *wd, uaecptr addr, uae_u32 b)
{
	if (addr < 0x40)
		return;
	if (addr >= CDMAC_ROM_OFFSET)
		return;

#if A2091_DEBUG_IO > 0
	write_log (_T("dmac_wput %04X=%04X PC=%08X\n"), addr, b & 65535, M68K_GETPC);
#endif

	addr &= ~1;
	switch (addr)
	{
	case 0x42:
		wd->cdmac.dmac_cntr = b;
		if (wd->cdmac.dmac_cntr & CNTR_PREST)
			dmac_reset (wd);
		break;
	case 0x80:
		wd->cdmac.dmac_wtc &= 0x0000ffff;
		wd->cdmac.dmac_wtc |= b << 16;
		break;
	case 0x82:
		wd->cdmac.dmac_wtc &= 0xffff0000;
		wd->cdmac.dmac_wtc |= b & 0xffff;
		break;
	case 0x84:
		wd->cdmac.dmac_acr &= 0x0000ffff;
		wd->cdmac.dmac_acr |= b << 16;
		break;
	case 0x86:
		wd->cdmac.dmac_acr &= 0xffff0000;
		wd->cdmac.dmac_acr |= b & 0xfffe;
		if (wd->cdmac.old_dmac)
			wd->cdmac.dmac_acr &= ~3;
		break;
	case 0x8e:
		wd->cdmac.dmac_dawr = b;
		break;
	case 0x90:
		wdscsi_sasr (&wd->wc, b);
		break;
	case 0x92:
		wdscsi_put (&wd->wc, wd, b);
		break;
	case 0xc2:
	case 0xc4:
	case 0xc6:
		break;
	case 0xe0:
		if (wd->cdmac.dmac_dma <= 0)
			scsi_dmac_a2091_start_dma (wd);
		break;
	case 0xe2:
		scsi_dmac_a2091_stop_dma (wd);
		break;
	case 0xe4:
		dmac_a2091_cint (wd);
		break;
	case 0xe8:
		/* FLUSH */
		wd->cdmac.dmac_istr |= ISTR_FE_FLG;
		break;
	}
}

static void dmac_a2091_write_byte (struct wd_state *wd, uaecptr addr, uae_u32 b)
{
	if (addr < 0x40)
		return;
	if (addr >= CDMAC_ROM_OFFSET)
		return;

#if A2091_DEBUG_IO > 0
	write_log (_T("dmac_bput %04X=%02X PC=%08X\n"), addr, b & 255, M68K_GETPC);
#endif

	switch (addr)
	{
	case 0x91:
		wdscsi_sasr (&wd->wc, b);
		break;
	case 0x93:
		wdscsi_put (&wd->wc, wd, b);
		break;
	case 0xa1:
	case 0xa3:
	case 0xa5:
	case 0xa7:
		write_xt_reg(wd, (addr - 0xa0) / 2, b);
		break;
	default:
		if (addr & 1)
			dmac_a2091_write_word (wd, addr, b);
		else
			dmac_a2091_write_word (wd, addr, b << 8);
	}
}

static uae_u32 REGPARAM2 dmac_a2091_lget (struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_a2091_read_word(wd, addr) << 16;
	v |= dmac_a2091_read_word(wd, addr + 2) & 0xffff;
	return v;
}

static uae_u32 REGPARAM2 dmac_a2091_wget(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_a2091_read_word(wd, addr);
	return v;
}

static uae_u32 REGPARAM2 dmac_a2091_bget(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_a2091_read_byte(wd, addr);
	return v;
}

static void REGPARAM2 dmac_a2091_lput(struct wd_state *wd, uaecptr addr, uae_u32 l)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	addr &= 65535;
	dmac_a2091_write_word(wd, addr + 0, l >> 16);
	dmac_a2091_write_word(wd, addr + 2, l);
}

static void REGPARAM2 dmac_a2091_wput(struct wd_state *wd, uaecptr addr, uae_u32 w)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	addr &= 65535;
	dmac_a2091_write_word(wd, addr, w);
}

extern addrbank dmaca2091_bank;
extern addrbank dmaca2091_2_bank;

static void REGPARAM2 dmac_a2091_bput(struct wd_state *wd, uaecptr addr, uae_u32 b)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	b &= 0xff;
	addr &= 65535;
	if (wd->autoconfig) {
		addrbank *ab = wd == &wd_a2091 ? &dmaca2091_bank : &dmaca2091_2_bank;
		if (addr == 0x48 && !wd->configured) {
			map_banks_z2 (ab, b, 0x10000 >> 16);
			wd->configured = 1;
			expamem_next (ab, NULL);
			return;
		}
		if (addr == 0x4c && !wd->configured) {
			wd->configured = 1;
			expamem_shutup(ab);
			return;
		}
		if (!wd->configured)
			return;
	}
	dmac_a2091_write_byte(wd, addr, b);
}

static uae_u32 REGPARAM2 dmac_a2091_wgeti(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v = 0xffff;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	if (addr >= CDMAC_ROM_OFFSET)
		v = (wd->rom[addr & wd->rom_mask] << 8) | wd->rom[(addr + 1) & wd->rom_mask];
	else
		write_log(_T("Invalid DMAC instruction access %08x\n"), addr);
	return v;
}
static uae_u32 REGPARAM2 dmac_a2091_lgeti(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_a2091_wgeti(wd, addr) << 16;
	v |= dmac_a2091_wgeti(wd, addr + 2);
	return v;
}

static int REGPARAM2 dmac_a2091_check(struct wd_state *wd, uaecptr addr, uae_u32 size)
{
	return 1;
}

static uae_u8 *REGPARAM2 dmac_a2091_xlate(struct wd_state *wd, uaecptr addr)
{
	addr &= 0xffff;
	addr += wd->rombank * wd->rom_size;
	if (addr >= 0x8000)
		addr = 0x8000;
	return wd->rom + addr;
}

static uae_u8 *REGPARAM2 dmac_a2091_xlate (uaecptr addr)
{
	return dmac_a2091_xlate(&wd_a2091, addr);
}
static int REGPARAM2 dmac_a2091_check (uaecptr addr, uae_u32 size)
{
	return dmac_a2091_check(&wd_a2091, addr, size);
}
static uae_u32 REGPARAM2 dmac_a2091_lgeti (uaecptr addr)
{
	return dmac_a2091_lgeti(&wd_a2091, addr);
}
static uae_u32 REGPARAM2 dmac_a2091_wgeti (uaecptr addr)
{
	return dmac_a2091_wgeti(&wd_a2091, addr);
}
static uae_u32 REGPARAM2 dmac_a2091_bget (uaecptr addr)
{
	return dmac_a2091_bget(&wd_a2091, addr);
}
static uae_u32 REGPARAM2 dmac_a2091_wget (uaecptr addr)
{
	return dmac_a2091_wget(&wd_a2091, addr);
}
static uae_u32 REGPARAM2 dmac_a2091_lget (uaecptr addr)
{
	return dmac_a2091_lget(&wd_a2091, addr);
}
static void REGPARAM2 dmac_a2091_bput (uaecptr addr, uae_u32 b)
{
	dmac_a2091_bput(&wd_a2091, addr, b);
}
static void REGPARAM2 dmac_a2091_wput (uaecptr addr, uae_u32 b)
{
	dmac_a2091_wput(&wd_a2091, addr, b);
}
static void REGPARAM2 dmac_a2091_lput (uaecptr addr, uae_u32 b)
{
	dmac_a2091_lput(&wd_a2091, addr, b);
}

static uae_u8 *REGPARAM2 dmac_a20912_xlate (uaecptr addr)
{
	return dmac_a2091_xlate(&wd_a2091_2, addr);
}
static int REGPARAM2 dmac_a20912_check (uaecptr addr, uae_u32 size)
{
	return dmac_a2091_check(&wd_a2091_2, addr, size);
}
static uae_u32 REGPARAM2 dmac_a20912_lgeti (uaecptr addr)
{
	return dmac_a2091_lgeti(&wd_a2091_2, addr);
}
static uae_u32 REGPARAM2 dmac_a20912_wgeti (uaecptr addr)
{
	return dmac_a2091_wgeti(&wd_a2091_2, addr);
}
static uae_u32 REGPARAM2 dmac_a20912_bget (uaecptr addr)
{
	return dmac_a2091_bget(&wd_a2091_2, addr);
}
static uae_u32 REGPARAM2 dmac_a20912_wget (uaecptr addr)
{
	return dmac_a2091_wget(&wd_a2091_2, addr);
}
static uae_u32 REGPARAM2 dmac_a20912_lget (uaecptr addr)
{
	return dmac_a2091_lget(&wd_a2091_2, addr);
}
static void REGPARAM2 dmac_a20912_bput (uaecptr addr, uae_u32 b)
{
	dmac_a2091_bput(&wd_a2091_2, addr, b);
}
static void REGPARAM2 dmac_a20912_wput (uaecptr addr, uae_u32 b)
{
	dmac_a2091_wput(&wd_a2091_2, addr, b);
}
static void REGPARAM2 dmac_a20912_lput (uaecptr addr, uae_u32 b)
{
	dmac_a2091_lput(&wd_a2091_2, addr, b);
}

addrbank dmaca2091_bank = {
	dmac_a2091_lget, dmac_a2091_wget, dmac_a2091_bget,
	dmac_a2091_lput, dmac_a2091_wput, dmac_a2091_bput,
	dmac_a2091_xlate, dmac_a2091_check, NULL, NULL, _T("A2091/A590"),
	dmac_a2091_lgeti, dmac_a2091_wgeti, ABFLAG_IO | ABFLAG_SAFE
};
addrbank dmaca2091_2_bank = {
	dmac_a20912_lget, dmac_a20912_wget, dmac_a20912_bget,
	dmac_a20912_lput, dmac_a20912_wput, dmac_a20912_bput,
	dmac_a20912_xlate, dmac_a20912_check, NULL, NULL, _T("A2091/A590 #2"),
	dmac_a20912_lgeti, dmac_a20912_wgeti, ABFLAG_IO | ABFLAG_SAFE
};

/* GVP Series I and II */

extern addrbank gvp_bank;
extern addrbank gvp_2_bank;

static uae_u32 dmac_gvp_read_byte(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v = 0;

	addr &= 0xffff;
	if (addr < 0x3e) {
		v = wd->dmacmemory[addr];
	} else if (addr >= GVP_ROM_OFFSET) {
		if (wd->gdmac.series2) {
			if (addr & 1) {
				v = wd->gdmac.version;
			} else {
				if (wd->rom) {
					if (wd->rombankswitcher && (addr & 0xffe0) == GVP_ROM_OFFSET)
						wd->rombank = (addr & 0x02) >> 1;
					v = wd->rom[(addr - GVP_ROM_OFFSET) / 2 + wd->rombank * 16384];
				}
			}
		} else {
			if (wd->rom) {
				v = wd->rom[addr - GVP_ROM_OFFSET];
			}
		}
	} else if (addr >= GVP_SERIES_I_RAM_OFFSET && !wd->gdmac.series2) {
		v =  wd->gdmac.buffer[wd->gdmac.bufoffset++];
		wd->gdmac.bufoffset &= GVP_SERIES_I_RAM_MASK;
	} else if (wd->configured) {
		if (wd->gdmac.series2) {
			switch (addr)
			{
				case 0x40:
				v = wd->gdmac.cntr >> 8;
				break;
				case 0x41:
				v = wd->gdmac.cntr;
				break;
				case 0x61: // SASR
				v = wdscsi_getauxstatus(&wd->wc);
				break;
				case 0x63: // SCMD
				v = wdscsi_get(&wd->wc, wd);
				break;
				default:
				write_log(_T("gvp_s2_bget_unk %04X PC=%08X\n"), addr, M68K_GETPC);
				break;
			}
		} else {
			switch (addr)
			{
				case 0x3e:
				v = wd->wc.auxstatus & ASR_INT;
				break;
				case 0x60: // SASR
				v = wdscsi_getauxstatus(&wd->wc);
				break;
				case 0x62: // SCMD
				v = wdscsi_get(&wd->wc, wd);
				break;
				default:
				write_log(_T("gvp_s1_bget_unk %04X PC=%08X\n"), addr, M68K_GETPC);
				break;
			}
		}
	} else {
		v = 0xff;
	}

#if GVP_S2_DEBUG_IO > 0
	write_log(_T("gvp_bget %04X=%02X PC=%08X\n"), addr, v, M68K_GETPC);
#endif

	return v;
}
static uae_u32 dmac_gvp_read_word(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v = 0;

	addr &= 0xffff;
	if (addr < 0x3e) {
		v = (wd->dmacmemory[addr] << 8) | wd->dmacmemory[addr + 1];
	} else if (addr >= GVP_ROM_OFFSET) {
		if (wd->gdmac.series2) {
			if (wd->rom) {
				if (wd->rombankswitcher && (addr & 0xffe0) == GVP_ROM_OFFSET)
					wd->rombank = (addr & 0x02) >> 1;
				v = (wd->rom[(addr - GVP_ROM_OFFSET) / 2 + wd->rombank * 16384] << 8) | wd->gdmac.version;
			} else {
				v = wd->gdmac.version;
			}
		} else {
			if (wd->rom) {
				v = (wd->rom[addr - GVP_ROM_OFFSET] << 8) | (wd->rom[addr - GVP_ROM_OFFSET + 1]);
			}
		}
	} else if (addr >= GVP_SERIES_I_RAM_OFFSET && !wd->gdmac.series2) {
#if GVP_S1_DEBUG_IO > 1
		int off = wd->gdmac.bufoffset;
#endif
		v =  wd->gdmac.buffer[wd->gdmac.bufoffset++] << 8;
		wd->gdmac.bufoffset &= GVP_SERIES_I_RAM_MASK;
		v |= wd->gdmac.buffer[wd->gdmac.bufoffset++] << 0;
		wd->gdmac.bufoffset &= GVP_SERIES_I_RAM_MASK;
#if GVP_S1_DEBUG_IO > 1
		write_log(_T("gvp_s1_wget sram %d %04x\n"), off, v);
#endif
	} else if (wd->configured) {
		if (wd->gdmac.series2) {
			switch (addr)
			{
				case 0x40:
				v = wd->gdmac.cntr;
				break;
				case 0x68:
				v = wd->gdmac.bank;
				break;
				case 0x70:
				v = wd->gdmac.addr >> 16;
				break;
				case 0x72:
				v = wd->gdmac.addr;
				break;
				default:
				write_log(_T("gvp_s2_wget_unk %04X PC=%08X\n"), addr, M68K_GETPC);
				break;
			}
#if GVP_S2_DEBUG_IO > 0
			write_log(_T("gvp_s2_wget %04X=%04X PC=%08X\n"), addr, v, M68K_GETPC);
#endif
		} else {
#if GVP_S1_DEBUG_IO > 0
			write_log(_T("gvp_s1_wget %04X=%04X PC=%08X\n"), addr, v, M68K_GETPC);
#endif
		}
	} else {
		v = 0xffff;
	}

	return v;
}

static void dmac_gvp_write_word(struct wd_state *wd, uaecptr addr, uae_u32 b)
{
	addr &= 0xffff;

	if (addr >= GVP_ROM_OFFSET)
		return;

	if (addr >= GVP_SERIES_I_RAM_OFFSET && !wd->gdmac.series2) {
#if GVP_S1_DEBUG_IO > 1
		int off = wd->gdmac.bufoffset;
#endif
		wd->gdmac.buffer[wd->gdmac.bufoffset++] = b >> 8;
		wd->gdmac.bufoffset &= GVP_SERIES_I_RAM_MASK;
		wd->gdmac.buffer[wd->gdmac.bufoffset++] = b;
		wd->gdmac.bufoffset &= GVP_SERIES_I_RAM_MASK;
#if GVP_S1_DEBUG_IO > 1
		write_log(_T("gvp_s1_wput sram %d %04x\n"), off, b);
#endif
		return;
	}

	if (wd->gdmac.series2) {
#if GVP_S2_DEBUG_IO > 0
		write_log(_T("gvp_s2_wput %04X=%04X PC=%08X\n"), addr, b & 65535, M68K_GETPC);
#endif
		switch (addr)
		{
			case 0x40:
			b &= ~(1 | 2);
			wd->gdmac.cntr = b;
			break;
			case 0x68: // bank
			if (b != 0)
				write_log(_T("bank %02x\n"), b);
			break;
			case 0x70: // ACR
			wd->gdmac.addr &= 0x0000ffff;
			wd->gdmac.addr |= (b & 0xff) << 16;
			wd->gdmac.addr &= wd->gdmac.addr_mask;
			break;
			case 0x72: // ACR
			wd->gdmac.addr &= 0xffff0000;
			wd->gdmac.addr |= b;
			wd->gdmac.addr &= wd->gdmac.addr_mask;
			break;
			case 0x76: // START DMA
			wd->gdmac.dma_on = 1;
			break;
			case 0x78: // STOP DMA
			wd->gdmac.dma_on = 0;
			break;
			case 0x74: // "secret1"
			case 0x7a: // "secret2"
			case 0x7c: // "secret3"
			write_log(_T("gvp_s2_wput_config %04X=%04X PC=%08X\n"), addr, b & 65535, M68K_GETPC);
			break;
			default:
			write_log(_T("gvp_s2_wput_unk %04X=%04X PC=%08X\n"), addr, b & 65535, M68K_GETPC);
			break;
		}
	} else {
#if GVP_S1_DEBUG_IO > 0
		write_log(_T("gvp_s1_wput %04X=%04X PC=%08X\n"), addr, b & 65535, M68K_GETPC);
#endif
	}
}

static void dmac_gvp_write_byte(struct wd_state *wd, uaecptr addr, uae_u32 b)
{
	addr &= 0xffff;

	if (addr >= GVP_ROM_OFFSET)
		return;

	if (addr >= GVP_SERIES_I_RAM_OFFSET && !wd->gdmac.series2) {
		wd->gdmac.buffer[wd->gdmac.bufoffset++] = b;
		wd->gdmac.bufoffset &= GVP_SERIES_I_RAM_MASK;
		return;
	}

	if (wd->gdmac.series2) {
#if GVP_S2_DEBUG_IO > 0
		write_log(_T("gvp_s2_bput %04X=%02X PC=%08X\n"), addr, b & 255, M68K_GETPC);
#endif
		switch (addr)
		{
			case 0x40:
			wd->gdmac.cntr &= 0x00ff;
			wd->gdmac.cntr |= b << 8;
			break;
			case 0x41:
			b &= ~(1 | 2);
			wd->gdmac.cntr &= 0xff00;
			wd->gdmac.cntr |= b << 0;
			break;
			case 0x61: // SASR
			wdscsi_sasr(&wd->wc, b);
			break;
			case 0x63: // SCMD
			wdscsi_put(&wd->wc, wd, b);
			break;
		
			case 0x74: // "secret1"
			case 0x75:
			case 0x7a: // "secret2"
			case 0x7b:
			case 0x7c: // "secret3"
			case 0x7d:
			write_log(_T("gvp_s2_bput_config %04X=%04X PC=%08X\n"), addr, b & 255, M68K_GETPC);
			break;
			default:
			write_log(_T("gvp_s2_bput_unk %04X=%02X PC=%08X\n"), addr, b & 255, M68K_GETPC);
			break;
		}
	} else {
#if GVP_S1_DEBUG_IO > 0
		write_log(_T("gvp_s1_bput %04X=%02X PC=%08X\n"), addr, b & 255, M68K_GETPC);
#endif
		switch (addr)
		{
			case 0x60: // SASR
			wdscsi_sasr(&wd->wc, b);
			break;
			case 0x62: // SCMD
			wdscsi_put(&wd->wc, wd, b);
			break;
		
			// 68:
			// 00 CPU SRAM access 
			// ff WD SRAM access

			// 6c:
			// 28 0010 startup reset?
			// b8 1011 before CPU reading from SRAM
			// a8 1100 before CPU writing to SRAM
			// e8 1110 before starting WD write DMA
			// f8 1111 access done/start WD read DMA
			// 08 = intena?

			case 0x68:
#if GVP_S1_DEBUG_IO > 0
			write_log(_T("gvp_s1_bput_s1 %04X=%04X PC=%08X\n"), addr, b & 255, M68K_GETPC);
#endif
			wd->gdmac.bufoffset = 0;
			break;
			case 0x6c:
#if GVP_S1_DEBUG_IO > 0
			write_log(_T("gvp_s1_bput_s1 %04X=%04X PC=%08X\n"), addr, b & 255, M68K_GETPC);
#endif
			wd->gdmac.cntr = b & 8;
			break;
		
			default:
			write_log(_T("gvp_s1_bput_unk %04X=%02X PC=%08X\n"), addr, b & 255, M68K_GETPC);
			break;
		}
	}

}

static uae_u32 REGPARAM2 dmac_gvp_lget(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_gvp_read_word(wd, addr) << 16;
	v |= dmac_gvp_read_word(wd, addr + 2) & 0xffff;
	return v;
}

static uae_u32 REGPARAM2 dmac_gvp_wget(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_gvp_read_word(wd, addr);
	return v;
}

static uae_u32 REGPARAM2 dmac_gvp_bget(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_gvp_read_byte(wd, addr);
	return v;
}

static void REGPARAM2 dmac_gvp_lput(struct wd_state *wd, uaecptr addr, uae_u32 l)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	addr &= 65535;
	dmac_gvp_write_word(wd, addr + 0, l >> 16);
	dmac_gvp_write_word(wd, addr + 2, l);
}

static void REGPARAM2 dmac_gvp_wput(struct wd_state *wd, uaecptr addr, uae_u32 w)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	addr &= 65535;
	dmac_gvp_write_word(wd, addr, w);
}
static void REGPARAM2 dmac_gvp_bput(struct wd_state *wd, uaecptr addr, uae_u32 b)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	b &= 0xff;
	addr &= 65535;
	if (wd->autoconfig) {
		addrbank *ab = wd == &wd_gvp ? &gvp_bank : &gvp_2_bank;
		if (addr == 0x48 && !wd->configured) {
			map_banks_z2(ab, b, 0x10000 >> 16);
			wd->configured = 1;
			expamem_next(ab, NULL);
			return;
		}
		if (addr == 0x4c && !wd->configured) {
			wd->configured = 1;
			expamem_shutup(ab);
			return;
		}
		if (!wd->configured)
			return;
	}
	dmac_gvp_write_byte(wd, addr, b);
}

static uae_u32 REGPARAM2 dmac_gvp_wgeti(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v = 0xffff;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	if (addr >= GVP_ROM_OFFSET) {
		addr -= GVP_ROM_OFFSET;
		v = (wd->rom[addr & wd->rom_mask] << 8) | wd->rom[(addr + 1) & wd->rom_mask];
	} else {
		write_log(_T("Invalid GVP instruction access %08x\n"), addr);
	}
	return v;
}
static uae_u32 REGPARAM2 dmac_gvp_lgeti(struct wd_state *wd, uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	v = dmac_gvp_wgeti(wd, addr) << 16;
	v |= dmac_gvp_wgeti(wd, addr + 2);
	return v;
}

static int REGPARAM2 dmac_gvp_check(struct wd_state *wd, uaecptr addr, uae_u32 size)
{
	return 1;
}

static uae_u8 *REGPARAM2 dmac_gvp_xlate(struct wd_state *wd, uaecptr addr)
{
	addr &= 0xffff;
	return wd->rom + addr;
}

static uae_u8 *REGPARAM2 dmac_gvp_xlate(uaecptr addr)
{
	return dmac_gvp_xlate(&wd_gvp, addr);
}
static int REGPARAM2 dmac_gvp_check(uaecptr addr, uae_u32 size)
{
	return dmac_gvp_check(&wd_gvp, addr, size);
}
static uae_u32 REGPARAM2 dmac_gvp_lgeti(uaecptr addr)
{
	return dmac_gvp_lgeti(&wd_gvp, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_wgeti(uaecptr addr)
{
	return dmac_gvp_wgeti(&wd_gvp, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_bget(uaecptr addr)
{
	return dmac_gvp_bget(&wd_gvp, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_wget(uaecptr addr)
{
	return dmac_gvp_wget(&wd_gvp, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_lget(uaecptr addr)
{
	return dmac_gvp_lget(&wd_gvp, addr);
}
static void REGPARAM2 dmac_gvp_bput(uaecptr addr, uae_u32 b)
{
	dmac_gvp_bput(&wd_gvp, addr, b);
}
static void REGPARAM2 dmac_gvp_wput(uaecptr addr, uae_u32 b)
{
	dmac_gvp_wput(&wd_gvp, addr, b);
}
static void REGPARAM2 dmac_gvp_lput(uaecptr addr, uae_u32 b)
{
	dmac_gvp_lput(&wd_gvp, addr, b);
}

addrbank gvp_bank = {
	dmac_gvp_lget, dmac_gvp_wget, dmac_gvp_bget,
	dmac_gvp_lput, dmac_gvp_wput, dmac_gvp_bput,
	dmac_gvp_xlate, dmac_gvp_check, NULL, NULL, _T("GVP"),
	dmac_gvp_lgeti, dmac_gvp_wgeti, ABFLAG_IO | ABFLAG_SAFE
};

static uae_u8 *REGPARAM2 dmac_gvp_2_xlate(uaecptr addr)
{
	return dmac_gvp_xlate(&wd_gvp_2, addr);
}
static int REGPARAM2 dmac_gvp_2_check(uaecptr addr, uae_u32 size)
{
	return dmac_gvp_check(&wd_gvp_2, addr, size);
}
static uae_u32 REGPARAM2 dmac_gvp_2_lgeti(uaecptr addr)
{
	return dmac_gvp_lgeti(&wd_gvp_2, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_2_wgeti(uaecptr addr)
{
	return dmac_gvp_wgeti(&wd_gvp_2, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_2_bget(uaecptr addr)
{
	return dmac_gvp_bget(&wd_gvp_2, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_2_wget(uaecptr addr)
{
	return dmac_gvp_wget(&wd_gvp_2, addr);
}
static uae_u32 REGPARAM2 dmac_gvp_2_lget(uaecptr addr)
{
	return dmac_gvp_lget(&wd_gvp_2, addr);
}
static void REGPARAM2 dmac_gvp_2_bput(uaecptr addr, uae_u32 b)
{
	dmac_gvp_bput(&wd_gvp_2, addr, b);
}
static void REGPARAM2 dmac_gvp_2_wput(uaecptr addr, uae_u32 b)
{
	dmac_gvp_wput(&wd_gvp_2, addr, b);
}
static void REGPARAM2 dmac_gvp_2_lput(uaecptr addr, uae_u32 b)
{
	dmac_gvp_lput(&wd_gvp_2, addr, b);
}

addrbank gvp_2_bank = {
	dmac_gvp_2_lget, dmac_gvp_2_wget, dmac_gvp_2_bget,
	dmac_gvp_2_lput, dmac_gvp_2_wput, dmac_gvp_2_bput,
	dmac_gvp_2_xlate, dmac_gvp_2_check, NULL, NULL, _T("GVP #2"),
	dmac_gvp_2_lgeti, dmac_gvp_2_wgeti, ABFLAG_IO | ABFLAG_SAFE
};

/* SUPERDMAC (A3000 mainboard built-in) */

static void mbdmac_write_word (struct wd_state *wd, uae_u32 addr, uae_u32 val)
{
#if A3000_DEBUG_IO > 1
	write_log (_T("DMAC_WWRITE %08X=%04X PC=%08X\n"), addr, val & 0xffff, M68K_GETPC);
#endif
	addr &= 0xfffe;
	switch (addr)
	{
	case 0x02:
		wd->cdmac.dmac_dawr = val;
		break;
	case 0x04:
		wd->cdmac.dmac_wtc &= 0x0000ffff;
		wd->cdmac.dmac_wtc |= val << 16;
		break;
	case 0x06:
		wd->cdmac.dmac_wtc &= 0xffff0000;
		wd->cdmac.dmac_wtc |= val & 0xffff;
		break;
	case 0x0a:
		wd->cdmac.dmac_cntr = val;
		if (wd->cdmac.dmac_cntr & SCNTR_PREST)
			dmac_reset (wd);
		break;
	case 0x0c:
		wd->cdmac.dmac_acr &= 0x0000ffff;
		wd->cdmac.dmac_acr |= val << 16;
		break;
	case 0x0e:
		wd->cdmac.dmac_acr &= 0xffff0000;
		wd->cdmac.dmac_acr |= val & 0xfffe;
		break;
	case 0x12:
		if (wd->cdmac.dmac_dma <= 0)
			scsi_dmac_a2091_start_dma (wd);
		break;
	case 0x16:
		if (wd->cdmac.dmac_dma) {
			/* FLUSH */
			wd->cdmac.dmac_istr |= ISTR_FE_FLG;
			wd->cdmac.dmac_dma = 0;
		}
		break;
	case 0x1a:
		dmac_a2091_cint(wd);
		break;
	case 0x1e:
		/* ISTR */
		break;
	case 0x3e:
		scsi_dmac_a2091_stop_dma (wd);
		break;
	case 0x40:
	case 0x48:
		wdscsi_sasr(&wd->wc, val);
		break;
	case 0x42:
	case 0x46:
		wdscsi_put(&wd->wc, wd, val);
		break;
	}
}

static void mbdmac_write_byte (struct wd_state *wd, uae_u32 addr, uae_u32 val)
{
#if A3000_DEBUG_IO > 1
	write_log (_T("DMAC_BWRITE %08X=%02X PC=%08X\n"), addr, val & 0xff, M68K_GETPC);
#endif
	addr &= 0xffff;
	switch (addr)
	{

	case 0x41:
	case 0x49:
		wdscsi_sasr(&wd->wc, val);
		break;
	case 0x43:
	case 0x47:
		wdscsi_put (&wd->wc, wd, val);
		break;
	default:
		if (addr & 1)
			mbdmac_write_word (wd, addr, val);
		else
			mbdmac_write_word (wd, addr, val << 8);
	}
}

static uae_u32 mbdmac_read_word (struct wd_state *wd, uae_u32 addr)
{
#if A3000_DEBUG_IO > 1
	uae_u32 vaddr = addr;
#endif
	uae_u32 v = 0xffffffff;

	addr &= 0xfffe;
	switch (addr)
	{
	case 0x02:
		v = wd->cdmac.dmac_dawr;
		break;
	case 0x04:
	case 0x06:
		v = 0xffff;
		break;
	case 0x0a:
		v = wd->cdmac.dmac_cntr;
		break;
	case 0x0c:
		v = wd->cdmac.dmac_acr >> 16;
		break;
	case 0x0e:
		v = wd->cdmac.dmac_acr;
		break;
	case 0x12:
		if (wd->cdmac.dmac_dma <= 0)
			scsi_dmac_a2091_start_dma (wd);
		v = 0;
		break;
	case 0x1a:
		dmac_a2091_cint (wd);
		v = 0;
		break;;
	case 0x1e:
		v = wd->cdmac.dmac_istr;
		if (v & ISTR_INTS)
			v |= ISTR_INT_P;
		wd->cdmac.dmac_istr &= ~15;
		if (!wd->cdmac.dmac_dma)
			v |= ISTR_FE_FLG;
		break;
	case 0x3e:
		if (wd->cdmac.dmac_dma) {
			scsi_dmac_a2091_stop_dma (wd);
			wd->cdmac.dmac_istr |= ISTR_FE_FLG;
		}
		v = 0;
		break;
	case 0x40:
	case 0x48:
		v = wdscsi_getauxstatus(&wd->wc);
		break;
	case 0x42:
	case 0x46:
		v = wdscsi_get(&wd->wc, wd);
		break;
	}
#if A3000_DEBUG_IO > 1
	write_log (_T("DMAC_WREAD %08X=%04X PC=%X\n"), vaddr, v & 0xffff, M68K_GETPC);
#endif
	return v;
}

static uae_u32 mbdmac_read_byte (struct wd_state *wd, uae_u32 addr)
{
#if A3000_DEBUG_IO > 1
	uae_u32 vaddr = addr;
#endif
	uae_u32 v = 0xffffffff;

	addr &= 0xffff;
	switch (addr)
	{
	case 0x41:
	case 0x49:
		v = wdscsi_getauxstatus (&wd->wc);
		break;
	case 0x43:
	case 0x47:
		v = wdscsi_get (&wd->wc, wd);
		break;
	default:
		v = mbdmac_read_word (wd, addr);
		if (!(addr & 1))
			v >>= 8;
		break;
	}
#if A3000_DEBUG_IO > 1
	write_log (_T("DMAC_BREAD %08X=%02X PC=%X\n"), vaddr, v & 0xff, M68K_GETPC);
#endif
	return v;
}


static uae_u32 REGPARAM3 mbdmac_lget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 mbdmac_wget (uaecptr) REGPARAM;
static uae_u32 REGPARAM3 mbdmac_bget (uaecptr) REGPARAM;
static void REGPARAM3 mbdmac_lput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 mbdmac_wput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM3 mbdmac_bput (uaecptr, uae_u32) REGPARAM;

static uae_u32 REGPARAM2 mbdmac_lget (uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	v =  mbdmac_read_word (&wd_a3000, addr + 0) << 16;
	v |= mbdmac_read_word (&wd_a3000, addr + 2) << 0;
	return v;
}
static uae_u32 REGPARAM2 mbdmac_wget (uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	v =  mbdmac_read_word (&wd_a3000, addr);
	return v;
}
static uae_u32 REGPARAM2 mbdmac_bget (uaecptr addr)
{
#ifdef JIT
	special_mem |= S_READ;
#endif
	return mbdmac_read_byte (&wd_a3000, addr);
}
static void REGPARAM2 mbdmac_lput (uaecptr addr, uae_u32 l)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	if ((addr & 0xffff) == 0x40) {
		// long write to 0x40 = write byte to SASR
		mbdmac_write_byte (&wd_a3000, 0x41, l);
	} else {
		mbdmac_write_word (&wd_a3000, addr + 0, l >> 16);
		mbdmac_write_word (&wd_a3000, addr + 2, l >> 0);
	}
}
static void REGPARAM2 mbdmac_wput (uaecptr addr, uae_u32 w)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	mbdmac_write_word (&wd_a3000, addr + 0, w);
}
static void REGPARAM2 mbdmac_bput (uaecptr addr, uae_u32 b)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	mbdmac_write_byte (&wd_a3000, addr, b);
}

addrbank mbdmac_a3000_bank = {
	mbdmac_lget, mbdmac_wget, mbdmac_bget,
	mbdmac_lput, mbdmac_wput, mbdmac_bput,
	default_xlate, default_check, NULL, NULL, _T("A3000 DMAC"),
	dummy_lgeti, dummy_wgeti, ABFLAG_IO | ABFLAG_SAFE
};

static void ew (struct wd_state *wd, int addr, uae_u32 value)
{
	addr &= 0xffff;
	if (addr == 00 || addr == 02 || addr == 0x40 || addr == 0x42) {
		wd->dmacmemory[addr] = (value & 0xf0);
		wd->dmacmemory[addr + 2] = (value & 0x0f) << 4;
	} else {
		wd->dmacmemory[addr] = ~(value & 0xf0);
		wd->dmacmemory[addr + 2] = ~((value & 0x0f) << 4);
	}
}

static void *scsi_thread (void *wdv)
{
	struct wd_state *wds = (struct wd_state*)wdv;
	struct wd_chip_state *wd = &wds->wc;
	for (;;) {
		uae_u32 v = read_comm_pipe_u32_blocking (&wds->requests);
		if (wds->scsi_thread_running == 0 || v == 0xfffffff)
			break;
		int cmd = v & 0x7f;
		int msg = (v >> 8) & 0xff;
		int unit = (v >> 24) & 0xff;
		wd->scsi = wds->scsis[unit];
		//write_log (_T("scsi_thread got msg=%d cmd=%d\n"), msg, cmd);
		if (msg == 0) {
			if (WD33C93_DEBUG > 0)
				write_log (_T("%s command %02X\n"), WD33C93, cmd);
			switch (cmd)
			{
			case WD_CMD_RESET:
				wd_cmd_reset(wd, true);
				break;
			case WD_CMD_ABORT:
				wd_cmd_abort (wd);
				break;
			case WD_CMD_SEL:
				wd_cmd_sel (wd, wds, false);
				break;
			case WD_CMD_SEL_ATN:
				wd_cmd_sel (wd, wds, true);
				break;
			case WD_CMD_SEL_ATN_XFER:
				wd_cmd_sel_xfer (wd, wds, true);
				break;
			case WD_CMD_SEL_XFER:
				wd_cmd_sel_xfer (wd, wds, false);
				break;
			case WD_CMD_TRANS_INFO:
				wd_cmd_trans_info (wds, wd->scsi);
				break;
			case WD_CMD_TRANS_ADDR:
				wd_cmd_trans_addr(wd, wds);
				break;
			case WD_CMD_NEGATE_ACK:
				if (wd->wd_phase == CSR_MSGIN && wd->wd_selected)
					wd_do_transfer_in(wd, wd->scsi, false);
				break;
			default:
				wd->wd_busy = false;
				write_log (_T("%s unimplemented/unknown command %02X\n"), WD33C93, cmd);
				set_status (wd, CSR_INVALID, 10);
				break;
			}
		} else if (msg == 1) {
			wd_do_transfer_in (wd, wd->scsi, false);
		} else if (msg == 2) {
			wd_do_transfer_out (wd, wd->scsi);
		} else if (msg == 3) {
			wd_do_transfer_in (wd, wd->scsi, true);
		}
	}
	wds->scsi_thread_running = -1;
	return 0;
}

void init_wd_scsi (struct wd_state *wd)
{
	wd->configured = 0;
	wd->enabled = true;
	wd->wc.wd_used = 0;
	wd->wc.wd33c93_ver = 1;
	if (wd == &wd_cdtv) {
		wd->cdtv = true;
		wd->name = _T("CDTV");
	}
	if (!wd->scsi_thread_running) {
		wd->scsi_thread_running = 1;
		init_comm_pipe (&wd->requests, 100, 1);
		uae_start_thread (_T("scsi"), scsi_thread, wd, NULL);
	}
}

int a3000_add_scsi_unit (int ch, struct uaedev_config_info *ci)
{
	struct wd_state *wd = &wd_a3000;
	if (ci->type == UAEDEV_CD)
		return add_scsi_cd (wd->scsis, ch, ci->device_emu_unit);
	else if (ci->type == UAEDEV_TAPE)
		return add_scsi_tape (wd->scsis, ch, ci->rootdir, ci->readonly);
	else
		return add_scsi_hd (wd->scsis, ch, NULL, ci, 2);
}

void a3000scsi_reset (void)
{
	struct wd_state *wd = &wd_a3000;
	init_wd_scsi (wd);
	wd->enabled = true;
	wd->configured = -1;
	wd->dmac_type = COMMODORE_SDMAC;
	map_banks (&mbdmac_a3000_bank, 0xDD, 1, 0);
	wd_cmd_reset (&wd->wc, false);
	reset_dmac(wd);
	wd->name = _T("A3000");
}

void a3000scsi_free (void)
{
	struct wd_state *wd = &wd_a3000;
	scsi_freenative(wd->scsis);
	if (wd->scsi_thread_running > 0) {
		wd->scsi_thread_running = 0;
		write_comm_pipe_u32 (&wd->requests, 0xffffffff, 1);
		while(wd->scsi_thread_running == 0)
			sleep_millis (10);
		wd->scsi_thread_running = 0;
	}
}

int a2091_add_scsi_unit(int ch, struct uaedev_config_info *ci)
{
	struct wd_state *wd = wda2091[ci->controller_type_unit];

	if (ci->type == UAEDEV_CD)
		return add_scsi_cd(wd->scsis, ch, ci->device_emu_unit);
	else if (ci->type == UAEDEV_TAPE)
		return add_scsi_tape(wd->scsis, ch, ci->rootdir, ci->readonly);
	else
		return add_scsi_hd(wd->scsis, ch, NULL, ci, 1);
}

int gvp_add_scsi_unit(int ch, struct uaedev_config_info *ci)
{
	struct wd_state *wd = gvpscsi[ci->controller_type_unit];

	if (ci->type == UAEDEV_CD)
		return add_scsi_cd(wd->scsis, ch, ci->device_emu_unit);
	else if (ci->type == UAEDEV_TAPE)
		return add_scsi_tape(wd->scsis, ch, ci->rootdir, ci->readonly);
	else
		return add_scsi_hd(wd->scsis, ch, NULL, ci, 2);
}

void a2091_free_device (struct wd_state *wd)
{
	scsi_freenative(wd->scsis);
	xfree (wd->rom);
	wd->rom = NULL;
}
void a2091_free (void)
{
	a2091_free_device(&wd_a2091);
	a2091_free_device(&wd_a2091_2);
}

static void a2091_reset_device(struct wd_state *wd)
{
	wd->configured = 0;
	wd->wc.wd_used = 0;
	wd->wc.wd33c93_ver = 1;
	wd->dmac_type = COMMODORE_DMAC;
	wd->cdmac.old_dmac = 0;
	if (currprefs.scsi == 2)
		scsi_addnative(wd->scsis);
	wd_cmd_reset (&wd->wc, false);
	reset_dmac(wd);
	if (wd == &wd_a2091)
		wd->name = _T("A2091/A590");
	if (wd == &wd_a2091_2)
		wd->name = _T("A2091/A590 #2");
	xt_reset(wd);
}

void a2091_reset (void)
{
	a2091_reset_device(&wd_a2091);
	a2091_reset_device(&wd_a2091_2);
}

addrbank *a2091_init (int devnum)
{
	struct wd_state *wd = wda2091[devnum];
	int roms[6];
	int slotsize;
	struct romconfig *rc = NULL;

	if (devnum > 0 && !wd->enabled)
		return &expamem_null;

	init_wd_scsi(wd);
	wd->configured = 0;
	wd->autoconfig = true;
	memset (wd->dmacmemory, 0xff, sizeof wd->dmacmemory);
	ew (wd, 0x00, 0xc0 | 0x01 | 0x10);
	/* A590/A2091 hardware id */
	ew(wd, 0x04, wd->cdmac.old_dmac ? 0x02 : 0x03);
	/* commodore's manufacturer id */
	ew (wd, 0x10, 0x02);
	ew (wd, 0x14, 0x02);
	/* rom vector */
	ew (wd, 0x28, CDMAC_ROM_VECTOR >> 8);
	ew (wd, 0x2c, CDMAC_ROM_VECTOR);

	ew (wd, 0x18, 0x00); /* ser.no. Byte 0 */
	ew (wd, 0x1c, 0x00); /* ser.no. Byte 1 */
	ew (wd, 0x20, 0x00); /* ser.no. Byte 2 */
	ew (wd, 0x24, 0x00); /* ser.no. Byte 3 */

	roms[0] = 55; // 7.0
	roms[1] = 54; // 6.6
	roms[2] = 53; // 6.0
	roms[3] = 56; // guru
	roms[4] = 87;
	roms[5] = -1;

	wd->rombankswitcher = 0;
	wd->rombank = 0;
	slotsize = 65536;
	wd->rom = xcalloc (uae_u8, slotsize);
	memset(wd->rom, 0xff, slotsize);
	wd->rom_size = 16384;
	wd->rom_mask = wd->rom_size - 1;
	rc = get_device_romconfig(&currprefs, devnum, ROMTYPE_A2091);
	if (rc && !rc->autoboot_disabled) {
		if (is_device_rom(&currprefs, devnum, ROMTYPE_A2091)) {
			struct zfile *z = read_device_rom(&currprefs, devnum, ROMTYPE_A2091, roms);
			if (z) {
				write_log (_T("A590/A2091 BOOT ROM '%s'\n"), zfile_getname (z));
				wd->rom_size = zfile_size (z);
				zfile_fread (wd->rom, wd->rom_size, 1, z);
				zfile_fclose (z);
				if (wd->rom_size == 32768) {
					wd->rombankswitcher = 1;
					for (int i = wd->rom_size - 1; i >= 0; i--) {
						wd->rom[i * 2 + 0] = wd->rom[i];
						wd->rom[i * 2 + 1] = 0xff;
					}
				} else {
					for (int i = 1; i < slotsize / wd->rom_size; i++)
						memcpy (wd->rom + i * wd->rom_size, wd->rom, wd->rom_size);
				}
				wd->rom_mask = wd->rom_size - 1;
			} else {
				romwarning (roms);
			}
		}
	}
	return wd == &wd_a2091 ? &dmaca2091_bank : &dmaca2091_2_bank;
}

void gvp_free_device (struct wd_state *wd)
{
	scsi_freenative(wd->scsis);
	xfree (wd->rom);
	wd->rom = NULL;
}
void gvp_free (void)
{
	gvp_free_device(&wd_gvp);
	gvp_free_device(&wd_gvp_2);
}

static void gvp_reset_device(struct wd_state *wd)
{
	wd->configured = 0;
	wd->wc.wd_used = 0;
	wd->wc.wd33c93_ver = 1;
	wd->dmac_type = wd->gdmac.series2 ? GVP_DMAC_S2 : GVP_DMAC_S1;
	if (currprefs.scsi == 2)
		scsi_addnative(wd->scsis);
	wd_cmd_reset (&wd->wc, false);
	reset_dmac(wd);
	if (wd == &wd_gvp)
		wd->name = _T("GVP");
	if (wd == &wd_gvp_2)
		wd->name = _T("GPV #2");
}

void gvp_reset (void)
{
	gvp_reset_device(&wd_gvp);
	gvp_reset_device(&wd_gvp_2);
}

static const uae_u8 gvp_scsi_i_autoconfig[16] = { 0xd1, 0x02, 0x00, 0x00, 0x07, 0xe1, 0xee, 0xee, 0xee, 0xee, 0x80, 0x00 };
static const uae_u8 gvp_scsi_ii_autoconfig[16] = { 0xd1, 0x0b, 0x00, 0x00, 0x07, 0xe1, 0xee, 0xee, 0xee, 0xee, 0x80, 0x00 };

static bool is_gvp_accelerator(void)
{
	return currprefs.cpuboard_type == BOARD_GVP_A530 ||
		currprefs.cpuboard_type == BOARD_GVP_GFORCE_030;
}

static addrbank *gvp_init(int devnum, bool series2)
{
	struct wd_state *wd = gvpscsi[devnum];
	int roms[6];
	bool isscsi = true;
	struct romconfig *rc = NULL;

	if (devnum > 0 && !wd->enabled && currprefs.cpuboard_type != BOARD_GVP_A530)
		return &expamem_null;

	init_wd_scsi(wd);
	wd->name = _T("GVP");
	wd->configured = 0;
	wd->autoconfig = true;
	wd->rombankswitcher = 0;
	memset(wd->dmacmemory, 0xff, sizeof wd->dmacmemory);
	wd->gdmac.series2 = series2;

	roms[0] = 109;
	roms[1] = 110;
	roms[2] = 111;
	roms[3] = -1;

	wd->rom_size = 32768;
	wd->rom = xcalloc(uae_u8, wd->rom_size);
	memset(wd->rom, 0xff, wd->rom_size);
	wd->rom_mask = 32768 - 1;

	rc = get_device_romconfig(&currprefs, devnum, series2 ? ROMTYPE_GVPS2 : ROMTYPE_GVPS1);
	if (rc && !rc->autoboot_disabled) {
		if (is_device_rom(&currprefs, devnum, series2 ? ROMTYPE_GVPS2 : ROMTYPE_GVPS1)) {
			struct zfile *z = read_device_rom(&currprefs, devnum, series2 ? ROMTYPE_GVPS2 : ROMTYPE_GVPS1, roms);
			if (z) {
				write_log(_T("GVP BOOT ROM '%s'\n"), zfile_getname(z));
				int size = zfile_size(z);
				if (series2) {
					zfile_fread(wd->rom, 1, wd->rom_size, z);
				} else {
					xfree(wd->gdmac.buffer);
					wd->gdmac.buffer = xcalloc(uae_u8, GVP_SERIES_I_RAM_MASK + 1);
					for (int i = 0; i < 16384; i++) {
						uae_u8 b;
						zfile_fread(&b, 1, 1, z);
						wd->rom[i] = b;
					}
				}
				zfile_fclose(z);
				if (series2 && size > 16384) {
					wd->rombankswitcher = 1;
				}
			} else {
				isscsi = false;
				if (!is_gvp_accelerator())
					romwarning(roms);
			}
		}
	} else {
		isscsi = false;
	}

	wd->gdmac.version = GVP_SERIESII;
	wd->gdmac.addr_mask = 0x00ffffff;
	if (currprefs.cpuboard_type == BOARD_GVP_A530) {
		wd->gdmac.version = isscsi ? GVP_A530_SCSI : GVP_A530;
		wd->gdmac.addr_mask = 0x01ffffff;
	} else if (currprefs.cpuboard_type == BOARD_GVP_GFORCE_030) {
		wd->gdmac.version = isscsi ? GVP_GFORCE_030_SCSI : GVP_GFORCE_030;
		wd->gdmac.addr_mask = 0x01ffffff;
	}

	for (int i = 0; i < 16; i++) {
		uae_u8 b = wd->gdmac.series2 ? gvp_scsi_ii_autoconfig[i] : gvp_scsi_i_autoconfig[i];
		ew(wd, i * 4, b);
	}
	gvp_reset_device(wd);
	return wd == &wd_gvp ? &gvp_bank : &gvp_2_bank;
}

addrbank *gvp_init_s1(int devnum)
{
	return gvp_init(devnum, false);
}
addrbank *gvp_init_s2(int devnum)
{
	return gvp_init(devnum, true);
}
addrbank *gvp_init_accelerator(int devnum)
{
	return gvp_init(1, true);
}

uae_u8 *save_scsi_dmac (int wdtype, int *len, uae_u8 *dstptr)
{
	struct wd_state *wd = wdscsi[wdtype];
	uae_u8 *dstbak, *dst;

	if (!wd->enabled)
		return NULL;
	if (dstptr)
		dstbak = dst = dstptr;
	else
		dstbak = dst = xmalloc (uae_u8, 1000);

	// model (0=original,1=rev2,2=superdmac)
	save_u32 (currprefs.cs_mbdmac == 1 ? 2 : 1);
	save_u32 (0); // reserved flags
	save_u8(wd->cdmac.dmac_istr);
	save_u8(wd->cdmac.dmac_cntr);
	save_u32(wd->cdmac.dmac_wtc);
	save_u32(wd->cdmac.dmac_acr);
	save_u16(wd->cdmac.dmac_dawr);
	save_u32(wd->cdmac.dmac_dma ? 1 : 0);
	save_u8 (wd->configured);
	*len = dst - dstbak;
	return dstbak;
}

uae_u8 *restore_scsi_dmac (int wdtype, uae_u8 *src)
{
	struct wd_state *wd = wdscsi[wdtype];
	restore_u32 ();
	restore_u32 ();
	wd->cdmac.dmac_istr = restore_u8();
	wd->cdmac.dmac_cntr = restore_u8();
	wd->cdmac.dmac_wtc = restore_u32();
	wd->cdmac.dmac_acr = restore_u32();
	wd->cdmac.dmac_dawr = restore_u16();
	restore_u32 ();
	wd->configured = restore_u8 ();
	return src;
}

uae_u8 *save_scsi_device (int wdtype, int num, int *len, uae_u8 *dstptr)
{
	uae_u8 *dstbak, *dst;
	struct scsi_data *s;
	struct wd_state *wd = wdscsi[wdtype];

	if (!wd->enabled)
		return NULL;
	s = wd->scsis[num];
	if (!s)
		return NULL;
	if (dstptr)
		dstbak = dst = dstptr;
	else
		dstbak = dst = xmalloc (uae_u8, 1000);
	save_u32 (num);
	save_u32 (s->device_type); // flags
	switch (s->device_type)
	{
	case UAEDEV_HDF:
	case 0:
		save_u64 (s->hfd->size);
		save_string (s->hfd->hfd.ci.rootdir);
		save_u32 (s->hfd->hfd.ci.blocksize);
		save_u32 (s->hfd->hfd.ci.readonly);
		save_u32 (s->hfd->cyls);
		save_u32 (s->hfd->heads);
		save_u32 (s->hfd->secspertrack);
		save_u64 (s->hfd->hfd.virtual_size);
		save_u32 (s->hfd->hfd.ci.sectors);
		save_u32 (s->hfd->hfd.ci.surfaces);
		save_u32 (s->hfd->hfd.ci.reserved);
		save_u32 (s->hfd->hfd.ci.bootpri);
		save_u32 (s->hfd->ansi_version);
		if (num == 7) {
			save_u16(wd->cdmac.xt_cyls);
			save_u16(wd->cdmac.xt_heads);
			save_u16(wd->cdmac.xt_sectors);
			save_u8(wd->cdmac.xt_status);
			save_u8(wd->cdmac.xt_control);
		}
	break;
	case UAEDEV_CD:
		save_u32 (s->cd_emu_unit);
	break;
	case UAEDEV_TAPE:
		save_u32 (s->cd_emu_unit);
		save_u32 (s->tape->blocksize);
		save_u32 (s->tape->wp);
		save_string (s->tape->tape_dir);
	break;
	}
	*len = dst - dstbak;
	return dstbak;
}

uae_u8 *restore_scsi_device (int wdtype, uae_u8 *src)
{
	struct wd_state *wd = wdscsi[wdtype];
	int num, num2;
	struct hd_hardfiledata *hfd;
	struct scsi_data *s;
	uae_u64 size;
	uae_u32 flags;
	int blocksize, readonly;
	TCHAR *path;

	num = restore_u32 ();

	flags = restore_u32 ();
	switch (flags & 15)
	{
	case UAEDEV_HDF:
	case 0:
		hfd = xcalloc (struct hd_hardfiledata, 1);
		s = wd->scsis[num] = scsi_alloc_hd (num, hfd);
		size = restore_u64 ();
		path = restore_string ();
		_tcscpy (s->hfd->hfd.ci.rootdir, path);
		blocksize = restore_u32 ();
		readonly = restore_u32 ();
		s->hfd->cyls = restore_u32 ();
		s->hfd->heads = restore_u32 ();
		s->hfd->secspertrack = restore_u32 ();
		s->hfd->hfd.virtual_size = restore_u64 ();
		s->hfd->hfd.ci.sectors = restore_u32 ();
		s->hfd->hfd.ci.surfaces = restore_u32 ();
		s->hfd->hfd.ci.reserved = restore_u32 ();
		s->hfd->hfd.ci.bootpri = restore_u32 ();
		s->hfd->ansi_version = restore_u32 ();
		s->hfd->hfd.ci.blocksize = blocksize;
		if (num == 7) {
			wd->cdmac.xt_cyls = restore_u16();
			wd->cdmac.xt_heads = restore_u8();
			wd->cdmac.xt_sectors = restore_u8();
			wd->cdmac.xt_status = restore_u8();
			wd->cdmac.xt_control = restore_u8();
		}
		if (size)
			add_scsi_hd (wd->scsis, num, hfd, NULL, s->hfd->ansi_version);
		xfree (path);
	break;
	case UAEDEV_CD:
		num2 = restore_u32 ();
		add_scsi_cd (wd->scsis, num, num2);
	break;
	case UAEDEV_TAPE:
		num2 = restore_u32 ();
		blocksize = restore_u32 ();
		readonly = restore_u32 ();
		path = restore_string ();
		add_scsi_tape (wd->scsis, num, path, readonly != 0);
		xfree (path);
	break;
	}
	return src;
}

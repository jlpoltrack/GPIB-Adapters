/*
  STM32 port of xyphro UsbGpib TestAndMeasurement.c (MIT License, Copyright (c) 2019 Kai Gossner),
  itself based on the LUFA TestAndMeasurement demo:

             LUFA Library
     Copyright (C) Dean Camera, 2019.

  dean [at] fourwalledcubicle [dot] com
           www.lufa-lib.org

  Copyright 2019  Dean Camera (dean [at] fourwalledcubicle [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaims all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

/* USBTMC/USB488 front end on a custom TinyUSB class driver (see "TinyUSB class driver"). Bulk messages become
 * GPIB jobs (gpib_bus.h); usbtmc_task() is a state machine that never waits, so USB and GPIB both run from the main
 * loop. Control requests only set flags, and READ_STATUS_BYTE is answered once its serial poll job finishes. */

#include "usbtmc.h"
#include "instr.h"
#include "gpib_bus.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

#ifndef MIN /* the Pico SDK has its own */
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/** Capabilities in the byte layout of the original TMC_Capabilities_t (USBTMC Table 37 + USB488 Table 8) */
static const uint8_t Capabilities[24] =
	{
		TMC_STATUS_SUCCESS, 0x00,
		0x00, 0x01,    /* bcdUSBTMC 1.00 */
		0x04,          /* interface: PulseIndicateSupported */
		0x01,          /* device: SupportsAbortINOnMatch */
		0, 0, 0, 0, 0, 0,
		0x00, 0x01,    /* bcdUSB488 1.00 */
		0x07,          /* 488 interface: trigger, REN control, 488.2 */
		0x07,          /* 488 device: DT1, RL1, SR1 */
		0, 0, 0, 0, 0, 0, 0, 0,
	};

/** Current TMC control request that is being processed */
static uint8_t RequestInProgress = 0;

/** Stream callback abort flag for bulk IN data */
static volatile bool IsTMCBulkINReset = false;

/** Stream callback abort flag for bulk OUT data */
static volatile bool IsTMCBulkOUTReset = false;

/** Flag that a selective device clear should be executed */
static volatile bool handleSDC = false;

/** Flag that a local lockout should be executed */
static volatile bool handleLocalLockout = false;

/** Flag that a goto local should be executed */
static volatile bool handleGoToLocal = false;

/** Flag that a REN state countrol should be executed */
static volatile uint8_t handleRenControl = 0;

/** Flag that a ReadStatusByte should be executed */
static volatile bool    handleReadStatusByte = false;
static uint8_t handleReadStatusByte_btagvalue = 0;

/** Last used tag value for data transfers */
static uint8_t CurrentTransferTag = 0;

/** Length of last data transfer, for reporting to the host in case an in-progress transfer is aborted */
static uint16_t LastTransferLength = 0;

/** This will be set true after a indicator pulse command is received. If the next GPIB command starts with '!', a parameter has to be set */
static bool s_nextwrite_mightbeparameterset = false;

static uint32_t s_remaining_bytes_receive=0;

/** Bulk IN buffer: TMC header followed by the GPIB read data.
 *  As the message header in USBTMC indicates how much data is read and the instrument
 *  does not tell us over GPIB how much data it is, we have to buffer it in ram
 *  if the data transfer over USB should be efficient.
 */
CFG_TUSB_MEM_ALIGN static uint8_t in_buffer[1024];
static uint8_t * const readbuffer = &in_buffer[sizeof(TMC_MessageHeader_t)];
/* One word short of the buffer: a transfer ending mid-message must end on a short packet, not a full one plus a ZLP.
 * pyvisa-py waits for more data after a full last packet, and hangs if libusb already took the ZLP. Stays word aligned. */
#define READBUFFER_SIZE (sizeof(in_buffer) - sizeof(TMC_MessageHeader_t) - 4u)
_Static_assert(((sizeof(TMC_MessageHeader_t) + READBUFFER_SIZE) % TMC_IO_EPSIZE) != 0, "transfer would end on a full packet");

/**
 * When SRQ is asserted the status byte is automatically read and transfered as interrupt transfer to Host.
 * However, the Host Visa might not have events enabled and then a status byte can be lost.
 * The Visa will for that reason issue normally after that interrupt transfer a control in transfer to read
 * the status byte again. Or this is issued manually by the application.
 * The below byte is ored during control transfer to the status byte and then cleared after read to ensure
 * that no data is lost.
 */
static uint8_t srq_statusbyte = 0x00;

/* Buffer for responses from internal command queries*/
static uint8_t internal_response_buffer_rpos = 0;
static uint8_t internal_response_buffer_len = 0;
static uint8_t internal_response_buffer[128]; /* `!net` and the other `!` queries; uint8_t lengths */


/**********************************************************************************************************
 * Endpoint layer (replaces the LUFA Endpoint_* calls)
 **********************************************************************************************************/

static uint8_t s_rhport;
static uint8_t s_ep_out, s_ep_in, s_ep_int;
static volatile bool s_tmc_opened;
static volatile bool s_usb_reset;
static bool s_tmc_used; /* a USBTMC host has spoken since the last bus reset: until then SRQ is the CDC side's */

CFG_TUSB_MEM_ALIGN static uint8_t out_buffer[TMC_IO_EPSIZE];
static volatile uint16_t out_len;
static uint16_t out_rpos;
static volatile bool out_received;

CFG_TUSB_MEM_ALIGN static uint8_t notify_buffer[TMC_NOTIFICATION_EPSIZE];
static uint8_t s_notify_queue[4][TMC_NOTIFICATION_EPSIZE];
static uint8_t s_notify_head, s_notify_count;

/* static, as a deferred serial poll answers the control request after the callback returned */
static uint8_t ctrl_response[8];

static inline bool Endpoint_IsOUTReceived(void)
{
	return out_received;
}

static inline uint8_t Endpoint_Read_8(void)
{
	return (out_rpos < out_len) ? out_buffer[out_rpos++] : 0;
}

/* release the received OUT packet and arm the endpoint for the next one */
static void Endpoint_ClearOUT(void)
{
	out_received = false;
	out_rpos = 0;
	out_len = 0;
	if (s_tmc_opened && !usbd_edpt_busy(s_rhport, s_ep_out))
		usbd_edpt_xfer(s_rhport, s_ep_out, out_buffer, TMC_IO_EPSIZE, false);
}

static inline bool usb_alive(void)
{
	return s_tmc_opened && tud_mounted();
}

static void queue_notification(uint8_t b0, uint8_t b1)
{
	if (s_notify_count >= 4)
		return;
	uint8_t *n = s_notify_queue[(s_notify_head + s_notify_count++) % 4];
	n[0] = b0;
	n[1] = b1;
}

static void notify_task(void)
{
	if (!s_notify_count || usbd_edpt_busy(s_rhport, s_ep_int))
		return;
	memcpy(notify_buffer, s_notify_queue[s_notify_head], sizeof(notify_buffer));
	s_notify_head = (s_notify_head + 1) % 4;
	s_notify_count--;
	usbd_edpt_xfer(s_rhport, s_ep_int, notify_buffer, sizeof(notify_buffer), false);
}


/**********************************************************************************************************
 * Control requests
 **********************************************************************************************************/

static void put_u32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

#define REQTYPE_IN_CLASS_INTERFACE 0xA1
#define REQTYPE_IN_CLASS_ENDPOINT  0xA2

/* Replaces EVENT_USB_Device_ControlRequest. Returning false stalls the request. */
static bool TMC_ControlRequest(uint8_t rhport, tusb_control_request_t const *req)
{
	uint8_t TMCRequestStatus = TMC_STATUS_SUCCESS;

	s_tmc_used = true;
	switch (req->bRequest)
	{
		case Req_ReadStatusByte: /* answered from usbtmc_task() once the serial poll is done */
			handleReadStatusByte_btagvalue = (uint8_t)req->wValue;
			handleReadStatusByte = true;
			return true;

		case Req_InitiateAbortBulkOut:
			if (req->bmRequestType != REQTYPE_IN_CLASS_ENDPOINT)
				return false;
			/* Check that no split transaction is already in progress and the data transfer tag is valid */
			if (RequestInProgress != 0)
			{
				TMCRequestStatus = TMC_STATUS_SPLIT_IN_PROGRESS;
			}
			else if ((uint8_t)req->wValue != CurrentTransferTag)
			{
				TMCRequestStatus = TMC_STATUS_TRANSFER_NOT_IN_PROGRESS;
			}
			else
			{
				/* Save the split request for later checking when a new request is received */
				RequestInProgress = Req_InitiateAbortBulkOut;
				/* Indicate that all in-progress/pending data OUT requests should be aborted */
				IsTMCBulkOUTReset = true;
			}

			ctrl_response[0] = TMCRequestStatus;
			ctrl_response[1] = CurrentTransferTag; // address Issue #96
			return tud_control_xfer(rhport, req, ctrl_response, 2);

		case Req_CheckAbortBulkOutStatus:
			if (req->bmRequestType != REQTYPE_IN_CLASS_ENDPOINT)
				return false;
			/* Check that an ABORT BULK OUT transaction has been requested and that the request has completed */
			if (RequestInProgress != Req_InitiateAbortBulkOut)
			  TMCRequestStatus = TMC_STATUS_SPLIT_NOT_IN_PROGRESS;
			else if (IsTMCBulkOUTReset)
			  TMCRequestStatus = TMC_STATUS_PENDING;
			else
			  RequestInProgress = 0;

			memset(ctrl_response, 0, 8);
			ctrl_response[0] = TMCRequestStatus;
			put_u32_le(&ctrl_response[4], LastTransferLength);
			return tud_control_xfer(rhport, req, ctrl_response, 8);

		case Req_InitiateAbortBulkIn:
			if (req->bmRequestType != REQTYPE_IN_CLASS_ENDPOINT)
				return false;
			/* Check that no split transaction is already in progress and the data transfer tag is valid */
			if (RequestInProgress != 0)
			{
				TMCRequestStatus = TMC_STATUS_SPLIT_IN_PROGRESS;
			}
			else if ((uint8_t)req->wValue != CurrentTransferTag)
			{
				TMCRequestStatus = TMC_STATUS_TRANSFER_NOT_IN_PROGRESS;
			}
			else
			{
				/* Save the split request for later checking when a new request is received */
				RequestInProgress = Req_InitiateAbortBulkIn;
				/* Indicate that all in-progress/pending data IN requests should be aborted */
				IsTMCBulkINReset = true;
			}

			ctrl_response[0] = TMCRequestStatus;
			ctrl_response[1] = CurrentTransferTag;
			return tud_control_xfer(rhport, req, ctrl_response, 2);

		case Req_CheckAbortBulkInStatus:
			if (req->bmRequestType != REQTYPE_IN_CLASS_ENDPOINT)
				return false;
			/* Check that an ABORT BULK IN transaction has been requested and that the request has completed */
			if (RequestInProgress != Req_InitiateAbortBulkIn)
			  TMCRequestStatus = TMC_STATUS_SPLIT_NOT_IN_PROGRESS;
			else if (IsTMCBulkINReset)
			  TMCRequestStatus = TMC_STATUS_PENDING;
			else
			  RequestInProgress = 0;

			memset(ctrl_response, 0, 8);
			ctrl_response[0] = TMCRequestStatus;
			put_u32_le(&ctrl_response[4], LastTransferLength);
			return tud_control_xfer(rhport, req, ctrl_response, 8);

		case Req_InitiateClear:
			if (req->bmRequestType != REQTYPE_IN_CLASS_INTERFACE)
				return false;
			/* Check that no split transaction is already in progress */
			if (RequestInProgress != 0)
			{
				TMCRequestStatus = TMC_STATUS_SPLIT_IN_PROGRESS;
			}
			else
			{
				/* Indicate that all in-progress/pending data IN and OUT requests should be aborted */
				IsTMCBulkINReset  = true;
				IsTMCBulkOUTReset = true;
				handleSDC = true; // trigger handling of SDC command to device

				/* Save the split request for later checking when a new request is received */
				RequestInProgress = Req_InitiateClear;
			}
			ctrl_response[0] = TMCRequestStatus;
			return tud_control_xfer(rhport, req, ctrl_response, 1);

		case Req_CheckClearStatus:
			if (req->bmRequestType != REQTYPE_IN_CLASS_INTERFACE)
				return false;
			/* Check that a CLEAR transaction has been requested and that the request has completed */
			if (RequestInProgress != Req_InitiateClear)
				TMCRequestStatus = TMC_STATUS_SPLIT_NOT_IN_PROGRESS;
			else if (IsTMCBulkINReset || IsTMCBulkOUTReset || handleSDC)
				TMCRequestStatus = TMC_STATUS_PENDING;
			else
			{
				TMCRequestStatus = TMC_STATUS_SUCCESS;
				RequestInProgress = 0;
			}
			ctrl_response[0] = TMCRequestStatus;
			ctrl_response[1] = 0;
			return tud_control_xfer(rhport, req, ctrl_response, 2);

		case Req_GetCapabilities:
			if (req->bmRequestType != REQTYPE_IN_CLASS_INTERFACE)
				return false;
			return tud_control_xfer(rhport, req, (void *)(uintptr_t)Capabilities, sizeof(Capabilities));

		case Req_IndicatorPulse:
			board_led_pulse(250);
			s_nextwrite_mightbeparameterset = true;
			ctrl_response[0] = TMC_STATUS_SUCCESS;
			return tud_control_xfer(rhport, req, ctrl_response, 1);

		case Req_RenControl:
			handleRenControl = 0x80 | (((req->wValue & 0xff) == 1) ? 1 : 0);
			ctrl_response[0] = TMC_STATUS_SUCCESS;
			return tud_control_xfer(rhport, req, ctrl_response, 1);

		case Req_LocalLockout:
			handleLocalLockout = true; // trigger handling of local lockout within TMC_TASK
			ctrl_response[0] = TMC_STATUS_SUCCESS;
			return tud_control_xfer(rhport, req, ctrl_response, 1);

		case Req_GoToLocal:
			handleGoToLocal = true; // trigger handling of local lockout within TMC_TASK
			ctrl_response[0] = TMC_STATUS_SUCCESS;
			return tud_control_xfer(rhport, req, ctrl_response, 1);

		default:
			return false;
	}
}

/**********************************************************************************************************
 * Internal ('!') commands: see instr.c. Length counts the bytes after the '!'.
 **********************************************************************************************************/

static void ProcessInternalCommand(uint8_t Length)
{
	internal_response_buffer_rpos = 0;
	internal_response_buffer_len = instr_command(&out_buffer[out_rpos], MIN(Length, (uint16_t)(out_len - out_rpos)),
	                                             internal_response_buffer, sizeof(internal_response_buffer));
	out_rpos = out_len;
}


/**********************************************************************************************************
 * Bulk transfers
 **********************************************************************************************************/

static enum {
	TMC_IDLE,
	TMC_OUT_JOB,    /* writing an OUT packet's payload to GPIB */
	TMC_OUT_NEXT,   /* waiting for the next OUT packet of a DEV_DEP_MSG_OUT */
	TMC_IN_JOB,     /* reading GPIB for a DEV_DEP_MSG_IN */
	TMC_IN_SEND,    /* response on bulk IN */
	TMC_CTRL_JOB,   /* trigger, clear, local, SRQ poll */
	TMC_ZLP,        /* retire the host's bulk-IN request after an abort */
} s_state;

static enum { CTRL_SRQ, CTRL_SDC, CTRL_CLEAR, CTRL_LLO, CTRL_GTL, CTRL_TRIGGER } s_ctrl;

static gpib_job_t s_job;        /* bulk transfers and the control actions */
static gpib_job_t s_spoll_job;  /* READ_STATUS_BYTE, cuts in as a priority job */
static uint8_t    s_status_byte, s_spoll_byte;
static bool       s_rstb_running;
static bool       s_zlp_after_clear;
static bool       s_in_queued, s_in_zlp;
static uint32_t   s_srq_holdoff;
static TMC_MessageHeader_t s_in_header;

bool TMC_LastMessageComplete = true;
bool TMC_eom;
bool TMC_InLastMessageComplete = true;

static void TMC_resetstates(void)
{
	internal_response_buffer_len = 0;
	handleRenControl = 0;
	handleGoToLocal = false;
	handleSDC = false;
	handleLocalLockout = false;
	TMC_LastMessageComplete = true;
	TMC_InLastMessageComplete = true;
	s_remaining_bytes_receive = 0;
	s_rstb_running = false; /* gpib_ifc() aborts its job: rstb_task() polls again if still requested */
	board_led_busy(false);

	gpib_ifc();
}

static void submit(uint8_t op, uint16_t timeout_ms)
{
	s_job = (gpib_job_t){ .op = op, .addr = gpib_addr, .buf = &s_status_byte, .timeout_ms = timeout_ms };
	gpib_ren(true); /* ensure that remote control is enabled */
	gpib_submit(&s_job, GPIB_OWNER_TMC);
}

static void start_ctrl(uint8_t op, uint8_t kind)
{
	submit(op, 500);
	s_ctrl = kind;
	s_state = TMC_CTRL_JOB;
}

static void start_out(uint8_t *payload, uint16_t len, bool last, bool eom)
{
	uint16_t avail = (uint16_t)(out_len - (payload - out_buffer));
	len = MIN(len, avail); /* a short packet carries less than the header announced */
	s_job = (gpib_job_t){ .op = GPIB_OP_WRITE, .addr = gpib_addr, .buf = payload, .len = len,
	                      .flags = (eom ? GPIB_EOI : 0) | (last ? GPIB_END : 0) };
	gpib_ren(true);
	gpib_submit(&s_job, GPIB_OWNER_TMC);
	board_led_busy(true);
	s_state = TMC_OUT_JOB;
}

static void start_in_send(uint16_t len, bool last)
{
	s_in_header.TransferSize = len;
	s_in_header.MessageIDSpecific.DeviceIN.LastMessageTransaction = last;
	s_in_header.MessageIDSpecific.DeviceIN.TermChar = 0;
	WriteTMCHeader(&s_in_header);
	s_in_queued = false;
	s_in_zlp = false;
	s_state = TMC_IN_SEND;
}

static void handle_out_header(TMC_MessageHeader_t *h)
{
	bool lastmessage;
	uint8_t curlen;

	s_remaining_bytes_receive = h->TransferSize;
	curlen = MIN(TMC_IO_EPSIZE-sizeof(TMC_MessageHeader_t), h->TransferSize);
	s_remaining_bytes_receive -= curlen;

	TMC_eom = (h->MessageIDSpecific.DeviceOUT.LastMessageTransaction != 0);
	lastmessage = (s_remaining_bytes_receive==0);

	/* check, if this is an internal command (=sidechannel for configuration settings) */
	bool internal = TMC_LastMessageComplete && s_nextwrite_mightbeparameterset && (curlen > 0) && (out_buffer[out_rpos] == '!');
	s_nextwrite_mightbeparameterset = false;
	TMC_LastMessageComplete = lastmessage;
	if (internal)
	{
		out_rpos++;
		ProcessInternalCommand(curlen - 1);
		Endpoint_ClearOUT();
		s_state = lastmessage ? TMC_IDLE : TMC_OUT_NEXT;
		return;
	}
	start_out(&out_buffer[out_rpos], curlen, lastmessage, TMC_eom && lastmessage);
}

static void handle_in_header(TMC_MessageHeader_t *h)
{
	uint16_t curlen16;

	Endpoint_ClearOUT();
	s_in_header = *h;

	curlen16 = READBUFFER_SIZE;
	if (curlen16 > h->TransferSize)
		curlen16 = h->TransferSize;

	/* New additional method to set termination. Pyvisa example:
	   dev is here an opened device ressource)
		  dev.set_visa_attribute(visa.constants.VI_ATTR_TERMCHAR_EN, True)
		  dev.set_visa_attribute(visa.constants.VI_ATTR_TERMCHAR, ord('\r') ) # pass \0 for eoi, \n for \n or eoi and \r for \r or eoi. All other options will use EOI only termination
	*/
	/* The configured termination (instr_get_readterm(): '!term', stored setting); a TermChar overrides it for this read */
	char term = instr_get_readterm();
	bool term_enabled = (term == '\n') || (term == '\r');
	if (h->MessageIDSpecific.DeviceIN.LastMessageTransaction & 0x02) // the read termination character is set as part of this read transaction request
	{
		term = (char)h->MessageIDSpecific.DeviceIN.TermChar;
		term_enabled = true;
	}

	/* Check if a response from an internal query is in the buffer */
	if (internal_response_buffer_len)
	{ // internal response present
		// Add response to buffers... This might look too complicated, but handles also partial reads properly (e.g. responses could be read byte by byte)
		curlen16 = internal_response_buffer_len-internal_response_buffer_rpos; // count of unsent bytes from internal response buffer
		if (curlen16 > h->TransferSize)
			curlen16 = h->TransferSize;

		memcpy(readbuffer, &(internal_response_buffer[internal_response_buffer_rpos]), curlen16);

		internal_response_buffer_rpos += curlen16;
		bool lastmessage = (internal_response_buffer_rpos >= internal_response_buffer_len);
		if (lastmessage)
		{
			internal_response_buffer_len = 0; // Mark internal response as "sent"
			internal_response_buffer_rpos = 0;
		}
		TMC_InLastMessageComplete = lastmessage;
		start_in_send(curlen16, lastmessage);
		return;
	}

	// no internal response present, read from device!
	s_job = (gpib_job_t){ .op = GPIB_OP_READ, .addr = gpib_addr, .buf = readbuffer, .len = curlen16,
	                      .flags = term_enabled ? GPIB_TERM : 0, .term = (uint8_t)term };
	gpib_ren(true);
	gpib_submit(&s_job, GPIB_OWNER_TMC);
	board_led_busy(true);
	s_state = TMC_IN_JOB;
}

static void ctrl_job_done(void)
{
	switch (s_ctrl)
	{
		case CTRL_SRQ: /* push the status byte over the interrupt channel */
		{
			uint8_t raw = (s_job.status == GPIB_OK) ? s_status_byte : 0;
			uint8_t statusReg = raw | 0x40;
			srq_statusbyte |= (statusReg & ~0x40);
			queue_notification(0x80 | 0x01, statusReg);
			if (!(raw & 0x40)) /* not ours (or no answer): don't poll back to back */
				s_srq_holdoff = board_millis() + 100;
			break;
		}
		case CTRL_SDC:
			handleSDC = false;
			break;
		case CTRL_CLEAR:
			TMC_resetstates();
			if (s_zlp_after_clear)
			{
				s_state = TMC_ZLP;
				return;
			}
			break;
		case CTRL_LLO:
			handleLocalLockout = false;
			break;
		case CTRL_GTL:
			handleGoToLocal = false;
			break;
		case CTRL_TRIGGER:
			Endpoint_ClearOUT();
			break;
	}
	s_state = TMC_IDLE;
}

static void tmc_idle(void)
{
	TMC_MessageHeader_t MessageHeader;

	/* handle service request (SRQ line goes down): read the status byte and push it as an interrupt transfer */
	if (s_tmc_used && (gpib_last_owner() == GPIB_OWNER_TMC) && gpib_srq() && ((int32_t)(board_millis() - s_srq_holdoff) >= 0))
	{
		submit(GPIB_OP_SPOLL, 500);
		s_ctrl = CTRL_SRQ;
		s_state = TMC_CTRL_JOB;
		return;
	}

	/* handle actions triggered by control transfer in a synchronous manner here */
	if (handleSDC)
	{
		start_ctrl(GPIB_OP_SDC, CTRL_SDC);
		return;
	}
	if (handleLocalLockout)
	{
		start_ctrl(GPIB_OP_LLO, CTRL_LLO);
		return;
	}
	if (TMC_LastMessageComplete && TMC_InLastMessageComplete && !Endpoint_IsOUTReceived())
	{ // Issue report from Michael. When executing a long write transfer and then gotolocal, goto local does not happen unless a delay is placed inbetween
		if (handleGoToLocal)
		{
			start_ctrl(GPIB_OP_GTL, CTRL_GTL);
			return;
		}
		if (handleRenControl)
		{
			gpib_ren(handleRenControl & 1);
			handleRenControl = 0;
		}
	}

	/* Try to read in a TMC message from the interface, process if one is available */
	if (!ReadTMCHeader(&MessageHeader))
		return;
	s_tmc_used = true;
	switch (MessageHeader.MessageID)
	{
		case TMC_MESSAGEID_TRIGGER:
			start_ctrl(GPIB_OP_GET, CTRL_TRIGGER);
			break;
		case TMC_MESSAGEID_DEV_DEP_MSG_OUT:
			handle_out_header(&MessageHeader);
			break;
		case TMC_MESSAGEID_DEV_DEP_MSG_IN:
			handle_in_header(&MessageHeader);
			break;
		default:
			usbd_edpt_stall(s_rhport, s_ep_out); /* re-armed when the host clears the halt */
			out_received = false;
			break;
	}
}

/* READ_STATUS_BYTE: serial poll as a priority job, then the control reply and the interrupt notification */
static void rstb_task(void)
{
	if (handleReadStatusByte && !s_rstb_running)
	{
		s_spoll_job = (gpib_job_t){ .op = GPIB_OP_SPOLL, .addr = gpib_addr, .buf = &s_spoll_byte, .timeout_ms = 1500 };
		gpib_ren(true); /* ensure that remote control is enabled */
		s_rstb_running = gpib_submit_priority(&s_spoll_job);
		return;
	}
	if (!handleReadStatusByte || (s_rstb_running && (s_spoll_job.status == GPIB_PENDING)))
		return;

	uint8_t btag = handleReadStatusByte_btagvalue;
	uint8_t statusReg = (s_rstb_running && (s_spoll_job.status == GPIB_OK)) ? s_spoll_byte : 0;
	statusReg |= srq_statusbyte; // or previously read autoread status byte (Visa will issue a read status byte after receiving a SRQ interrupt transfer)
	srq_statusbyte = 0x00; // clear previously read autoread status byte
	s_rstb_running = false;
	handleReadStatusByte = false;

	/* TinyUSB still holds the deferred request */
	ctrl_response[0] = TMC_STATUS_SUCCESS;
	ctrl_response[1] = btag;
	ctrl_response[2] = statusReg;
	tud_control_xfer(s_rhport, NULL, ctrl_response, 3);
	queue_notification(btag | 0x80, statusReg);
}

/* Host abort or clear: stop the GPIB job, then (for INITIATE_CLEAR) send SDC before resetting */
static void tmc_abort(void)
{
	bool in_pending = (s_state == TMC_IN_JOB) || (s_zlp_after_clear && (s_state == TMC_CTRL_JOB));

	gpib_abort(&s_job);
	board_led_busy(false);
	if (Endpoint_IsOUTReceived())
		Endpoint_ClearOUT();
	IsTMCBulkINReset = false;
	IsTMCBulkOUTReset = false;
	s_remaining_bytes_receive = 0;

	if (handleSDC)
	{ /* SDC has to be sent before clearing state, because SDC request is stopped in reset states */
		s_zlp_after_clear = in_pending;
		start_ctrl(GPIB_OP_SDC, CTRL_CLEAR);
		return;
	}
	TMC_resetstates();
	s_state = in_pending ? TMC_ZLP : TMC_IDLE;
}

/** Function to manage TMC data transmission and reception to and from the host. */
void usbtmc_task(void)
{
	/* Device must be connected and configured for the task to run */
	if (s_usb_reset || !usb_alive())
	{
		if (s_usb_reset || (s_state != TMC_IDLE))
		{
			s_usb_reset = false;
			RequestInProgress = 0; /* a host gone mid abort/clear must not block the next one */
			handleReadStatusByte = false;
			TMC_resetstates();
			s_state = TMC_IDLE;
		}
		return;
	}

	notify_task();
	rstb_task();

	if (IsTMCBulkOUTReset || IsTMCBulkINReset)
	{
		tmc_abort();
		return;
	}

	switch (s_state)
	{
		case TMC_IDLE:
			tmc_idle();
			break;

		case TMC_OUT_JOB:
			if (s_job.status == GPIB_PENDING)
				break;
			board_led_busy(false);
			Endpoint_ClearOUT();
			s_state = s_remaining_bytes_receive ? TMC_OUT_NEXT : TMC_IDLE;
			break;

		case TMC_OUT_NEXT: /* receiving further bytes to be sent over GPIB */
			if (Endpoint_IsOUTReceived())
			{
				LastTransferLength = 0;
				uint8_t curlen = MIN(TMC_IO_EPSIZE, s_remaining_bytes_receive);
				s_remaining_bytes_receive -= curlen;
				bool lastmessage = (s_remaining_bytes_receive==0);
				TMC_LastMessageComplete = lastmessage;
				start_out(out_buffer, curlen, lastmessage, TMC_eom && lastmessage);
			}
			break;

		case TMC_IN_JOB:
			if (s_job.status == GPIB_PENDING)
				break;
			board_led_busy(false);
			{ /* a failed read ends the message */
				bool lastmessage = s_job.end || (s_job.status != GPIB_OK);
				TMC_InLastMessageComplete = lastmessage;
				start_in_send(s_job.done, lastmessage);
			}
			break;

		case TMC_IN_SEND:
			if (usbd_edpt_busy(s_rhport, s_ep_in))
				break;
			if (!s_in_queued)
			{
				uint16_t total = sizeof(TMC_MessageHeader_t) + s_in_header.TransferSize;
				usbd_edpt_xfer(s_rhport, s_ep_in, in_buffer, total, false);
				s_in_queued = true;
				s_in_zlp = ((total % TMC_IO_EPSIZE) == 0); /* the transfer ends with a full packet */
			}
			else if (s_in_zlp)
			{
				usbd_edpt_xfer(s_rhport, s_ep_in, NULL, 0, false);
				s_in_zlp = false;
			}
			else
			{
				s_state = TMC_IDLE;
			}
			break;

		case TMC_CTRL_JOB:
			if (s_job.status != GPIB_PENDING)
				ctrl_job_done();
			break;

		case TMC_ZLP: /* the host does not retire an aborted bulk-IN request, so answer it with an empty packet */
			if (!usbd_edpt_busy(s_rhport, s_ep_in))
			{
				usbd_edpt_xfer(s_rhport, s_ep_in, NULL, 0, false);
				s_state = TMC_IDLE;
			}
			break;
	}
}

/** Attempts to read in the TMC message header from the TMC interface.
 *
 *  \param[out] MessageHeader  Pointer to a location where the read header (if any) should be stored
 *
 *  \return Boolean \c true if a header was read, \c false otherwise
 */
bool ReadTMCHeader(TMC_MessageHeader_t* const MessageHeader)
{
	/* Abort if no command has been sent from the host */
	if (!(Endpoint_IsOUTReceived()))
	  return false;

	if (out_len < sizeof(TMC_MessageHeader_t))
	{ /* runt packet, cannot be a valid header */
		Endpoint_ClearOUT();
		return false;
	}

	memcpy(MessageHeader, out_buffer, sizeof(TMC_MessageHeader_t));
	out_rpos = sizeof(TMC_MessageHeader_t);

	/* Store the new command tag value for later use */
	CurrentTransferTag = MessageHeader->Tag;

	/* Indicate if the command has been aborted or not */
	return !IsTMCBulkOUTReset;
}

/** Places the response header in front of the read data in the bulk IN buffer */
bool WriteTMCHeader(TMC_MessageHeader_t* const MessageHeader)
{
	/* Set the message tag of the command header */
	MessageHeader->Tag        =  CurrentTransferTag;
	MessageHeader->InverseTag = ~CurrentTransferTag;

	memcpy(in_buffer, MessageHeader, sizeof(TMC_MessageHeader_t));

	/* Indicate if the command has been aborted or not */
	return !IsTMCBulkINReset;
}

/**********************************************************************************************************
 * TinyUSB class driver
 **********************************************************************************************************/

static void tmc_drv_init(void)
{
}

static bool tmc_drv_deinit(void)
{
	return true;
}

static void tmc_drv_reset(uint8_t rhport)
{
	(void)rhport;
	s_tmc_opened = false;
	s_tmc_used = false;
	s_usb_reset = true;
	out_received = false;
	handleReadStatusByte = false;
	s_notify_count = 0;
	s_ep_out = s_ep_in = s_ep_int = 0;
}

static uint16_t tmc_drv_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
	TU_VERIFY(itf->bInterfaceClass == TUSB_CLASS_APPLICATION_SPECIFIC && itf->bInterfaceSubClass == 0x03, 0);

	uint8_t const *p = (uint8_t const *)itf;
	uint16_t len = tu_desc_len(p);
	p = tu_desc_next(p);

	for (uint8_t i = 0; i < itf->bNumEndpoints; i++)
	{
		TU_VERIFY(len + tu_desc_len(p) <= max_len, 0);
		TU_ASSERT(tu_desc_type(p) == TUSB_DESC_ENDPOINT, 0);
		tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
		TU_ASSERT(usbd_edpt_open(rhport, ep), 0);

		if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT)
			s_ep_int = ep->bEndpointAddress;
		else if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN)
			s_ep_in = ep->bEndpointAddress;
		else
			s_ep_out = ep->bEndpointAddress;

		len += tu_desc_len(p);
		p = tu_desc_next(p);
	}

	s_rhport = rhport;
	s_tmc_opened = true;
	Endpoint_ClearOUT();
	return len;
}

static bool tmc_drv_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
	if (stage != CONTROL_STAGE_SETUP)
		return true;

	if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD)
	{ /* CLEAR_FEATURE(ENDPOINT_HALT): TinyUSB already cleared it, which also cancels a queued OUT transfer */
		if ((request->bRequest == TUSB_REQ_CLEAR_FEATURE) && (tu_u16_low(request->wIndex) == s_ep_out))
		{
			out_received = false;
			Endpoint_ClearOUT();
		}
		return true;
	}

	if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS)
		return false;

	return TMC_ControlRequest(rhport, request);
}

static bool tmc_drv_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
	(void)rhport;
	if ((ep_addr == s_ep_out) && (result == XFER_RESULT_SUCCESS))
	{
		out_len = (uint16_t)xferred_bytes;
		out_rpos = 0;
		out_received = true;
	}
	return true;
}

static usbd_class_driver_t const tmc_driver =
{
	.name            = "USBTMC-GPIB",
	.init            = tmc_drv_init,
	.deinit          = tmc_drv_deinit,
	.reset           = tmc_drv_reset,
	.open            = tmc_drv_open,
	.control_xfer_cb = tmc_drv_control_xfer_cb,
	.xfer_cb         = tmc_drv_xfer_cb,
	.xfer_isr        = NULL,
	.sof             = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
	*driver_count = 1;
	return &tmc_driver;
}

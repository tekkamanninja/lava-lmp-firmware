/*
 * lava-lmp firmware: init
 *
 * This originally came from the NXP example code for CDC ACM
 * http://www.lpcware.com/content/forum/usb-rom-driver-examples-using-lpcxpresso-lpc11uxx#comment-2526
 *
 * I stripped it down and modified it considerably.  My changes are -->
 *
 * Copyright (C) 2012 Linaro Ltd
 * Author: Andy Green <andy.green@linaro.org>
 *
 * Licensed under LGPL2
 *
 * The original copyright notice is retained below
 */


/***********************************************************************
*   Copyright(C) 2011, NXP Semiconductor
*   All rights reserved.
*
* Software that is described herein is for illustrative purposes only
* which provides customers with programming information regarding the
* products. This software is supplied "AS IS" without any warranties.
* NXP Semiconductors assumes no responsibility or liability for the
* use of the software, conveys no license or title under any patent,
* copyright, or mask work right to the product. NXP Semiconductors
* reserves the right to make changes in the software without
* notification. NXP Semiconductors also make no representation or
* warranty that such application will be suitable for the specified
* use without further testing or modification.
**********************************************************************/
#include <string.h>
#include "LPC11Uxx.h"            
#include <power_api.h>
#include "mw_usbd_rom_api.h"
#include "lava-lmp.h"

extern uint8_t VCOM_DeviceDescriptor[];
extern uint8_t VCOM_ConfigDescriptor[];

static USBD_API_T *usbapi;

extern void lmp_parse(const unsigned char *buf, int len);

#define _(c) c, 0

/* This is the static part of the USB string descriptors */

const uint8_t VCOM_StringDescriptor[] = {

	/* Index 0x00: LANGID Codes */
	0x04,                              /* bLength */
	USB_STRING_DESCRIPTOR_TYPE,        /* bDescriptorType */
	WBVAL(0x0409), /* US English */    /* wLANGID */

	/* Index 0x01: Manufacturer */
	(10 * 2 + 2),                        /* bLength (3 Char + Type + len) */
	USB_STRING_DESCRIPTOR_TYPE,        /* bDescriptorType */
	_('L'), _('i'), _('n'), _('a'), _('r'), _('o'), _(' '),
						_('L'), _('t'), _('d'), 

	/* Index 0x02: Product */
	(7 * 2 + 2),                        /* bLength (3 Char + Type + len) */
	USB_STRING_DESCRIPTOR_TYPE,        /* bDescriptorType */
	_('L'), _('a'), _('v'), _('a'), _('L'), _('M'), _('P'), 

	/* Index 0x03: Interface 0, Alternate Setting 0 */
	(4 * 2 + 2),			/* bLength (4 Char + Type + len) */
	USB_STRING_DESCRIPTOR_TYPE,	/* bDescriptorType */\
	_('V'), _('C'), _('O'), _('M'),

	/* Index 0x04: Serial Number */
	(16 * 2 + 2),			/* bLength (16 Char + Type + len) */
	USB_STRING_DESCRIPTOR_TYPE,	/* bDescriptorType */

	/* 
	 * add 16 x 2-byte wide chars here in copied version
	 * for effective serial
	 */
};

const uint8_t VCOM_UnsetSerial[] = {
	_('U'), _('n'), _('s'), _('e'), _('t'), _(' '), _('S'), _('e'),
	_('r'), _('i'), _('a'), _('l'), _(' '), _('N'), _('u'), _('m'),
};

#define RX_BUFS 4

/* this state and buffers go into the middle of the "USB RAM" */

struct vcom_data {
	unsigned char string_descriptor[sizeof(VCOM_StringDescriptor) +
		(USB_SERIAL_NUMBER_CHARS * 2)] __attribute__ ((aligned(4)));
	USBD_HANDLE_T hUsb;
	USBD_HANDLE_T hCdc;
	unsigned char rxBuf[RX_BUFS][USB_HS_MAX_BULK_PACKET + 1];
	unsigned char txBuf[USB_HS_MAX_BULK_PACKET];
	volatile uint8_t rx_next_write;
	volatile uint8_t rx_next_read;
	volatile uint8_t rxlen[RX_BUFS];
	volatile uint8_t txlen;
	volatile uint8_t pend_tx;
	volatile uint8_t pend_rx;
	uint8_t _stuff;
};

struct vcom_data _vcom;
struct vcom_data * const vcom = &_vcom;
char ascii_serial[USB_SERIAL_NUMBER_CHARS + 1];
char flash_led;

static USBD_API_INIT_PARAM_T usb_param = {
	.usb_reg_base = LPC_USB_BASE,
	.mem_base = 0x10001000,
	.mem_size = 0x640,
	.max_num_ep = 3,
};

static const USB_CORE_DESCS_T desc = {
	.device_desc = VCOM_DeviceDescriptor,
	.string_desc = (unsigned char *)&_vcom.string_descriptor,
	.full_speed_desc = VCOM_ConfigDescriptor,
	.high_speed_desc = VCOM_ConfigDescriptor,
};

static USBD_CDC_INIT_PARAM_T cdc_param = {
	.mem_base = 0x10001640,
	.mem_size = 0x1c0,
	.cif_intf_desc = &VCOM_ConfigDescriptor[USB_CONFIGUARTION_DESC_SIZE],
	.dif_intf_desc = &VCOM_ConfigDescriptor[USB_CONFIGUARTION_DESC_SIZE +
		USB_INTERFACE_DESC_SIZE + 0x0013 + USB_ENDPOINT_DESC_SIZE],
};

static void USB_pin_clk_init(void)
{
	/* Enable AHB clock to the GPIO domain. */
	LPC_SYSCON->SYSAHBCLKCTRL |= 1 << 6;

	/* Enable AHB clock to the USB block and USB RAM. */
	LPC_SYSCON->SYSAHBCLKCTRL |= (1 << 14) | (1 << 27);

	LPC_IOCON->PIO0_3 = 1 << 0; /* Secondary function VBUS */
	LPC_IOCON->PIO0_6 = 1 << 0; /* Secondary function SoftConn */

	LPC_GPIO->DIR[0] |= 1 << 6;
	LPC_GPIO->CLR[0] = 1 << 6;
}

void usb_queue_tx(const unsigned char *buf, int len)
{
	while (vcom->txlen)
		;

	NVIC_DisableIRQ(USB_IRQn);

	memcpy(vcom->txBuf, buf, len);
	vcom->txlen = len;

	/* not expecting any "in" IRQ to send anything, do it ourselves */
	if (!vcom->pend_tx) {
		vcom->txlen -= usbapi->hw->WriteEP(vcom->hUsb,
					  USB_CDC_EP_BULK_IN, vcom->txBuf, len);
		vcom->pend_tx = 1;
	}

	/* a pending "in" IRQ should happen soon and send buffered stuff */
	NVIC_EnableIRQ(USB_IRQn);
}

void usb_queue_string(const char *buf)
{
	int n = strlen(buf);
	int m;

	while (n) {
		m = n;
		if (m > 30)
			m = 30;
		usb_queue_tx((unsigned char *)buf, m);
		buf += m;
		n -= m;
	}
}

/*
 * we just sent something back to host, if there's more waiting send it now
 */

static ErrorCode_t VCOM_in(USBD_HANDLE_T hUsb, void *data, uint32_t event) 
{
	if (event != USB_EVT_IN)
		return LPC_OK;

	if (!vcom->txlen) {
		vcom->pend_tx = 0;
		return LPC_OK;
	}

	vcom->pend_tx = 1;
	vcom->txlen -= usbapi->hw->WriteEP(hUsb, USB_CDC_EP_BULK_IN,
						     vcom->txBuf, vcom->txlen);
	return LPC_OK;
}

/*
 * something has arrived from the host
 */

static ErrorCode_t VCOM_out(USBD_HANDLE_T hUsb, void *data, uint32_t event) 
{
	if (event != USB_EVT_OUT)
		return LPC_OK;

	if (vcom->rxlen[vcom->rx_next_write] || vcom->pend_rx) {
		/* can't cope with it right now, foreground will get it later */
		vcom->pend_rx = 1;

		return LPC_OK;
	}

	vcom->rxlen[vcom->rx_next_write] =
		usbapi->hw->ReadEP(hUsb, USB_CDC_EP_BULK_OUT,
				vcom->rxBuf[vcom->rx_next_write]);

	if (vcom->rx_next_write == RX_BUFS - 1)
		vcom->rx_next_write = 0;
	else
		vcom->rx_next_write++;

	return LPC_OK;
}

void USB_IRQHandler(void)
{
       usbapi->hw->ISR(vcom->hUsb);
}

int main(void)
{
	unsigned char *p = &_vcom.string_descriptor[sizeof VCOM_StringDescriptor];
	int n;
	static unsigned short q;

	SystemInit();
	SystemCoreClockUpdate();
	lava_lmp_pin_init();
	USB_pin_clk_init();

	/* synthesize custom string descriptor using serial from EEPROM */
	memcpy(&vcom->string_descriptor, VCOM_StringDescriptor,
						  sizeof VCOM_StringDescriptor);
	lava_lmp_eeprom(EEPROM_RESERVED_OFFSET, EEPROM_READ, p,
			USB_SERIAL_NUMBER_CHARS * 2);

	if (*p == 0xff || *p == 0x00)
		memcpy(p, VCOM_UnsetSerial, sizeof(VCOM_UnsetSerial));

	for (n = 0; n < USB_SERIAL_NUMBER_CHARS; n++)
		ascii_serial[n] = p[n * 2];
	ascii_serial[USB_SERIAL_NUMBER_CHARS] = '\0';

	usbapi = (USBD_API_T *)((*(ROM **)(0x1FFF1FF8))->pUSBD);
	if (usbapi->hw->Init(&vcom->hUsb, (USB_CORE_DESCS_T *)&desc, &usb_param))
		goto spin;
	if (usbapi->cdc->init(vcom->hUsb, &cdc_param, &vcom->hCdc))
		goto spin;
	usbapi->core->RegisterEpHandler(vcom->hUsb,
			((USB_CDC_EP_BULK_IN & 0xf) << 1) + 1, VCOM_in, vcom);
	usbapi->core->RegisterEpHandler(vcom->hUsb,
			(USB_CDC_EP_BULK_OUT & 0xf) << 1, VCOM_out, vcom);

	vcom->rx_next_read = 0;
	vcom->rx_next_write = 0;

	NVIC_EnableIRQ(USB_IRQn); /* enable USB0 IRQ */
	usbapi->hw->Connect(vcom->hUsb, 1); /* USB Connect */

	/* foreground code feeds board-specific JSON parser */

	while (1) {
		if (!vcom->rxlen[vcom->rx_next_read]) {
			if (!vcom->pend_rx) {
				if (flash_led && !(q++ & 0x7fff)) {
					if (q & 0x8000)
						LPC_GPIO->SET[0] = 1 << 2;
					else
						LPC_GPIO->CLR[0] = 1 << 2;
				}


				if (idle_ok)
					lmp_json_callback_board(NULL, -1);
				continue;
			}
			NVIC_DisableIRQ(USB_IRQn);
			vcom->rxlen[vcom->rx_next_write] = usbapi->hw->ReadEP(
				vcom->hUsb, USB_CDC_EP_BULK_OUT, vcom->rxBuf[vcom->rx_next_write]);
			if (vcom->rx_next_write == RX_BUFS - 1)
				vcom->rx_next_write = 0;
			else
				vcom->rx_next_write++;
			vcom->pend_rx = 0;
			NVIC_EnableIRQ(USB_IRQn);
		}
		lmp_parse(vcom->rxBuf[vcom->rx_next_read], vcom->rxlen[vcom->rx_next_read]);
		NVIC_DisableIRQ(USB_IRQn);
		vcom->rxlen[vcom->rx_next_read] = 0;
		if (vcom->rx_next_read == RX_BUFS - 1)
			vcom->rx_next_read = 0;
		else
			vcom->rx_next_read++;
		NVIC_EnableIRQ(USB_IRQn);
	}

spin:
	while (1) {
		LPC_GPIO->SET[1] = 1 << 20;
		LPC_GPIO->CLR[1] = 1 << 20;
	}
}

void _exit(int a)
{
	while (1)
		;
}


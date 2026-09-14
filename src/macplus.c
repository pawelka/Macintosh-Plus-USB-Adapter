/********************************** (C) COPYRIGHT *******************************
* File Name          : Derived from USBHostHUB_KM.C -> nonquad.c -> macplus.c
* Author             : pkarpins/JJM/WCH
* Version            : V1.0
* Date               : 2018/07/24 (WCH) / 2021/02/06 (JJM) / 2024/06/04 (pkarpins)
*******************************************************************************/

#include <ch554.h>
#include <debug.h>
#include "usbhost.h"
#include <ch554_usb.h>
#include <stdio.h>
#include <string.h>

#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0
#endif

#if DEBUG_SERIAL
#include "small_print.h"
#else
#define printstr(a)
#define printhex2(a)
#define printx2(a)
#define printhex4(a)
#define printlf()
#define putch(a)
#endif

__code uint8_t  SetupGetDevDescr[] = { USB_REQ_TYP_IN, USB_GET_DESCRIPTOR, 0x00, USB_DESCR_TYP_DEVICE, 0x00, 0x00, sizeof( USB_DEV_DESCR ), 0x00 };
__code uint8_t  SetupGetCfgDescr[] = { USB_REQ_TYP_IN, USB_GET_DESCRIPTOR, 0x00, USB_DESCR_TYP_CONFIG, 0x00, 0x00, 0x04, 0x00 };
__code uint8_t  SetupSetUsbAddr[] = { USB_REQ_TYP_OUT, USB_SET_ADDRESS, USB_DEVICE_ADDR, 0x00, 0x00, 0x00, 0x00, 0x00 };
__code uint8_t  SetupSetUsbConfig[] = { USB_REQ_TYP_OUT, USB_SET_CONFIGURATION, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
__code uint8_t  SetupSetUsbInterface[] = { USB_REQ_RECIP_INTERF, USB_SET_INTERFACE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
__code uint8_t  SetupClrEndpStall[] = { USB_REQ_TYP_OUT | USB_REQ_RECIP_ENDP, USB_CLEAR_FEATURE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
__code uint8_t  SetupSetHIDIdle[]= { 0x21,HID_SET_IDLE,0x00,0x00,0x00,0x00,0x00,0x00 };
__code uint8_t  SetupGetHIDDevReport[] = { 0x81, USB_GET_DESCRIPTOR, 0x00, USB_DESCR_TYP_REPORT, 0x00, 0x00, 0xFF, 0x00 };
__code uint8_t  GetProtocol[] = { 0xc0,0x33,0x00,0x00,0x00,0x00,0x02,0x00 };

__xdata uint8_t  UsbDevEndp0Size;                                                      
__xdata __at (0x0380) uint8_t  RxBuffer[ MAX_PACKET_SIZE ];                            // IN, must even address
__xdata __at (0x03C0) uint8_t  TxBuffer[ MAX_PACKET_SIZE ];                            // OUT, must even address

__code __at (0x3FF8) uint8_t DevInfo[8];

__code __at (0x3600) uint8_t FirmwareID[]="pkarpins - Release 01.00 (" __DATE__ ")";


__code __at (0x3700) uint8_t DevTable[16]={   0x02,0x04, 0x08,0x20,			//	Mouse Params (16 bytes)
                                              0x00,0x00, 0x00,0x00,			// Spare - Future Button 1/2 ?
											  0x81,0x01, 0x08,0x20,			// Spare
											  0x00,0x00, 0x00,0x00			// Spare
};


uint8_t Set_Port = 0;
__idata _RootHubDev ThisUsbDev;                                                   //ROOT
__bit FoundNewDev;

typedef struct  {
	uint8_t  xcnt,ycnt;		//2 - pulse counter
	int16_t  xdelta,ydelta;	//4 - increment value for accumulator
	uint16_t xval,yval;		//4 - accumulator value
	uint8_t xph,yph;		//2 - phase
	uint8_t sdiv,smul;		//2 - config : DPI div, counter mult.
	uint8_t minf,maxf;		//2          : min increment, max increment
} mouse_params;             // -> 16b

__bit qmouse_mode;
__bit neg_mvmt,prev_neg_mvmt;
__bit keyboard_caps_lock;

__idata mouse_params p;

volatile uint8_t timer=0;
uint8_t hid_priority;
uint8_t mouse_prev_buttons;
__idata uint8_t keyboard_prev_keys[6];
uint8_t keyboard_prev_modifiers;

#define MAC_NULL_TRANSITION 0x7B
#define MAC_MODEL_NUMBER    0x09
#define MAC_TEST_RESPONSE   0x7D
#define MAC_KEYBUF_SIZE     16
#define MAC_CMD_START_US    420
#define MAC_SEND_SETUP_US   40
#define MAC_SEND_LOW_US     160
#define MAC_SEND_HIGH_US    140
#define KEYBOARD_POLL_TICK  0x08
#define HID_PRIORITY_KEYBOARD 0
#define HID_PRIORITY_MOUSE    1
#define MOUSE_TIMER_HZ      15873UL
#define TIMER0_RELOAD       ((uint8_t)(256UL - (FREQ_SYS / 12UL / MOUSE_TIMER_HZ)))

#define add_sat255(v,i) { uint8_t s=v+i; if (CY) s=255; v=s; }

#if DEBUG_SERIAL
#define PRINT(a) a
#else
#define PRINT(a)
#endif

__idata uint8_t mac_keybuf[MAC_KEYBUF_SIZE];
uint8_t mac_keybuf_head;
uint8_t mac_keybuf_tail;
__idata uint8_t mac_key_down[16];

#define DBG_STATE(a) PRINT({printstr(a);printlf();})
#define DBG_HEX2(a,b) PRINT({printstr(a);printx2(b);printlf();})

void SetHidPriority(uint8_t priority, __code const char *reason)
{
#if !DEBUG_SERIAL
	reason;
#endif
	if (hid_priority != priority) {
		PRINT({
			printstr("prio ");
			if (priority == HID_PRIORITY_MOUSE) printstr("mouse ");
			else printstr("kbd ");
			printstr(reason);
			printlf();
		})
	}
	hid_priority = priority;
}

SBIT(MAC_DATA,0xB0,3);   // P3.3, Macintosh Plus keyboard DATA
SBIT(MAC_CLOCK,0xB0,4);  // P3.4, Macintosh Plus keyboard CLOCK

SBIT(MOUSE_BUTTON,0x90,5); // P1.5, mouse button (DB9 pin 7), active low
SBIT(LED,0xB0,2);         // P3.2, LED, active low

#define P1_MOUSE_QUAD_MASK 0xD2 // P1.1, P1.4, P1.6, P1.7
#define P1_MOUSE_ALL_MASK  0xF2 // Quadrature plus button on P1.5

#define INIT_P3 P3        |= 0x1B;  \
                    P3_MOD_OC = (P3_MOD_OC & ~0x1B) | 0x0C;  \
                    P3_DIR_PU = (P3_DIR_PU & ~0x1B) | 0x1A;

#define USBLED_NO_DEVICE()  { LED = 0; }
#define USBLED_DEVICE()     { LED = 1; }

uint8_t KeyboardKeyWasPressed(uint8_t key)
{
	for (uint8_t i = 0; i != sizeof(keyboard_prev_keys); i++)
		if (keyboard_prev_keys[i] == key) return 1;
	return 0;
}

uint8_t KeyboardKeyIsPressed(uint8_t key)
{
	for (uint8_t i = 2; i != 8; i++)
		if (RxBuffer[i] == key) return 1;
	return 0;
}

uint8_t MacKeybufIsEmpty(void)
{
	return mac_keybuf_head == mac_keybuf_tail;
}

uint8_t MacKeyIsDown(uint8_t key)
{
	key &= 0x7F;
	return mac_key_down[key >> 3] & (1 << (key & 7));
}

void MacSetKeyDown(uint8_t key, uint8_t down)
{
	uint8_t mask;

	key &= 0x7F;
	mask = 1 << (key & 7);
	if (down) mac_key_down[key >> 3] |= mask;
	else mac_key_down[key >> 3] &= ~mask;
}

uint8_t MacQueueKey(uint8_t key)
{
	uint8_t next = (mac_keybuf_head + 1) & (MAC_KEYBUF_SIZE - 1);
	if (next == mac_keybuf_tail) {
		DBG_HEX2("macq drop ", mac_keybuf[mac_keybuf_tail]);
		mac_keybuf_tail = (mac_keybuf_tail + 1) & (MAC_KEYBUF_SIZE - 1);
	}
	mac_keybuf[mac_keybuf_head] = key;
	mac_keybuf_head = next;
	DBG_HEX2("macq ", key);
	return 1;
}

uint8_t MacPopKey(void)
{
	uint8_t key;

	if (MacKeybufIsEmpty()) return MAC_NULL_TRANSITION;
	key = mac_keybuf[mac_keybuf_tail];
	mac_keybuf_tail = (mac_keybuf_tail + 1) & (MAC_KEYBUF_SIZE - 1);
	return key;
}

uint8_t KeyboardHidIsModifier(uint8_t key)
{
	return (key >= 0xE0) && (key <= 0xE7);
}

uint8_t KeyboardHidToMac(uint8_t key)
{
	switch (key) {
		case 0x2A: return 0x67; // backspace
		case 0x35: return 0x65; // `~
		case 0x1E: return 0x25; // 1
		case 0x1F: return 0x27; // 2
		case 0x20: return 0x29; // 3
		case 0x21: return 0x2B; // 4
		case 0x22: return 0x2F; // 5
		case 0x23: return 0x2D; // 6
		case 0x24: return 0x35; // 7
		case 0x25: return 0x39; // 8
		case 0x26: return 0x33; // 9
		case 0x27: return 0x3B; // 0
		case 0x2D: return 0x37; // -
		case 0x2E: return 0x31; // =
		case 0x31: return 0x55; // backslash
		case 0x2B: return 0x61; // tab
		case 0x14: return 0x19; // q
		case 0x1A: return 0x1B; // w
		case 0x08: return 0x1D; // e
		case 0x15: return 0x1F; // r
		case 0x17: return 0x23; // t
		case 0x1C: return 0x21; // y
		case 0x18: return 0x41; // u
		case 0x0C: return 0x45; // i
		case 0x12: return 0x3F; // o
		case 0x13: return 0x47; // p
		case 0x2F: return 0x43; // [
		case 0x30: return 0x3D; // ]
		case 0x28: return 0x49; // enter
		case 0x39: return 0x73; // caps lock
		case 0x04: return 0x01; // a
		case 0x16: return 0x03; // s
		case 0x07: return 0x05; // d
		case 0x09: return 0x07; // f
		case 0x0A: return 0x0B; // g
		case 0x0B: return 0x09; // h
		case 0x0D: return 0x4D; // j
		case 0x0E: return 0x51; // k
		case 0x0F: return 0x4B; // l
		case 0x33: return 0x53; // ;
		case 0x34: return 0x4F; // '
		case 0x1D: return 0x0D; // z
		case 0x1B: return 0x0F; // x
		case 0x06: return 0x11; // c
		case 0x19: return 0x13; // v
		case 0x05: return 0x17; // b
		case 0x11: return 0x5B; // n
		case 0x10: return 0x5D; // m
		case 0x36: return 0x57; // ,
		case 0x37: return 0x5F; // .
		case 0x38: return 0x59; // /
		case 0x2C: return 0x63; // space
		case 0x4C: return 0x0F; // delete -> numpad clear
		case 0x52: return 0x1B; // up
		case 0x50: return 0x0D; // left
		case 0x51: return 0x11; // down
		case 0x4F: return 0x05; // right
		case 0xE1:
		case 0xE5: return 0x71; // shift
		case 0xE0:
		case 0xE2:
		case 0xE4:
		case 0xE6: return 0x75; // option
		case 0xE3:
		case 0xE7: return 0x6F; // command
		default: return MAC_NULL_TRANSITION;
	}
}

uint8_t KeyboardQueueTransition(uint8_t hid_key, uint8_t released)
{
	uint8_t mac_key = KeyboardHidToMac(hid_key);
	if (mac_key == MAC_NULL_TRANSITION) return 0;
	if (released) {
		if (!MacKeyIsDown(mac_key)) return 0;
		DBG_HEX2("key up ", mac_key);
		if (MacQueueKey(mac_key | 0x80)) MacSetKeyDown(mac_key, 0);
		return 0;
	}
	else {
		SetHidPriority(HID_PRIORITY_KEYBOARD, "key");
		DBG_HEX2("key dn ", mac_key);
		if (KeyboardHidIsModifier(hid_key)) {
			if (MacKeyIsDown(mac_key)) return 0;
			if (!MacQueueKey(mac_key)) return 0;
			MacSetKeyDown(mac_key, 1);
			return 1;
		}
		if (MacKeyIsDown(mac_key)) return 0;
		if (!MacQueueKey(mac_key)) return 0;
		MacSetKeyDown(mac_key, 1);
		return 1;
	}
}

uint8_t KeyboardProcessModifiers(uint8_t modifiers)
{
	uint8_t mask;
	uint8_t hid_key;
	uint8_t key_pressed = 0;

	for (uint8_t i = 0; i != 8; i++) {
		mask = 1 << i;
		hid_key = 0xE0 + i;
		if ((keyboard_prev_modifiers & mask) && !(modifiers & mask)) KeyboardQueueTransition(hid_key, 1);
		if (!(keyboard_prev_modifiers & mask) && (modifiers & mask)) key_pressed |= KeyboardQueueTransition(hid_key, 0);
	}
	keyboard_prev_modifiers = modifiers;
	return key_pressed;
}

uint8_t KeyboardProcessReport(uint8_t len)
{
	uint8_t modifiers;
	uint8_t key;
	uint8_t key_pressed;

	if (len < 8) return 0;
	modifiers = RxBuffer[0];
	key_pressed = KeyboardProcessModifiers(modifiers);

	for (uint8_t i = 0; i != sizeof(keyboard_prev_keys); i++) {
		key = keyboard_prev_keys[i];
		if ((key != 0) && (key != 1) && !KeyboardKeyIsPressed(key)) KeyboardQueueTransition(key, 1);
	}

	for (uint8_t i = 2; i != 8; i++) {
		key = RxBuffer[i];
		if ((key == 0) || (key == 1) || KeyboardKeyWasPressed(key)) continue;
		key_pressed |= KeyboardQueueTransition(key, 0);
	}

	memcpy(keyboard_prev_keys, &RxBuffer[2], sizeof(keyboard_prev_keys));
	return key_pressed;
}

void MacKeyboardInit(void)
{
	MAC_CLOCK = 1;
	MAC_DATA = 1;
	P3 |= 0x18;
	P3_MOD_OC |= 0x08;  // DATA on P3.3: open-drain
	P3_MOD_OC &= ~0x10; // CLOCK on P3.4: push-pull
	P3_DIR_PU |= 0x18;
}

void MacDataRelease(void)
{
	MAC_DATA = 1;
}

uint8_t MacReadData(void)
{
	return MAC_DATA;
}

uint8_t MacReadByte(void)
{
	uint8_t b = 0;

	for (uint8_t i = 0; i != 8; i++) {
		MAC_CLOCK = 0;
		mDelayuS(180);
		MAC_CLOCK = 1;
		mDelayuS(80);
		b = (b << 1) | MacReadData();
		mDelayuS(140);
	}
	return b;
}

void MacSendByte(uint8_t b)
{
	for (uint8_t m = 0x80; m != 0; m >>= 1) {
		MAC_DATA = (b & m) ? 1 : 0;
		mDelayuS(MAC_SEND_SETUP_US);
		MAC_CLOCK = 0;
		mDelayuS(MAC_SEND_LOW_US);
		MAC_CLOCK = 1;
		mDelayuS(MAC_SEND_HIGH_US);
	}
	MacDataRelease();
}

void MacSendKey(uint8_t key)
{
	DBG_HEX2("mac send ", key);
	MacSendByte(key);
}

uint8_t PollKeyboardHID(void)
{
	uint8_t endp, s, len;

	if (ThisUsbDev.DeviceStatus != ROOT_DEV_SUCCESS || !(ThisUsbDev.DeviceTypes & DEV_HAS_KEYBOARD)) return 0;

	SelectHubPort();
	endp = ThisUsbDev.KeyboardEndp;
	if ( !(endp & USB_ENDP_ADDR_MASK) ) {
#if DEBUG_SERIAL
		printstr("Keyboard no interrupt endpoint\n");
#endif
		return 0;
	}

	s = USBHostTransact( USB_PID_IN << 4 | endp & 0x7F, endp & 0x80 ? bUH_R_TOG | bUH_T_TOG : 0, 0 );
	if ( s == ERR_SUCCESS ){
		endp ^= 0x80;
		ThisUsbDev.KeyboardEndp = endp;
		len = USB_RX_LEN;
		if ( len ) {
#if DEBUG_SERIAL
			printstr("KBD RX: ");
			for ( s = 0; s != len; s ++ ) printx2(RxBuffer[s]);
			printlf();
#endif
			return KeyboardProcessReport(len);
		}
	}
	else if ( s != ( USB_PID_NAK | ERR_USB_TRANSFER ) ) {
#if DEBUG_SERIAL
		printstr("Err ");printhex2(s);
#endif
	}
	SetUsbSpeed( 1 );
	return 0;
}

uint8_t PollMouseHID(void)
{
	uint8_t endp, s, len, i;
	uint16_t delta;
	int8_t sval;

	if (ThisUsbDev.DeviceStatus != ROOT_DEV_SUCCESS || !(ThisUsbDev.DeviceTypes & DEV_HAS_MOUSE)) return 0;

	SelectHubPort();
	endp = ThisUsbDev.MouseEndp;
	if ( !(endp & USB_ENDP_ADDR_MASK) ) {
		PRINT(printstr("Mouse no interrupt endpoint\n");)
		return 0;
	}

	s = USBHostTransact( USB_PID_IN << 4 | endp & 0x7F, endp & 0x80 ? bUH_R_TOG | bUH_T_TOG : 0, 0 );
	if ( s == ERR_SUCCESS ){
		endp ^= 0x80;
		ThisUsbDev.MouseEndp = endp;
		len = USB_RX_LEN;
		if ( len ) {
			PRINT(printstr("RX: ");)
			PRINT({for ( i = 0; i != len; i ++ ) printx2(RxBuffer[i]); printlf();})

			i=RxBuffer[0];		// 0->Left,1->Right,2->Middle
			if ((i != mouse_prev_buttons) || RxBuffer[1] || RxBuffer[2]) {
				SetHidPriority(HID_PRIORITY_MOUSE, "move");
				PRINT({printstr("mouse ");printx2(RxBuffer[0]);printx2(RxBuffer[1]);printx2(RxBuffer[2]);printlf();})
			}
			mouse_prev_buttons = i;

			MOUSE_BUTTON = (i & 1) ? 0 : 1;
			EA=0;

			sval=RxBuffer[1];
			if (sval!=0)
			{
			 neg_mvmt=(sval<0); i=!neg_mvmt?sval:-sval;
			 i/=p.sdiv; prev_neg_mvmt=(p.xdelta<0);
			 if ( p.xcnt && ( neg_mvmt^prev_neg_mvmt ) )
				{ if (p.xcnt>=i) {p.xcnt-=i;neg_mvmt=!neg_mvmt;}
				              else {p.xcnt=(i-p.xcnt); }
				}
			 else
			    add_sat255(p.xcnt,i);

			 delta=p.minf+p.smul*p.xcnt;
			 if (delta>=p.maxf) delta=p.maxf;
			 if (!neg_mvmt) p.xdelta=delta; else p.xdelta=-delta;
			}

			sval=RxBuffer[2];
			if (sval!=0)
			{
			 neg_mvmt=(sval<0); i=!neg_mvmt?sval:-sval;
			 i/=p.sdiv; prev_neg_mvmt=(p.ydelta<0);
			 if ( p.ycnt && ( neg_mvmt^prev_neg_mvmt ) )
				{ if (p.ycnt>=i) {p.ycnt-=i;neg_mvmt=!neg_mvmt;}
				              else {p.ycnt=(i-p.ycnt); }
				}
			 else
			    add_sat255(p.ycnt,i);

			 delta=p.minf+p.smul*p.ycnt;
			 if (delta>=p.maxf) delta=p.maxf;
			 if (!neg_mvmt) p.ydelta=delta; else p.ydelta=-delta;
			}

			EA=1;
			PRINT({for(i=0;i!=16;i++) printx2((&p.xcnt)[i]); printlf();})
			SetUsbSpeed( 1 );
			return 1;
		}
	}
	else if ( s != ( USB_PID_NAK | ERR_USB_TRANSFER ) ) {
		PRINT({printstr("Err ");printhex2(s);})
	}
	SetUsbSpeed( 1 );
	return 0;
}

uint8_t PollHIDByPriority(void)
{
	if (hid_priority == HID_PRIORITY_MOUSE) {
		PollMouseHID();
		return PollKeyboardHID();
	}
	else {
		if (PollKeyboardHID()) return 1;
		PollMouseHID();
	}
	return 0;
}

void MacInquiry(void)
{
	MacSendKey(MacPopKey());
}

uint8_t MacReadCommandIfPending(uint8_t *cmd)
{
	uint16_t timeout;

	MacDataRelease();
	if (MacReadData()) return 0;
	mDelayuS(20);
	if (MacReadData()) return 0;

	// Prefer stable keyboard timing after a key event. While the mouse is the
	// active HID device, leave Timer0 running so quadrature pulses are not lost.
	if (hid_priority == HID_PRIORITY_KEYBOARD) EA = 0;
	mDelayuS(MAC_CMD_START_US);
	*cmd = MacReadByte();
	timeout = 1000;
	while (!MacReadData() && --timeout);
	mDelayuS(20);
	return 1;
}

void MacKeyboardTask(void)
{
	uint8_t cmd;

	if (!MacReadCommandIfPending(&cmd)) return;
	DBG_HEX2("mac cmd ", cmd);
	switch (cmd) {
		case 0x10:
			MacInquiry();
			break;
		case 0x12:
			MacSendKey(MacPopKey());
			break;
		case 0x14:
			MacSendByte(MAC_MODEL_NUMBER);
			break;
		case 0x16:
			MacSendByte(MAC_TEST_RESPONSE);
			break;
	}
	EA = 1;
}

//              P1.  4   1   6   7
// Mouse output 	X1 	X0 	Y0 	Y1

// If needed - logic analyser - time spent in Timer Interrupt
// SBIT(DEBUGP,0x90,0);

void Timer0_ISR(void) __interrupt (INT_NO_TMR0) __using(1) {
// DEBUGP=1;

	timer++;

	if (qmouse_mode)
	{
	 if (p.xcnt) {
		p.xval+=p.xdelta;
		if (((p.xval>>8)&0xff)!=p.xph) {
		  p.xph=p.xval>>8;
		  p.xcnt--;
		  ACC=p.xph;
		  __asm 
			jnb	ACC.1,00001$
			cpl	ACC.0
00001$:
			rrc	A
			mov	0x91,c
			rrc	A
			mov	0x94,c
		  __endasm;
		}
	 } else {p.xdelta=0;}

	 if (p.ycnt) {
		p.yval+=p.ydelta;
		if (((p.yval>>8)&0xff)!=p.yph) {
		  p.yph=p.yval>>8;
		  p.ycnt--;
		  ACC=p.yph;
		  __asm 
			jnb	ACC.1,00003$
			cpl	ACC.0
00003$:
			rrc	A
			mov	0x97,c
			rrc	A
			mov	0x96,c
		  __endasm;
		}
	 } else {p.ydelta=0;}
	}

// DEBUGP=0;
}

void main( )
{
    uint8_t   s;
    uint8_t i;

    CfgFsys( );	

	// Port 3 initialisation - depends on Hardware Version
	INIT_P3;
	MacKeyboardInit();
	MAC_CLOCK = 0; // Hold the keyboard clock low during the startup reset pulse

    P1 |= P1_MOUSE_ALL_MASK; // Release the active-low button; idle quadrature high
    P1_MOD_OC = (P1_MOD_OC & ~P1_MOUSE_ALL_MASK) | 0x20;
    P1_DIR_PU = (P1_DIR_PU & ~P1_MOUSE_ALL_MASK) | 0x20;

    mDelaymS(50);

#if DEBUG_SERIAL
	PIN_FUNC &= ~bUART0_PIN_X;		// UART0 TX on P3.1, 9600 8N1
    mInitSTDIO( );
    printstr( "Start @ChipID=");printhex2(CHIP_ID);printlf();
#endif

    T2MOD &= 0xEF;				// Timer0 clock = Fsys/12
	TMOD = TMOD & 0xF0 | 0x02; 	// Timer0 mode 2, 8-bit auto-reload
	TH0 = TIMER0_RELOAD; TR0 = 1;
    ET0=1;
	MAC_CLOCK = 1; // End keyboard reset pulse on P3.4
	EA=1;

    InitUSB_Host( );
    FoundNewDev = 0;
	SetHidPriority(HID_PRIORITY_KEYBOARD, "init");
	mouse_prev_buttons = 0;
	USBLED_NO_DEVICE();

#if DEBUG_SERIAL
again:
    printstr( "Wait Device In\n" );

	printstr("Config Data [0..7]: ");
	for ( i = 0; i != 8; i++ ){
		printx2(DevInfo[i]);
	}
	printlf();
#endif

    while ( 1 )
    {
// checkRootHubConnections ?
        s = ERR_SUCCESS;
        if ( UIF_DETECT ){                                                      
            UIF_DETECT = 0;                                                 
            s = AnalyzeRootHub( );                                              
            if ( s == ERR_USB_CONNECT ) {
				DBG_STATE("usb connect");
				FoundNewDev = 1;
			}
				if ( s == ERR_USB_DISCON ) 	{
                PRINT(printstr( "Disconnect\n");)
				DBG_STATE("usb disconnect");
				USBLED_NO_DEVICE();
				qmouse_mode=0;     
				P1_DIR_PU &= ~P1_MOUSE_QUAD_MASK;
				MacKeyboardInit();
				MOUSE_BUTTON=1;
				SetHidPriority(HID_PRIORITY_KEYBOARD, "discon");
				mouse_prev_buttons = 0;
			}
        }
		if ( FoundNewDev ){
            FoundNewDev = 0;
//          mDelaymS( 200 );
            s = EnumAllRootDevice( );                                       
            if ( s != ERR_SUCCESS ){						
            PRINT({printstr( "EnumAllRootDev err = ");printhex2(s);printlf();})
            }
			else
			{
				USBLED_DEVICE();
				PRINT({printstr("usb ready flags ");printx2(ThisUsbDev.DeviceTypes);printlf();})

				if ( ThisUsbDev.DeviceTypes & DEV_HAS_MOUSE )
				{
					// SetBootProto();
					
					memset(&p,0,sizeof(p));
					SetHidPriority(HID_PRIORITY_MOUSE, "enum");
					mouse_prev_buttons = 0;
					i=0;
					if (DevTable[0+i]!=0xFF)
	 					{p.sdiv=DevTable[0+i]; p.smul=DevTable[1+i];
	  					 p.minf=DevTable[2+i]; p.maxf=DevTable[3+i];}
					else 
	 					{p.sdiv=1; p.smul=2; p.minf=0; p.maxf=250;}

					PRINT({printstr("Mouse Params : "); printx2(p.sdiv); printx2(p.smul);})
	                PRINT({printx2(p.minf); printx2(p.maxf); printlf();})

					qmouse_mode=1;
					P1_DIR_PU |= P1_MOUSE_QUAD_MASK;
				}

				if ( ThisUsbDev.DeviceTypes & DEV_HAS_KEYBOARD )
				{
					memset(keyboard_prev_keys,0,sizeof(keyboard_prev_keys));
					keyboard_prev_modifiers=0;
					keyboard_caps_lock=0;
					mac_keybuf_head=mac_keybuf_tail=0;
					memset(mac_key_down,0,sizeof(mac_key_down));
					if (!(ThisUsbDev.DeviceTypes & DEV_HAS_MOUSE)) {
							qmouse_mode=0;
							P1_DIR_PU &= ~P1_MOUSE_QUAD_MASK;
							MacKeyboardInit();
					}
				}

			}
        }

// pollHIDdevice ?
		if (timer&KEYBOARD_POLL_TICK)		// 15874/8 -> ~2kHz (0.5ms)
		{
		  timer=0;

		  PollHIDByPriority();
		  MacKeyboardTask();
		}	//End test if timer

		MacKeyboardTask();

    }	// End While
}

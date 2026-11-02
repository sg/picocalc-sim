#pragma once

// Key codes as the PicoCalc's STM32 keyboard controller reports them over I2C.

#define PC_KEY_ALT 0xA1
#define PC_KEY_LSHIFT 0xA2
#define PC_KEY_RSHIFT 0xA3
#define PC_KEY_SYM 0xA4
#define PC_KEY_CTRL 0xA5

#define PC_KEY_BACKSPACE 0x08
#define PC_KEY_TAB 0x09
#define PC_KEY_ENTER 0x0A
#define PC_KEY_SPACE 0x20

#define PC_KEY_ESC 0xB1
#define PC_KEY_LEFT 0xB4
#define PC_KEY_UP 0xB5
#define PC_KEY_DOWN 0xB6
#define PC_KEY_RIGHT 0xB7

#define PC_KEY_BREAK 0xD0
#define PC_KEY_INSERT 0xD1
#define PC_KEY_HOME 0xD2
#define PC_KEY_DEL 0xD4
#define PC_KEY_END 0xD5
#define PC_KEY_PAGE_UP 0xD6
#define PC_KEY_PAGE_DOWN 0xD7

#define PC_KEY_CAPS_LOCK 0xC1

#define PC_KEY_F1 0x81
#define PC_KEY_F2 0x82
#define PC_KEY_F3 0x83
#define PC_KEY_F4 0x84
#define PC_KEY_F5 0x85
#define PC_KEY_F6 0x86
#define PC_KEY_F7 0x87
#define PC_KEY_F8 0x88
#define PC_KEY_F9 0x89
#define PC_KEY_F10 0x90

#define PC_KEY_POWER 0x91

#define PC_KEY_STATE_IDLE 0
#define PC_KEY_STATE_PRESSED 1
#define PC_KEY_STATE_HOLD 2
#define PC_KEY_STATE_RELEASED 3

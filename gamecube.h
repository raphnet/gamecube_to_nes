#include "gamepad.h"

#define GCN64_REPORT_SIZE	8

Gamepad *gamecubeGetGamepad(void);

#define GC_GET_START(report) (report[6] & 0x01)
#define GC_GET_Y(report) (report[6] & 0x02)
#define GC_GET_X(report) (report[6] & 0x04)
#define GC_GET_B(report) (report[6] & 0x08)
#define GC_GET_A(report) (report[6] & 0x10)
#define GC_GET_L(report) (report[6] & 0x20)
#define GC_GET_R(report) (report[6] & 0x40)
#define GC_GET_Z(report) (report[6] & 0x80)

#define GC_GET_DPAD_UP(report) (report[7] & 0x01)
#define GC_GET_DPAD_DOWN(report) (report[7] & 0x02)
#define GC_GET_DPAD_RIGHT(report) (report[7] & 0x04)
#define GC_GET_DPAD_LEFT(report) (report[7] & 0x08)


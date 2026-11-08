// Sega Dreamcast input: the first controller drives the PS1 pad on port 0.
// Port 1 always reports as disconnected.
//
// A=Cross, B=Circle, X=Square, Y=Triangle, triggers=L1/R1, Start=Start,
// D-pad=D-pad, analog stick=left stick. The standard controller has no
// Select, L2 or R2; controllers with C/Z/D buttons map them to R2, L2 and
// Select.
#include <psyz.h>
#include <psyz/log.h>
#include <libetc.h>
#include <string.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#define TRIGGER_THRESHOLD 128

void MyPadInit(int mode) { (void)mode; }

static u_long ReadButtons(const cont_state_t* st) {
    u_long pressed = 0;
    uint32_t b = st->buttons;
    if (b & CONT_Y) pressed |= PADRup;
    if (b & CONT_A) pressed |= PADRdown;
    if (b & CONT_X) pressed |= PADRleft;
    if (b & CONT_B) pressed |= PADRright;
    if (b & CONT_DPAD_UP) pressed |= PADLup;
    if (b & CONT_DPAD_DOWN) pressed |= PADLdown;
    if (b & CONT_DPAD_LEFT) pressed |= PADLleft;
    if (b & CONT_DPAD_RIGHT) pressed |= PADLright;
    if (st->ltrig >= TRIGGER_THRESHOLD) pressed |= PADL1;
    if (st->rtrig >= TRIGGER_THRESHOLD) pressed |= PADR1;
    if (b & CONT_Z) pressed |= PADL2;
    if (b & CONT_C) pressed |= PADR2;
    if (b & CONT_START) pressed |= PADstart;
    if (b & CONT_D) pressed |= PADselect;
    return pressed;
}

static char StickByte(int v) {
    v += 128; // KOS reports -128..127 with 0 centred; the PS1 uses 0..255
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (char)v;
}

void Psyz_PadsPoll(void) {
    char frame[PSYZ_PAD_BUF_LEN];
    cont_state_t empty = {0};
    const cont_state_t* st = &empty;
    maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (dev) {
        const cont_state_t* status = maple_dev_status(dev);
        if (status) {
            st = status;
        }
    }

    // see sdl3_common.h BuildPadFrame: PSY-Q's high byte (d-pad,
    // Start/Select) belongs in wire byte 2 and the low byte (face buttons,
    // shoulders) in wire byte 3, both active-low
    u_long pressed = ReadButtons(st);
    PsyzControllerKind kind = Psyz_PadsSetKind(0, 0, PSYZ_CTRL_QUERY_KIND);
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x00;
    frame[1] = (char)kind;
    switch (kind) {
    case PSYZ_CTRL_ANALOG_PAD:
    case PSYZ_CTRL_ANALOG_STICK:
        frame[2] = (char)(~(pressed >> 8) & 0xFF);
        frame[3] = (char)(~pressed & 0xFF);
        // one analog stick: it drives the PS1 left stick
        frame[4] = (char)0x80;
        frame[5] = (char)0x80;
        frame[6] = StickByte(st->joyx);
        frame[7] = StickByte(st->joyy);
        break;
    case PSYZ_CTRL_DISCONNECTED:
        memset(frame, 0xFF, sizeof(frame));
        break;
    case PSYZ_CTRL_DIGITAL_PAD:
    default:
        frame[1] = (char)PSYZ_CTRL_DIGITAL_PAD;
        frame[2] = (char)(~(pressed >> 8) & 0xFF);
        frame[3] = (char)(~pressed & 0xFF);
        break;
    }
    Psyz_PadsSet(0, frame, sizeof(frame));

    memset(frame, 0xFF, sizeof(frame));
    Psyz_PadsSet(1, frame, sizeof(frame));
}

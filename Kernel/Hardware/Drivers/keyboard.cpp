#include "keyboard.hpp"

/* Mapping */
static char* altgr_l1 = "@ſ€®þ←↓→œþ";
static char* altgr_l2 = "ªßðđŋħ̉ĸł";
static char* altgr_l3 = "«»©„“”µ";
static char* altgr_l0 = "!@?$??{[]}";
static char* shift_l1 = "QWERTYUIOP";
static char* shift_l2 = "ASDFGHJKL";
static char* shift_l3 = "ZXCVBNM";
static char* shift_l0 = "!\"#?%&/()=";
static char* l1 = "qwertyuiop";
static char* l2 = "asdfghjkl";
static char* l3 = "zxcvbnm";
static char* l0 = "123456789";

KeyboardDriver* KeyboardDriver::active = 0;

KeyboardDriver::KeyboardDriver(InterruptManager* interrupt_manager)
    : InterruptHandler(interrupt_manager, 0x21)
    , data_port(0x60)
    , command_port(0x64)
{
    active = this;
    while (command_port.read() & 0x1)
        data_port.read();
    command_port.write(0xae);
    command_port.write(0x20);
    uint8_t status = (data_port.read() | 1) & ~0x10;
    command_port.write(0x60);
    data_port.write(status);
    data_port.write(0xf4);
}

KeyboardDriver::~KeyboardDriver()
{
}

bool KeyboardDriver::has_unread_event()
{
    if (current_event >= events_index)
        return false;
    return true;
}

int KeyboardDriver::keyboard_event(keyboard_event_t* event)
{
    if (current_event >= events_index)
        return 0;

    memcpy(event, &events[current_event], sizeof(keyboard_event_t));
    current_event++;

    if (current_event >= events_index) {
        current_event = 0;
        events_index = 0;
    }
    return sizeof(keyboard_event_t);
}

/* TODO: English layout & more character support */
uint8_t KeyboardDriver::key_a(uint8_t key)
{
    if (key == ENTER_PRESSED)
        return '\n';

    if (key == SPACE_PRESSED)
        return ' ';

    if (key == BACKSPACE_PRESSED)
        return '\b';

    if (key == POINT_RELEASED) {
        if (is_shift)
            return ':';
        return '.';
    }

    if (key == COMMA_PRESSED) {
        if (is_shift)
            return ';';
        return ',';
    }

    if (key == SLASH_RELEASED) {
        if (is_shift)
            return '_';
        return '-';
    }

    if (key == ZERO_PRESSED) {
        if (is_shift)
            return shift_l0[9];
        if (is_altgr)
            return altgr_l0[9];
        return '0';
    }

    if (key == PLUS_PRESSED) {
        if (is_shift)
            return '?';
        if (is_altgr)
            return '\\';
        return '+';
    }

    if (key == PIPE_PRESSED) {
        if (is_shift)
            return '>';
        if (is_altgr)
            return '|';
        return '<';
    }

    /* Row 1 */
    if (key >= ONE_PRESSED && key <= NINE_PRESSED) {
        if (is_altgr)
            return altgr_l0[key - ONE_PRESSED];
        if (is_shift)
            return shift_l0[key - ONE_PRESSED];
        return l0[key - ONE_PRESSED];
    }

    /* Row 2 */
    if (key >= Q_PRESSED && key <= ENTER_PRESSED) {
        if (is_altgr)
            return altgr_l1[key - Q_PRESSED];
        if (is_shift)
            return shift_l1[key - Q_PRESSED];
        return l1[key - Q_PRESSED];
    }

    /* Row 3 */
    else if (key >= A_PRESSED && key <= L_PRESSED) {
        if (is_altgr)
            return altgr_l2[key - A_PRESSED];
        if (is_shift)
            return shift_l2[key - A_PRESSED];
        return l2[key - A_PRESSED];
    }

    /* Row 4 */
    else if (key >= Y_PRESSED && key <= M_PRESSED) {
        if (is_altgr)
            return altgr_l3[key - Y_PRESSED];
        if (is_shift)
            return shift_l3[key - Y_PRESSED];
        return l3[key - Y_PRESSED];
    }

    /* ESC */
    if (key == 1)
        return 27;

    return 0;
}

int KeyboardDriver::get_key_presses(int raw)
{
    if (raw == 1)
        return keys_pressed_raw;
    return keys_pressed;
}

uint8_t KeyboardDriver::read_key()
{
    uint8_t lastkey = 0;
    if (command_port.read() & 1)
        lastkey = data_port.read();
    return lastkey;
}

char KeyboardDriver::get_key()
{
    uint8_t c = 0;
    while (c == 0) {
        c = read_key();
        if (c == SHIFT_PRESSED)
            is_shift = 1;

        if (c == SHIFT_RELEASED)
            is_shift = 0;

        if (c == ALTGR_PRESSED)
            is_altgr = 1;

        if (c == ALTGR_RELEASED)
            is_altgr = 0;
    }
    if (key_a(c) != 0)
        return key_a(c);
    return 0;
}

char KeyboardDriver::get_last_key(int raw)
{
    if (raw == 1)
        return last_key_raw;
    return last_key;
}

void KeyboardDriver::on_key(uint8_t keypress)
{
    if (keypress == SHIFT_PRESSED)
        is_shift = 1;

    if (keypress == SHIFT_RELEASED)
        is_shift = 0;

    if (keypress == ALTGR_PRESSED)
        is_altgr = 1;

    if (keypress == ALTGR_RELEASED)
        is_altgr = 0;

    last_key = key_a(keypress);
    if (last_key != 0) {
        events[events_index].key = last_key;
        events_index++;

        if (events_index >= MAX_KEYBOARD_EVENTS) {
            events_index = 0;
            current_event = 0;
        } else {
            TM->test_poll();
        }
    }

    keys_pressed += (last_key == '\b') ? -1 : 1;
    if (last_key == 10)
        keys_pressed = 0;
    last_key_raw = keypress;
    keys_pressed_raw++;
}

uint32_t KeyboardDriver::interrupt(uint32_t esp)
{
    uint8_t key = data_port.read();
    on_key(key);
    return esp;
}

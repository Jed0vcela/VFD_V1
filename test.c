#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

// SPI pins
#define PIN_SCK  2
#define PIN_MOSI 3
#define PIN_MISO 4  // unused but needed for SPI init
#define PIN_CS   5  // latch pin

// Button pins (active low with pullup)
#define BTN_TIME_UP    17
#define BTN_TIME_DOWN  18
#define BTN_BRIGHT_UP  21
#define BTN_BRIGHT_DOWN 22

// SPI instance
#define SPI_PORT spi0
#define SPI_BAUDRATE 1000000  // 1 MHz

// Display parameters
#define NUM_DIGITS 13
#define NUM_SPECIAL 3

// Special character bit positions (bits 0-2 in digit mask after rotation)
#define SPECIAL_M_BIT 0  // "M" character (last bit sent over SPI)
#define SPECIAL_DASH_BIT 1  // "-" character
#define SPECIAL_E_BIT 2  // "E" character

// Digit positions are shifted right by 3 (bits 3-15)
#define DIGIT_BIT_OFFSET 3

// Global variables
uint8_t brightness = 30;  // 0-63
uint8_t digit = 0;

int32_t time_offset_seconds = 0;  // User adjustment

// Special character enable flags
bool special_m_enabled = false;
bool special_dash_enabled = false;
bool special_e_enabled = false;

// Button state tracking
typedef struct {
    uint8_t pin;
    bool pressed;
    uint32_t press_start_time;
    uint32_t last_action_time;
    uint32_t hold_count;
} button_state_t;

button_state_t btn_time_up = {BTN_TIME_UP, false, 0, 0, 0};
button_state_t btn_time_down = {BTN_TIME_DOWN, false, 0, 0, 0};
button_state_t btn_bright_up = {BTN_BRIGHT_UP, false, 0, 0, 0};
button_state_t btn_bright_down = {BTN_BRIGHT_DOWN, false, 0, 0, 0};

// 7-segment lookup table
const uint8_t seg7[12] = {
    0x11,        // 0
    0b11011011,  // 1
    0b01000101,  // 2
    0b01001001,  // 3
    0b10001011,  // 4
    0b00101001,  // 5
    0b00100001,  // 6
    0b01011011,  // 7
    0x01,        // 8
    0x09,        // 9
    0xff,        // blank
    0xfe         // dash
};

// Bit reversal table for 6-bit values
const uint8_t rev6[64] = {
    0,32,16,48,8,40,24,56,4,36,20,52,12,44,28,60,
    2,34,18,50,10,42,26,58,6,38,22,54,14,46,30,62,
    1,33,17,49,9,41,25,57,5,37,21,53,13,45,29,61,
    3,35,19,51,11,43,27,59,7,39,23,55,15,47,31,63
};

// Number array for 13 digits, initialization
uint8_t number[NUM_DIGITS] = {10,10,10,10,4,5,6,7,8,9,5,7,3};

/**
 * Send 32 bits via SPI and pulse latch pin
 */
void send_32bit(uint32_t value) {
    uint8_t buf[4];
    
    // Split into 4 bytes (MSB first)
    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >> 8)  & 0xFF;
    buf[3] = value & 0xFF;
    
    // Send via SPI
    spi_write_blocking(SPI_PORT, buf, 4);
    
    // Pulse latch/CS pin
    gpio_put(PIN_CS, 1);
    sleep_us(1);
    gpio_put(PIN_CS, 0);
}

/**
 * Send display data for one digit (0-12)
 */
void send_display_digit(uint8_t dig) {
    uint32_t bright, num, dig_mask, frame;
    
    // Clamp brightness to 6 bits
    if (brightness > 63) {
        brightness = 63;
    }
    
    // Reverse brightness bits and shift to position [31:26]
    bright = rev6[brightness];
    bright = bright << 26;
    bright = (~bright) & 0xFC000000;
    
    uint8_t tmp_num = number[dig];
    
    // Segment data in bits [23:16]
    num = ((uint32_t)seg7[tmp_num]) << 16;
    
    // Digit mask: digits are in bits [15:3], special chars in bits [2:0]
    dig_mask = 1 << (dig + DIGIT_BIT_OFFSET);
    dig_mask = (~dig_mask) & 0x0000FFF8;

    uint32_t special = (!special_e_enabled << 2) | (!special_dash_enabled << 1) | !special_m_enabled;

    // Combine and send
    frame = bright + num + dig_mask + special;
    send_32bit(frame);
}

/**
 * Update a specific digit value
 */
void set_digit_value(uint8_t pos, uint8_t value) {
    if (pos < NUM_DIGITS) {
        number[pos] = value;
    }
}

/**
 * Set brightness (0-63)
 */
void set_brightness(uint8_t bright) {
    if (bright > 63) {
        bright = 63;
    }
    brightness = bright;
}

/**
 * Initialize buttons with pullup
 */
void init_buttons(void) {
    gpio_init(BTN_TIME_UP);
    gpio_set_dir(BTN_TIME_UP, GPIO_IN);
    gpio_pull_up(BTN_TIME_UP);
    
    gpio_init(BTN_TIME_DOWN);
    gpio_set_dir(BTN_TIME_DOWN, GPIO_IN);
    gpio_pull_up(BTN_TIME_DOWN);
    
    gpio_init(BTN_BRIGHT_UP);
    gpio_set_dir(BTN_BRIGHT_UP, GPIO_IN);
    gpio_pull_up(BTN_BRIGHT_UP);
    
    gpio_init(BTN_BRIGHT_DOWN);
    gpio_set_dir(BTN_BRIGHT_DOWN, GPIO_IN);
    gpio_pull_up(BTN_BRIGHT_DOWN);
}

/**
 * Calculate time adjustment with smooth linear acceleration
 * Returns number of seconds to adjust based on hold duration
 */
int32_t calculate_time_adjustment(uint32_t hold_count) {
    // Linear acceleration: starts at 1 second, increases smoothly
    
    int32_t adjustment = 1 + (hold_count / 2);
    
    return adjustment;
}

/**
 * Process button state with debouncing and acceleration
 */
void process_button(button_state_t *btn) {
    bool current_state = !gpio_get(btn->pin);  // Active low, so invert
    uint32_t now = to_ms_since_boot(get_absolute_time());
    
    if (current_state && !btn->pressed) {
        // Button just pressed
        btn->pressed = true;
        btn->press_start_time = now;
        btn->last_action_time = now;
        btn->hold_count = 0;
    } else if (!current_state && btn->pressed) {
        // Button released
        btn->pressed = false;
        btn->hold_count = 0;
    }
}

/**
 * Handle time adjustment buttons with smooth acceleration
 */
void handle_time_buttons(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    
    // Process button states
    process_button(&btn_time_up);
    process_button(&btn_time_down);
    
    // Handle time up
    if (btn_time_up.pressed) {
        uint32_t interval = 100;  // Base interval: 100ms (10 Hz)
        
        // Gradually speed up the repeat rate for smoother acceleration
        if (btn_time_up.hold_count > 30) {
            interval = 50;  // 20 Hz after 3 seconds
        }
        if (btn_time_up.hold_count > 100) {
            interval = 25;  // 40 Hz after 10 seconds
        }
        
        if (now - btn_time_up.last_action_time >= interval) {
            int32_t adjustment = calculate_time_adjustment(btn_time_up.hold_count);
            time_offset_seconds += adjustment;
            btn_time_up.last_action_time = now;
            btn_time_up.hold_count++;
        }
    }
    
    // Handle time down
    if (btn_time_down.pressed) {
        uint32_t interval = 100;  // Base interval: 100ms
        
        if (btn_time_down.hold_count > 30) {
            interval = 50;
        }
        if (btn_time_down.hold_count > 100) {
            interval = 25;
        }
        
        if (now - btn_time_down.last_action_time >= interval) {
            int32_t adjustment = calculate_time_adjustment(btn_time_down.hold_count);
            time_offset_seconds -= adjustment;
            btn_time_down.last_action_time = now;
            btn_time_down.hold_count++;
        }
    }
}

/**
 * Handle brightness adjustment buttons
 */
void handle_brightness_buttons(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    
    // Process button states
    process_button(&btn_bright_up);
    process_button(&btn_bright_down);
    
    // Handle brightness up - faster repeat
    if (btn_bright_up.pressed) {
        uint32_t interval = 50;  // 50ms = 20 Hz (much faster!)
        
        // Even faster after holding for a bit
        if (btn_bright_up.hold_count > 10) {
            interval = 25;  // 40 Hz after 0.5 seconds
        }
        
        if (now - btn_bright_up.last_action_time >= interval) {
            if (brightness < 63) {
                brightness++;
            }
            btn_bright_up.last_action_time = now;
            btn_bright_up.hold_count++;
        }
    } else {
        btn_bright_up.hold_count = 0;
    }
    
    // Handle brightness down - faster repeat
    if (btn_bright_down.pressed) {
        uint32_t interval = 50;  // 50ms = 20 Hz
        
        if (btn_bright_down.hold_count > 10) {
            interval = 25;  // 40 Hz after 0.5 seconds
        }
        
        if (now - btn_bright_down.last_action_time >= interval) {
            if (brightness > 0) {
                brightness--;
            }
            btn_bright_down.last_action_time = now;
            btn_bright_down.hold_count++;
        }
    } else {
        btn_bright_down.hold_count = 0;
    }
}

/**
 * Initialize SPI and GPIO
 */
void init_hardware(void) {
    // Initialize stdio for printf (USB)
    stdio_init_all();
    
    // Initialize SPI
    spi_init(SPI_PORT, SPI_BAUDRATE);
    
    // Set SPI format: 8 bits, CPOL=0, CPHA=0, MSB first
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    
    // Initialize SPI pins
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    
    // Initialize CS/latch pin
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 0);  // idle low
    
    // Initialize buttons
    init_buttons();
}

/**
 * Refresh the entire display (call this continuously)
 */
void refresh_display(void) {
    // Display all 13 digits
    for (uint8_t i = 0; i < NUM_DIGITS; i++) {
        send_display_digit(i);
    }
}

/**
 * Calculate and display clock time
 */
void clock_calculate_HMS(void) {
    // Get time in seconds
    uint64_t total_seconds = to_us_since_boot(get_absolute_time()) / 1000000ULL;
    
    // Add offset
    int64_t adjusted_seconds = (int64_t)total_seconds + time_offset_seconds;
    
    // Extract time components
    int hours = (adjusted_seconds / 3600) % 24;
    int minutes = (adjusted_seconds / 60) % 60;
    int seconds = adjusted_seconds % 60;
    
    // Handle negative offset
    if (hours < 0) hours += 24;
    if (minutes < 0) minutes += 60;
    if (seconds < 0) seconds += 60;

    //>> 16 shifts right by 16 bits = divides by 65536 seconds (~less than 18 hours)
    //& 0x7 masks to 3 bits = gives values 0-7
    //% 5 reduces to 0-4 range
    uint32_t pos_offset = ((adjusted_seconds >> 16) & 0x7) % 5;

    for(uint8_t i = 0; i < 4; i++) {
        if(pos_offset-i > 0) {
            set_digit_value(i, 10);
        }
        set_digit_value(i, 10);  // blank all digits initially
    }
    for(uint8_t i = 9; i < 13; i++) {
        if(pos_offset-i > 0) {
            set_digit_value(i, 10);
        }
        set_digit_value(i, 10);  // blank all digits initially
    }


    set_digit_value(4-pos_offset, 10);
    set_digit_value(5-pos_offset, hours / 10);
    set_digit_value(6-pos_offset, hours % 10);
    set_digit_value(7-pos_offset, 10);
    set_digit_value(8-pos_offset, minutes / 10);
    set_digit_value(9-pos_offset, minutes % 10);
    set_digit_value(10-pos_offset, 10);
    set_digit_value(11-pos_offset, seconds / 10);
    set_digit_value(12-pos_offset, seconds % 10);
}


int main(void) {
    init_hardware();

    special_m_enabled = false;
    special_dash_enabled = false;
    special_e_enabled = false;
    
    set_brightness(10);
    
    printf("Clock started\n");
    
    // Main loop
    while (true) {
        // Handle button inputs
        handle_time_buttons();
        handle_brightness_buttons();
        
        // Update and display clock
        clock_calculate_HMS();
        refresh_display();
    }
    
    return 0;
}
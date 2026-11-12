/* ECE370 Smart Lighting System - MSP430FR6989.
 * Build setup and hardware assumptions: see BUILD_NOTES.md.
 */
#include <msp430fr6989.h>
#include <stdint.h>
#include <string.h>
#include "GrLib/grlib/grlib.h"
#include "LcdDriver/lcd_driver.h"
#include "LcdDriver/middle_driver.h"

#define SENSOR_ADDRESS 0x44u
#define SENSOR_CONFIG  0xC610u /* auto range, 100 ms, continuous, latched */
#define SECOND_TICKS   100u
#define I2C_LIMIT      20000UL
#define RED            BIT0
#define BOOSTER_RED    BIT6 /* P2.6 / J4.39; J5 must select the RGB red channel. */
#define GREEN          BIT7
#define S1             BIT1
#define S2             BIT2

static volatile uint16_t ticks;
static volatile uint8_t button_events;
static volatile uint8_t pwm_level = 4;
static Graphics_Context screen;
static uint8_t auto_mode = 1, manual_on, samples, avg_valid, sensor_error;
static uint8_t sensor_ready;
enum {
    SENSOR_OK, I2C_NO_ACK, I2C_TIMEOUT, I2C_BUS_BUSY, I2C_ARBITRATION,
    SENSOR_NOT_READY, SENSOR_OVERFLOW, SAMPLE_MISSED, SENSOR_CONFIG_ERROR,
    SENSOR_ID_ERROR
};
static uint8_t bus_error;
static uint16_t last_sensor_config;
static uint8_t config_seen;
static uint16_t manufacturer_id, device_id;
static uint32_t lux100, sum100, average100;
static uint16_t next_sample;

static void clock_init(void)
{
    CSCTL0_H = CSKEY_H;
    CSCTL1 = DCOFSEL_6; /* 8 MHz MCLK; DCORSEL=0. */
    CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3 = DIVA__1 | DIVS__8 | DIVM__1; /* Keep SMCLK at 1 MHz. */
    CSCTL0_H = 0;
}

/* Corrected implementations of the Lab 9 HAL; exclude middle_driver.c. */
void HAL_LCD_PortInit(void)
{
    P1SEL0 |= BIT4 | BIT6;
    P1SEL1 &= ~(BIT4 | BIT6);
    P9SEL0 &= ~BIT4; P9SEL1 &= ~BIT4;
    P9OUT |= BIT4; P9DIR |= BIT4;
    P2SEL0 &= ~(BIT3 | BIT5 | BIT6);
    P2SEL1 &= ~(BIT3 | BIT5 | BIT6);
    P2OUT = (P2OUT & ~(BIT3 | BIT5)) | BIT6;
    P2DIR |= BIT3 | BIT5 | BIT6;
}

void HAL_LCD_SpiInit(void)
{
    UCB0CTLW0 = UCSWRST | UCCKPH | UCMSB | UCMST | UCSYNC | UCSSEL__SMCLK;
    UCB0BRW = 1;
    UCB0CTLW0 &= ~UCSWRST;
}

void HAL_LCD_writeCommand(uint8_t command)
{
    while (UCB0STATW & UCBUSY) { }
    P2OUT &= ~BIT3;
    UCB0TXBUF = command;
    while (UCB0STATW & UCBUSY) { }
    P2OUT |= BIT3; /* Only change D/C after the last clock edge. */
}

void HAL_LCD_writeData(uint8_t data)
{
    while (UCB0STATW & UCBUSY) { }
    P2OUT |= BIT3;
    UCB0TXBUF = data;
    while (UCB0STATW & UCBUSY) { }
}

static void lcd_init(void)
{
    /* Use the Lab 9 controller settings with millisecond reset/sleep delays.
     * Its original Init uses 120/200 CPU cycles, too short for sleep-out.
     * All startup delays occur before the application timers are started.
     */
    HAL_LCD_PortInit();
    HAL_LCD_SpiInit();
    P9OUT &= ~BIT4; __delay_cycles(80000);   /* 10 ms at 8 MHz */
    P9OUT |= BIT4;  __delay_cycles(960000);  /* 120 ms */
    HAL_LCD_writeCommand(CM_SWRESET); __delay_cycles(1200000); /* 150 ms */
    HAL_LCD_writeCommand(CM_SLPOUT);  __delay_cycles(960000);  /* 120 ms */
    HAL_LCD_writeCommand(CM_GAMSET); HAL_LCD_writeData(0x04);
    HAL_LCD_writeCommand(CM_SETPWCTR);
    HAL_LCD_writeData(0x0A); HAL_LCD_writeData(0x14);
    HAL_LCD_writeCommand(CM_SETSTBA);
    HAL_LCD_writeData(0x0A); HAL_LCD_writeData(0x00);
    HAL_LCD_writeCommand(CM_COLMOD); HAL_LCD_writeData(0x05);
    Lcd_ScreenWidth = 128; Lcd_ScreenHeigth = 128;
    Lcd_PenSolid = 0; Lcd_FontSolid = 1; Lcd_FlagRead = 0;
    Lcd_TouchTrim = 0;
    Crystalfontz128x128_SetOrientation(LCD_ORIENTATION_UP);
    HAL_LCD_writeCommand(CM_NORON);
    HAL_LCD_writeCommand(CM_DISPON);
    Graphics_initContext(&screen, &g_sCrystalfontz128x128);
    Graphics_setBackgroundColor(&screen, GRAPHICS_COLOR_BLACK);
    Graphics_setForegroundColor(&screen, GRAPHICS_COLOR_WHITE);
    Graphics_setFont(&screen, &g_sFontFixed6x8);
    Graphics_clearDisplay(&screen);
}

static void line(uint8_t y, const char *text)
{
    static char previous[7][22];
    static uint8_t drawn;
    uint8_t row = (uint8_t)((y - 4u) / 18u);
    uint8_t mask = (uint8_t)(1u << row);
    Graphics_Rectangle r = {0, 0, 127, 0};
    /* Avoid repainting the title, mode, unchanged average and instructions
     * for every sensor sample. All seven screen rows have fixed positions.
     */
    if ((drawn & mask) && strcmp(previous[row], text) == 0) return;
    strncpy(previous[row], text, sizeof previous[row] - 1);
    previous[row][sizeof previous[row] - 1] = 0;
    drawn |= mask;
    r.yMin = y; r.yMax = y + 9;
    Graphics_setForegroundColor(&screen, GRAPHICS_COLOR_BLACK);
    Graphics_fillRectangle(&screen, &r);
    Graphics_setForegroundColor(&screen, GRAPHICS_COLOR_WHITE);
    Graphics_drawString(&screen, (int8_t *)text, AUTO_STRING_LENGTH,
                        0, y, OPAQUE_TEXT);
}

/* Bounded formatting: largest OPT3001 result is 83,865.60 lux. */
static void number_line(uint8_t y, const char *label, uint32_t value,
                        uint8_t decimal)
{
    char text[22], digits[10];
    uint8_t n = 0, d = 0;
    uint32_t whole = decimal ? value / 100UL : value;
    while (*label && n < 10) text[n++] = *label++;
    do { digits[d++] = (char)('0' + whole % 10UL); whole /= 10UL; }
    while (whole && d < sizeof digits);
    while (d) text[n++] = digits[--d];
    if (decimal) {
        text[n++] = '.';
        text[n++] = (char)('0' + (value / 10UL) % 10UL);
        text[n++] = (char)('0' + value % 10UL);
    }
    text[n] = 0;
    line(y, text);
}

static void hex_line(uint8_t y, const char *label, uint16_t value)
{
    char text[22];
    static const char hex[] = "0123456789ABCDEF";
    uint8_t n = 0, i;
    while (*label && n < 14) text[n++] = *label++;
    for (i = 0; i < 4; ++i)
        text[n++] = hex[(value >> (12 - 4 * i)) & 15u];
    text[n] = 0;
    line(y, text);
}

static void display(void)
{
    line(4, "SMART LIGHTING");
    line(22, auto_mode ? "Mode: AUTO" : "Mode: MANUAL");
    if (auto_mode) {
        if (sensor_error) {
            switch (sensor_error) {
            case I2C_NO_ACK: line(40, "I2C: NO ACK"); break;
            case I2C_TIMEOUT: line(40, "I2C: TIMEOUT"); break;
            case I2C_BUS_BUSY: line(40, "I2C: BUS BUSY"); break;
            case I2C_ARBITRATION: line(40, "I2C: ARBITRATION"); break;
            case SENSOR_NOT_READY: line(40, "SENSOR: NOT READY"); break;
            case SENSOR_OVERFLOW: line(40, "SENSOR: OVERFLOW"); break;
            case SENSOR_CONFIG_ERROR: line(40, "SENSOR: CONFIG ERROR"); break;
            case SENSOR_ID_ERROR: line(40, "SENSOR: ID ERROR"); break;
            default: line(40, "SAMPLE DEADLINE MISS"); break;
            }
        }
        else if (samples || avg_valid) number_line(40, "Lux: ", lux100, 1);
        else line(40, "Lux: waiting");
        if (sensor_error == SENSOR_ID_ERROR)
            hex_line(58, "MFG: 0x", manufacturer_id);
        else if (sensor_error && config_seen)
            hex_line(58, "Config: 0x", last_sensor_config);
        else if (avg_valid) number_line(58, "Avg: ", average100, 1);
        else line(58, "Avg: collecting");
        if (sensor_error == SENSOR_ID_ERROR)
            hex_line(76, "DEV: 0x", device_id);
        else number_line(76, "Level: ", pwm_level, 0);
        number_line(94, "Samples: ", samples, 0);
    } else {
        line(40, manual_on ? "Red LED: ON" : "Red LED: OFF");
        line(58, "S2: toggle LED"); line(76, ""); line(94, "");
    }
    line(112, "S1: change mode");
}

static void Initialize_I2C(void)
{
    P4SEL0 &= ~(BIT0 | BIT1);
    P4SEL1 |= BIT0 | BIT1; /* secondary function = UCB1SDA/UCB1SCL */
    UCB1CTLW0 = UCSWRST | UCMST | UCMODE_3 | UCSYNC | UCSSEL__SMCLK;
    UCB1BRW = 64; /* Explicit project requirement: NOT 8. ~15.625 kHz. */
    UCB1CTLW1 = UCASTP_2;
    UCB1TBCNT = 1;
    UCB1I2CSA = SENSOR_ADDRESS;
    UCB1IE = 0; /* Bounded foreground polling; no I2C ISR needed. */
    UCB1CTLW0 &= ~UCSWRST;
}

static uint8_t bus_idle(void)
{
    uint32_t guard = I2C_LIMIT;
    while ((UCB1CTLW0 & UCTXSTP) || (UCB1STATW & UCBBUSY))
        if (--guard == 0) { bus_error = I2C_BUS_BUSY; return 0; }
    return 1;
}

static uint8_t wait_flag(uint16_t flag)
{
    uint32_t guard = I2C_LIMIT;
    while (!(UCB1IFG & flag)) {
        if (UCB1IFG & UCNACKIFG) { bus_error = I2C_NO_ACK; return 0; }
        if (UCB1IFG & UCALIFG) { bus_error = I2C_ARBITRATION; return 0; }
        if (--guard == 0) { bus_error = I2C_TIMEOUT; return 0; }
    }
    if (UCB1IFG & UCNACKIFG) { bus_error = I2C_NO_ACK; return 0; }
    if (UCB1IFG & UCALIFG) { bus_error = I2C_ARBITRATION; return 0; }
    return 1;
}

static uint8_t i2c_fail(void)
{
    uint32_t guard = I2C_LIMIT;
    if (UCB1IFG & UCNACKIFG) bus_error = I2C_NO_ACK;
    else if (UCB1IFG & UCALIFG) bus_error = I2C_ARBITRATION;
    else if (!bus_error) bus_error = I2C_TIMEOUT;
    UCB1CTLW0 |= UCTXSTP;
    while ((UCB1CTLW0 & UCTXSTP) && --guard) { }
    Initialize_I2C(); /* Software reset; stuck physical bus is reported, not hung. */
    return 0;
}

static uint8_t start_counted(uint8_t count, uint8_t transmit)
{
    bus_error = SENSOR_OK;
    if (!bus_idle()) return i2c_fail();
    /* Program the byte counter only in reset. Clear stale RX/TX/STOP flags.
     * Hardware generates the final NACK and STOP at the exact byte boundary.
     */
    UCB1CTLW0 = UCSWRST | UCMST | UCMODE_3 | UCSYNC | UCSSEL__SMCLK;
    if (transmit) UCB1CTLW0 |= UCTR;
    UCB1CTLW1 = UCASTP_2;
    UCB1TBCNT = count;
    UCB1BRW = 64;
    UCB1I2CSA = SENSOR_ADDRESS;
    UCB1IE = 0;
    UCB1IFG = 0;
    UCB1CTLW0 &= ~UCSWRST;
    UCB1CTLW0 |= UCTXSTT;
    return 1;
}

static uint8_t finish_counted(void)
{
    if (!wait_flag(UCSTPIFG)) return i2c_fail();
    if (!bus_idle()) return i2c_fail();
    return 1;
}

static uint8_t write_register(uint8_t reg, uint16_t data)
{
    if (!start_counted(3, 1)) return 0;
    if (!wait_flag(UCTXIFG0)) return i2c_fail();
    UCB1TXBUF = reg;
    if (!wait_flag(UCTXIFG0)) return i2c_fail();
    UCB1TXBUF = (uint8_t)(data >> 8);
    if (!wait_flag(UCTXIFG0)) return i2c_fail();
    UCB1TXBUF = (uint8_t)data;
    return finish_counted();
}

static uint8_t read_register(uint8_t reg, uint16_t *data)
{
    uint8_t hi, lo;
    /* The OPT3001 retains the selected register across STOP. Select it in
     * a one-byte write, then start a separate, exactly two-byte read.
     */
    if (!start_counted(1, 1)) return 0;
    if (!wait_flag(UCTXIFG0)) return i2c_fail();
    UCB1TXBUF = reg;
    if (!finish_counted()) return 0;
    if (!start_counted(2, 0)) return 0;
    if (!wait_flag(UCRXIFG0)) return i2c_fail();
    hi = UCB1RXBUF;
    if (!wait_flag(UCRXIFG0)) return i2c_fail();
    lo = UCB1RXBUF;
    if (!finish_counted()) return 0;
    *data = ((uint16_t)hi << 8) | lo;
    return 1;
}

static uint8_t sensor_setup(void)
{
    if (!read_register(0x7E, &manufacturer_id) ||
        !read_register(0x7F, &device_id)) {
        sensor_error = bus_error;
        return 0;
    }
    /* Fixed read-only words verify the link before trusting any lux data. */
    if (manufacturer_id != 0x5449u || device_id != 0x3001u) {
        sensor_error = SENSOR_ID_ERROR;
        return 0;
    }
    if (!write_register(1, SENSOR_CONFIG)) {
        sensor_error = bus_error;
        return 0;
    }
    sensor_error = SENSOR_NOT_READY;
    return 1;
}

static uint8_t measure(uint32_t *result)
{
    uint16_t config, raw;
    if (!sensor_ready) {
        sensor_ready = sensor_setup();
        return 0; /* Wait until a later second for the first conversion. */
    }
    if (!read_register(1, &config)) {
        sensor_ready = 0; sensor_error = bus_error; return 0;
    }
    last_sensor_config = config;
    config_seen = 1;
    /* Both M=10 and M=11 mean continuous conversions. Check the fields that
     * affect measurement/decoding, not the exact spelling of continuous mode
     * or the comparator/interrupt settings (the INT pin is not used here).
     */
    if ((config & 0xFC04u) != (SENSOR_CONFIG & 0xFC04u)) {
        sensor_ready = 0; sensor_error = SENSOR_CONFIG_ERROR; return 0;
    }
    /* CRF bit7: a fresh conversion; OVF bit8: out of range. */
    if (!(config & BIT7)) { sensor_error = SENSOR_NOT_READY; return 0; }
    if (config & BIT8) { sensor_error = SENSOR_OVERFLOW; return 0; }
    if (!read_register(0, &raw)) {
        sensor_ready = 0; sensor_error = bus_error; return 0;
    }
    *result = (uint32_t)(raw & 0x0FFFu) << (raw >> 12);
    return 1; /* lux = 0.01 * mantissa * 2^exponent */
}

static uint8_t level_from_sum(uint32_t sum)
{
    /* Compare the four-sample sum to avoid average-rounding boundary errors. */
    if (sum < 40000UL) return 4;
    if (sum < 80000UL) return 3;
    if (sum < 120000UL) return 2;
    if (sum < 160000UL) return 1;
    return 0;
}

static void discard_batch(void)
{
    samples = 0; sum100 = 0; avg_valid = 0;
}

static void timers_init(void)
{
    TA0CCR0 = 999; /* 1 MHz / 1000 = 1 kHz PWM */
    TA0CCR1 = 250;
    TA0CCTL0 = CCIE;
    TA0CCTL1 = 0;
    TA0CTL = TASSEL__SMCLK | MC__UP | TACLR;
    TA1CCR0 = 1249; /* 1 MHz / 8 / 1250 = 100 Hz */
    TA1CCTL0 = CCIE;
    TA1CTL = TASSEL__SMCLK | ID__8 | MC__UP | TACLR;
}

int main(void)
{
    uint16_t now, due;
    uint8_t events, dirty = 0;
    WDTCTL = WDTPW | WDTHOLD;
    clock_init();
    P1SEL0 &= ~(RED | S1 | S2); P1SEL1 &= ~(RED | S1 | S2);
    P1DIR = (P1DIR | RED) & ~(S1 | S2);
    P1OUT |= RED | S1 | S2; P1REN |= S1 | S2; P1IE &= ~(S1 | S2);
    P9SEL0 &= ~GREEN; P9SEL1 &= ~GREEN;
    P9DIR |= GREEN; P9OUT |= GREEN;
    PM5CTL0 &= ~LOCKLPM5;
    lcd_init();
    Initialize_I2C();
    sensor_ready = sensor_setup();
    display(); /* Finish the initial screen BEFORE starting sample deadlines. */
    timers_init();
    next_sample = SECOND_TICKS;
    __enable_interrupt();
    for (;;) {
        __disable_interrupt();
        now = ticks;
        events = button_events; button_events = 0;
        __enable_interrupt();
        if (events & S1) {
            auto_mode ^= 1;
            discard_batch();
            sensor_error = 0;
            if (auto_mode) {
                P9OUT |= GREEN;
                pwm_level = 4;
                next_sample = now + SECOND_TICKS;
            } else {
                P9OUT &= ~GREEN;
                manual_on = 0; pwm_level = 0;
            }
            dirty = 1;
        } else if ((events & S2) && !auto_mode) {
            manual_on ^= 1; pwm_level = manual_on ? 4 : 0;
            dirty = 1;
        }
        /* Signed modular comparison is valid: serviced at least every 10 ms. */
        if (auto_mode && (int16_t)(now - next_sample) >= 0) {
            due = next_sample;
            next_sample += SECOND_TICKS;
            if ((uint16_t)(now - due) >= SECOND_TICKS) {
                /* Do not manufacture several samples from one late reading. */
                discard_batch(); sensor_error = SAMPLE_MISSED; pwm_level = 4;
                next_sample = now + SECOND_TICKS;
            } else if (measure(&lux100)) {
                sensor_error = 0;
                sum100 += lux100;
                if (++samples == 4) {
                    average100 = sum100 / 4UL;
                    pwm_level = level_from_sum(sum100);
                    avg_valid = 1;
                    samples = 0; sum100 = 0;
                }
            } else {
                discard_batch(); pwm_level = 4; /* measure supplies the reason. */
            }
            dirty = 1;
        }
        if (dirty) { display(); dirty = 0; }
        /* Atomic work check + sleep prevents lost button/tick wakeups. */
        __disable_interrupt();
        if (!button_events &&
            (!auto_mode || (int16_t)(ticks - next_sample) < 0))
            __bis_SR_register(LPM0_bits | GIE);
        else __enable_interrupt();
    }
}

#pragma vector = TIMER0_A0_VECTOR
__interrupt void pwm_period_isr(void)
{
    uint8_t level = pwm_level;
    TA0CCTL1 = 0; /* Discard previous CCR1 flag before changing duty. */
    if (level == 0) {
        P1OUT &= ~RED;
        P2OUT &= ~BOOSTER_RED;
    }
    else {
        P1OUT |= RED;
        P2OUT |= BOOSTER_RED;
        if (level < 4) {
            TA0CCR1 = (uint16_t)level * 250u;
            TA0CCTL1 = CCIE;
        }
    }
    /* PWM interrupts return directly to sleep: foreground has no work here. */
}

#pragma vector = TIMER0_A1_VECTOR
__interrupt void pwm_falling_isr(void)
{
    if (TA0IV == TA0IV_TACCR1) {
        P1OUT &= ~RED;
        P2OUT &= ~BOOSTER_RED;
    }
}

#pragma vector = TIMER1_A0_VECTOR
__interrupt void scheduler_isr(void)
{
    static uint8_t stable, candidate, count;
    uint8_t pressed = (uint8_t)(~P1IN) & (S1 | S2);
    ++ticks;
    if (pressed != candidate) { candidate = pressed; count = 1; }
    else if (count < 3) ++count;
    if (count == 3 && stable != candidate) {
        button_events |= candidate & (uint8_t)~stable;
        stable = candidate;
    }
    __bic_SR_register_on_exit(LPM0_bits);
}

/* LCD HAL pin/configuration code adapted from the user's TI-derived Lab 9.
 * Copyright (c) 2015, Texas Instruments Incorporated. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of Texas Instruments Incorporated nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

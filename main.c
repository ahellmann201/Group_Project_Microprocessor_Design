
#include <msp430fr6989.h>
#include "Grlib/grlib/grlib.h"
#include "LcdDriver/lcd_driver.h"
#include <stdio.h>

#define redLED BIT0
#define greenLED BIT7
#define BUT1 BIT1
#define BUT2 BIT2
#define SENSOR_ADDR 0x44

// Function headers
void Initialize_I2C(void);
int i2c_write_word(unsigned char i2c_address,
                   unsigned char i2c_reg, unsigned int data);
int i2c_read_word(unsigned char i2c_address,
                  unsigned char i2c_reg);
void Initialize_Timer(void);
void Initialize_PWM(void);
void Initialize_Buttons(void);
void config_ACLK_to_32KHz_crystal(void);
void Update_Display(void);
unsigned char Calculate_Brightness(unsigned long lux);

// Graphics context
Graphics_Context g_sContext;

// Shared variables
volatile unsigned char timer_flag = 0;
volatile unsigned char display_flag = 1;
volatile unsigned char auto_mode = 1;
volatile unsigned char manual_on = 0;
volatile unsigned char brightness = 0;
volatile unsigned char pwm_count = 0;

unsigned char sample_count = 0;
unsigned long lux_total = 0;
unsigned long average_lux = 0;
unsigned long current_lux = 0;

void main(void)
{
    unsigned int data;
    unsigned long lux100;

    WDTCTL = WDTPW | WDTHOLD;
    PM5CTL0 &= ~LOCKLPM5;

    // Configure LEDs
    P1DIR |= redLED;
    P1OUT &= ~redLED;

    P9DIR |= greenLED;
    P9OUT |= greenLED;

    // Configure SMCLK to 8 MHz (Lab 9)
    CSCTL0 = CSKEY;
    CSCTL3 &= ~(BIT4 | BIT5 | BIT6);
    CSCTL0_H = 0;

    config_ACLK_to_32KHz_crystal();

    // Initialize LCD (Lab 9)
    Crystalfontz128x128_Init();
    Crystalfontz128x128_SetOrientation(0);

    Graphics_initContext(&g_sContext,
                         &g_sCrystalfontz128x128);

    Graphics_setBackgroundColor(&g_sContext,
                                GRAPHICS_COLOR_DEEP_SKY_BLUE);
    Graphics_setForegroundColor(&g_sContext,
                                GRAPHICS_COLOR_DARK_ORANGE);

    Graphics_clearDisplay(&g_sContext);
    GrContextFontSet(&g_sContext, &g_sFontCm12);

    Initialize_I2C();
    Initialize_Buttons();
    Initialize_Timer();
    Initialize_PWM();

    // Configure OPT3001 (original sensor lab)
    i2c_write_word(SENSOR_ADDR, 0x01, 0x7604);

    __enable_interrupt();

    Update_Display();
    display_flag = 0;

    while(1)
    {
        if(timer_flag)
        {
            __disable_interrupt();
            timer_flag = 0;
            __enable_interrupt();

            if(auto_mode)
            {
                // Original sensor lab calculation
                data = i2c_read_word(SENSOR_ADDR, 0x00);

                if(data != 0xFFFF)
                {
                    data = data & 0x0FFF;
                    lux100 = (unsigned long)data * 128;

                    current_lux = lux100 / 100;

                    lux_total += lux100;
                    sample_count++;

                    if(sample_count >= 4)
                    {
                        average_lux =
                            (lux_total / 4) / 100;

                        brightness =
                            Calculate_Brightness(average_lux);

                        sample_count = 0;
                        lux_total = 0;
                    }

                    display_flag = 1;
                }
            }
        }

        if(display_flag)
        {
            __disable_interrupt();
            display_flag = 0;
            __enable_interrupt();

            Update_Display();
        }

        // Low power mode
        __disable_interrupt();

        if(!timer_flag && !display_flag)
        {
            __bis_SR_register(LPM0_bits | GIE);
        }
        else
        {
            __enable_interrupt();
        }
    }
}

// Determine brightness from average lux
unsigned char Calculate_Brightness(unsigned long lux)
{
    if(lux < 100)
        return 4;
    else if(lux < 200)
        return 3;
    else if(lux < 300)
        return 2;
    else if(lux < 400)
        return 1;
    else
        return 0;
}

// LCD display
void Update_Display(void)
{
    char mystring[24];
    static unsigned char previous_mode = 2;

    // Clear screen only when changing modes
    if(previous_mode != auto_mode)
    {
        Graphics_clearDisplay(&g_sContext);
        previous_mode = auto_mode;

        if(auto_mode)
        {
            Graphics_drawStringCentered(
                &g_sContext, "Mode: Auto",
                AUTO_STRING_LENGTH, 64, 15, OPAQUE_TEXT);
        }
        else
        {
            Graphics_drawStringCentered(
                &g_sContext, "Mode: Manual",
                AUTO_STRING_LENGTH, 64, 25, OPAQUE_TEXT);
        }
    }

    if(auto_mode)
    {
        sprintf(mystring, "Lux: %lu      ", current_lux);

        Graphics_drawString(
            &g_sContext, (int8_t *)mystring,
            AUTO_STRING_LENGTH, 8, 45, OPAQUE_TEXT);

        sprintf(mystring, "Avg: %lu      ", average_lux);

        Graphics_drawString(
            &g_sContext, (int8_t *)mystring,
            AUTO_STRING_LENGTH, 8, 70, OPAQUE_TEXT);

        sprintf(mystring, "Brightness: %u  ",
                (unsigned int)brightness);

        Graphics_drawString(
            &g_sContext, (int8_t *)mystring,
            AUTO_STRING_LENGTH, 8, 100, OPAQUE_TEXT);
    }
    else
    {
        Graphics_drawStringCentered(
            &g_sContext,
            manual_on ? "LED: ON " : "LED: OFF",
            AUTO_STRING_LENGTH, 64, 70, OPAQUE_TEXT);
    }
}

// One-second timer
void Initialize_Timer(void)
{
    TA0CCR0 = 32767;
    TA0CCTL0 = CCIE;
    TA0CTL = TASSEL__ACLK | MC__UP | TACLR;
}

// Software PWM timer
void Initialize_PWM(void)
{
    TA1CCR0 = 81;
    TA1CCTL0 = CCIE;
    TA1CTL = TASSEL__ACLK | MC__UP | TACLR;
}

// Configure buttons
void Initialize_Buttons(void)
{
    P1DIR &= ~(BUT1 | BUT2);
    P1REN |= (BUT1 | BUT2);
    P1OUT |= (BUT1 | BUT2);

    P1IE |= (BUT1 | BUT2);
    P1IES |= (BUT1 | BUT2);
    P1IFG &= ~(BUT1 | BUT2);
}

// One-second timer interrupt
#pragma vector = TIMER0_A0_VECTOR
__interrupt void Timer_A_ISR(void)
{
    timer_flag = 1;
    __bic_SR_register_on_exit(LPM0_bits);
}

// PWM interrupt
#pragma vector = TIMER1_A0_VECTOR
__interrupt void PWM_ISR(void)
{
    pwm_count++;

    if(pwm_count >= 4)
        pwm_count = 0;

    if(auto_mode)
    {
        if(pwm_count < brightness)
            P1OUT |= redLED;
        else
            P1OUT &= ~redLED;
    }
    else
    {
        if(manual_on)
            P1OUT |= redLED;
        else
            P1OUT &= ~redLED;
    }
}

// Button interrupts
#pragma vector = PORT1_VECTOR
__interrupt void Port1_ISR(void)
{
    if(P1IFG & BUT1)
    {
        auto_mode ^= 1;

        if(auto_mode)
        {
            P9OUT |= greenLED;
        }
        else
        {
            P9OUT &= ~greenLED;
        }

        sample_count = 0;
        lux_total = 0;
        display_flag = 1;

        P1IFG &= ~BUT1;
    }

    if(P1IFG & BUT2)
    {
        if(!auto_mode)
        {
            manual_on ^= 1;
            display_flag = 1;
        }

        P1IFG &= ~BUT2;
    }

    __bic_SR_register_on_exit(LPM0_bits);
}

// Configure ACLK to 32 KHz crystal
void config_ACLK_to_32KHz_crystal(void)
{
    PJSEL1 &= ~BIT4;
    PJSEL0 |= BIT4;

    CSCTL0 = CSKEY;

    do
    {
        CSCTL5 &= ~LFXTOFFG;
        SFRIFG1 &= ~OFIFG;
    }
    while((CSCTL5 & LFXTOFFG) != 0);

    CSCTL0_H = 0;
}

// I2C initialization from original sensor lab
void Initialize_I2C(void)
{
    UCB1CTLW0 |= UCSWRST;

    P4SEL1 |= (BIT1 | BIT0);
    P4SEL0 &= ~(BIT1 | BIT0);

    UCB1CTLW0 |= UCMODE_3 | UCMST | UCSSEL_3;

    // Required project change
    UCB1BRW = 64;

    UCB1CTLW0 &= ~UCSWRST;
}

// I2C write function from original sensor lab
int i2c_write_word(unsigned char i2c_address,
                   unsigned char i2c_reg, unsigned int data)
{
    unsigned char byte1, byte2;

    byte1 = (data >> 8) & 0xFF;
    byte2 = data & 0xFF;

    UCB1I2CSA = i2c_address;
    UCB1CTLW0 |= UCTR;
    UCB1CTLW0 |= UCTXSTT;

    while((UCB1IFG & UCTXIFG0) == 0) {}

    UCB1TXBUF = i2c_reg;

    while((UCB1CTLW0 & UCTXSTT) != 0) {}

    UCB1TXBUF = byte1;

    while((UCB1IFG & UCTXIFG0) == 0) {}

    UCB1TXBUF = byte2;

    while((UCB1IFG & UCTXIFG0) == 0) {}

    UCB1CTLW0 |= UCTXSTP;

    while((UCB1CTLW0 & UCTXSTP) != 0) {}

    return 0;
}

// I2C read function from original sensor lab
int i2c_read_word(unsigned char i2c_address,
                  unsigned char i2c_reg)
{
    unsigned char byte1, byte2;
    unsigned int data2;

    byte1 = 111;
    byte2 = 111;

    UCB1I2CSA = i2c_address;

    UCB1IFG &= ~UCTXIFG0;
    UCB1CTLW0 |= UCTR;
    UCB1CTLW0 |= UCTXSTT;

    while((UCB1IFG & UCTXIFG0) == 0) {}

    UCB1TXBUF = i2c_reg;

    while((UCB1CTLW0 & UCTXSTT) != 0) {}

    if((UCB1IFG & UCNACKIFG) != 0)
        return -1;

    UCB1CTLW0 &= ~UCTR;
    UCB1CTLW0 |= UCTXSTT;

    while((UCB1IFG & UCRXIFG0) == 0) {}

    byte1 = UCB1RXBUF;

    while((UCB1CTLW0 & UCTXSTT) != 0) {}

    UCB1CTLW0 |= UCTXSTP;

    while((UCB1IFG & UCRXIFG0) == 0) {}

    byte2 = UCB1RXBUF;

    while((UCB1CTLW0 & UCTXSTP) != 0) {}

    data2 = ((byte1 << 8) | (byte2 & 0xFF));

    return data2;
}

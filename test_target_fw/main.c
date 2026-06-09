#include <mcs51/8051.h>

/* Simple delay function in milliseconds (calibrated for 8 MHz single-cycle AT89LP core) */
void delay_ms(unsigned int ms) {
    unsigned int i, j;
    for (i = 0; i < ms; i++) {
        for (j = 0; j < 1000; j++) {
            __asm__("nop");
        }
    }
}

void main(void) {
    while (1) {
        P1_0 = 1; /* Turn P1.0 (Physical Pin 1) HIGH */
        delay_ms(2000);
        
        P1_0 = 0; /* Turn P1.0 (Physical Pin 1) LOW */
        delay_ms(500);
    }
}

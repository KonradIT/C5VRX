#pragma once

#include <stdint.h>

/* Minimal 3x5 pixel font: 5 rows of 3 bits, top row first, bit 14 = top
 * left. Covers space, digits, upper-case letters and : . - / % + ?. Lower-case
 * letters are drawn as upper case; unknown characters as a filled box. */
static inline uint16_t font3x5_rows(unsigned r0, unsigned r1, unsigned r2, unsigned r3, unsigned r4)
{
    return (uint16_t)((r0 << 12) | (r1 << 9) | (r2 << 6) | (r3 << 3) | r4);
}

static inline uint16_t font3x5_glyph(char ch)
{
    /* Each row is written as three binary digits (101 = left and right
     * pixel); B() turns that decimal-looking literal into the 3-bit value. */
#define B(x) ((((x) / 100) % 10) << 2 | (((x) / 10) % 10) << 1 | ((x) % 10))
#define G(a, b, c, d, e) font3x5_rows(B(a), B(b), B(c), B(d), B(e))
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    switch (ch) {
    case ' ': return 0;
    case '0': return G(111, 101, 101, 101, 111);
    case '1': return G(10, 110, 10, 10, 111);
    case '2': return G(111, 1, 111, 100, 111);
    case '3': return G(111, 1, 111, 1, 111);
    case '4': return G(101, 101, 111, 1, 1);
    case '5': return G(111, 100, 111, 1, 111);
    case '6': return G(111, 100, 111, 101, 111);
    case '7': return G(111, 1, 1, 10, 10);
    case '8': return G(111, 101, 111, 101, 111);
    case '9': return G(111, 101, 111, 1, 111);
    case 'A': return G(10, 101, 111, 101, 101);
    case 'B': return G(110, 101, 110, 101, 110);
    case 'C': return G(11, 100, 100, 100, 11);
    case 'D': return G(110, 101, 101, 101, 110);
    case 'E': return G(111, 100, 110, 100, 111);
    case 'F': return G(111, 100, 110, 100, 100);
    case 'G': return G(11, 100, 101, 101, 11);
    case 'H': return G(101, 101, 111, 101, 101);
    case 'I': return G(111, 10, 10, 10, 111);
    case 'J': return G(1, 1, 1, 101, 10);
    case 'K': return G(101, 101, 110, 101, 101);
    case 'L': return G(100, 100, 100, 100, 111);
    case 'M': return G(101, 111, 111, 101, 101);
    case 'N': return G(110, 101, 101, 101, 101);
    case 'O': return G(10, 101, 101, 101, 10);
    case 'P': return G(110, 101, 110, 100, 100);
    case 'Q': return G(10, 101, 101, 110, 11);
    case 'R': return G(110, 101, 110, 101, 101);
    case 'S': return G(11, 100, 10, 1, 110);
    case 'T': return G(111, 10, 10, 10, 10);
    case 'U': return G(101, 101, 101, 101, 111);
    case 'V': return G(101, 101, 101, 101, 10);
    case 'W': return G(101, 101, 111, 111, 101);
    case 'X': return G(101, 101, 10, 101, 101);
    case 'Y': return G(101, 101, 10, 10, 10);
    case 'Z': return G(111, 1, 10, 100, 111);
    case ':': return G(0, 10, 0, 10, 0);
    case '.': return G(0, 0, 0, 0, 10);
    case '-': return G(0, 0, 111, 0, 0);
    case '/': return G(1, 1, 10, 100, 100);
    case '%': return G(101, 1, 10, 100, 101);
    case '+': return G(0, 10, 111, 10, 0);
    case '?': return G(111, 1, 11, 0, 10);
    default:  return G(111, 111, 111, 111, 111);
    }
#undef G
#undef B
}

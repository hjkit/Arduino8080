//
// Intel 8080 CPU emulation on an Arduino Nano
//            derived from Z80PACK
// Copyright 2024, Udo Munk & Thomas Eberhardt
//
// History:
// 04-MAY-2024 Release 1.0 implements a very basic 8080 system
// 06-MAY-2024 Release 1.1 add support for a ROM in flash
// 07-MAY-2024 Release 1.2 move 8080 memory into a FRAM
// 13-MAY-2024 Release 1.2.1 can run MITS Altair BASIC
// 17-MAY-2024 Release 1.3 switch to code minimized CPU from Thomas Eberhardt
// 18-MAY-2024 Release 1.4 read 8080 code from a file on SD into FRAM
// 20-MAY-2024 Release 1.5.1 added interrupt key and register dump for debugging
//

//#define DEBUG // enables some debug messages

#include <SPI.h>
#include "Adafruit_FRAM_SPI.h"
#include "SdFat.h"

// unused analog pin to seed random generator
#define UAP 7
// chip select pin for FRAM (default)
#define FRAM_CS 10
// chip select pin for SD card
#define SD_CS 9
// interrupt pin, interrupt 0 is already used by the SPI stuff
// so we use interrupt 1 at the D3 pin
#define USERINT 3

// data types for the 8080 CPU
typedef unsigned char BYTE;
typedef unsigned int  WORD;

// project includes
#include "simcore.h"
#include "memsim.h"
#include "iosim.h"
#include "config.h"

// 8080 CPU state
struct cpu_state {
  BYTE A, B, C, D, E, H, L, F, IFF;
  WORD PC8, SP8; // funny names because SP already is a macro here
} cpu_state;

// other global variables
CPUState State = Running;         // CPU state
unsigned long tstates = 0;        // executed T-states

// Insure FAT16/FAT32 only
SdFat32 SD;

// Precomputed table for fast sign, zero and parity flag calculation
#define _ 0
#define S S_FLAG
#define Z Z_FLAG
#define P P_FLAG
static const BYTE szp_flags[256] = {
/*00*/ Z | P,     _,     _,     P,     _,     P,     P,     _,
/*08*/     _,     P,     P,     _,     P,     _,     _,     P,
/*10*/     _,     P,     P,     _,     P,     _,     _,     P,
/*18*/     P,     _,     _,     P,     _,     P,     P,     _,
/*20*/     _,     P,     P,     _,     P,     _,     _,     P,
/*28*/     P,     _,     _,     P,     _,     P,     P,     _,
/*30*/     P,     _,     _,     P,     _,     P,     P,     _,
/*38*/     _,     P,     P,     _,     P,     _,     _,     P,
/*40*/     _,     P,     P,     _,     P,     _,     _,     P,
/*48*/     P,     _,     _,     P,     _,     P,     P,     _,
/*50*/     P,     _,     _,     P,     _,     P,     P,     _,
/*58*/     _,     P,     P,     _,     P,     _,     _,     P,
/*60*/     P,     _,     _,     P,     _,     P,     P,     _,
/*68*/     _,     P,     P,     _,     P,     _,     _,     P,
/*70*/     _,     P,     P,     _,     P,     _,     _,     P,
/*78*/     P,     _,     _,     P,     _,     P,     P,     _,
/*80*/     S, S | P, S | P,     S, S | P,     S,     S, S | P,
/*88*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*90*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*98*/     S, S | P, S | P,     S, S | P,     S,     S, S | P,
/*a0*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*a8*/     S, S | P, S | P,     S, S | P,     S,     S, S | P,
/*b0*/     S, S | P, S | P,     S, S | P,     S,     S, S | P,
/*b8*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*c0*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*c8*/     S, S | P, S | P,     S, S | P,     S,     S, S | P,
/*d0*/     S, S | P, S | P,     S, S | P,     S,     S, S | P,
/*d8*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*e0*/     S, S | P, S | P,     S, S | P,     S,     S, S | P,
/*e8*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*f0*/ S | P,     S,     S, S | P,     S, S | P, S | P,     S,
/*f8*/     S, S | P, S | P,     S, S | P,     S,     S, S | P
};
#undef _
#undef S
#undef Z
#undef P

#define SZP_FLAGS (S_FLAG | Z_FLAG | P_FLAG)

// This function initializes the CPU into the documented
// state. Prevents assumptions about register contents
// of a just powered CPU, like HL is always 0 in the
// beginning, which is not the case with the silicon.
static void init_cpu(struct cpu_state *cpu_state)
{
  cpu_state->PC8 = 0;
  cpu_state->SP8 = random(65535);
  cpu_state->A = random(255);
  cpu_state->B = random(255);
  cpu_state->C = random(255);
  cpu_state->D = random(255);
  cpu_state->E = random(255);
  cpu_state->H = random(255);
  cpu_state->L = random(255);
  cpu_state->F = random(255);
}

// This function builds the 8080 central processing unit.
// The opcode where PC points to is fetched from the memory
// and PC incremented by one. The opcode is then dispatched
// to execute code, which emulates this 8080 opcode.
//
// For a description of how the arithmetic flags calculation works see:
// http://emulators.com/docs/lazyoverflowdetect_final.pdf
//
// The formula for subtraction carry outs was determined by staring
// intently at some truth tables.
//
// cout contains the carry outs for every bit

void cpu_8080(struct cpu_state *cpu_state)
{
  BYTE A, B, C, D, E, H, L, F, IFF;
  WORD PC8, SP8;
  BYTE t, res, cout, P;
  WORD addr;

  A = cpu_state->A;
  B = cpu_state->B;
  C = cpu_state->C;
  D = cpu_state->D;
  E = cpu_state->E;
  H = cpu_state->H;
  L = cpu_state->L;
  F = cpu_state->F;
  IFF = cpu_state->IFF;
  PC8 = cpu_state->PC8;
  SP8 = cpu_state->SP8;

  do {
    t = 4; // minimum clock cycles for M1

    switch (memrdr(PC8++)) { // execute next opcode

    case 0x00:			/* NOP */
    case 0x08:			/* NOP* */
    case 0x10:			/* NOP* */
    case 0x18:			/* NOP* */
    case 0x20:			/* NOP* */
    case 0x28:			/* NOP* */
    case 0x30:			/* NOP* */
    case 0x38:			/* NOP* */
      break;

    case 0x7f:			/* MOV A,A */
    case 0x40:			/* MOV B,B */
    case 0x49:			/* MOV C,C */
    case 0x52:			/* MOV D,D */
    case 0x5b:			/* MOV E,E */
    case 0x64:			/* MOV H,H */
    case 0x6d:			/* MOV L,L */
      t++;
      break;

    case 0x01:			/* LXI B,nn */
      C = memrdr(PC8++);
      B = memrdr(PC8++);
      t += 6;
      break;

    case 0x02:			/* STAX B */
      memwrt((B << 8) | C, A);
      t += 3;
      break;

    case 0x03:			/* INX B */
      addr = ((B << 8) | C) + 1;
      B = (addr >> 8);
      C = (addr & 0xff);
      t++;
      break;

    case 0x04:			/* INR B */
      P = B;
      res = ++B;
    finish_inr:
      cout = ((P & 1) | ((P | 1) & ~res));
      F = ((F & C_FLAG) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      t++;
      break;

    case 0x05:			/* DCR B */
      P = B;
      res = --B;
    finish_dcr:
      cout = ((~P & 1) | ((~P | 1) & res));
      F = ((F & C_FLAG) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      F ^= H_FLAG;
      t++;
      break;

    case 0x06:			/* MVI B,n */
      B = memrdr(PC8++);
      t += 3;
      break;

    case 0x07:			/* RLC */
      res = ((A & 128) >> 7) & 1;
      F = (F & ~C_FLAG) | (res << C_SHIFT);
      A = (A << 1) | res;
      break;

    case 0x09:			/* DAD B */
      addr = ((H << 8) | L);
      addr += ((B << 8) | C);
      cout = ((H & B) | ((H | B) & ~(addr >> 8)));
    finish_dad:
      F = ((F & ~C_FLAG) | (((cout >> 7) & 1) << C_SHIFT));
      H = (addr >> 8);
      L = (addr & 0xff);
      t += 6;
      break;

    case 0x0a:			/* LDAX B */
      A = memrdr((B << 8) | C);
      t += 3;
      break;

    case 0x0b:			/* DCX B */
      addr = ((B << 8) | C) - 1;
      B = (addr >> 8);
      C = (addr & 0xff);
      t++;
      break;

    case 0x0c:			/* INR C */
      P = C;
      res = ++C;
      goto finish_inr;

    case 0x0d:			/* DCR C */
      P = C;
      res = --C;
      goto finish_dcr;

    case 0x0e:			/* MVI C,n */
      C = memrdr(PC8++);
      t += 3;
      break;

    case 0x0f:			/* RRC */
      res = A & 1;
      F = (F & ~C_FLAG) | (res << C_SHIFT);
      A = (A >> 1) | (res << 7);
      break;

    case 0x11:			/* LXI D,nn */
      E = memrdr(PC8++);
      D = memrdr(PC8++);
      t += 6;
      break;

    case 0x12:			/* STAX D */
      memwrt((D << 8) | E, A);
      t += 3;
      break;

    case 0x13:			/* INX D */
      addr = ((D << 8) | E) + 1;
      D = (addr >> 8);
      E = (addr & 0xff);
      t++;
      break;

    case 0x14:			/* INR D */
      P = D;
      res = ++D;
      goto finish_inr;

    case 0x15:			/* DCR D */
      P = D;
      res = --D;
      goto finish_dcr;

    case 0x16:			/* MVI D,n */
      D = memrdr(PC8++);
      t += 3;
      break;

    case 0x17:			/* RAL */
      res = (F >> C_SHIFT) & 1;
      F = (F & ~C_FLAG) | ((((A & 128) >> 7) & 1) << C_SHIFT);
      A = (A << 1) | res;
      break;

    case 0x19:			/* DAD D */
      addr = ((H << 8) | L);
      addr += ((D << 8) | E);
      cout = ((H & D) | ((H | D) & ~(addr >> 8)));
      goto finish_dad;

    case 0x1a:			/* LDAX D */
      A = memrdr((D << 8) | E);
      t += 3;
      break;

    case 0x1b:			/* DCX D */
      addr = ((D << 8) | E) - 1;
      D = (addr >> 8);
      E = (addr & 0xff);
      t++;
      break;

    case 0x1c:			/* INR E */
      P = E;
      res = ++E;
      goto finish_inr;

    case 0x1d:			/* DCR E */
      P = E;
      res = --E;
      goto finish_dcr;

    case 0x1e:			/* MVI E,n */
      E = memrdr(PC8++);
      t += 3;
      break;

    case 0x1f:			/* RAR */
      res = (F >> C_SHIFT) & 1;
      F = (F & ~C_FLAG) | ((A & 1) << C_SHIFT);
      A = (A >> 1) | (res << 7);
      break;

    case 0x21:			/* LXI H,nn */
      L = memrdr(PC8++);
      H = memrdr(PC8++);
      t += 6;
      break;

    case 0x22:			/* SHLD nn */
      addr = memrdr(PC8++);
      addr |= memrdr(PC8++) << 8;
      memwrt(addr, L);
      memwrt(addr + 1, H);
      t += 12;
      break;

    case 0x23:			/* INX H */
      addr = ((H << 8) | L) + 1;
      H = (addr >> 8);
      L = (addr & 0xff);
      t++;
      break;

    case 0x24:			/* INR H */
      P = H;
      res = ++H;
      goto finish_inr;

    case 0x25:			/* DCR H */
      P = H;
      res = --H;
      goto finish_dcr;

    case 0x26:			/* MVI H,n */
      H = memrdr(PC8++);
      t += 3;
      break;

// Rewrote DAA after looking at
// https://zeptobars.com/en/read/KR580VM80A-intel-i8080-verilog-reverse-engineering
// in the verilog file.
// It is just a normal addition with a special operand setup and the carry flag
// is set to the same value as the condition of the second "if". That is what
// makes DAA a bit strange.

    case 0x27:			/* DAA */
      P = 0;
      if (((A & 0xf) > 9) || (F & H_FLAG))
        P |= 0x06;
      if ((A > 0x99) || (F & C_FLAG))
        P |= 0x60;
      res = A + P;
      cout = ((A & P) | ((A | P) & ~res));
      F = (((A > 0x99) << C_SHIFT) | (F & C_FLAG) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      A = res;
      break;

    case 0x29:			/* DAD H */
      addr = ((H << 8) | L) << 1;
      cout = (H | (H & ~(addr >> 8)));
      goto finish_dad;

    case 0x2a:			/* LHLD nn */
      addr = memrdr(PC8++);
      addr |= memrdr(PC8++) << 8;
      L = memrdr(addr);
      H = memrdr(addr + 1);
      t += 12;
      break;

    case 0x2b:			/* DCX H */
      addr = ((H << 8) | L) - 1;
      H = (addr >> 8);
      L = (addr & 0xff);
      t++;
      break;

    case 0x2c:			/* INR L */
      P = L;
      res = ++L;
      goto finish_inr;

    case 0x2d:			/* DCR L */
      P = L;
      res = --L;
      goto finish_dcr;

    case 0x2e:			/* MVI L,n */
      L = memrdr(PC8++);
      t += 3;
      break;

    case 0x2f:			/* CMA */
      A = ~A;
      break;

    case 0x31:			/* LXI SP,nn */
      SP8 = memrdr(PC8++);
      SP8 |= memrdr(PC8++) << 8;
      t += 6;
      break;

    case 0x32:			/* STA nn */
      addr = memrdr(PC8++);
      addr |= memrdr(PC8++) << 8;
      memwrt(addr, A);
      t += 9;
      break;

    case 0x33:			/* INX SP */
      SP8++;
      t++;
      break;

    case 0x34:			/* INR M */
      addr = (H << 8) | L;
      P = memrdr(addr);
      res = P + 1;
      memwrt(addr, res);
      t += 5;
      goto finish_inr;

    case 0x35:			/* DCR M */
      addr = (H << 8) | L;
      P = memrdr(addr);
      res = P - 1;
      memwrt(addr, res);
      t += 5;
      goto finish_dcr;

    case 0x36:			/* MVI M,n */
      memwrt((H << 8) | L, memrdr(PC8++));
      t += 6;
      break;

    case 0x37:			/* STC */
      F |= C_FLAG;
      break;

    case 0x39:			/* DAD SP */
      addr = ((H << 8) | L) + SP8;
      cout = ((H & (SP8 >> 8)) | ((H | (SP8 >> 8)) & ~(addr >> 8)));
      goto finish_dad;

    case 0x3a:			/* LDA nn */
      addr = memrdr(PC8++);
      addr |= memrdr(PC8++) << 8;
      A = memrdr(addr);
      t += 9;
      break;

    case 0x3b:			/* DCX SP */
      SP8--;
      t++;
      break;

    case 0x3c:			/* INR A */
      P = A;
      res = ++A;
      goto finish_inr;

    case 0x3d:			/* DCR A */
      P = A;
      res = --A;
      goto finish_dcr;

    case 0x3e:			/* MVI A,n */
      A = memrdr(PC8++);
      t += 3;
      break;

    case 0x3f:			/* CMC */
      F ^= C_FLAG;
      break;

    case 0x41:			/* MOV B,C */
      B = C;
      t++;
      break;

    case 0x42:			/* MOV B,D */
      B = D;
      t++;
      break;

    case 0x43:			/* MOV B,E */
      B = E;
      t++;
      break;

    case 0x44:			/* MOV B,H */
      B = H;
      t++;
      break;

    case 0x45:			/* MOV B,L */
      B = L;
      t++;
      break;

    case 0x46:			/* MOV B,M */
      B = memrdr((H << 8) | L);
      t += 3;
      break;

    case 0x47:			/* MOV B,A */
      B = A;
      t++;
      break;

    case 0x48:			/* MOV C,B */
      C = B;
      t++;
      break;

    case 0x4a:			/* MOV C,D */
      C = D;
      t++;
      break;

    case 0x4b:			/* MOV C,E */
      C = E;
      t++;
      break;

    case 0x4c:			/* MOV C,H */
      C = H;
      t++;
      break;

    case 0x4d:			/* MOV C,L */
      C = L;
      t++;
      break;

    case 0x4e:			/* MOV C,M */
      C = memrdr((H << 8) | L);
      t += 3;
      break;

    case 0x4f:			/* MOV C,A */
      C = A;
      t++;
      break;

    case 0x50:			/* MOV D,B */
      D = B;
      t++;
      break;

    case 0x51:			/* MOV D,C */
      D = C;
      t++;
      break;

    case 0x53:			/* MOV D,E */
      D = E;
      t++;
      break;

    case 0x54:			/* MOV D,H */
      D = H;
      t++;
      break;

    case 0x55:			/* MOV D,L */
      D = L;
      t++;
      break;

    case 0x56:			/* MOV D,M */
      D = memrdr((H << 8) | L);
      t += 3;
      break;

    case 0x57:			/* MOV D,A */
      D = A;
      t++;
      break;

    case 0x58:			/* MOV E,B */
      E = B;
      t++;
      break;

    case 0x59:			/* MOV E,C */
      E = C;
      t++;
      break;

    case 0x5a:			/* MOV E,D */
      E = D;
      t++;
      break;

    case 0x5c:			/* MOV E,H */
      E = H;
      t++;
      break;

    case 0x5d:			/* MOV E,L */
      E = L;
      t++;
      break;

    case 0x5e:			/* MOV E,M */
      E = memrdr((H << 8) | L);
      t += 3;
      break;

    case 0x5f:			/* MOV E,A */
      E = A;
      t++;
      break;

    case 0x60:			/* MOV H,B */
      H = B;
      t++;
      break;

    case 0x61:			/* MOV H,C */
      H = C;
      t++;
      break;

    case 0x62:			/* MOV H,D */
      H = D;
      t++;
      break;

    case 0x63:			/* MOV H,E */
      H = E;
      t++;
      break;

    case 0x65:			/* MOV H,L */
      H = L;
      t++;
      break;

    case 0x66:			/* MOV H,M */
      H = memrdr((H << 8) | L);
      t += 3;
      break;

    case 0x67:			/* MOV H,A */
      H = A;
      t++;
      break;

    case 0x68:			/* MOV L,B */
      L = B;
      t++;
      break;

    case 0x69:			/* MOV L,C */
      L = C;
      t++;
      break;

    case 0x6a:			/* MOV L,D */
      L = D;
      t++;
      break;

    case 0x6b:			/* MOV L,E */
      L = E;
      t++;
      break;

    case 0x6c:			/* MOV L,H */
      L = H;
      t++;
      break;

    case 0x6e:			/* MOV L,M */
      L = memrdr((H << 8) | L);
      t += 3;
      break;

    case 0x6f:			/* MOV L,A */
      L = A;
      t++;
      break;

    case 0x70:			/* MOV M,B */
      memwrt((H << 8) | L, B);
      t += 3;
      break;

    case 0x71:			/* MOV M,C */
      memwrt((H << 8) | L, C);
      t += 3;
      break;

    case 0x72:			/* MOV M,D */
      memwrt((H << 8) | L, D);
      t += 3;
      break;

    case 0x73:			/* MOV M,E */
      memwrt((H << 8) | L, E);
      t += 3;
      break;

    case 0x74:			/* MOV M,H */
      memwrt((H << 8) | L, H);
      t += 3;
      break;

    case 0x75:			/* MOV M,L */
      memwrt((H << 8) | L, L);
      t += 3;
      break;

    case 0x76:			/* HLT */
      // cry and die, no interrupts yet
      State = Halted;
      t += 3;
      break;

    case 0x77:			/* MOV M,A */
      memwrt((H << 8) | L, A);
      t += 3;
      break;

    case 0x78:			/* MOV A,B */
      A = B;
      t++;
      break;

    case 0x79:			/* MOV A,C */
      A = C;
      t++;
      break;

    case 0x7a:			/* MOV A,D */
      A = D;
      t++;
      break;

    case 0x7b:			/* MOV A,E */
      A = E;
      t++;
      break;

    case 0x7c:			/* MOV A,H */
      A = H;
      t++;
      break;

    case 0x7d:			/* MOV A,L */
      A = L;
      t++;
      break;

    case 0x7e:			/* MOV A,M */
      A = memrdr((H << 8) | L);
      t += 3;
      break;

    case 0x80:			/* ADD B */
      P = B;
    finish_add:
      res = A + P;
      cout = ((A & P) | ((A | P) & ~res));
      F = ((((cout >> 7) & 1) << C_SHIFT) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      A = res;
      break;

    case 0x81:			/* ADD C */
      P = C;
      goto finish_add;

    case 0x82:			/* ADD D */
      P = D;
      goto finish_add;

    case 0x83:			/* ADD E */
      P = E;
      goto finish_add;

    case 0x84:			/* ADD H */
      P = H;
      goto finish_add;

    case 0x85:			/* ADD L */
      P = L;
      goto finish_add;

    case 0x86:			/* ADD M */
      P = memrdr((H << 8) | L);
      t += 3;
      goto finish_add;

    case 0x87:			/* ADD A */
      P = A;
      goto finish_add;

    case 0x88:			/* ADC B */
      P = B;
    finish_adc:
      res = A + P + ((F >> C_SHIFT) & 1);
      cout = ((A & P) | ((A | P) & ~res));
      F = ((((cout >> 7) & 1) << C_SHIFT) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      A = res;
      break;

    case 0x89:			/* ADC C */
      P = C;
      goto finish_adc;

    case 0x8a:			/* ADC D */
      P = D;
      goto finish_adc;

    case 0x8b:			/* ADC E */
      P = E;
      goto finish_adc;

    case 0x8c:			/* ADC H */
      P = H;
      goto finish_adc;

    case 0x8d:			/* ADC L */
      P = L;
      goto finish_adc;

    case 0x8e:			/* ADC M */
      P = memrdr((H << 8) | L);
      t += 3;
      goto finish_adc;

    case 0x8f:			/* ADC A */
      P = A;
      goto finish_adc;

    case 0x90:			/* SUB B */
      P = B;
    finish_sub:
      res = A - P;
      cout = ((~A & P) | ((~A | P) & res));
      F = ((((cout >> 7) & 1) << C_SHIFT) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      F ^= H_FLAG;
      A = res;
      break;

    case 0x91:			/* SUB C */
      P = C;
      goto finish_sub;

    case 0x92:			/* SUB D */
      P = D;
      goto finish_sub;

    case 0x93:			/* SUB E */
      P = E;
      goto finish_sub;

    case 0x94:			/* SUB H */
      P = H;
      goto finish_sub;

    case 0x95:			/* SUB L */
      P = L;
      goto finish_sub;

    case 0x96:			/* SUB M */
      P = memrdr((H << 8) | L);
      t += 3;
      goto finish_sub;

    case 0x97:			/* SUB A */
      F = Z_FLAG | H_FLAG | P_FLAG;
      // S_FLAG and C_FLAG cleared
      A = 0;
      break;

    case 0x98:			/* SBB B */
      P = B;
    finish_sbb:
      res = A - P - ((F >> C_SHIFT) & 1);
      cout = ((~A & P) | ((~A | P) & res));
      F = ((((cout >> 7) & 1) << C_SHIFT) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      F ^= H_FLAG;
      A = res;
      break;

    case 0x99:			/* SBB C */
      P = C;
      goto finish_sbb;

    case 0x9a:			/* SBB D */
      P = D;
      goto finish_sbb;

    case 0x9b:			/* SBB E */
      P = E;
      goto finish_sbb;

    case 0x9c:			/* SBB H */
      P = H;
      goto finish_sbb;

    case 0x9d:			/* SBB L */
      P = L;
      goto finish_sbb;

    case 0x9e:			/* SBB M */
      P = memrdr((H << 8) | L);
      t += 3;
      goto finish_sbb;

    case 0x9f:			/* SBB A */
      P = A;
      goto finish_sbb;

    case 0xa0:			/* ANA B */
      P = B;
    finish_ana:
      res = A & P;
      F = (((((A | P) >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      // C_FLAG cleared
      A = res;
      break;

    case 0xa1:			/* ANA C */
      P = C;
      goto finish_ana;

    case 0xa2:			/* ANA D */
      P = D;
      goto finish_ana;

    case 0xa3:			/* ANA E */
      P = E;
      goto finish_ana;

    case 0xa4:			/* ANA H */
      P = H;
      goto finish_ana;

    case 0xa5:			/* ANA L */
      P = L;
      goto finish_ana;

    case 0xa6:			/* ANA M */
      P = memrdr((H << 8) | L);
      t += 3;
      goto finish_ana;

    case 0xa7:			/* ANA A */
      P = A;
      goto finish_ana;

    case 0xa8:			/* XRA B */
      A ^= B;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xa9:			/* XRA C */
      A ^= C;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xaa:			/* XRA D */
      A ^= D;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xab:			/* XRA E */
      A ^= E;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xac:			/* XRA H */
      A ^= H;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xad:			/* XRA L */
      A ^= L;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xae:			/* XRA M */
      A ^= memrdr((H << 8) | L);
      t += 3;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xaf:			/* XRA A */
      A = 0;
      F = Z_FLAG | P_FLAG;
      // S_FLAG, H_FLAG and C_FLAG cleared
      break;

    case 0xb0:			/* ORA B */
      A |= B;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb1:			/* ORA C */
      A |= C;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb2:			/* ORA D */
      A |= D;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb3:			/* ORA E */
      A |= E;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb4:			/* ORA H */
      A |= H;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb5:			/* ORA L */
      A |= L;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb6:			/* ORA M */
      A |= memrdr((H << 8) | L);
      t += 3;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb7:			/* ORA A */
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xb8:			/* CMP B */
      P = B;
    finish_cmp:
      res = A - P;
      cout = ((~A & P) | ((~A | P) & res));
      F = ((((cout >> 7) & 1) << C_SHIFT) |
           (((cout >> 3) & 1) << H_SHIFT) |
           szp_flags[res]);
      F ^= H_FLAG;
      break;

    case 0xb9:			/* CMP C */
      P = C;
      goto finish_cmp;

    case 0xba:			/* CMP D */
      P = D;
      goto finish_cmp;

    case 0xbb:			/* CMP E */
      P = E;
      goto finish_cmp;

    case 0xbc:			/* CMP H */
      P = H;
      goto finish_cmp;

    case 0xbd:			/* CMP L */
      P = L;
      goto finish_cmp;

    case 0xbe:			/* CMP M */
      P = memrdr((H << 8) | L);
      t += 3;
      goto finish_cmp;

    case 0xbf:			/* CMP A */
      F = Z_FLAG | H_FLAG | P_FLAG;
      // S_FLAG and C_FLAG cleared
      break;

    case 0xc0:			/* RNZ */
      res = !(F & Z_FLAG);
    finish_retc:
      t++;
      if (res)
        goto finish_ret;
      break;

    case 0xc1:			/* POP B */
      C = memrdr(SP8++);
      B = memrdr(SP8++);
      t += 6;
      break;

    case 0xc2:			/* JNZ nn */
      res = !(F & Z_FLAG);
    finish_jmpc:
      addr = memrdr(PC8++);
      addr |= memrdr(PC8++) << 8;
      t += 6;
      if (res)
        PC8 = addr;
      break;

    case 0xc3:			/* JMP nn */
    case 0xcb:			/* JMP* nn */
      addr = memrdr(PC8++);
      addr |= memrdr(PC8) << 8;
      t += 6;
      PC8 = addr;
      break;

    case 0xc4:			/* CNZ nn */
      res = !(F & Z_FLAG);
    finish_callc:
      addr = memrdr(PC8++);
      addr |= memrdr(PC8++) << 8;
      t += 7;
      if (res)
        goto finish_call;
      break;

    case 0xc5:			/* PUSH B */
      memwrt(--SP8, B);
      memwrt(--SP8, C);
      t += 7;
      break;

    case 0xc6:			/* ADI n */
      P = memrdr(PC8++);
      t += 3;
      goto finish_add;

    case 0xc7:			/* RST 0 */
      addr = 0;
      t++;
      goto finish_call;

    case 0xc8:			/* RZ */
      res = (F & Z_FLAG);
      goto finish_retc;

    case 0xc9:			/* RET */
    case 0xd9:			/* RET* */
    finish_ret:
      addr = memrdr(SP8++);
      addr |= memrdr(SP8++) << 8;
      t += 6;
      PC8 = addr;
      break;

    case 0xca:			/* JZ nn */
      res = (F & Z_FLAG);
      goto finish_jmpc;

    case 0xcc:			/* CZ nn */
      res = (F & Z_FLAG);
      goto finish_callc;

    case 0xcd:			/* CALL nn */
    case 0xdd:			/* CALL* nn */
    case 0xed:			/* CALL* nn */
    case 0xfd:			/* CALL* nn */
      addr = memrdr(PC8++);
      addr |= memrdr(PC8++) << 8;
      t += 7;
    finish_call:
      memwrt(--SP8, PC8 >> 8);
      memwrt(--SP8, PC8);
      t += 6;
      PC8 = addr;
      break;

    case 0xce:			/* ACI n */
      P = memrdr(PC8++);
      t += 3;
      goto finish_adc;

    case 0xcf:			/* RST 1 */
      addr = 0x08;
      t++;
      goto finish_call;

    case 0xd0:			/* RNC */
      res = !(F & C_FLAG);
      goto finish_retc;

    case 0xd1:			/* POP D */
      E = memrdr(SP8++);
      D = memrdr(SP8++);
      t += 6;
      break;

    case 0xd2:			/* JNC nn */
      res = !(F & C_FLAG);
      goto finish_jmpc;

    case 0xd3:			/* OUT n */
      P = memrdr(PC8++);
      io_out(P, P, A);
      t += 6;
      break;

    case 0xd4:			/* CNC nn */
      res = !(F & C_FLAG);
      goto finish_callc;

    case 0xd5:			/* PUSH D */
      memwrt(--SP8, D);
      memwrt(--SP8, E);
      t += 7;
      break;

    case 0xd6:			/* SUI n */
      P = memrdr(PC8++);
      t += 3;
      goto finish_sub;

    case 0xd7:			/* RST 2 */
      addr = 0x10;
      t++;
      goto finish_call;

    case 0xd8:			/* RC */
      res = (F & C_FLAG);
      goto finish_retc;

    case 0xda:			/* JC nn */
      res = (F & C_FLAG);
      goto finish_jmpc;

    case 0xdb:			/* IN n */
      P = memrdr(PC8++);
      A = io_in(P, P);
      t += 6;
      break;

    case 0xdc:			/* CC nn */
      res = (F & C_FLAG);
      goto finish_callc;

    case 0xde:			/* SBI n */
      P = memrdr(PC8++);
      t += 3;
      goto finish_sbb;

    case 0xdf:			/* RST 3 */
      addr = 0x18;
      t++;
      goto finish_call;

    case 0xe0:			/* RPO */
      res = !(F & P_FLAG);
      goto finish_retc;

    case 0xe1:			/* POP H */
      L = memrdr(SP8++);
      H = memrdr(SP8++);
      t += 6;
      break;

    case 0xe2:			/* JPO nn */
      res = !(F & P_FLAG);
      goto finish_jmpc;

    case 0xe3:			/* XTHL */
      P = memrdr(SP8);
      memwrt(SP8, L);
      L = P;
      P = memrdr(SP8 + 1);
      memwrt(SP8 + 1, H);
      H = P;
      t += 14;
      break;

    case 0xe4:			/* CPO nn */
      res = !(F & P_FLAG);
      goto finish_callc;

    case 0xe5:			/* PUSH H */
      memwrt(--SP8, H);
      memwrt(--SP8, L);
      t += 7;
      break;

    case 0xe6:			/* ANI n */
      P = memrdr(PC8++);
      t += 3;
      goto finish_ana;

    case 0xe7:			/* RST 4 */
      addr = 0x20;
      t++;
      goto finish_call;

    case 0xe8:			/* RPE */
      res = (F & P_FLAG);
      goto finish_retc;

    case 0xe9:			/* PCHL */
      PC8 = (H << 8) | L;
      t++;
      break;

    case 0xea:			/* JPE nn */
      res = (F & P_FLAG);
      goto finish_jmpc;

    case 0xeb:			/* XCHG */
      P = D;
      D = H;
      H = P;
      P = E;
      E = L;
      L = P;
      break;

    case 0xec:			/* CPE nn */
      res = (F & P_FLAG);
      goto finish_callc;

    case 0xee:			/* XRI n */
      A ^= memrdr(PC8++);
      t += 3;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xef:			/* RST 5 */
      addr = 0x28;
      t++;
      goto finish_call;

    case 0xf0:			/* RP */
      res = !(F & S_FLAG);
      goto finish_retc;

    case 0xf1:			/* POP PSW */
      F = memrdr(SP8++);
      A = memrdr(SP8++);
      t += 6;
      break;

    case 0xf2:			/* JP nn */
      res = !(F & S_FLAG);
      goto finish_jmpc;

    case 0xf3:			/* DI */
      IFF = 0;
      break;

    case 0xf4:			/* CP nn */
      res = !(F & S_FLAG);
      goto finish_callc;

    case 0xf5:			/* PUSH PSW */
      memwrt(--SP8, A);
      memwrt(--SP8, ((F & ~(Y_FLAG | X_FLAG)) | N_FLAG));
      t += 7;
      break;

    case 0xf6:			/* ORI n */
      A |= memrdr(PC8++);
      t += 3;
      F = szp_flags[A];
      // H_FLAG and C_FLAG cleared
      break;

    case 0xf7:			/* RST 6 */
      addr = 0x30;
      t++;
      goto finish_call;

    case 0xf8:			/* RM */
      res = (F & S_FLAG);
      goto finish_retc;

    case 0xf9:			/* SPHL */
      SP8 = (H << 8) | L;
      t++;
      break;

    case 0xfa:			/* JM nn */
      res = (F & S_FLAG);
      goto finish_jmpc;

    case 0xfb:			/* EI */
      IFF = 3;
      break;

    case 0xfc:			/* CM nn */
      res = (F & S_FLAG);
      goto finish_callc;

    case 0xfe:			/* CPI n */
      P = memrdr(PC8++);
      t += 3;
      goto finish_cmp;

    case 0xff:			/* RST 7 */
      addr = 0x38;
      t++;
      goto finish_call;

    default:
      t = 0;        /* silence compiler */
      break;
    }

    tstates += t; // account for the executed instruction

  } while (State == Running);

  cpu_state->A = A;
  cpu_state->B = B;
  cpu_state->C = C;
  cpu_state->D = D;
  cpu_state->E = E;
  cpu_state->H = H;
  cpu_state->L = L;
  cpu_state->F = F;
  cpu_state->IFF = IFF;
  cpu_state->PC8 = PC8;
  cpu_state->SP8 = SP8;
}

// interrupt handler for break switch, stops CPU
void user_int(void)
{
  State = Interrupted;
}

#ifdef DEBUG
// from the Arduino Memory Guide
void display_freeram()
{
  Serial.print(F("SRAM left: "));
  Serial.println(freeRam());
}

int freeRam()
{
  extern int __heap_start, *__brkval;
  int v;

  return (int) &v - (__brkval == 0 ?  
         (int) &__heap_start : (int) __brkval);  
}
#endif

void setup()
{
  // put your setup code here, to run once:
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(USERINT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(USERINT), user_int, FALLING);

  Serial.begin(9600);
  while (!Serial)
    ; // Wait for serial port to connect. Needed for native USB

  // if this analog pin is not wired the value is unpredictable
  randomSeed(analogRead(UAP));

  // make sure we run with highest possible SPI clock
  SPI.setClockDivider(SPI_CLOCK_DIV2);

  if (!fram.begin()) {
    Serial.println(F("No FRAM found"));
    exit(1);
  }
  fram.writeEnable(true); // always leave write enabled
  
  if (!SD.begin(SD_CS)) {
    Serial.println(F("SD failed"));
    exit(1);
  }
}

void loop()
{
  // put your main code here, to run repeatedly:
  // variables for measuring the run time
  unsigned long start, stop;

  // we have no memory for a fancy banner
  Serial.println(F("\f8080-SIM v1.5.1\n"));

  // initialize and configure all virtual hardware
  init_cpu(&cpu_state);
  init_memory();
  config();

  // run the 8080 CPU with whatever code is in memory
  start = millis();
  cpu_8080(&cpu_state);
  stop = millis();
  
  // 8080 CPU stopped working, signal this on builtin LED
  digitalWrite(LED_BUILTIN, true);

  // tell us why it stopped running
  Serial.println();
  Serial.print(F("CPU was "));
  if (State == Halted)
    Serial.println(F("halted"));
  else if (State == Interrupted)
    Serial.println(F("interrupted"));
  
  // print register dump
  Serial.println(F("PC\tA\tSZHPC\tB:C\tD:E\tH:L\tSP"));
  Serial.print(cpu_state.PC8, HEX);
  Serial.print(F("\t"));
  Serial.print(cpu_state.A, HEX);
  Serial.print(F("\t"));
  Serial.print(cpu_state.F & S_FLAG ? 1 : 0, DEC);
  Serial.print(cpu_state.F & Z_FLAG ? 1 : 0, DEC);
  Serial.print(cpu_state.F & H_FLAG ? 1 : 0, DEC);
  Serial.print(cpu_state.F & P_FLAG ? 1 : 0, DEC);
  Serial.print(cpu_state.F & C_FLAG ? 1 : 0, DEC);
  Serial.print(F("\t"));
  Serial.print(cpu_state.B, HEX);
  Serial.print(F(":"));
  Serial.print(cpu_state.C, HEX);
  Serial.print(F("\t"));
  Serial.print(cpu_state.D, HEX);
  Serial.print(F(":"));
  Serial.print(cpu_state.E, HEX);
  Serial.print(F("\t"));
  Serial.print(cpu_state.H, HEX);
  Serial.print(F(":"));
  Serial.print(cpu_state.L, HEX);
  Serial.print(F("\t"));
  Serial.print(cpu_state.SP8, HEX);
  Serial.println();

  // print some execution statistics
  Serial.println();
  Serial.print(F("8080 ran "));
  Serial.print(stop - start, DEC);
  Serial.print(F(" ms and executed "));
  Serial.print(tstates, DEC);
  Serial.println(F(" t-states"));
  Serial.print(F("Clock frequency "));
  Serial.print(float(tstates) / float(stop - start) / 1000.0, 2);
  Serial.println(F(" MHz"));
#ifdef DEBUG
  display_freeram();
#endif
  Serial.println();
  Serial.flush();
  exit(0);
}

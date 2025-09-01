// src/exceptions.c
#include <stdint.h>
#include "console.h"
#include "idt.h"

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

static const char* exn_name[32] = {
  "Divide-by-zero","Debug","NMI","Breakpoint","Overflow","BOUND","Invalid Opcode","Device NA",
  "Double Fault","Coprocessor Overrun","Invalid TSS","Segment Not Present","Stack Fault",
  "General Protection","Page Fault","Reserved","x87 FP Error","Alignment Check","Machine Check",
  "SIMD FP","Virtualization","Reserved","Reserved","Reserved","Reserved","Reserved","Reserved",
  "Reserved","Reserved","Security Exception","Reserved","Reserved"
};

static void put_dec(uint32_t x){
  char b[11]; int i=10; b[i]=0;
  do { b[--i]='0'+(x%10); x/=10; } while(x);
  console_puts(&b[i]);
}
static void put_hex(uint32_t x){
  char b[11]; b[0]='0'; b[1]='x';
  for(int i=0;i<8;i++){ uint32_t n=(x>>((7-i)*4))&0xF; b[2+i]= n<10? '0'+n : 'A'+(n-10); }
  b[10]=0; console_puts(b);
}

void exceptions_init(void){
  void (*v[32])(void) = {
    isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,isr12,isr13,isr14,isr15,
    isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31
  };
  for(int i=0;i<32;i++) set_gate(i,(uint32_t)v[i]);
}

void isr_handler_c(uint32_t n, uint32_t err){
  console_puts("\n[EXCEPTION] ");
  console_puts(exn_name[n]);
  console_puts(" (#"); put_dec(n); console_puts(") err="); put_hex(err); console_puts("\n");
  console_puts("System halted.\n");
  __asm__ __volatile__("cli");
  for(;;) __asm__ __volatile__("hlt");
}

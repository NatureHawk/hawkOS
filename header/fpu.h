#pragma once
#include <stdint.h>

// Floating-point unit bring-up and per-task state.
//
// Nothing in the kernel used floating point until the media code arrived: an
// MPEG audio decoder is float from end to end, and a video decoder keeps its
// clock in doubles. Once any task touches the FPU its registers are part of
// that task's context, so the scheduler saves and restores them on every
// switch the same way it does the general registers -- otherwise one task's
// half-finished arithmetic turns up in another task's results.
//
// The save is eager rather than lazy (no CR0.TS trap): there are a handful of
// tasks, a switch happens at most a hundred times a second, and an fxsave is
// a few hundred cycles. The trap-driven version saves nothing measurable here
// and adds an exception path to get wrong.

#define FPU_STATE_SIZE 512u     // what fxsave writes; fnsave needs only 108

// Turns the FPU (and SSE, where the CPU has it) on and records a clean state
// that fpu_init_state() hands to every new task. Must run before task_init().
void fpu_init(void);

// 1 if fxsave/fxrstor are in use, 0 if the CPU only has fnsave.
int  fpu_has_fxsr(void);

// `area` must be 16-byte aligned and FPU_STATE_SIZE bytes long.
void fpu_init_state(uint8_t* area);
void fpu_save(uint8_t* area);
void fpu_restore(const uint8_t* area);

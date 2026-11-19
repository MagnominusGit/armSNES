/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// Fast mode's GSU recompiler, included by gsu.c. Runs of instructions in the code cache, or in
// the ROM outside it, are compiled to host code that does what the interpreter's loop does,
// instruction by instruction: it stops before an instruction once the GSU is ahead of the S-CPU, steps the clock for each fetch
// with the ROM and RAM buffers, and leaves every register, flag and field as the loop would. A
// block starts with no prefix pending and ends after an instruction that writes R15, or before one
// it doesn't compile, which the interpreter then runs.

#if defined(__x86_64__) || defined(__aarch64__)
#define GSU_JIT 1

#include <stddef.h>
#include <sys/mman.h>

#if defined(__x86_64__)
#include "../../shared/emit/x64/emit.h"
#else
#include "../../shared/emit/a64/emit.h"
#endif

enum { JIT_CODE_SIZE = 16 << 20, JIT_BLOCK_MAX = 48, JIT_BYTES = 96, JIT_SET_BITS = 13, JIT_TABLE = 4 << JIT_SET_BITS };

bool gsuJitEnabled = true;

static uint8_t* jitCode;
static uint8_t* jitStart;  // past the trampolines
static uint8_t* jitFree;
static void (*jitEnter)(const void* code, u64 until);
static const uint8_t* jitExit;
static const uint8_t* jitExitState;

// What an exit leaves, for one that only names its index: R15, the pipeline, the opcode run before
// (if it was in the block) and the prefix pending.
static struct JitExitInfo {
	u16 pc;
	u16 prefix;  // SFR's B, ALT1 and ALT2 bits
	u8 pipe, last;
	bool hasLast;
	u8 sreg, dreg;
} jitExits[1 << 17];
static u32 jitExitCount;

static void jitApplyExit(u32 i) {
	const struct JitExitInfo* x = &jitExits[i];
	gsu.r[15] = x->pc;
	gsu.pipeline = x->pipe;
	if (x->hasLast) {
		gsu.opcode = x->last;
	}
	if (x->prefix | x->sreg | x->dreg) {
		gsu.sfr = (gsu.sfr & ~(1 << SFR_B | 1 << SFR_ALT1 | 1 << SFR_ALT2)) | x->prefix;
		gsu.sreg = x->sreg;
		gsu.dreg = x->dreg;
	}
}

// Blocks by R15 and the pipeline byte they start with, and the clock speed and multiplier they were
// made for. Each keeps the bytes it was made from: it runs while they are in valid lines of the
// cache, wherever CBR has put them, so a cache flushed and filled again finds its blocks.
// Four ways to a set. Blocks time things otherwise than the interpreter, so whether one runs must
// follow from the GSU's state alone: a block is made the first time its start is reached.
static struct JitEntry {
	const void* code;  // NULL: not made yet
	u32 gen;
	u32 checked;  // the cache epoch its bytes were last found in the cache in
	u16 pc;
	u16 count;  // times reached before it was made
	u8 pipe;
	bool clsr, ms0;
	u8 length;  // the program's bytes from pc on that the block fetches
	u8 lines;   // which cache lines it could reach are valid (jitLines); 0 for a block in the ROM
	u8 pbr;     // a block in the ROM: its bank, and where the cache is, which it stops short of
	u16 cbr;
} jitTable[JIT_TABLE];
static u8 jitVictim[1 << JIT_SET_BITS];
static u8 jitBytes[JIT_TABLE][JIT_BYTES];
// A block's code where none can be made: the first instruction is left to the interpreter.
static const u8 jitNone;
// Bumped when the code buffer starts again, which drops every block.
static u32 jitGen = 1;
// Bumped when lines of the cache are flushed, written or filled: until then, bytes found in it stay,
// and chains (made to blocks for the lines valid then) hold. Chains from a block in the ROM to
// another hold until the cache moves or is flushed, or the clock speed or multiplier changes.
static u32 jitEpoch = 1, jitRomEpoch = 1;

static void gsuJitInvalidate(void) {
	jitEpoch++;
	jitRomEpoch++;
}

static void gsuJitLines(void) {
	jitEpoch++;
}

// Every block dropped: the state they were made against is gone.
static void gsuJitDrop(void) {
	gsuJitInvalidate();
	jitGen++;
}

// Blocks chained: an exit to a known R15 and pipeline jumps to the block for them, found when the
// loop first went there in the same cache epoch. The exit names its slot as it returns, for the loop
// to fill.
static struct JitSlot {
	u32 epoch;
	u32 pc;
	const void* target;
	u8 pipe;
	bool rom;  // from a block in the ROM to another
} jitSlots[1 << 16];
static u32 jitSlotCount;
static struct JitSlot* jitLastSlot;

#define GOFF(field) ((u32) offsetof(struct Gsu, field))

// --- Backend ---

static struct Emitter jitE;

#if defined(__x86_64__)

// rbx the GSU, r12 its clock, r13 the clock it runs up to; values in eax, ecx, edx, esi, edi, r8d.
// Value 6 is SFR, kept in rbp while blocks run.
static const int jReg[7] = { RAX, RCX, RDX, RSI, RDI, R8, RBP };
#define J_BASE RBX
#define J_CLOCK R12
#define J_UNTIL R13
#define J_EPOCH R14
#define J_ROM_EPOCH R15

static void jMov(int d, int a) {
	xMovRR(&jitE, 4, jReg[d], jReg[a]);
}

static void jMovI(int d, u32 imm) {
	xMovIKeepFlags(&jitE, jReg[d], imm);
}

static void jLd(int width, int d, u32 off) {
	if (width == 4) {
		xLoad(&jitE, 4, jReg[d], M(J_BASE, off));
	} else {
		xMovzx(&jitE, jReg[d], width, M(J_BASE, off));
	}
}

static void jSt(int width, u32 off, int s) {
	xStore(&jitE, width, M(J_BASE, off), jReg[s]);
}

static void jStI(int width, u32 off, u32 imm) {
	xStoreI(&jitE, width, M(J_BASE, off), imm);
}

enum { J_ADD, J_SUB, J_AND, J_OR, J_XOR };
static const int jAluX[5] = { X_ADD, X_SUB, X_AND, X_OR, X_XOR };

static void jAlu(int op, int d, int a, int b) {
	if (d == b && d != a) {
		// d = a op d: only commutative ones come this way.
		xAluRR(&jitE, jAluX[op], 4, R(jReg[d]), jReg[a]);
		return;
	}
	xMovRR(&jitE, 4, jReg[d], jReg[a]);
	xAluRR(&jitE, jAluX[op], 4, R(jReg[d]), jReg[b]);
}

static void jAluI(int op, int d, int a, u32 imm) {
	xMovRR(&jitE, 4, jReg[d], jReg[a]);
	xAluI(&jitE, jAluX[op], 4, R(jReg[d]), (int32_t) imm);
}

enum { J_LSL, J_LSR, J_ASR };

static void jShift(int op, int d, int a, int amount) {
	static const int x[3] = { X_SHL, X_SHR, X_SAR };
	xMovRR(&jitE, 4, jReg[d], jReg[a]);
	xShiftI(&jitE, x[op], 4, R(jReg[d]), amount);
}

enum { J_SXTB, J_SXTH, J_UXTB, J_UXTH };

static void jExt(int kind, int d, int a) {
	if (kind == J_SXTB || kind == J_SXTH) {
		xMovsx(&jitE, jReg[d], kind == J_SXTB ? 1 : 2, R(jReg[a]));
	} else {
		xMovzx(&jitE, jReg[d], kind == J_UXTB ? 1 : 2, R(jReg[a]));
	}
}

static void jMul(int d, int a, int b) {
	xMovRR(&jitE, 4, jReg[d], jReg[a]);
	xImul(&jitE, 4, jReg[d], R(jReg[b]));
}

// d = (a != 0) or (a == 0).
static void jSetNe0(int d, int a) {
	xTest(&jitE, 4, R(jReg[a]), jReg[a]);
	xSetcc(&jitE, CC_NE, R(jReg[d]));
	xMovzx(&jitE, jReg[d], 1, R(jReg[d]));
}

static void jSetEq0(int d, int a) {
	xTest(&jitE, 4, R(jReg[a]), jReg[a]);
	xSetcc(&jitE, CC_E, R(jReg[d]));
	xMovzx(&jitE, jReg[d], 1, R(jReg[d]));
}

// Forward branches, patched with jPatchHere.
static uint8_t* jBranchZ(int a) {
	xTest(&jitE, 4, R(jReg[a]), jReg[a]);
	return xJcc(&jitE, CC_E, NULL);
}

static uint8_t* jBranchNZ(int a) {
	xTest(&jitE, 4, R(jReg[a]), jReg[a]);
	return xJcc(&jitE, CC_NE, NULL);
}

static void jPatchHere(uint8_t* site) {
	patchHere(&jitE, site);
}

static void jPatchTo(uint8_t* site, const void* target) {
	patchRel32(site, target);
}

// To the site's target when the GSU is past the clock it runs up to.
static uint8_t* jDeadline(void) {
	xAluRR(&jitE, X_CMP, 8, R(J_UNTIL), J_CLOCK);
	return xJcc(&jitE, CC_B, NULL);
}

// An exit that names its JitExitInfo.
static void jExitInfo(u32 index) {
	xMovIKeepFlags(&jitE, RDI, index);
	xJmp(&jitE, jitExitState);
}

static void jClockAdd(u64 amount) {
	if (amount < 0x80000000u) {
		xAluI(&jitE, X_ADD, 8, R(J_CLOCK), (int32_t) amount);
	} else {
		xMovI64(&jitE, R9, amount);
		xAluRR(&jitE, X_ADD, 8, R(J_CLOCK), R9);
	}
}

static void jCallI(const void* fn, u32 imm) {
	xMovIKeepFlags(&jitE, RDI, imm);
	xMovI64(&jitE, R11, (u64) (uintptr_t) fn);
	xCallR(&jitE, R11);
}

static void jCallR(const void* fn, int a) {
	xMovRR(&jitE, 4, RDI, jReg[a]);
	xMovI64(&jitE, R11, (u64) (uintptr_t) fn);
	xCallR(&jitE, R11);
}

// A call to a C function that returns a value, into value 0 (eax).
static void jCallV(const void* fn) {
	xMovI64(&jitE, R11, (u64) (uintptr_t) fn);
	xCallR(&jitE, R11);
}

static uint8_t* jJump(void) {
	return xJmp(&jitE, NULL);
}

static void jJumpExit(void) {
	xJmp(&jitE, jitExit);
}

static void jJumpTo(const void* target) {
	xJmp(&jitE, target);
}

// To the slot's block if it was found in this epoch; on, if not, with the slot named.
static void jChain(struct JitSlot* slot) {
	xMovI64(&jitE, R11, (u64) (uintptr_t) slot);
	xAluRM(&jitE, X_CMP, 4, slot->rom ? J_ROM_EPOCH : J_EPOCH, M(R11, 0));
	uint8_t* miss = xJcc(&jitE, CC_NE, NULL);
	xJmpM(&jitE, M(R11, offsetof(struct JitSlot, target)));
	patchHere(&jitE, miss);
	xMovI64(&jitE, R10, (u64) (uintptr_t) &jitLastSlot);
	xStore(&jitE, 8, M(R10, 0), R11);
}

// To the site's target if a equals imm, or is at most imm (unsigned).
static uint8_t* jBranchEqI(int a, u32 imm) {
	xAluI(&jitE, X_CMP, 4, R(jReg[a]), (int32_t) imm);
	return xJcc(&jitE, CC_E, NULL);
}

// The clock out to gsu.th.clock for a C function that moves it on, and back.
static void jClockOut(void) {
	xStore(&jitE, 8, M(J_BASE, GOFF(th.clock)), J_CLOCK);
}

static void jClockIn(void) {
	xLoad(&jitE, 8, J_CLOCK, M(J_BASE, GOFF(th.clock)));
}

// void enter(code, until): the state's registers loaded, the block jumped to; the exit stores the
// clock back and returns.
static void jTrampolines(void) {
	jitEnter = (void (*)(const void*, u64)) jitE.p;
	xPush(&jitE, RBX);
	xPush(&jitE, RBP);
	xPush(&jitE, R12);
	xPush(&jitE, R13);
	xPush(&jitE, R14);
	xPush(&jitE, R15);
	xAluI(&jitE, X_SUB, 8, R(RSP), 8);
	xMovI64(&jitE, J_BASE, (u64) (uintptr_t) &gsu);
	xLoad(&jitE, 8, J_CLOCK, M(J_BASE, GOFF(th.clock)));
	xMovRR(&jitE, 8, J_UNTIL, RSI);
	xMovzx(&jitE, RBP, 2, M(J_BASE, GOFF(sfr)));
	xMovI64(&jitE, R11, (u64) (uintptr_t) &jitEpoch);
	xLoad(&jitE, 4, J_EPOCH, M(R11, 0));
	xMovI64(&jitE, R11, (u64) (uintptr_t) &jitRomEpoch);
	xLoad(&jitE, 4, J_ROM_EPOCH, M(R11, 0));
	xJmpR(&jitE, RDI);
	jitExitState = jitE.p;
	xStore(&jitE, 2, M(J_BASE, GOFF(sfr)), RBP);
	xStore(&jitE, 8, M(J_BASE, GOFF(th.clock)), J_CLOCK);
	xMovI64(&jitE, R11, (u64) (uintptr_t) jitApplyExit);
	xCallR(&jitE, R11);
	uint8_t* done = xJmp(&jitE, NULL);
	jitExit = jitE.p;
	xStore(&jitE, 2, M(J_BASE, GOFF(sfr)), RBP);
	xStore(&jitE, 8, M(J_BASE, GOFF(th.clock)), J_CLOCK);
	patchHere(&jitE, done);
	xAluI(&jitE, X_ADD, 8, R(RSP), 8);
	xPop(&jitE, R15);
	xPop(&jitE, R14);
	xPop(&jitE, R13);
	xPop(&jitE, R12);
	xPop(&jitE, RBP);
	xPop(&jitE, RBX);
	xRet(&jitE);
}

static void jFlush(uint8_t* start, uint8_t* end) {
	(void) start;
	(void) end;
}

#else

// x19 the GSU, x20 its clock, x21 the clock it runs up to; values in w9-w14; w16 and w17 scratch.
// Value 6 is SFR, kept in w24 while blocks run.
static const int jReg[7] = { 9, 10, 11, 12, 13, 14, 24 };
#define J_BASE 19
#define J_CLOCK 20
#define J_UNTIL 21
#define J_EPOCH 22
#define J_ROM_EPOCH 23
#define J_TMP 16
#define J_TMP2 17

static void jMov(int d, int a) {
	aMov(&jitE, jReg[d], jReg[a]);
}

static void jMovI(int d, u32 imm) {
	aMovImm(&jitE, jReg[d], imm);
}

static void jLd(int width, int d, u32 off) {
	if (width == 1) {
		aLdrb(&jitE, jReg[d], J_BASE, off);
	} else if (width == 2) {
		aLdrh(&jitE, jReg[d], J_BASE, off);
	} else {
		aLdr(&jitE, jReg[d], J_BASE, off);
	}
}

static void jSt(int width, u32 off, int s) {
	if (width == 1) {
		aStrb(&jitE, jReg[s], J_BASE, off);
	} else if (width == 2) {
		aStrh(&jitE, jReg[s], J_BASE, off);
	} else {
		aStr(&jitE, jReg[s], J_BASE, off);
	}
}

static void jStI(int width, u32 off, u32 imm) {
	int r = ZR;
	if (imm) {
		aMovImm(&jitE, J_TMP, imm);
		r = J_TMP;
	}
	if (width == 1) {
		aStrb(&jitE, r, J_BASE, off);
	} else if (width == 2) {
		aStrh(&jitE, r, J_BASE, off);
	} else {
		aStr(&jitE, r, J_BASE, off);
	}
}

enum { J_ADD, J_SUB, J_AND, J_OR, J_XOR };
static const uint32_t jAluA[5] = { A_ADD, A_SUB, A_AND, A_ORR, A_EOR };

static void jAlu(int op, int d, int a, int b) {
	aDp(&jitE, jAluA[op], jReg[d], jReg[a], jReg[b], S_LSL, 0);
}

static void jAluI(int op, int d, int a, u32 imm) {
	aMovImm(&jitE, J_TMP, imm);
	aDp(&jitE, jAluA[op], jReg[d], jReg[a], J_TMP, S_LSL, 0);
}

enum { J_LSL, J_LSR, J_ASR };

static void jShift(int op, int d, int a, int amount) {
	if (op == J_LSL) {
		aLslImm(&jitE, jReg[d], jReg[a], amount);
	} else if (op == J_LSR) {
		aLsrImm(&jitE, jReg[d], jReg[a], amount);
	} else {
		aAsrImm(&jitE, jReg[d], jReg[a], amount);
	}
}

enum { J_SXTB, J_SXTH, J_UXTB, J_UXTH };

static void jExt(int kind, int d, int a) {
	if (kind == J_SXTB) {
		aSxtb(&jitE, jReg[d], jReg[a]);
	} else if (kind == J_SXTH) {
		aSxth(&jitE, jReg[d], jReg[a]);
	} else if (kind == J_UXTB) {
		aUbfx(&jitE, jReg[d], jReg[a], 0, 8);
	} else {
		aUxth(&jitE, jReg[d], jReg[a]);
	}
}

static void jMul(int d, int a, int b) {
	aMadd(&jitE, jReg[d], jReg[a], jReg[b], ZR);
}

static void jSetNe0(int d, int a) {
	aDpImm(&jitE, A_SUBS_IMM, ZR, jReg[a], 0);
	aCset(&jitE, jReg[d], C_NE);
}

static void jSetEq0(int d, int a) {
	aDpImm(&jitE, A_SUBS_IMM, ZR, jReg[a], 0);
	aCset(&jitE, jReg[d], C_EQ);
}

static uint8_t* jBranchZ(int a) {
	return aCbz(&jitE, jReg[a], NULL);
}

static uint8_t* jBranchNZ(int a) {
	return aCbnz(&jitE, jReg[a], NULL);
}

static void jPatchHere(uint8_t* site) {
	patchHere(&jitE, site);
}

static void jPatchTo(uint8_t* site, const void* target) {
	patchBranch(site, target);
}

static uint8_t* jDeadline(void) {
	aDp(&jitE, A_SUBS | W64, ZR, J_UNTIL, J_CLOCK, S_LSL, 0);
	return aBcond(&jitE, C_LO, NULL);
}

static void jExitInfo(u32 index) {
	aMovImm(&jitE, 0, index);
	aB(&jitE, jitExitState);
}

static void jClockAdd(u64 amount) {
	aMovImm64(&jitE, J_TMP, amount);
	aDp(&jitE, A_ADD | W64, J_CLOCK, J_CLOCK, J_TMP, S_LSL, 0);
}

static void jCallI(const void* fn, u32 imm) {
	aMovImm(&jitE, 0, imm);
	aMovImm64(&jitE, J_TMP, (u64) (uintptr_t) fn);
	aBlr(&jitE, J_TMP);
}

static void jCallR(const void* fn, int a) {
	aMov(&jitE, 0, jReg[a]);
	aMovImm64(&jitE, J_TMP, (u64) (uintptr_t) fn);
	aBlr(&jitE, J_TMP);
}

static void jCallV(const void* fn) {
	aMovImm64(&jitE, J_TMP, (u64) (uintptr_t) fn);
	aBlr(&jitE, J_TMP);
	aMov(&jitE, jReg[0], 0);
}

static uint8_t* jJump(void) {
	return aB(&jitE, NULL);
}

static void jJumpExit(void) {
	aB(&jitE, jitExit);
}

static void jJumpTo(const void* target) {
	aB(&jitE, target);
}

static void jChain(struct JitSlot* slot) {
	aMovImm64(&jitE, J_TMP, (u64) (uintptr_t) slot);
	aLdr(&jitE, J_TMP2, J_TMP, 0);
	aDp(&jitE, A_SUBS, ZR, J_TMP2, slot->rom ? J_ROM_EPOCH : J_EPOCH, S_LSL, 0);
	uint8_t* miss = aBcond(&jitE, C_NE, NULL);
	aLdrX(&jitE, J_TMP2, J_TMP, offsetof(struct JitSlot, target));
	aBr(&jitE, J_TMP2);
	patchHere(&jitE, miss);
	aMovImm64(&jitE, J_TMP2, (u64) (uintptr_t) &jitLastSlot);
	aStrX(&jitE, J_TMP, J_TMP2, 0);
}

static uint8_t* jBranchEqI(int a, u32 imm) {
	aMovImm(&jitE, J_TMP, imm);
	aDp(&jitE, A_SUBS, ZR, jReg[a], J_TMP, S_LSL, 0);
	return aBcond(&jitE, C_EQ, NULL);
}

static void jClockOut(void) {
	aStrX(&jitE, J_CLOCK, J_BASE, GOFF(th.clock));
}

static void jClockIn(void) {
	aLdrX(&jitE, J_CLOCK, J_BASE, GOFF(th.clock));
}

static void jTrampolines(void) {
	jitEnter = (void (*)(const void*, u64)) jitE.p;
	aPush(&jitE, 29, 30, 64);
	aStp(&jitE, 19, 20, 16);
	aStp(&jitE, 21, 22, 32);
	aStp(&jitE, 23, 24, 48);
	aMovImm64(&jitE, J_BASE, (u64) (uintptr_t) &gsu);
	aLdrX(&jitE, J_CLOCK, J_BASE, GOFF(th.clock));
	aMovX(&jitE, J_UNTIL, 1);
	aLdrh(&jitE, 24, J_BASE, GOFF(sfr));
	aMovImm64(&jitE, J_TMP, (u64) (uintptr_t) &jitEpoch);
	aLdr(&jitE, J_EPOCH, J_TMP, 0);
	aMovImm64(&jitE, J_TMP, (u64) (uintptr_t) &jitRomEpoch);
	aLdr(&jitE, J_ROM_EPOCH, J_TMP, 0);
	aBr(&jitE, 0);
	jitExitState = jitE.p;
	aStrh(&jitE, 24, J_BASE, GOFF(sfr));
	aStrX(&jitE, J_CLOCK, J_BASE, GOFF(th.clock));
	aMovImm64(&jitE, J_TMP, (u64) (uintptr_t) jitApplyExit);
	aBlr(&jitE, J_TMP);
	uint8_t* done = aB(&jitE, NULL);
	jitExit = jitE.p;
	aStrh(&jitE, 24, J_BASE, GOFF(sfr));
	aStrX(&jitE, J_CLOCK, J_BASE, GOFF(th.clock));
	patchHere(&jitE, done);
	aLdp(&jitE, 23, 24, 48);
	aLdp(&jitE, 21, 22, 32);
	aLdp(&jitE, 19, 20, 16);
	aPop(&jitE, 29, 30, 64);
	aRet(&jitE);
}

static void jFlush(uint8_t* start, uint8_t* end) {
	__builtin___clear_cache((char*) start, (char*) end);
}

#endif

// --- Front end ---

// SFR's value register while blocks run; C functions find it in memory.
#define J_SFRV 6

static void jitSfrOut(void) {
	jSt(2, GOFF(sfr), J_SFRV);
}

static void jitSfrIn(void) {
	jLd(2, J_SFRV, GOFF(sfr));
}

// An instruction a block calls out for, with its state in memory, run as the interpreter runs it.
static void jitOne(u32 opcode) {
	u32 n;
	gsu.opcode = opcode;
	switch (opcode) {
#define GSU_CLOCK gsu.th.clock
#define GSU_PIPELINE gsu.pipeline
#include "gsu_ops.inc"
#undef GSU_CLOCK
#undef GSU_PIPELINE
	}
next:
	if (gsu.modified[14]) {
		gsu.modified[14] = false;
		updateROMBuffer();
	}
}

// What the loop would have at the top of an instruction.
struct JitState {
	u32 pc;      // R15, the address the next byte is fetched from
	u8 pipe;     // the instruction to run
	int last;    // the opcode run before it (-1: none in this block)
	bool b, alt1, alt2;
	u32 sreg, dreg;
};

static struct JitFront {
	struct JitState s;
	bool clsr;
	// Exits waiting to be made: where they branch from and the state they leave.
	struct {
		uint8_t* site;
		struct JitState s;
	} exits[JIT_BLOCK_MAX * 2 + 4];
	u32 exitCount;
	u32 pending;  // clocks not given to the clock and the buffers yet
	u32 start;    // the block's first R15
	u8 startPipe;
	const uint8_t* startCode;
	u32 fetched;  // how many bytes from start on it fetches
	bool rom;     // fetched from the ROM, outside the cache
	bool romIdle; // no ROM buffer read pending: set by a fetch's wait, until R14 is written or jitOne runs
} jf;

static u8 jitRomByte(u32 address) {
	u32 rom = gsu.pbr << 16 | (u16) address;
	return romAt(gsu.pbr < 0x40 ? ((rom & 0x3f0000) >> 1) | (rom & 0x7fff) : rom);
}

// A byte of the program, if it is in a valid line of the cache, and no further than a block keeps.
static bool jitByte(u32 address, u8* byte) {
	u32 offset = (u16) (address - gsu.cbr);
	u32 index = (u16) (address - jf.start);
	if (jf.rom) {
		if (offset < 512 || index >= JIT_BYTES) {
			return false;
		}
		*byte = jitRomByte(address);
		return true;
	}
	if (offset >= 512 || !gsu.cacheValid[offset >> 4] || index >= JIT_BYTES || (u16) (jf.start - gsu.cbr) + index >= 512) {
		return false;
	}
	*byte = gsu.cache[offset];
	return true;
}

// A byte the block fetches, counted in what it keeps.
static u8 jitFetch(u32 address) {
	u8 byte = 0;
	jitByte(address, &byte);
	u32 index = (u16) (address - jf.start) + 1;
	jf.fetched = index > jf.fetched ? index : jf.fetched;
	return byte;
}

// The prefix written out as it is at s.
static void jitWritePrefix(const struct JitState* s) {
	jAluI(J_AND, J_SFRV, J_SFRV, ~(1u << SFR_B | 1u << SFR_ALT1 | 1u << SFR_ALT2) & 0xffff);
	u32 bits = s->b << SFR_B | s->alt1 << SFR_ALT1 | s->alt2 << SFR_ALT2;
	if (bits) {
		jAluI(J_OR, J_SFRV, J_SFRV, bits);
	}
	jStI(4, GOFF(sreg), s->sreg);
	jStI(4, GOFF(dreg), s->dreg);
}

static bool jitClean(const struct JitState* s) {
	return !s->b && !s->alt1 && !s->alt2 && !s->sreg && !s->dreg;
}

// The state the loop keeps in memory, written out as it is at s. The prefix in memory is clean
// while a block runs (a block starts with it so, and jitOne leaves it so): only one pending is
// written.
static void jitWriteState(const struct JitState* s) {
	jStI(2, GOFF(r[15]), s->pc & 0xffff);
	jStI(1, GOFF(pipeline), s->pipe);
	if (s->last >= 0) {
		jStI(1, GOFF(opcode), s->last);
	}
	if (!jitClean(s)) {
		jitWritePrefix(s);
	}
}

// An exit leaving the state s, through its JitExitInfo.
static void jitExitStub(const struct JitState* s) {
	struct JitExitInfo* x = &jitExits[jitExitCount];
	*x = (struct JitExitInfo) { (u16) s->pc, (u16) (s->b << SFR_B | s->alt1 << SFR_ALT1 | s->alt2 << SFR_ALT2), s->pipe,
		(u8) (s->last >= 0 ? s->last : 0), s->last >= 0, (u8) s->sreg, (u8) s->dreg };
	jExitInfo(jitExitCount++);
}

// The end of a block, to the state out: chained to the block for it when the prefix is clean.
static void jitExitTo(const struct JitState* out) {
	if (jitClean(out) && jitSlotCount < sizeof(jitSlots) / sizeof(*jitSlots)) {
		struct JitSlot* slot = &jitSlots[jitSlotCount++];
		*slot = (struct JitSlot) { 0, out->pc, NULL, out->pipe, jf.rom && (u16) (out->pc - gsu.cbr) >= 512 };
		if (out->last >= 0) {
			jStI(1, GOFF(opcode), out->last);
		}
		jChain(slot);
	}
	jitExitStub(out);
}

static void jitExitLater(uint8_t* site, const struct JitState* s) {
	jf.exits[jf.exitCount].site = site;
	jf.exits[jf.exitCount].s = *s;
	jf.exitCount++;
}

// One fetch: the buffers stepped, then the clock.
// Fast mode's blocks don't step the clock and the ROM and RAM buffers at each fetch: the clocks are
// added up as the block is made, and given to them in one step before anything that sees them (a
// call to jitOne, an R14 written, the block's end). Only the buffers' times move, which nothing in
// between looks at, and the block's runs end at its end instead of an instruction sooner.
// From the ROM, a fetch first waits for a pending ROM buffer read, as readOpcodeBalanced does.
static void jitFlush(void);
static void jitRomWait(void);

static void jitFetchStep(void) {
	if (jf.rom) {
		if (!jf.romIdle) {
			jitFlush();
			jLd(4, 0, GOFF(romcl));
			uint8_t* skip = jBranchZ(0);
			jClockOut();
			jitSfrOut();
			jCallV((const void*) jitRomWait);
			jitSfrIn();
			jClockIn();
			jPatchHere(skip);
			jf.romIdle = true;
		}
		jf.pending += jf.clsr ? 5 : 6;
		return;
	}
	jf.pending += jf.clsr ? 1 : 2;
}

static void jitStep(u32 clocks) {
	jf.pending += clocks;
}

static void jitFlush(void) {
	if (!jf.pending) {
		return;
	}
	jLd(4, 0, GOFF(romcl));
	jLd(4, 1, GOFF(ramcl));
	jAlu(J_OR, 0, 0, 1);
	uint8_t* skip = jBranchZ(0);
	jitSfrOut();
	jCallI((const void*) stepBuffers, jf.pending);
	jitSfrIn();
	jPatchHere(skip);
	jClockAdd(gsu.th.scalar * jf.pending);
	jf.pending = 0;
}

// A register into d: R15 is the compile-time address it holds.
static void jitReadR(int d, u32 n, u32 r15) {
	if (n == 15) {
		jMovI(d, r15 & 0xffff);
	} else {
		jLd(2, d, GOFF(r[n]));
	}
}

// v into Rn (n < 15), as setR: marked written; R14 starts a ROM buffer read at the instruction's end.
static void jitWriteR(u32 n, int v) {
	jSt(2, GOFF(r[n]), v);
	// Below R14 the mark is never cleared, and blocks are dropped on power and state loads.
	if (n != 14 && !(n < 14 && gsu.modified[n])) {
		jStI(1, GOFF(modified[n]), 1);
	}
}

static void jitEndR14(void) {
	jf.romIdle = false;
	jitFlush();
	jAluI(J_OR, J_SFRV, J_SFRV, 1u << SFR_R);
	jStI(4, GOFF(romcl), jf.clsr ? 5 : 6);
	jStI(1, GOFF(modified[14]), 0);
}

// The flags in mask set from the bits in register f (already in place).
static void jitSetFlags(u32 mask, int f) {
	jAluI(J_AND, J_SFRV, J_SFRV, ~mask & 0xffff);
	jAlu(J_OR, J_SFRV, J_SFRV, f);
}

// S and Z from the 16-bit value in v, into f (scratch t).
static void jitSZ(int f, int v, int t) {
	jShift(J_LSR, f, v, 15);
	jShift(J_LSL, f, f, SFR_S);
	jExt(J_UXTH, t, v);
	jSetEq0(t, t);
	jShift(J_LSL, t, t, SFR_Z);
	jAlu(J_OR, f, f, t);
}

// A flag bit from whether v & mask is nonzero, ORed into f (scratch t).
static void jitFlagIf(int f, int v, u32 mask, u32 bit, int t) {
	jAluI(J_AND, t, v, mask);
	jSetNe0(t, t);
	jShift(J_LSL, t, t, bit);
	jAlu(J_OR, f, f, t);
}

// The destination written, as SET_DR; false if it is R15, which the block doesn't do.
static bool jitDR(u32 dreg, int v, bool* r14) {
	if (dreg == 15) {
		return false;
	}
	jitWriteR(dreg, v);
	*r14 |= dreg == 14;
	return true;
}

// Instructions made as a call to jitOne: plot, rpix, color, cmode, getb, getc, ramb, romb, the RAM
// loads and stores but lm/sm/lms/sms, and sbk. None writes R15 or fetches.
static bool jitCalled(const struct JitState* s) {
	u8 op = s->pipe;
	if (op == 0x4c) {
		return !s->alt1 || s->dreg != 15;
	}
	if (op == 0x4e || op == 0xdf || op == 0x90 || (op >= 0x30 && op <= 0x3b)) {
		return true;
	}
	return (op == 0xef || (op >= 0x40 && op <= 0x4b)) && s->dreg != 15;
}

// Whether an instruction can be compiled, and its operand bytes are in the cache.
static bool jitSupported(const struct JitState* s) {
	u8 op = s->pipe, x;
	u32 n = op & 15;
	if (jitCalled(s)) {
		return true;
	}
	switch (op >> 4) {
	case 0x0:
		if (op == 0x01 || op == 0x03 || op == 0x04) {
			return s->dreg != 15;
		}
		if (op >= 0x05) {
			return jitByte(s->pc + 1, &x);
		}
		return false;
	case 0x1:
		return !s->b || n != 15;
	case 0x2:
		return true;
	case 0x3:
		return op == 0x3c || op >= 0x3d;
	case 0x4:
		return (op == 0x4d || op == 0x4f) && s->dreg != 15;
	case 0x5:
	case 0x6:
		return s->dreg != 15;
	case 0x7:
	case 0xc:
		return s->dreg != 15;
	case 0x8:
		return s->dreg != 15;
	case 0x9:
		if (op >= 0x91 && op <= 0x94) {
			return true;
		}
		if (op >= 0x98 && op <= 0x9d) {
			return !s->alt1;
		}
		return (op == 0x95 || op == 0x96 || op == 0x97 || op == 0x9e || op == 0x9f) && s->dreg != 15 && !(op == 0x9f && s->alt1 && s->dreg == 4);
	case 0xa:
		return n != 15 && jitByte(s->pc + 1, &x);
	case 0xb:
		return !s->b || s->dreg != 15;
	case 0xd:
	case 0xe:
		return n != 15;
	case 0xf:
		return (n != 15 || (!s->alt1 && !s->alt2)) && jitByte(s->pc + 1, &x) && jitByte(s->pc + 2, &x);
	}
	return false;
}

static void jitRegsReset(struct JitState* s) {
	s->b = s->alt1 = s->alt2 = false;
	s->sreg = s->dreg = 0;
}

// PIPE: the pipeline's byte is the operand, the next one is fetched.
static u8 jitPipe(struct JitState* s) {
	u8 operand = s->pipe;
	jStI(1, GOFF(pipeOut), operand);
	s->pc = (u16) (s->pc + 1);
	jitFetchStep();
	s->pipe = jitFetch(s->pc);
	return operand;
}

// The block's end to a known R15 after op: back to its own start when it is that, which leaves the
// opcode as the loop would.
static void jitGoTo(const struct JitState* out, u8 op) {
	if (out->pc == jf.start && out->pipe == jf.startPipe && jitClean(out)) {
		jStI(1, GOFF(opcode), op);
		jJumpTo(jf.startCode);
		return;
	}
	jitExitTo(out);
}

// What jitOne reads: the prefix, R15, the clock and the buffers.
static void jitCallOut(const struct JitState* s, u8 op) {
	jitFlush();
	if (!jitClean(s)) {
		jitWritePrefix(s);
	}
	jStI(2, GOFF(r[15]), s->pc & 0xffff);
	jClockOut();
	jitSfrOut();
	jCallI((const void*) jitOne, op);
	jitSfrIn();
	jClockIn();
	jf.romIdle = false;
}

static void jitColor(u32 v) {
	gsu.colr = color(v);
}

static void jitCmode(u32 v) {
	setPOR(v);
}

// SYNC_ROM_BUFFER's wait, the clock in memory.
static void jitRomWait(void) {
	stepBalanced(gsu.romcl);
}

// lm and lms after their operands: the word at the address (v's low half) in the RAM, into Rn (v >> 16).
static void jitLoad(u32 v) {
	u32 n = v >> 16, address = v & 0xffff;
	if (gsu.ramcl) {
		stepBalanced(gsu.ramcl);
	}
	readBalanced(0x700000 + (gsu.rambr << 16) + address);
	gsu.lo = gsu.result;
	readBalanced(0x700000 + (gsu.rambr << 16) + (address ^ 1));
	gsu.r[n] = (u8) gsu.result << 8 | gsu.lo;
	if (n != 14) {
		gsu.modified[n] = true;
	}
}

// sm and sms after their operands: Rn to the RAM through the buffer, a byte at a time.
static void jitStore(u32 v) {
	u32 n = v >> 16, address = v & 0xffff;
	for (u32 i = 0; i < 2; ++i) {
		if (gsu.ramcl) {
			stepBalanced(gsu.ramcl);
		}
		gsu.ramcl = gsu.clsr ? 5 : 6;
		gsu.ramar = address ^ i;
		gsu.ramdr = gsu.r[n] >> (i * 8);
	}
}

static void jitRam(bool load, u32 n, u32 address) {
	jStI(2, GOFF(ramaddr), address);
	jitFlush();
	jClockOut();
	jitSfrOut();
	jCallI(load ? (const void*) jitLoad : (const void*) jitStore, address | n << 16);
	jitSfrIn();
	jClockIn();
}

// plot as plotBalanced does it when no row of the pixel cache is flushed; 0, with nothing done, when
// one would be.
static u32 jitPlot(void) {
	u32 x = gsu.r[1] & 0xff, y = gsu.r[2] & 0xff;
	if (!gsu.por.transparent) {
		if (gsu.scmr.md == 3) {
			if (gsu.por.freezehigh ? (gsu.colr & 0x0f) == 0 : gsu.colr == 0) {
				return 1;
			}
		} else if ((gsu.colr & 0x0f) == 0) {
			return 1;
		}
	}
	u32 c = gsu.colr;
	if (gsu.por.dither && gsu.scmr.md != 3) {
		if ((x ^ y) & 1) {
			c >>= 4;
		}
		c &= 0x0f;
	}
	u32 bit = (x & 7) ^ 7;
	if ((u16) ((y << 5) + (x >> 3)) != gsu.pixelcache[0].offset || (gsu.pixelcache[0].bitpend | 1 << bit) == 0xff) {
		return 0;
	}
	gsu.pixelcache[0].data[bit] = c;
	gsu.pixelcache[0].bitpend |= 1 << bit;
	return 1;
}

// Compiles one instruction at the state s, which it moves on. Returns false when the block ends
// with it (it wrote R15).
static bool jitInstruction(struct JitState* s) {
	u8 op = s->pipe;
	u32 n = op & 15;
	bool r14 = false;
	const u32 sr = s->sreg, dr = s->dreg;
	// The fetch at the top of the loop.
	jitFetchStep();
	s->pipe = jitFetch(s->pc);
	if (op == 0x4c && !s->alt1) {
		// plot, then R1 + 1
		jitFlush();
		jCallV((const void*) jitPlot);
		uint8_t* flushes = jBranchZ(0);
		jLd(2, 0, GOFF(r[1]));
		jAluI(J_ADD, 0, 0, 1);
		jExt(J_UXTH, 0, 0);
		jitWriteR(1, 0);
		uint8_t* done = jJump();
		jPatchHere(flushes);
		jitCallOut(s, op);
		jPatchHere(done);
		jitRegsReset(s);
		s->last = op;
		s->pc = (u16) (s->pc + 1);
		return true;
	}
	if (op == 0x4e) {
		// color / cmode
		jitReadR(0, sr, s->pc);
		jCallR(s->alt1 ? (const void*) jitCmode : (const void*) jitColor, 0);
		jitRegsReset(s);
		s->last = op;
		s->pc = (u16) (s->pc + 1);
		return true;
	}
	if ((op == 0xef && dr != 14 && dr != 15) || (op == 0xdf && (!s->alt2 || s->alt1))) {
		// getb, getc and romb, once a ROM read under way is done.
		jitFlush();
		jLd(4, 0, GOFF(romcl));
		uint8_t* done = jBranchZ(0);
		jClockOut();
		jitSfrOut();
		jCallI((const void*) jitRomWait, 0);
		jitSfrIn();
		jClockIn();
		jPatchHere(done);
		if (op == 0xdf && s->alt2) {
			jitReadR(0, sr, s->pc);
			jAluI(J_AND, 0, 0, 0x7f);
			jSt(1, GOFF(rombr), 0);
			jitRegsReset(s);
			s->last = op;
			s->pc = (u16) (s->pc + 1);
			return true;
		}
		jLd(1, 1, GOFF(romdr));
		jSt(4, GOFF(result), 1);
		if (op == 0xdf) {
			jCallR((const void*) jitColor, 1);
		} else {
			if (s->alt1 && !s->alt2) {
				jitReadR(0, sr, s->pc);
				jAluI(J_AND, 0, 0, 0xff);
				jShift(J_LSL, 1, 1, 8);
				jAlu(J_OR, 1, 1, 0);
			} else if (s->alt2 && !s->alt1) {
				jitReadR(0, sr, s->pc);
				jAluI(J_AND, 0, 0, 0xff00);
				jAlu(J_OR, 1, 1, 0);
			} else if (s->alt1 && s->alt2) {
				jExt(J_SXTB, 1, 1);
				jExt(J_UXTH, 1, 1);
			}
			jitDR(dr, 1, &r14);
		}
		jitRegsReset(s);
		s->last = op;
		s->pc = (u16) (s->pc + 1);
		return true;
	}
	if (jitCalled(&(struct JitState) { .pipe = op, .alt1 = s->alt1, .dreg = s->dreg })) {
		jitCallOut(s, op);
		jitRegsReset(s);
		s->last = op;
		s->pc = (u16) (s->pc + 1);
		return true;
	}
	switch (op >> 4) {
	case 0x0:
		if (op == 0x01) {
			jitRegsReset(s);
		} else if (op == 0x03) {
			// lsr
			jitReadR(0, sr, s->pc);
			jAluI(J_AND, 2, 0, 1);
			jShift(J_LSL, 2, 2, SFR_CY);
			jShift(J_LSR, 1, 0, 1);
			jitDR(dr, 1, &r14);
			jitSZ(3, 1, 4);
			jAlu(J_OR, 2, 2, 3);
			jitSetFlags(1u << SFR_CY | 1u << SFR_S | 1u << SFR_Z, 2);
			jitRegsReset(s);
		} else if (op == 0x04) {
			// rol
			jitReadR(0, sr, s->pc);
			jMov(1, J_SFRV);
			jShift(J_LSR, 1, 1, SFR_CY);
			jAluI(J_AND, 1, 1, 1);
			jShift(J_LSL, 2, 0, 1);
			jAlu(J_OR, 2, 2, 1);
			jExt(J_UXTH, 2, 2);
			jitDR(dr, 2, &r14);
			jitSZ(3, 2, 4);
			jitFlagIf(3, 0, 0x8000, SFR_CY, 4);
			jitSetFlags(1u << SFR_CY | 1u << SFR_S | 1u << SFR_Z, 3);
			jitRegsReset(s);
		} else {
			// Branches: the displacement, then R15 + it if taken. The prefix stays.
			u8 disp = jitPipe(s);
			jStI(4, GOFF(result), disp);
			u32 after = s->pc;
			jitFlush();
			jMov(0, J_SFRV);
			int take = 1;
			switch (op) {
			case 0x05: jMovI(1, 1); break;
			case 0x06: case 0x07:
				jShift(J_LSR, 1, 0, SFR_S);
				jShift(J_LSR, 2, 0, SFR_OV);
				jAlu(J_XOR, 1, 1, 2);
				jAluI(J_AND, 1, 1, 1);
				if (op == 0x06) {
					jAluI(J_XOR, 1, 1, 1);
				}
				break;
			default: {
				static const u8 bit[16] = { [0x8] = SFR_Z, [0x9] = SFR_Z, [0xa] = SFR_S, [0xb] = SFR_S, [0xc] = SFR_CY, [0xd] = SFR_CY, [0xe] = SFR_OV, [0xf] = SFR_OV };
				jShift(J_LSR, 1, 0, bit[op]);
				jAluI(J_AND, 1, 1, 1);
				if (!(op & 1)) {
					jAluI(J_XOR, 1, 1, 1);
				}
			}
			}
			uint8_t* taken = jBranchNZ(take);
			struct JitState out = *s;
			out.last = op;
			out.pc = (u16) (after + 1);
			jitExitTo(&out);
			jPatchHere(taken);
			out.pc = (u16) (after + (s8) disp);
			jitGoTo(&out, op);
			return false;
		}
		break;
	case 0x1:
		if (!s->b) {
			s->dreg = n;
		} else {
			jitReadR(0, sr, s->pc);
			jitWriteR(n, 0);
			r14 |= n == 14;
			jitRegsReset(s);
		}
		break;
	case 0x2:
		s->sreg = s->dreg = n;
		s->b = true;
		break;
	case 0x3:
		if (op == 0x3c) {
			// loop
			jitFlush();
			jLd(2, 0, GOFF(r[12]));
			jAluI(J_SUB, 0, 0, 1);
			jExt(J_UXTH, 0, 0);
			jitWriteR(12, 0);
			jitSZ(1, 0, 2);
			jitSetFlags(1u << SFR_S | 1u << SFR_Z, 1);
			jitRegsReset(s);
			uint8_t* again = jBranchNZ(0);
			struct JitState out = *s;
			out.last = op;
			out.pc = (u16) (s->pc + 1);
			jitExitTo(&out);
			jPatchHere(again);
			jLd(2, 0, GOFF(r[13]));
			if (out.pipe == jf.startPipe) {
				// Back to this block's start when R13 is it, the opcode left as the loop would.
				jStI(1, GOFF(opcode), op);
				jPatchTo(jBranchEqI(0, jf.start), jf.startCode);
			}
			// R15 = R13, from a register: written here, then the rest of the state.
			out.pc = 0;
			jitWriteState(&out);
			jLd(2, 0, GOFF(r[13]));
			jSt(2, GOFF(r[15]), 0);
			jJumpExit();
			return false;
		}
		// Each sets its own flag and leaves the other.
		s->b = false;
		s->alt1 |= op != 0x3e;
		s->alt2 |= op != 0x3d;
		break;
	case 0x4:
		jitReadR(0, sr, s->pc);
		if (op == 0x4d) {
			jShift(J_LSR, 1, 0, 8);
			jShift(J_LSL, 2, 0, 8);
			jAlu(J_OR, 1, 1, 2);
		} else {
			jAluI(J_XOR, 1, 0, 0xffff);
		}
		jExt(J_UXTH, 1, 1);
		jitDR(dr, 1, &r14);
		jitSZ(2, 1, 3);
		jitSetFlags(1u << SFR_S | 1u << SFR_Z, 2);
		jitRegsReset(s);
		break;
	case 0x5:
	case 0x6: {
		bool add = op < 0x60, imm, carry, write = true;
		if (add) {
			imm = s->alt2;
			carry = s->alt1;
		} else {
			imm = s->alt2 && !s->alt1;
			carry = !s->alt2 && s->alt1;
			write = !s->alt2 || !s->alt1;
		}
		jitReadR(0, sr, s->pc);
		if (imm) {
			jMovI(1, n);
		} else {
			jitReadR(1, n, s->pc);
		}
		// r = SR +- n (+- carry), as 32 bits.
		jAlu(add ? J_ADD : J_SUB, 2, 0, 1);
		if (carry) {
			jMov(3, J_SFRV);
			jShift(J_LSR, 3, 3, SFR_CY);
			jAluI(J_AND, 3, 3, 1);
			if (add) {
				jAlu(J_ADD, 2, 2, 3);
			} else {
				jAluI(J_XOR, 3, 3, 1);
				jAlu(J_SUB, 2, 2, 3);
			}
		}
		// OV: add ~(SR ^ n) & (n ^ r), sub (SR ^ n) & (SR ^ r); bit 15.
		jAlu(J_XOR, 3, 0, 1);
		if (add) {
			jAluI(J_XOR, 3, 3, 0xffffffff);
			jAlu(J_XOR, 4, 1, 2);
		} else {
			jAlu(J_XOR, 4, 0, 2);
		}
		jAlu(J_AND, 3, 3, 4);
		jShift(J_LSR, 3, 3, 15);
		jAluI(J_AND, 3, 3, 1);
		jShift(J_LSL, 3, 3, SFR_OV);
		// CY: add r >= 0x10000, sub r >= 0 (signed).
		if (add) {
			jShift(J_LSR, 4, 2, 16);
			jSetNe0(4, 4);
		} else {
			jShift(J_LSR, 4, 2, 31);
			jAluI(J_XOR, 4, 4, 1);
		}
		jShift(J_LSL, 4, 4, SFR_CY);
		jAlu(J_OR, 3, 3, 4);
		jExt(J_UXTH, 2, 2);
		jitSZ(4, 2, 1);
		jAlu(J_OR, 3, 3, 4);
		if (write) {
			jitDR(dr, 2, &r14);
		}
		jitSetFlags(1u << SFR_OV | 1u << SFR_S | 1u << SFR_CY | 1u << SFR_Z, 3);
		jitRegsReset(s);
		break;
	}
	case 0x7:
	case 0xc:
		jitReadR(0, sr, s->pc);
		if (op == 0x70) {
			// merge
			jLd(2, 0, GOFF(r[7]));
			jAluI(J_AND, 0, 0, 0xff00);
			jLd(2, 1, GOFF(r[8]));
			jShift(J_LSR, 1, 1, 8);
			jAlu(J_OR, 1, 0, 1);
			jitDR(dr, 1, &r14);
			jMovI(2, 0);
			jitFlagIf(2, 1, 0xc0c0, SFR_OV, 3);
			jitFlagIf(2, 1, 0x8080, SFR_S, 3);
			jitFlagIf(2, 1, 0xe0e0, SFR_CY, 3);
			jitFlagIf(2, 1, 0xf0f0, SFR_Z, 3);
			jitSetFlags(1u << SFR_OV | 1u << SFR_S | 1u << SFR_CY | 1u << SFR_Z, 2);
		} else if (op == 0xc0) {
			// hib
			jShift(J_LSR, 1, 0, 8);
			jitDR(dr, 1, &r14);
			jMovI(2, 0);
			jitFlagIf(2, 1, 0x80, SFR_S, 3);
			jSetEq0(3, 1);
			jShift(J_LSL, 3, 3, SFR_Z);
			jAlu(J_OR, 2, 2, 3);
			jitSetFlags(1u << SFR_S | 1u << SFR_Z, 2);
		} else {
			if (s->alt2) {
				jMovI(1, n);
			} else {
				jitReadR(1, n, s->pc);
			}
			if (op < 0x80) {
				if (s->alt1) {
					jAluI(J_XOR, 1, 1, 0xffffffff);
				}
				jAlu(J_AND, 2, 0, 1);
			} else {
				jAlu(s->alt1 ? J_XOR : J_OR, 2, 0, 1);
			}
			jExt(J_UXTH, 2, 2);
			jitDR(dr, 2, &r14);
			jitSZ(3, 2, 4);
			jitSetFlags(1u << SFR_S | 1u << SFR_Z, 3);
		}
		jitRegsReset(s);
		break;
	case 0x8:
		// mult / umult
		jitReadR(0, sr, s->pc);
		if (s->alt2) {
			jMovI(1, n);
		} else {
			jitReadR(1, n, s->pc);
		}
		jExt(s->alt1 ? J_UXTB : J_SXTB, 0, 0);
		jExt(s->alt1 ? J_UXTB : J_SXTB, 1, 1);
		jMul(2, 0, 1);
		jExt(J_UXTH, 2, 2);
		jitDR(dr, 2, &r14);
		jitSZ(3, 2, 4);
		jitSetFlags(1u << SFR_S | 1u << SFR_Z, 3);
		jitRegsReset(s);
		if (!gsu.cfgr.ms0) {
			jitStep(jf.clsr ? 1 : 2);
		}
		break;
	case 0x9:
		if (op >= 0x91 && op <= 0x94) {
			// link
			jMovI(0, (s->pc + n) & 0xffff);
			jitWriteR(11, 0);
			jitRegsReset(s);
			break;
		}
		if (op >= 0x98 && op <= 0x9d) {
			// jmp: R15 from a register, written here, then the rest of the state.
			jitFlush();
			jitRegsReset(s);
			struct JitState out = *s;
			out.last = op;
			out.pc = 0;
			jitWriteState(&out);
			jLd(2, 0, GOFF(r[n]));
			jSt(2, GOFF(r[15]), 0);
			jJumpExit();
			return false;
		}
		jitReadR(0, sr, s->pc);
		if (op == 0x95) {
			jExt(J_SXTB, 1, 0);
		} else if (op == 0x96) {
			// asr / div2
			jExt(J_SXTH, 1, 0);
			jShift(J_ASR, 1, 1, 1);
			if (s->alt1) {
				jAluI(J_ADD, 2, 0, 1);
				jShift(J_LSR, 2, 2, 16);
				jAlu(J_ADD, 1, 1, 2);
			}
		} else if (op == 0x97) {
			// ror
			jMov(1, J_SFRV);
			jShift(J_LSR, 1, 1, SFR_CY);
			jAluI(J_AND, 1, 1, 1);
			jShift(J_LSL, 1, 1, 15);
			jShift(J_LSR, 2, 0, 1);
			jAlu(J_OR, 1, 1, 2);
		} else if (op == 0x9e) {
			jAluI(J_AND, 1, 0, 0xff);
		} else {
			// fmult / lmult: (s16) SR * (s16) R6
			jExt(J_SXTH, 1, 0);
			jLd(2, 2, GOFF(r[6]));
			jExt(J_SXTH, 2, 2);
			jMul(3, 1, 2);
			if (s->alt1) {
				jExt(J_UXTH, 1, 3);
				jitWriteR(4, 1);
			}
			jShift(J_LSR, 1, 3, 16);
			jExt(J_UXTH, 1, 1);
			jitDR(dr, 1, &r14);
			jitSZ(2, 1, 4);
			jitFlagIf(2, 3, 0x8000, SFR_CY, 4);
			jitSetFlags(1u << SFR_S | 1u << SFR_CY | 1u << SFR_Z, 2);
			jitRegsReset(s);
			jitStep((gsu.cfgr.ms0 ? 3 : 7) * (jf.clsr ? 1 : 2));
			break;
		}
		jExt(J_UXTH, 1, 1);
		jitDR(dr, 1, &r14);
		if (op == 0x9e) {
			jMovI(2, 0);
			jitFlagIf(2, 1, 0x80, SFR_S, 3);
			jSetEq0(3, 1);
			jShift(J_LSL, 3, 3, SFR_Z);
			jAlu(J_OR, 2, 2, 3);
			jitSetFlags(1u << SFR_S | 1u << SFR_Z, 2);
		} else {
			jitSZ(2, 1, 3);
			if (op == 0x96 || op == 0x97) {
				jitFlagIf(2, 0, 1, SFR_CY, 3);
				jitSetFlags(1u << SFR_CY | 1u << SFR_S | 1u << SFR_Z, 2);
			} else {
				jitSetFlags(1u << SFR_S | 1u << SFR_Z, 2);
			}
		}
		jitRegsReset(s);
		break;
	case 0xa: {
		u8 v = jitPipe(s);
		jStI(4, GOFF(result), v);
		if (s->alt1 || s->alt2) {
			// lms / sms
			jitRam(s->alt1, n, v << 1);
			r14 |= s->alt1 && n == 14;
			jitRegsReset(s);
			break;
		}
		// ibt
		jMovI(0, (u16) (s8) v);
		jitWriteR(n, 0);
		r14 |= n == 14;
		jitRegsReset(s);
		break;
	}
	case 0xb:
		if (!s->b) {
			s->sreg = n;
		} else {
			// moves
			jitReadR(0, n, s->pc);
			jitDR(dr, 0, &r14);
			jMovI(2, 0);
			jitFlagIf(2, 0, 0x80, SFR_OV, 3);
			jitSZ(3, 0, 4);
			jAlu(J_OR, 2, 2, 3);
			jitSetFlags(1u << SFR_OV | 1u << SFR_S | 1u << SFR_Z, 2);
			jitRegsReset(s);
		}
		break;
	case 0xd:
	case 0xe:
		jLd(2, 0, GOFF(r[n]));
		jAluI(op < 0xe0 ? J_ADD : J_SUB, 0, 0, 1);
		jExt(J_UXTH, 0, 0);
		jitWriteR(n, 0);
		r14 |= n == 14;
		jitSZ(1, 0, 2);
		jitSetFlags(1u << SFR_S | 1u << SFR_Z, 1);
		jitRegsReset(s);
		break;
	case 0xf: {
		u8 lo = jitPipe(s);
		u8 hi = jitPipe(s);
		jStI(4, GOFF(result), hi);
		if (s->alt1 || s->alt2) {
			// lm / sm
			jitRam(s->alt1, n, hi << 8 | lo);
			r14 |= s->alt1 && n == 14;
			jitRegsReset(s);
			break;
		}
		// iwt
		jStI(1, GOFF(lo), lo);
		if (n == 15) {
			jitFlush();
			jitRegsReset(s);
			struct JitState out = *s;
			out.last = op;
			out.pc = hi << 8 | lo;
			jitGoTo(&out, op);
			return false;
		}
		jMovI(0, hi << 8 | lo);
		jitWriteR(n, 0);
		r14 |= n == 14;
		jitRegsReset(s);
		break;
	}
	}
	if (r14) {
		jitEndR14();
	}
	s->last = op;
	s->pc = (u16) (s->pc + 1);
	return true;
}

static const void* jitCompile(u32 pc, u8 pipe) {
	if (!jitCode) {
		jitCode = mmap(NULL, JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (jitCode == MAP_FAILED) {
			jitCode = NULL;
			gsuJitEnabled = false;
			return NULL;
		}
		jitE = (struct Emitter) { jitCode, jitCode, jitCode + JIT_CODE_SIZE };
		jTrampolines();
		jFlush(jitCode, jitE.p);
		jitStart = jitCode + (((jitE.p - jitCode) + 15) & ~15);
		jitFree = jitStart;
	}
	// Room for a block, or a fresh start.
	if (jitFree + 64 * 1024 > jitCode + JIT_CODE_SIZE || jitSlotCount + 4 * JIT_BLOCK_MAX > sizeof(jitSlots) / sizeof(*jitSlots) ||
	    jitExitCount + 4 * JIT_BLOCK_MAX > sizeof(jitExits) / sizeof(*jitExits)) {
		jitFree = jitStart;
		jitSlotCount = 0;
		jitExitCount = 0;
		jitGen++;
	}
	jitE = (struct Emitter) { jitFree, jitFree, jitCode + JIT_CODE_SIZE };
	jf.clsr = gsu.clsr;
	jf.exitCount = 0;
	jf.pending = 0;
	jf.start = pc;
	jf.startPipe = pipe;
	jf.fetched = 0;
	jf.rom = (u16) (pc - gsu.cbr) >= 512;
	jf.romIdle = false;
	struct JitState s = { .pc = pc, .pipe = pipe, .last = -1 };
	u8 x;
	if (!jitByte(pc, &x) || !jitSupported(&s)) {
		return &jitNone;
	}
	const uint8_t* start = jitE.p;
	jf.startCode = start;
	for (u32 count = 0;; count++) {
		if (count == JIT_BLOCK_MAX || !jitByte(s.pc, &x) || !jitSupported(&s)) {
			jitFlush();
			jitExitTo(&s);
			break;
		}
		if (!count) {
			jitExitLater(jDeadline(), &s);
		}
		if (!jitInstruction(&s)) {
			break;
		}
	}
	for (u32 i = 0; i < jf.exitCount; ++i) {
		jPatchTo(jf.exits[i].site, jitE.p);
		jitExitStub(&jf.exits[i].s);
	}
	if (jitE.p > jitE.end) {
		jitFree = jitE.end;
		return NULL;
	}
	jFlush((uint8_t*) start, jitE.p);
	jitFree = jitE.p;
	while ((uintptr_t) jitFree & 15) {
		jitFree++;
	}
	return start;
}

// Whether the length bytes of the program at pc are in valid lines of the cache, and are these.
static bool jitInCache(u32 pc, u32 length, const u8* bytes) {
	u32 offset = (u16) (pc - gsu.cbr);
	if (offset + length > 512) {
		return false;
	}
	for (u32 line = offset >> 4; line <= (offset + length - 1) >> 4; ++line) {
		if (!gsu.cacheValid[line]) {
			return false;
		}
	}
	return !memcmp(&gsu.cache[offset], bytes, length);
}

// The valid lines among those a block from offset could reach, 0xff if all are. A block stops where
// they do, so blocks are kept for each set: its extent depends only on the state it is entered in.
static u32 jitValid, jitValidEpoch;

static u8 jitLines(u32 offset) {
	if (jitValidEpoch != jitEpoch) {
		jitValidEpoch = jitEpoch;
		jitValid = 0;
		for (u32 line = 0; line < 32; ++line) {
			jitValid |= (u32) gsu.cacheValid[line] << line;
		}
	}
	u32 last = offset + JIT_BYTES - 1 < 511 ? offset + JIT_BYTES - 1 : 511;
	u32 all = (1u << ((last >> 4) - (offset >> 4) + 1)) - 1;
	u32 mask = jitValid >> (offset >> 4) & all;
	return mask == all ? 0xff : (u8) mask;
}

// An instruction the interpreter runs outside the cache: no block comes next for an exit to chain to.
static inline void gsuJitPass(void) {
	jitLastSlot = NULL;
}

// Runs a block for R15 and the pipeline, if there is one or can be; false if not.
static bool gsuJitRun(u32 pc, u8 pipe, u64 until) {
	struct JitSlot* last = jitLastSlot;
	jitLastSlot = NULL;
	u32 offset = (u16) (pc - gsu.cbr);
	bool rom = offset >= 512;
	if (rom ? gsu.pbr > 0x5f : !gsu.cacheValid[offset >> 4]) {
		return false;
	}
	u8 lines = rom ? 0 : jitLines(offset);
	u8 pbr = rom ? gsu.pbr : 0;
	u16 cbr = rom ? gsu.cbr : 0;
	u32 set = (pc * 0x9e3779b1u + pipe * 0x85ebca6bu + (lines | pbr << 8 | cbr << 16) * 0xc2b2ae35u) >> (32 - JIT_SET_BITS);
	u32 index = set * 4;
	struct JitEntry* e = NULL;
	for (u32 way = 0; way < 4; ++way, ++index) {
		struct JitEntry* t = &jitTable[index];
		if (t->gen == jitGen && t->pc == pc && t->pipe == pipe && t->clsr == gsu.clsr && t->ms0 == gsu.cfgr.ms0 && t->lines == lines &&
		    t->pbr == pbr && t->cbr == cbr) {
			e = t;
			break;
		}
	}
	if (!e) {
		index = set * 4 + (jitVictim[set]++ & 3);
		e = &jitTable[index];
		*e = (struct JitEntry) { NULL, jitGen, 0, (u16) pc, 0, pipe, gsu.clsr, gsu.cfgr.ms0, 0, lines, pbr, cbr };
	}
	if (!rom && e->code && e->checked != jitEpoch) {
		if (!jitInCache(pc, e->length, jitBytes[index])) {
			e->code = NULL;
			e->count = 0;
		}
		e->checked = jitEpoch;
	}
	if (!e->code) {
		e->code = jitCompile(pc, pipe);
		if (!e->code) {
			return false;
		}
		// The bytes it fetched: from pc to the furthest one.
		e->length = jf.fetched ? jf.fetched : 1;
		if (!rom) {
			memcpy(jitBytes[index], &gsu.cache[offset], e->length);
		}
		e->checked = jitEpoch;
	}
	if (e->code == &jitNone) {
		return false;
	}
	// The exit that came here named a slot for this block: chain it.
	if (last && last->pc == pc && last->pipe == pipe && e->gen == jitGen) {
		last->target = e->code;
		last->epoch = last->rom ? jitRomEpoch : jitEpoch;
	}
	jitEnter(e->code, until);
	return true;
}

#else
#define GSU_JIT 0
static void gsuJitInvalidate(void) {
}
static void gsuJitLines(void) {
}
static inline void gsuJitPass(void) {
}
static void gsuJitDrop(void) {
}
bool gsuJitEnabled;
#endif

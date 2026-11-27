/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// Fast mode's SA-1 recompiler, included by sa1.c: a block (see blockMake) becomes host code that does
// what blockRun does for it. Each instruction takes its fetches' steps, leaves the bus and PC as its
// fetches would, and then either runs as host code (the plain loads, stores, ALU and register
// instructions, and the branches that end blocks) or calls sa1BlockInstruction. Its accesses to I-RAM
// read it in place with the steps fastRead takes; any other goes through sa1FastRead or sa1FastWrite.
// Host-code instructions leave lastCycle out: a block runs only while it can't raise an interrupt
// (blockGoOn), which only a call out can change, and after each the block looks again. Between
// instructions it stops where sa1Run would: once the SA-1 is past the S-CPU.

#if defined(__x86_64__) || defined(__aarch64__)
#define SA1_JIT 1

#include <stddef.h>
#include <sys/mman.h>

// The 65816's register names are macros here; the emitters and the backend use some of them.
#pragma push_macro("R")
#undef R
#pragma push_macro("PC")
#undef PC
#pragma push_macro("A")
#undef A
#pragma push_macro("X")
#undef X
#pragma push_macro("Y")
#undef Y
#pragma push_macro("Z")
#undef Z
#pragma push_macro("S")
#undef S
#pragma push_macro("D")
#undef D
#pragma push_macro("B")
#undef B
#pragma push_macro("U")
#undef U
#pragma push_macro("V")
#undef V
#pragma push_macro("W")
#undef W
#pragma push_macro("CF")
#undef CF
#pragma push_macro("ZF")
#undef ZF
#pragma push_macro("IF")
#undef IF
#pragma push_macro("DF")
#undef DF
#pragma push_macro("XF")
#undef XF
#pragma push_macro("MF")
#undef MF
#pragma push_macro("VF")
#undef VF
#pragma push_macro("NF")
#undef NF
#pragma push_macro("EF")
#undef EF

#if defined(__x86_64__)
#include "../../shared/emit/x64/emit.h"
#else
#include "../../shared/emit/a64/emit.h"
#endif

// Past the shared code, the buffer is in stretches: blocks' code in the first part of one, and in the
// rest what they seldom run (the paths after a call out, decimal mode, a chain still to be found) and the
// bytes they were made from, close enough for any branch between the two.
enum { JIT_CODE_SIZE = 32 << 20, JIT_STRETCH = 1 << 20, JIT_HOT_PART = 704 << 10, JIT_BLOCK_MOST = 64 << 10 };

#define SO(field) ((u32) offsetof(struct Sa1, field))

// The part of the stretch being written, and the other.
static struct Emitter jE, jAway;

static void jStepsOut(void);
static void jStepsKept(void);

static void jSwap(void) {
	jStepsOut();
	struct Emitter part = jE;
	jE = jAway;
	jAway = part;
}

// A block's exit to a known block further on: that block's code once it's been found (blockRun fills it
// in after an exit through it found it empty, jitPending), so the exit goes straight on as sa1Run would.
static struct JitCell {
	const void* target;
	u32 key;
} jitCells[1 << 16];
static u32 jitCellCount;
static u32 jitCellsGeneration;
static struct JitCell* jitPending;

// Set where an instruction called out on a path it may not take (jGoOnIfCalled after it): kept in w25
// on AArch64.
static u8 jitCalled;
static uint8_t* jCode;
static uint8_t* jBlocksStart;  // past the shared code
static uint8_t* jBody;  // where the code jitMake made last starts past its prologue
static const uint8_t* jSyncOut;
static const uint8_t* jSyncIn;
static bool jFailed;

static u32 jitGoOn(void) {
	return blockGoOn();
}

// --- Backend: values in four registers, the SA-1 in a fifth; scratch registers besides ---

enum { J_ADD, J_SUB, J_AND, J_OR, J_XOR };
enum { JC_EQ, JC_NE, JC_HS, JC_LO };

#if defined(__x86_64__)

static const int jv[4] = { RAX, RCX, RDX, R8 };
#define JB RBX

static void jLd(int width, int d, u32 off) {
	if (width == 4) {
		xLoad(&jE, 4, jv[d], M(JB, off));
	} else {
		xMovzx(&jE, jv[d], width, M(JB, off));
	}
}

static void jSt(int width, u32 off, int s) {
	xStore(&jE, width, M(JB, off), jv[s]);
}

static void jStI(int width, u32 off, u32 imm) {
	xStoreI(&jE, width, M(JB, off), imm);
}

static void jMovI(int d, u32 imm) {
	xMovI(&jE, jv[d], imm);
}

static void jMov(int d, int a) {
	xMovRR(&jE, 4, jv[d], jv[a]);
}

static const int jAluX[5] = { X_ADD, X_SUB, X_AND, X_OR, X_XOR };

static void jAluI(int op, int d, u32 imm) {
	xAluI(&jE, jAluX[op], 4, R(jv[d]), (int32_t) imm);
}

static void jAlu(int op, int d, int s) {
	xAluRR(&jE, jAluX[op], 4, R(jv[d]), jv[s]);
}

static void jShr(int d, int n) {
	xShiftI(&jE, X_SHR, 4, R(jv[d]), n);
}

static void jShl(int d, int n) {
	xShiftI(&jE, X_SHL, 4, R(jv[d]), n);
}

static const int jCc[4] = { CC_E, CC_NE, CC_AE, CC_B };

static void jCmpI(int a, u32 imm) {
	jStepsOut();
	xAluI(&jE, X_CMP, 4, R(jv[a]), (int32_t) imm);
}

static void jCmp(int a, int b) {
	jStepsOut();
	xAluRR(&jE, X_CMP, 4, R(jv[a]), jv[b]);
}

// The flag byte at off = the condition of the last compare.
static void jSetFlag(int cond, u32 off) {
	jStepsKept();
	xSetcc(&jE, jCc[cond], M(JB, off));
}

static uint8_t* jBranch(int cond) {
	jStepsKept();
	return xJcc(&jE, jCc[cond], NULL);
}

static uint8_t* jJump(void) {
	jStepsOut();
	return xJmp(&jE, NULL);
}

static void jJumpTo(const uint8_t* to) {
	jStepsOut();
	xJmp(&jE, to);
}

// A branch in either part to here, unless code has been left out of a part that's full.
static void jPatch(uint8_t* site) {
	jStepsOut();
	if (jE.p <= jE.end && jAway.p <= jAway.end) {
		patchRel32(site, jE.p);
	}
}

static void jArg(int n, int v) {
	xMovRR(&jE, 4, n ? RSI : RDI, jv[v]);
}

static void jArgP(int n, const void* p) {
	xMovI64(&jE, n ? RSI : RDI, (u64) (uintptr_t) p);
}

// The clock while a block runs: r14 the SA-1's, r15 the S-CPU's, rbp what a step adds (2 x the scalar);
// r13 the steps since the H and V counters were last brought up to date.
// Out: the clock, H and V counters as fastSteps leaves them, r13 starting again at 0.
static void jSyncOutMake(void) {
	jSyncOut = jE.p;
	xStore(&jE, 8, M(JB, SO(th.clock)), R14);
	xMovzx(&jE, R9, 2, M(JB, SO(status.hcounter)));
	xAluRR(&jE, X_ADD, 4, R(R9), R13);
	xAluRR(&jE, X_ADD, 4, R(R9), R13);
	uint8_t* line = jE.p;
	xAluI(&jE, X_CMP, 4, R(R9), 1364);
	uint8_t* same = xJcc(&jE, CC_B, NULL);
	xAluI(&jE, X_SUB, 4, R(R9), 1364);
	xMovzx(&jE, R10, 2, M(JB, SO(status.vcounter)));
	xAluI(&jE, X_ADD, 4, R(R10), 1);
	xMovzx(&jE, R11, 2, M(JB, SO(status.scanlines)));
	xAluRR(&jE, X_CMP, 4, R(R10), R11);
	uint8_t* within = xJcc(&jE, CC_B, NULL);
	xMovI(&jE, R10, 0);
	patchHere(&jE, within);
	xStore(&jE, 2, M(JB, SO(status.vcounter)), R10);
	xJmp(&jE, line);
	patchHere(&jE, same);
	xStore(&jE, 2, M(JB, SO(status.hcounter)), R9);
	xMovI(&jE, R13, 0);
	xRet(&jE);
}

// In: both clocks again.
static void jSyncInMake(void) {
	jSyncIn = jE.p;
	xLoad(&jE, 8, R14, M(JB, SO(th.clock)));
	xMovI64(&jE, R11, (u64) (uintptr_t) &cpu.th.clock);
	xLoad(&jE, 8, R15, M(R11, 0));
	xRet(&jE);
}

// A call: the clock out before it and in after; its result is in value 0.
static void jCall(const void* fn) {
	jStepsOut();
	xCall(&jE, jSyncOut);
	xMovI64(&jE, R11, (u64) (uintptr_t) fn);
	xCallR(&jE, R11);
	xPush(&jE, RAX);
	xCall(&jE, jSyncIn);
	xPop(&jE, RAX);
}

// A byte of I-RAM at the address in value a into value d.
static void jLdIram(int d, int a) {
	xMovzx(&jE, jv[d], 1, MI(JB, jv[a], 1, SO(iram)));
}

static void jStIram(int a, int s) {
	xStore(&jE, 1, MI(JB, jv[a], 1, SO(iram)), jv[s]);
}

// A byte of the cartridge's RAM at the offset in value a: into value d, or from value s.
static void jLdRam(int d, int a) {
	xMovI64(&jE, R11, (u64) (uintptr_t) &cart.ram);
	xLoad(&jE, 8, R11, M(R11, 0));
	xMovzx(&jE, jv[d], 1, MI(R11, jv[a], 1, 0));
}

static void jStRam(int a, int s) {
	xMovI64(&jE, R11, (u64) (uintptr_t) &cart.ram);
	xLoad(&jE, 8, R11, M(R11, 0));
	xStore(&jE, 1, MI(R11, jv[a], 1, 0), jv[s]);
}

// A byte at an absolute address: loaded into d, or set to imm.
static void jLdAbs8(int d, const void* p) {
	xMovI64(&jE, R11, (u64) (uintptr_t) p);
	xMovzx(&jE, jv[d], 1, M(R11, 0));
}

static void jStAbs8(const void* p, u32 imm) {
	xMovI64(&jE, R11, (u64) (uintptr_t) p);
	xStoreI(&jE, 1, M(R11, 0), imm);
}

static void jCalledSet(u32 imm) {
	jStAbs8(&jitCalled, imm);
}

// A branch taken once something has called out.
static uint8_t* jIfCalled(void) {
	jStepsOut();
	xMovI64(&jE, R11, (u64) (uintptr_t) &jitCalled);
	xAluI(&jE, X_CMP, 1, M(R11, 0), 0);
	return xJcc(&jE, CC_NE, NULL);
}

// imm stored at two offsets, width bytes each.
static void jStI2(int width, u32 off1, u32 off2, u32 imm) {
	jStI(width, off1, imm);
	jStI(width, off2, imm);
}

// Value d = value b << 16, plus imm.
static void jShl16Add(int d, int b, u32 imm) {
	if (d != b) {
		jMov(d, b);
	}
	jShl(d, 16);
	jAluI(J_ADD, d, imm);
}

// Value d = value a shifted right by n.
static void jShrFrom(int d, int a, int n) {
	xMovRR(&jE, 4, jv[d], jv[a]);
	xShiftI(&jE, X_SHR, 4, R(jv[d]), n);
}

// loop.last on the way out of a block, set in r12 by the exit taken.
static void jLastOut(u32 pc) {
	xMovI(&jE, R12, pc);
}

// To the cell's block if there is one; if not, the cell left for blockRun and on to a jump in the other
// part, for the caller to place.
static uint8_t* jChainJump(struct JitCell* cell) {
	jStepsOut();
	xMovI64(&jE, R11, (u64) (uintptr_t) &cell->target);
	xLoad(&jE, 8, R11, M(R11, 0));
	xTest(&jE, 8, R(R11), R11);
	uint8_t* miss = xJcc(&jE, CC_E, NULL);
	xJmpR(&jE, R11);
	jSwap();
	jPatch(miss);
	xMovI64(&jE, R10, (u64) (uintptr_t) cell);
	xMovI64(&jE, R11, (u64) (uintptr_t) &jitPending);
	xStore(&jE, 8, M(R11, 0), R10);
	uint8_t* on = xJmp(&jE, NULL);
	jSwap();
	return on;
}

// Shared code: entered with a call from a block, the stack kept aligned for its calls to C.
static void jStubEnter(void) {
	xAluI(&jE, X_SUB, 8, R(RSP), 8);
}

static void jStubLeave(void) {
	jStepsOut();
	xAluI(&jE, X_ADD, 8, R(RSP), 8);
	xRet(&jE);
}

static void jCallStub(const uint8_t* stub) {
	jStepsOut();
	xCall(&jE, stub);
}

static void jStepsAdd(u32 n) {
	if (n == 1) {
		xAluRR(&jE, X_ADD, 8, R(R14), RBP);
	} else {
		xMovI(&jE, R10, n);
		xMovRR(&jE, 8, R9, RBP);
		xImul(&jE, 8, R9, R(R10));
		xAluRR(&jE, X_ADD, 8, R(R14), R9);
	}
	xAluI(&jE, X_ADD, 4, R(R13), (int32_t) n);
}

// To the site's target once the SA-1 is past the S-CPU.
static uint8_t* jLate(void) {
	jStepsOut();
	xAluRR(&jE, X_CMP, 8, R(R15), R14);
	return xJcc(&jE, CC_B, NULL);
}

static void jPrologue(void) {
	xPush(&jE, JB);
	xPush(&jE, RBP);
	xPush(&jE, R12);
	xPush(&jE, R13);
	xPush(&jE, R14);
	xPush(&jE, R15);
	xAluI(&jE, X_SUB, 8, R(RSP), 8);
	xMovI64(&jE, JB, (u64) (uintptr_t) &sa1);
	xMovI(&jE, R13, 0);
	xLoad(&jE, 8, RBP, M(JB, SO(th.scalar)));
	xAluRR(&jE, X_ADD, 8, R(RBP), RBP);
	xCall(&jE, jSyncIn);
}

static void jEpilogue(void) {
	jStepsOut();
	xCall(&jE, jSyncOut);
	xMovI64(&jE, R11, (u64) (uintptr_t) &loop.last);
	xStore(&jE, 4, M(R11, 0), R12);
	xAluI(&jE, X_ADD, 8, R(RSP), 8);
	xPop(&jE, R15);
	xPop(&jE, R14);
	xPop(&jE, R13);
	xPop(&jE, R12);
	xPop(&jE, RBP);
	xPop(&jE, JB);
	xRet(&jE);
}

static void jFlush(uint8_t* start, uint8_t* end) {
	(void) start;
	(void) end;
}

#else

static const int jv[4] = { 9, 10, 11, 12 };
#define JB 19
#define JT 16
#define JT2 17

static void jLd(int width, int d, u32 off) {
	if (width == 1) {
		aLdrb(&jE, jv[d], JB, off);
	} else if (width == 2) {
		aLdrh(&jE, jv[d], JB, off);
	} else {
		aLdr(&jE, jv[d], JB, off);
	}
}

static void jStR(int width, u32 off, int r) {
	if (width == 1) {
		aStrb(&jE, r, JB, off);
	} else if (width == 2) {
		aStrh(&jE, r, JB, off);
	} else {
		aStr(&jE, r, JB, off);
	}
}

static void jSt(int width, u32 off, int s) {
	jStR(width, off, jv[s]);
}

static void jStI(int width, u32 off, u32 imm) {
	if (!imm) {
		jStR(width, off, ZR);
		return;
	}
	aMovImm(&jE, JT, imm);
	jStR(width, off, JT);
}

static void jMovI(int d, u32 imm) {
	aMovImm(&jE, jv[d], imm);
}

static void jMov(int d, int a) {
	aMov(&jE, jv[d], jv[a]);
}

static const uint32_t jAluA[5] = { A_ADD, A_SUB, A_AND, A_ORR, A_EOR };

static void jAluI(int op, int d, u32 imm) {
	if ((op == J_ADD || op == J_SUB) && imm < 4096) {
		aDpImm(&jE, op == J_ADD ? A_ADD_IMM : A_SUB_IMM, jv[d], jv[d], imm);
		return;
	}
	static const uint32_t logic[5] = { 0, 0, A_AND_IMM, A_ORR_IMM, A_EOR_IMM };
	if (logic[op] && aLogicImm(&jE, logic[op], jv[d], jv[d], imm)) {
		return;
	}
	aMovImm(&jE, JT, imm);
	aDp(&jE, jAluA[op], jv[d], jv[d], JT, S_LSL, 0);
}

static void jAlu(int op, int d, int s) {
	aDp(&jE, jAluA[op], jv[d], jv[d], jv[s], S_LSL, 0);
}

static void jShr(int d, int n) {
	aLsrImm(&jE, jv[d], jv[d], n);
}

static void jShl(int d, int n) {
	aLslImm(&jE, jv[d], jv[d], n);
}

static const int jCc[4] = { C_EQ, C_NE, C_HS, C_LO };

static void jCmpI(int a, u32 imm) {
	jStepsOut();
	if (imm < 4096) {
		aDpImm(&jE, A_SUBS_IMM, ZR, jv[a], imm);
		return;
	}
	aMovImm(&jE, JT, imm);
	aDp(&jE, A_SUBS, ZR, jv[a], JT, S_LSL, 0);
}

static void jCmp(int a, int b) {
	jStepsOut();
	aDp(&jE, A_SUBS, ZR, jv[a], jv[b], S_LSL, 0);
}

static void jSetFlag(int cond, u32 off) {
	jStepsKept();
	aCset(&jE, JT, jCc[cond]);
	aStrb(&jE, JT, JB, off);
}

static uint8_t* jBranch(int cond) {
	jStepsKept();
	return aBcond(&jE, jCc[cond], NULL);
}

static uint8_t* jJump(void) {
	jStepsOut();
	return aB(&jE, NULL);
}

static void jJumpTo(const uint8_t* to) {
	jStepsOut();
	aB(&jE, to);
}

static void jPatch(uint8_t* site) {
	jStepsOut();
	if (jE.p <= jE.end && jAway.p <= jAway.end) {
		patchBranch(site, jE.p);
	}
}

static void jArg(int n, int v) {
	aMov(&jE, n, jv[v]);
}

static void jArgP(int n, const void* p) {
	aMovImm64(&jE, n, (u64) (uintptr_t) p);
}

// x22 the SA-1's clock, x23 the S-CPU's, x24 what a step adds; x21 the steps since the H and V counters
// were last brought up to date. Out and in as on x86-64; x15-x17 are theirs.
static void jSyncOutMake(void) {
	jSyncOut = jE.p;
	aStrX(&jE, 22, JB, SO(th.clock));
	aLdrh(&jE, JT, JB, SO(status.hcounter));
	aDp(&jE, A_ADD, JT, JT, 21, S_LSL, 1);
	uint8_t* line = jE.p;
	aDpImm(&jE, A_SUBS_IMM, ZR, JT, 1364);
	uint8_t* same = aBcond(&jE, C_LO, NULL);
	aDpImm(&jE, A_SUB_IMM, JT, JT, 1364);
	aLdrh(&jE, JT2, JB, SO(status.vcounter));
	aDpImm(&jE, A_ADD_IMM, JT2, JT2, 1);
	aLdrh(&jE, 15, JB, SO(status.scanlines));
	aDp(&jE, A_SUBS, ZR, JT2, 15, S_LSL, 0);
	uint8_t* within = aBcond(&jE, C_LO, NULL);
	aMov(&jE, JT2, ZR);
	patchHere(&jE, within);
	aStrh(&jE, JT2, JB, SO(status.vcounter));
	aB(&jE, line);
	patchHere(&jE, same);
	aStrh(&jE, JT, JB, SO(status.hcounter));
	aMovImm(&jE, 21, 0);
	aRet(&jE);
}

static void jSyncInMake(void) {
	jSyncIn = jE.p;
	aLdrX(&jE, 22, JB, SO(th.clock));
	aMovImm64(&jE, JT, (u64) (uintptr_t) &cpu.th.clock);
	aLdrX(&jE, 23, JT, 0);
	aRet(&jE);
}

static void jCall(const void* fn) {
	jStepsOut();
	aBl(&jE, jSyncOut);
	aMovImm64(&jE, JT, (u64) (uintptr_t) fn);
	aBlr(&jE, JT);
	aMov(&jE, jv[0], 0);
	aBl(&jE, jSyncIn);
}

static void jLdIram(int d, int a) {
	aAddImmX(&jE, JT, JB, SO(iram));
	aLdst(&jE, L_LDRB, jv[d], JT, jv[a], L_UXTW);
}

static void jStIram(int a, int s) {
	aAddImmX(&jE, JT, JB, SO(iram));
	aLdst(&jE, L_STRB, jv[s], JT, jv[a], L_UXTW);
}

static void jLdRam(int d, int a) {
	aMovImm64(&jE, JT, (u64) (uintptr_t) &cart.ram);
	aLdrX(&jE, JT, JT, 0);
	aLdst(&jE, L_LDRB, jv[d], JT, jv[a], L_UXTW);
}

static void jStRam(int a, int s) {
	aMovImm64(&jE, JT, (u64) (uintptr_t) &cart.ram);
	aLdrX(&jE, JT, JT, 0);
	aLdst(&jE, L_STRB, jv[s], JT, jv[a], L_UXTW);
}

static void jLdAbs8(int d, const void* p) {
	aMovImm64(&jE, JT, (u64) (uintptr_t) p);
	aLdrb(&jE, jv[d], JT, 0);
}

static void jStAbs8(const void* p, u32 imm) {
	aMovImm64(&jE, JT, (u64) (uintptr_t) p);
	if (imm) {
		aMovImm(&jE, JT2, imm);
		aStrb(&jE, JT2, JT, 0);
	} else {
		aStrb(&jE, ZR, JT, 0);
	}
}

static void jCalledSet(u32 imm) {
	aMovImm(&jE, 25, imm);
}

static uint8_t* jIfCalled(void) {
	jStepsOut();
	return aCbnz(&jE, 25, NULL);
}

static void jStI2(int width, u32 off1, u32 off2, u32 imm) {
	if (!imm) {
		jStR(width, off1, ZR);
		jStR(width, off2, ZR);
		return;
	}
	aMovImm(&jE, JT, imm);
	jStR(width, off1, JT);
	jStR(width, off2, JT);
}

static void jShl16Add(int d, int b, u32 imm) {
	aMovImm(&jE, JT, imm);
	aDp(&jE, A_ADD, jv[d], JT, jv[b], S_LSL, 16);
}

static void jShrFrom(int d, int a, int n) {
	aLsrImm(&jE, jv[d], jv[a], n);
}

// loop.last on the way out, in w20.
static void jLastOut(u32 pc) {
	aMovImm(&jE, 20, pc);
}

static uint8_t* jChainJump(struct JitCell* cell) {
	jStepsOut();
	aMovImm64(&jE, JT, (u64) (uintptr_t) &cell->target);
	aLdrX(&jE, JT, JT, 0);
	aDpImm(&jE, A_SUBS_IMM | W64, ZR, JT, 0);
	uint8_t* miss = aBcond(&jE, C_EQ, NULL);
	aBr(&jE, JT);
	jSwap();
	jPatch(miss);
	aMovImm64(&jE, JT, (u64) (uintptr_t) cell);
	aMovImm64(&jE, JT2, (u64) (uintptr_t) &jitPending);
	aStrX(&jE, JT, JT2, 0);
	uint8_t* on = aB(&jE, NULL);
	jSwap();
	return on;
}

static void jStubEnter(void) {
	aPush(&jE, 29, 30, 16);
}

static void jStubLeave(void) {
	jStepsOut();
	aPop(&jE, 29, 30, 16);
	aRet(&jE);
}

static void jCallStub(const uint8_t* stub) {
	jStepsOut();
	aBl(&jE, stub);
}

static void jStepsAdd(u32 n) {
	if (!(n & (n - 1))) {
		aDp(&jE, A_ADD | W64, 22, 22, 24, S_LSL, __builtin_ctz(n));
	} else {
		aMovImm(&jE, JT, n);
		// x22 += x24 * x16 (MADD)
		emit32(&jE, 0x9B000000 | (JT << 16) | (22 << 10) | (24 << 5) | 22);
	}
	aDpImm(&jE, A_ADD_IMM | W64, 21, 21, n);
}

static uint8_t* jLate(void) {
	jStepsOut();
	aDp(&jE, A_SUBS | W64, ZR, 23, 22, S_LSL, 0);
	return aBcond(&jE, C_LO, NULL);
}

static void jPrologue(void) {
	aPush(&jE, 29, 30, 80);
	aStp(&jE, 19, 20, 16);
	aStp(&jE, 21, 22, 32);
	aStp(&jE, 23, 24, 48);
	aStp(&jE, 25, 26, 64);
	aMovImm(&jE, 25, 0);
	aMovImm64(&jE, JB, (u64) (uintptr_t) &sa1);
	aMovImm(&jE, 21, 0);
	aLdrX(&jE, 24, JB, SO(th.scalar));
	aDp(&jE, A_ADD | W64, 24, 24, 24, S_LSL, 0);
	aBl(&jE, jSyncIn);
}

static void jEpilogue(void) {
	jStepsOut();
	aBl(&jE, jSyncOut);
	aMovImm64(&jE, JT, (u64) (uintptr_t) &loop.last);
	aStr(&jE, 20, JT, 0);
	aLdp(&jE, 25, 26, 64);
	aLdp(&jE, 23, 24, 48);
	aLdp(&jE, 21, 22, 32);
	aLdp(&jE, 19, 20, 16);
	aPop(&jE, 29, 30, 80);
	aRet(&jE);
}

static void jFlush(uint8_t* start, uint8_t* end) {
	__builtin___clear_cache((char*) start, (char*) end);
}

#endif

// Steps taken but not yet added to the clock: added where it can be seen (a compare, a call, a jump or
// the place one comes to), so the steps of a stretch of code between them cost one add.
static u32 jStepsDue;

static void jSteps(u32 n) {
	jStepsDue += n;
}

static void jStepsOut(void) {
	if (jStepsDue) {
		jStepsAdd(jStepsDue);
		jStepsDue = 0;
	}
}

// Where flags from a compare are used: no steps can be added between.
static void jStepsKept(void) {
	if (jStepsDue) {
		jFailed = true;
	}
}

#pragma pop_macro("R")
#pragma pop_macro("PC")
#pragma pop_macro("A")
#pragma pop_macro("X")
#pragma pop_macro("Y")
#pragma pop_macro("Z")
#pragma pop_macro("S")
#pragma pop_macro("D")
#pragma pop_macro("B")
#pragma pop_macro("U")
#pragma pop_macro("V")
#pragma pop_macro("W")
#pragma pop_macro("CF")
#pragma pop_macro("ZF")
#pragma pop_macro("IF")
#pragma pop_macro("DF")
#pragma pop_macro("XF")
#pragma pop_macro("MF")
#pragma pop_macro("VF")
#pragma pop_macro("NF")
#pragma pop_macro("EF")

// --- Front end ---

#define RO(field) SO(r.field)

// The bus and PC as the last instruction's fetches left them, while they're kept out of memory: they're
// stored only where they can be seen (a call out, an exit, a jump to another block).
static struct JitBus {
	bool kept;
	u32 ad;
	u8 dr;
	u16 pc;
} jBus;

// Set where an instruction calls out (jGoOnIfCalled after it).
static bool jMayCall;

// The C functions a block calls, each through shared code that brings the clock out and back in.
static const void* const jSharedFns[3] = { jitGoOn, sa1FastLastCycle, sa1BlockInstruction };
static const uint8_t* jShared[3];

static void jCallShared(const void* fn) {
	for (int n = 0; n < 3; ++n) {
		if (jSharedFns[n] == fn) {
			jCallStub(jShared[n]);
			return;
		}
	}
	jCall(fn);
}

static void jBusStore(const struct JitBus* bus) {
	if (bus->kept) {
		jStI2(4, RO(ad), RO(mar), bus->ad);
		jStI2(1, RO(dr), RO(mdr), bus->dr);
		jStI(2, RO(pc), bus->pc);
	}
}

// The bus and PC stored; still kept with keep, on a path that branches off.
static void jBusOut(bool keep) {
	jBusStore(&jBus);
	jBus.kept = jBus.kept && keep;
}

// The start of the instruction run last, as loop.last would be: the block's first until the next starts
// (sa1Run sets it so before the block, and a chained exit enters one at its first).
static u32 jLastPc;

static void jLast(u32 pc) {
	jLastPc = pc;
}

// The block's exits, patched to its end, each with the bus and loop.last as they were there.
static uint8_t* jExits[BLOCK_OPS * 8];
static struct JitBus jExitBus[BLOCK_OPS * 8];
static u32 jExitLast[BLOCK_OPS * 8];
static u32 jExitCount;

static void jExitWith(uint8_t* site, const struct JitBus* bus) {
	if (jExitCount < sizeof(jExits) / sizeof(*jExits)) {
		jExitBus[jExitCount] = *bus;
		jExitLast[jExitCount] = jLastPc;
		jExits[jExitCount++] = site;
	} else {
		jFailed = true;
	}
}

static void jExit(uint8_t* site) {
	jExitWith(site, &jBus);
}

// An exit where the bus is already stored.
static void jExitStored(uint8_t* site) {
	static const struct JitBus none = { false, 0, 0, 0 };
	jExitWith(site, &none);
}


// The key of the block at pc with M and X as given and the rest as now (blockKey).
static u32 jKey(u32 pc, bool m, bool x) {
	return pc | m << 24 | x << 25 | romConflict() << 26 | iramConflict() << 27 | EF << 28 | bwramConflict() << 29;
}

// An exit to pc, further on than the instruction at from: straight to its block once it's known.
static void jChain(u32 pc, u32 from, bool m, bool x) {
	if (pc <= from || jitCellCount >= sizeof(jitCells) / sizeof(*jitCells)) {
		jExitStored(jJump());
		return;
	}
	struct JitCell* cell = &jitCells[jitCellCount++];
	cell->target = NULL;
	cell->key = jKey(pc, m, x);
	jExitStored(jLate());
	jExitStored(jChainJump(cell));
}

// After a call out that can change what the block runs on: on if blockGoOn says so.
static void jGoOn(void) {
	jBusOut(false);
	jMayCall = true;
	jCallShared(jitGoOn);
	jCmpI(0, 0);
	jExit(jBranch(JC_EQ));
}

// The same once something has called out, in the stretch's other part.
static void jGoOnIfCalled(void) {
	jBusOut(false);
	uint8_t* called = jIfCalled();
	jSwap();
	jPatch(called);
	jCalledSet(0);
	jGoOn();
	uint8_t* back = jJump();
	jSwap();
	jPatch(back);
}

// The cartridge RAM's size less one where it's a power of two (0: none, or not one).
static u32 jRamMask(void) {
	return cart.ramSize && !(cart.ramSize & (cart.ramSize - 1)) ? cart.ramSize - 1 : 0;
}

// Where the address in value 0 reads BW-RAM as plain RAM (the 6000-7fff window outside bitmap mode, or
// 40-5f), its offset in the RAM into value 2; the sites where it doesn't go on in *other (up to 3).
static void jBwram(bool bankZero, uint8_t** other) {
	jMov(2, 0);
	jAluI(J_AND, 2, bankZero ? 0xe000 : 0x40e000);
	jCmpI(2, 0x6000);
	uint8_t* notWindow = jBranch(JC_NE);
	jLd(1, 3, SO(io.sw46));
	jCmpI(3, 0);
	other[0] = jBranch(JC_NE);
	jLd(1, 2, SO(io.cbm));
	jAluI(J_AND, 2, 0x1f);
	jShl(2, 13);
	jMov(3, 0);
	jAluI(J_AND, 3, 0x1fff);
	jAlu(J_OR, 2, 3);
	uint8_t* window = jJump();
	jPatch(notWindow);
	if (bankZero) {
		other[1] = jJump();
		other[2] = NULL;
	} else {
		jMov(2, 0);
		jAluI(J_AND, 2, 0xe00000);
		jCmpI(2, 0x400000);
		other[1] = jBranch(JC_NE);
		jMov(2, 0);
		other[2] = NULL;
	}
	jPatch(window);
	jAluI(J_AND, 2, jRamMask());
}

// A byte write of value 1 to the address in value 0 (24 bits, or 16 in bank 0), as sa1FastWrite
// makes it: to I-RAM in place while nothing watches the SA-1's writes and no I-RAM is protected
// from it; the rest through sa1FastWrite (jitCalled set where it wrote more than memory).
static void jWriteBody(bool bankZero, u32 iramSteps, u32 bwSteps) {
	jSt(4, RO(ad), 0);
	jSt(1, RO(dw), 1);
	jMov(2, 0);
	jAluI(J_AND, 2, bankZero ? 0xf800 : 0x40f800);
	jCmpI(2, 0x0000);
	uint8_t* iram1 = jBranch(JC_EQ);
	jCmpI(2, 0x3000);
	uint8_t* iram2 = jBranch(JC_EQ);
	uint8_t* bw[3] = { NULL, NULL, NULL }, * bwGuarded = NULL, * bwWatched = NULL, * bwDone = NULL;
	if (jRamMask()) {
		// BW-RAM as plain RAM while it isn't write-protected: a write to it ends a loop.
		jBwram(bankZero, bw);
		jLd(1, 3, SO(io.swen));
		jCmpI(3, 0);
		uint8_t* open = jBranch(JC_NE);
		jLd(1, 3, SO(io.cwen));
		jCmpI(3, 0);
		bwGuarded = jBranch(JC_EQ);
		jPatch(open);
		jLdAbs8(3, &ahead.on);
		jCmpI(3, 0);
		bwWatched = jBranch(JC_NE);
		jStAbs8(&loop.valid, 0);
		jSteps(bwSteps);
		jSt(4, RO(mar), 0);
		jSt(1, RO(mdr), 1);
		jStRam(2, 1);
		bwDone = jJump();
	}
	uint8_t* other = jJump();
	for (int n = 0; n < 3; ++n) {
		if (bw[n]) {
			jPatch(bw[n]);
		}
	}
	if (bwGuarded) {
		jPatch(bwGuarded);
		jPatch(bwWatched);
	}
	uint8_t* other2 = jJump();
	jPatch(iram1);
	jPatch(iram2);
	jLdAbs8(2, &ahead.on);
	jLd(1, 3, SO(io.ciwp));
	jCmpI(3, 0xff);
	uint8_t* guarded = jBranch(JC_NE);
	jCmpI(2, 0);
	uint8_t* watched = jBranch(JC_NE);
	// Writing what's there leaves a loop as it was.
	jMov(2, 0);
	jAluI(J_AND, 2, 0x7ff);
	jLdIram(3, 2);
	jCmp(3, 1);
	uint8_t* same = jBranch(JC_EQ);
	jStAbs8(&loop.valid, 0);
	jPatch(same);
	jSteps(iramSteps);
	jLd(4, 0, RO(ad));
	jLd(1, 1, RO(dw));
	jSt(4, RO(mar), 0);
	jSt(1, RO(mdr), 1);
	jAluI(J_AND, 0, 0x7ff);
	jStIram(0, 1);
	uint8_t* done = jJump();
	jPatch(other);
	jPatch(other2);
	jPatch(guarded);
	jPatch(watched);
	jLd(4, 0, RO(ad));
	jLd(1, 1, RO(dw));
	jArg(0, 0);
	jArg(1, 1);
	jCall(sa1FastWrite);
	jCmpI(0, 0);
	uint8_t* plain = jBranch(JC_NE);
	jCalledSet(1);
	jPatch(plain);
	jPatch(done);
	if (bwDone) {
		jPatch(bwDone);
	}
}

// ZF and NF of the value in a, width bytes wide (a is left as it was).
static void jFlagsZN(int a, int width) {
	jCmpI(a, 0);
	jSetFlag(JC_EQ, RO(zf));
	jShrFrom(3, a, width * 8 - 1);
	jSt(1, RO(n), 3);
}

// A byte read into value 1 from the address in value 0 (24 bits, or 16 in bank 0): I-RAM in place as
// fastRead reads it, the rest through sa1FastRead (jitCalled set for a register's).
static void jReadBody(bool bankZero, u32 iramSteps, u32 bwSteps) {
	jSt(4, RO(ad), 0);
	jMov(1, 0);
	jAluI(J_AND, 1, bankZero ? 0xf800 : 0x40f800);
	jCmpI(1, 0x0000);
	uint8_t* iram1 = jBranch(JC_EQ);
	jCmpI(1, 0x3000);
	uint8_t* iram2 = jBranch(JC_EQ);
	uint8_t* ram = NULL;
	uint8_t* other[3] = { NULL, NULL, NULL };
	if (jRamMask()) {
		jBwram(bankZero, other);
		jSt(4, RO(mar), 0);
		jSteps(bwSteps);
		jLdRam(1, 2);
		jSt(1, RO(dr), 1);
		jSt(1, RO(mdr), 1);
		ram = jJump();
	}
	for (int n = 0; n < 3; ++n) {
		if (other[n]) {
			jPatch(other[n]);
		}
	}
	jArg(0, 0);
	jCall(sa1FastRead);
	jMov(1, 0);
	// Of reads, only a register's can change what the block runs on.
	jLd(4, 0, RO(ad));
	jAluI(J_AND, 0, 0x40fe00);
	jCmpI(0, 0x002200);
	uint8_t* plain = jBranch(JC_NE);
	jCalledSet(1);
	jPatch(plain);
	uint8_t* done = jJump();
	jPatch(iram1);
	jPatch(iram2);
	jSt(4, RO(mar), 0);
	jSteps(iramSteps);
	jLd(4, 0, RO(ad));
	jAluI(J_AND, 0, 0x7ff);
	jLdIram(1, 0);
	jSt(1, RO(dr), 1);
	jSt(1, RO(mdr), 1);
	jPatch(done);
	if (ram) {
		jPatch(ram);
	}
}

// The reads and writes as shared code, one for each kind of address and the conflicts' steps: a block
// calls them with the address in value 0 and the byte in value 1.
static const uint8_t* jReadStubs[2][2][2];
static const uint8_t* jWriteStubs[2][2][2];

static void jAccessStubsMake(void) {
	for (int n = 0; n < 3; ++n) {
		jShared[n] = jE.p;
		jStubEnter();
		jCall(jSharedFns[n]);
		jStubLeave();
	}
	for (int zero = 0; zero < 2; ++zero) {
		for (int iramC = 0; iramC < 2; ++iramC) {
			for (int bwC = 0; bwC < 2; ++bwC) {
				jReadStubs[zero][iramC][bwC] = jE.p;
				jStubEnter();
				jReadBody(zero, 1 + 2 * iramC, 2 + 2 * bwC);
				jStubLeave();
				jWriteStubs[zero][iramC][bwC] = jE.p;
				jStubEnter();
				jWriteBody(zero, 1 + 2 * iramC, 2 + 2 * bwC);
				jStubLeave();
			}
		}
	}
}

static void jRead(bool bankZero, u32 iramSteps) {
	(void) iramSteps;
	jBusOut(false);
	jMayCall = true;
	jCallStub(jReadStubs[bankZero][iramConflict()][bwramConflict()]);
}

static void jWrite(bool bankZero, u32 iramSteps) {
	(void) iramSteps;
	jBusOut(false);
	jMayCall = true;
	jCallStub(jWriteStubs[bankZero][iramConflict()][bwramConflict()]);
}

enum Form { F_IMM, F_DIRECT, F_DIRECT_X, F_DIRECT_Y, F_ABS, F_ABS_X, F_ABS_Y, F_LONG, F_LONG_X, F_IND_Y, F_INDL, F_INDL_Y };
enum Kind { K_LOAD, K_AND, K_ORA, K_EOR, K_CMP, K_BIT, K_ADC, K_SBC, K_STORE, K_INC, K_DEC, K_ASL, K_LSR, K_ROL, K_ROR, K_TSB, K_TRB };
enum Reg { G_A, G_X, G_Y, G_Z };

static const u32 regOff[4] = { RO(a), RO(x), RO(y), RO(z) };

static const struct JitOp {
	u8 op, form, kind, reg;
} jitOps[] = {
	{ 0xa9, F_IMM, K_LOAD, G_A }, { 0xa2, F_IMM, K_LOAD, G_X }, { 0xa0, F_IMM, K_LOAD, G_Y },
	{ 0xa5, F_DIRECT, K_LOAD, G_A }, { 0xa6, F_DIRECT, K_LOAD, G_X }, { 0xa4, F_DIRECT, K_LOAD, G_Y },
	{ 0xb5, F_DIRECT_X, K_LOAD, G_A }, { 0xb4, F_DIRECT_X, K_LOAD, G_Y }, { 0xb6, F_DIRECT_Y, K_LOAD, G_X },
	{ 0xad, F_ABS, K_LOAD, G_A }, { 0xae, F_ABS, K_LOAD, G_X }, { 0xac, F_ABS, K_LOAD, G_Y },
	{ 0xbd, F_ABS_X, K_LOAD, G_A }, { 0xb9, F_ABS_Y, K_LOAD, G_A }, { 0xbc, F_ABS_X, K_LOAD, G_Y },
	{ 0xbe, F_ABS_Y, K_LOAD, G_X }, { 0xaf, F_LONG, K_LOAD, G_A }, { 0xbf, F_LONG_X, K_LOAD, G_A },
	{ 0xe0, F_IMM, K_CMP, G_X }, { 0xe4, F_DIRECT, K_CMP, G_X }, { 0xec, F_ABS, K_CMP, G_X },
	{ 0xc0, F_IMM, K_CMP, G_Y }, { 0xc4, F_DIRECT, K_CMP, G_Y }, { 0xcc, F_ABS, K_CMP, G_Y },
	{ 0x89, F_IMM, K_BIT, G_A }, { 0x24, F_DIRECT, K_BIT, G_A }, { 0x2c, F_ABS, K_BIT, G_A },
	{ 0x34, F_DIRECT_X, K_BIT, G_A }, { 0x3c, F_ABS_X, K_BIT, G_A },
	{ 0x85, F_DIRECT, K_STORE, G_A }, { 0x95, F_DIRECT_X, K_STORE, G_A }, { 0x8d, F_ABS, K_STORE, G_A },
	{ 0x9d, F_ABS_X, K_STORE, G_A }, { 0x99, F_ABS_Y, K_STORE, G_A }, { 0x8f, F_LONG, K_STORE, G_A },
	{ 0x9f, F_LONG_X, K_STORE, G_A }, { 0x86, F_DIRECT, K_STORE, G_X }, { 0x96, F_DIRECT_Y, K_STORE, G_X },
	{ 0x8e, F_ABS, K_STORE, G_X }, { 0x84, F_DIRECT, K_STORE, G_Y }, { 0x94, F_DIRECT_X, K_STORE, G_Y },
	{ 0x8c, F_ABS, K_STORE, G_Y }, { 0x64, F_DIRECT, K_STORE, G_Z }, { 0x74, F_DIRECT_X, K_STORE, G_Z },
	{ 0x9c, F_ABS, K_STORE, G_Z }, { 0x9e, F_ABS_X, K_STORE, G_Z },
	{ 0xe6, F_DIRECT, K_INC, G_A }, { 0xf6, F_DIRECT_X, K_INC, G_A }, { 0xee, F_ABS, K_INC, G_A },
	{ 0xfe, F_ABS_X, K_INC, G_A }, { 0xc6, F_DIRECT, K_DEC, G_A }, { 0xd6, F_DIRECT_X, K_DEC, G_A },
	{ 0xce, F_ABS, K_DEC, G_A }, { 0xde, F_ABS_X, K_DEC, G_A }, { 0x06, F_DIRECT, K_ASL, G_A },
	{ 0x16, F_DIRECT_X, K_ASL, G_A }, { 0x0e, F_ABS, K_ASL, G_A }, { 0x1e, F_ABS_X, K_ASL, G_A },
	{ 0x46, F_DIRECT, K_LSR, G_A }, { 0x56, F_DIRECT_X, K_LSR, G_A }, { 0x4e, F_ABS, K_LSR, G_A },
	{ 0x5e, F_ABS_X, K_LSR, G_A }, { 0x26, F_DIRECT, K_ROL, G_A }, { 0x36, F_DIRECT_X, K_ROL, G_A },
	{ 0x2e, F_ABS, K_ROL, G_A }, { 0x3e, F_ABS_X, K_ROL, G_A }, { 0x66, F_DIRECT, K_ROR, G_A },
	{ 0x76, F_DIRECT_X, K_ROR, G_A }, { 0x6e, F_ABS, K_ROR, G_A }, { 0x7e, F_ABS_X, K_ROR, G_A },
	{ 0x04, F_DIRECT, K_TSB, G_A }, { 0x0c, F_ABS, K_TSB, G_A }, { 0x14, F_DIRECT, K_TRB, G_A },
	{ 0x1c, F_ABS, K_TRB, G_A }, { 0xb1, F_IND_Y, K_LOAD, G_A }, { 0xa7, F_INDL, K_LOAD, G_A },
	{ 0xb7, F_INDL_Y, K_LOAD, G_A }, { 0x91, F_IND_Y, K_STORE, G_A }, { 0x87, F_INDL, K_STORE, G_A },
	{ 0x97, F_INDL_Y, K_STORE, G_A },
};

static bool jitModifies(enum Kind kind) {
	return kind >= K_INC;
}

// The ALU instructions on A: each kind in its eight forms, the opcode's low bits after the kind's.
static bool jitAluOp(u8 op, struct JitOp* to) {
	static const u8 kinds[8] = { K_ORA, K_AND, K_EOR, K_ADC, 0xff, 0xff, K_CMP, K_SBC };
	static const struct { u8 low, form; } forms[11] = {
		{ 0x09, F_IMM }, { 0x05, F_DIRECT }, { 0x0d, F_ABS }, { 0x1d, F_ABS_X },
		{ 0x19, F_ABS_Y }, { 0x15, F_DIRECT_X }, { 0x0f, F_LONG }, { 0x1f, F_LONG_X },
		{ 0x11, F_IND_Y }, { 0x07, F_INDL }, { 0x17, F_INDL_Y },
	};
	u8 kind = kinds[op >> 5];
	if (kind == 0xff) {
		return false;
	}
	for (int n = 0; n < 11; ++n) {
		if ((op & 0x1f) == forms[n].low) {
			*to = (struct JitOp) { op, forms[n].form, kind, G_A };
			return true;
		}
	}
	return false;
}

static bool jitFind1(u8 op, struct JitOp* to) {
	for (u32 n = 0; n < sizeof(jitOps) / sizeof(*jitOps); ++n) {
		if (jitOps[n].op == op) {
			*to = jitOps[n];
			return true;
		}
	}
	return jitAluOp(op, to);
}

static int jitIndex(enum Form form) {
	return form == F_DIRECT_X || form == F_ABS_X || form == F_LONG_X ? G_X : form == F_DIRECT_Y || form == F_ABS_Y ? G_Y : -1;
}

// The address of byte k of the operand into value 0 (value 2 is used too).
static void jAddress(enum Form form, u32 operand, u32 k) {
	int index = jitIndex(form);
	if (form == F_DIRECT || form == F_DIRECT_X || form == F_DIRECT_Y) {
		jLd(2, 0, RO(d));
		jAluI(J_ADD, 0, operand + k);
	} else if (form == F_LONG || form == F_LONG_X) {
		jMovI(0, operand + k);
	} else {
		jLd(1, 0, RO(b));
		jShl16Add(0, 0, operand + k);
	}
	if (index >= 0) {
		jLd(2, 2, regOff[index]);
		jAlu(J_ADD, 0, 2);
	}
	jAluI(J_AND, 0, form == F_DIRECT || form == F_DIRECT_X || form == F_DIRECT_Y ? 0xffff : 0xffffff);
}

// lastCycle before an access, where a call out before it could have made it raise an interrupt.
static void jLastCycleIfCalled(void) {
	jBusOut(false);
	jMayCall = true;
	uint8_t* called = jIfCalled();
	jSwap();
	jPatch(called);
	jCallShared(sa1FastLastCycle);
	uint8_t* back = jJump();
	jSwap();
	jPatch(back);
}

// CF from the compare before.
static void jFlagC(int cond) {
	jSetFlag(cond, RO(c));
}

// The ALU part of an instruction on the value in value 1 (width bytes).
static void jAluOp(enum Kind kind, enum Reg reg, int width, bool immediate) {
	u32 mask = width == 1 ? 0xff : 0xffff, sign = width == 1 ? 0x80 : 0x8000;
	switch (kind) {
	case K_LOAD:
		jSt(width, regOff[reg], 1);
		jFlagsZN(1, width);
		return;
	case K_CMP:
		jLd(width, 2, regOff[reg]);
		jCmp(2, 1);
		jFlagC(JC_HS);
		jAlu(J_SUB, 2, 1);
		jAluI(J_AND, 2, mask);
		jFlagsZN(2, width);
		return;
	case K_BIT:
		jLd(width, 2, RO(a));
		jAlu(J_AND, 2, 1);
		jCmpI(2, 0);
		jSetFlag(JC_EQ, RO(zf));
		if (!immediate) {
			jMov(2, 1);
			jShr(2, width * 8 - 2);
			jAluI(J_AND, 2, 1);
			jSt(1, RO(vf), 2);
			jMov(2, 1);
			jShr(2, width * 8 - 1);
			jSt(1, RO(n), 2);
		}
		return;
	case K_ADC:
	case K_SBC:
		if (kind == K_SBC) {
			jAluI(J_XOR, 1, mask);
		}
		jLd(width, 2, RO(a));
		jLd(1, 3, RO(c));
		jAlu(J_ADD, 3, 2);
		jAlu(J_ADD, 3, 1);
		// V: the result's sign unlike both of the operands'.
		jMov(0, 2);
		jAlu(J_XOR, 0, 3);
		jAlu(J_XOR, 1, 3);
		jAlu(J_AND, 0, 1);
		jAluI(J_AND, 0, sign);
		jCmpI(0, 0);
		jSetFlag(JC_NE, RO(vf));
		jCmpI(3, mask + 1);
		jFlagC(JC_HS);
		jAluI(J_AND, 3, mask);
		jSt(width, RO(a), 3);
		jFlagsZN(3, width);
		return;
	default:
		jLd(width, 2, RO(a));
		jAlu(kind == K_AND ? J_AND : kind == K_ORA ? J_OR : J_XOR, 2, 1);
		jSt(width, RO(a), 2);
		jFlagsZN(2, width);
		return;
	}
}

// The new value of a read-modify-write on value 1, and its flags.
static void jModify(enum Kind kind, int width) {
	u32 mask = width == 1 ? 0xff : 0xffff, top = width * 8 - 1;
	switch (kind) {
	case K_TSB:
	case K_TRB:
		jLd(width, 2, RO(a));
		jMov(0, 1);
		jAlu(J_AND, 0, 2);
		jCmpI(0, 0);
		jSetFlag(JC_EQ, RO(zf));
		if (kind == K_TRB) {
			jAluI(J_XOR, 2, mask);
			jAlu(J_AND, 1, 2);
		} else {
			jAlu(J_OR, 1, 2);
		}
		return;
	case K_ASL:
	case K_ROL:
		jLd(1, 3, RO(c));
		jMov(2, 1);
		jShr(2, top);
		jShl(1, 1);
		if (kind == K_ROL) {
			jAlu(J_OR, 1, 3);
		}
		jSt(1, RO(c), 2);
		break;
	case K_LSR:
	case K_ROR:
		jLd(1, 3, RO(c));
		jMov(2, 1);
		jAluI(J_AND, 2, 1);
		jShr(1, 1);
		if (kind == K_ROR) {
			jShl(3, top);
			jAlu(J_OR, 1, 3);
		}
		jSt(1, RO(c), 2);
		break;
	default:
		jAluI(kind == K_INC ? J_ADD : J_SUB, 1, 1);
		break;
	}
	jAluI(J_AND, 1, mask);
	jFlagsZN(1, width);
}

// The address of byte k of the data of (d),Y, [d] or [d],Y into value 0 (value 2 is used too).
static void jIndirectAddress(enum Form form, u32 k) {
	jLd(2, 0, RO(v));
	if (form == F_IND_Y) {
		jLd(1, 2, RO(b));
	} else {
		jLd(1, 2, RO(v) + 2);
	}
	jShl(2, 16);
	jAlu(J_OR, 0, 2);
	if (form != F_INDL) {
		jLd(2, 2, RO(y));
		jAlu(J_ADD, 0, 2);
	}
	if (k) {
		jAluI(J_ADD, 0, k);
	}
	jAluI(J_AND, 0, 0xffffff);
}

// (d),Y, [d] and [d],Y: the pointer read from the direct page, then the data.
static void jitIndirect(const struct BlockOp* o, enum Form form, enum Kind kind, int width, u32 iramSteps) {
	u32 operand = o->bytes[1];
	jStI(1, RO(u), operand);
	jLd(1, 0, RO(d));
	jCmpI(0, 0);
	uint8_t* even = jBranch(JC_EQ);
	jSteps(1);
	jPatch(even);
	for (u32 k = 0; k < (form == F_IND_Y ? 2u : 3u); ++k) {
		jLd(2, 0, RO(d));
		jAluI(J_ADD, 0, operand + k);
		jAluI(J_AND, 0, 0xffff);
		jRead(true, iramSteps);
		jSt(1, RO(v) + k, 1);
	}
	if (form == F_IND_Y) {
		// idle4 for a read, idle for a write.
		if (kind == K_STORE || !XF) {
			jSteps(1);
		} else {
			jLd(1, 0, RO(v));
			jLd(1, 2, RO(y));
			jAlu(J_ADD, 0, 2);
			jCmpI(0, 0x100);
			uint8_t* same = jBranch(JC_LO);
			jSteps(1);
			jPatch(same);
		}
	}
	for (int k = 0; k < width; ++k) {
		if (k == width - 1) {
			jLastCycleIfCalled();
		}
		jIndirectAddress(form, k);
		if (kind == K_STORE) {
			jLd(1, 1, RO(a) + k);
			jWrite(false, iramSteps);
		} else {
			jRead(false, iramSteps);
			jSt(1, RO(w) + k, 1);
		}
	}
	if (kind != K_STORE) {
		jLd(width, 1, RO(w));
		jAluOp(kind, G_A, width, false);
	}
}

// The rest of a host-code instruction after its fetches.
static void jitBody(const struct BlockOp* o, const struct JitOp* j, u32 iramSteps) {
	enum Form form = (enum Form) j->form;
	enum Kind kind = (enum Kind) j->kind;
	enum Reg reg = (enum Reg) j->reg;
	int width = (reg == G_X || reg == G_Y) ? (XF ? 1 : 2) : (MF ? 1 : 2);
	int index = jitIndex(form);
	if (form >= F_IND_Y) {
		jitIndirect(o, form, kind, width, iramSteps);
		return;
	}
	if (form == F_IMM) {
		u32 value = o->bytes[1] | (width == 2 ? o->bytes[2] << 8 : 0);
		// BIT # leaves its operand in U, the rest in W.
		jStI(width, kind == K_BIT ? RO(u) : RO(w), value);
		jMovI(1, value);
		jAluOp(kind, reg, width, true);
		return;
	}
	u32 operand = o->bytes[1];
	if (form == F_DIRECT || form == F_DIRECT_X || form == F_DIRECT_Y) {
		jStI(1, RO(u), operand);
		// idle2: a step more while D's low byte isn't zero; an indexed one idles once more.
		jLd(1, 0, RO(d));
		jCmpI(0, 0);
		uint8_t* even = jBranch(JC_EQ);
		jSteps(1);
		jPatch(even);
		if (index >= 0) {
			jSteps(1);
		}
	} else if (form == F_LONG || form == F_LONG_X) {
		operand |= o->bytes[2] << 8 | o->bytes[3] << 16;
		jStI(2, RO(v), operand & 0xffff);
		jStI(1, RO(v) + 2, operand >> 16);
	} else {
		operand |= o->bytes[2] << 8;
		jStI(2, RO(v), operand);
		if (index >= 0 && (kind == K_STORE || jitModifies(kind))) {
			jSteps(1);
		} else if (index >= 0) {
			// idle4: a step more with 16-bit index registers, or across a page.
			if (!XF) {
				jSteps(1);
			} else {
				jLd(1, 0, regOff[index]);
				jAluI(J_ADD, 0, operand & 0xff);
				jCmpI(0, 0x100);
				uint8_t* same = jBranch(JC_LO);
				jSteps(1);
				jPatch(same);
			}
		}
	}
	bool bankZero = form == F_DIRECT || form == F_DIRECT_X || form == F_DIRECT_Y;
	if (kind == K_STORE) {
		for (int k = 0; k < width; ++k) {
			if (k) {
				jLastCycleIfCalled();
			}
			jAddress(form, operand, k);
			jLd(1, 1, regOff[reg] + k);
			jWrite(bankZero, iramSteps);
		}
		return;
	}
	for (int k = 0; k < width; ++k) {
		if (k && !jitModifies(kind)) {
			jLastCycleIfCalled();
		}
		jAddress(form, operand, k);
		jRead(bankZero, iramSteps);
		jSt(1, RO(w) + k, 1);
	}
	jLd(width, 1, RO(w));
	if (!jitModifies(kind)) {
		jAluOp(kind, reg, width, false);
		return;
	}
	// Read-modify-write: an idle step, the new value, and its bytes written high first.
	jSteps(1);
	jLd(width, 1, RO(w));
	jModify(kind, width);
	jSt(width, RO(w), 1);
	for (int k = width - 1; k >= 0; --k) {
		if (k == 0) {
			jLastCycleIfCalled();
		}
		jAddress(form, operand, k);
		jLd(1, 1, RO(w) + k);
		jWrite(bankZero, iramSteps);
	}
}

// A branch that ends a block: whether it's one made host code, and its condition (flag offset, and
// whether the flag set takes it; ~0 for BRA).
static bool jitBranch(u8 op, u32* flag, bool* when) {
	switch (op) {
	case 0x10: *flag = RO(n); *when = false; return true;
	case 0x30: *flag = RO(n); *when = true; return true;
	case 0x50: *flag = RO(vf); *when = false; return true;
	case 0x70: *flag = RO(vf); *when = true; return true;
	case 0x90: *flag = RO(c); *when = false; return true;
	case 0xb0: *flag = RO(c); *when = true; return true;
	case 0xd0: *flag = RO(zf); *when = false; return true;
	case 0xf0: *flag = RO(zf); *when = true; return true;
	case 0x80: *flag = ~0u; *when = true; return true;
	}
	return false;
}

static bool inRom(u32 address) {
	return (address & 0x408000) == 0x008000 || (address & 0xc00000) == 0xc00000;
}

static void jitBranchBody(const struct BlockOp* o, u32 flag, bool when, u32 romSteps) {
	uint8_t* skip = NULL;
	if (flag != ~0u) {
		jLd(1, 0, flag);
		jCmpI(0, 0);
		skip = jBranch(when ? JC_EQ : JC_NE);
	}
	u32 next = (u16) (o->pc + 2), target = (u16) (next + (s8) o->bytes[1]);
	jStI(1, RO(u), o->bytes[1]);
	jStI(2, RO(v), target);
	// The idle step, and the penalty of a jump to an odd address in ROM.
	jSteps(1 + ((target & 1) && inRom((o->pc & 0xff0000) | target) ? romSteps : 0));
	jBusOut(true);
	jStI(2, RO(pc), target);
	jChain((o->pc & 0xff0000) | target, o->pc, MF, XF);
	if (skip) {
		jPatch(skip);
	}
}

// INX, INY, DEX, DEY, INC A, DEC A, TAX, TAY, TXA, TYA, CLC, SEC, ASL, LSR, ROL and ROR A (only
// whether it's one, with ask).
static bool jitImplied(u8 op, bool ask) {
	enum Reg from = G_A, to = G_A;
	int delta = 0, shift = -1;
	switch (op) {
	case 0xe8: to = G_X; delta = 1; break;
	case 0xc8: to = G_Y; delta = 1; break;
	case 0xca: to = G_X; delta = -1; break;
	case 0x88: to = G_Y; delta = -1; break;
	case 0x1a: to = G_A; delta = 1; break;
	case 0x3a: to = G_A; delta = -1; break;
	case 0xaa: from = G_A; to = G_X; break;
	case 0xa8: from = G_A; to = G_Y; break;
	case 0x8a: from = G_X; to = G_A; break;
	case 0x98: from = G_Y; to = G_A; break;
	case 0x0a: case 0x4a: case 0x2a: case 0x6a: shift = op; break;
	case 0x18: case 0x38:
		if (ask) {
			return true;
		}
		jSteps(1);
		jStI(1, RO(c), op == 0x38);
		return true;
	default:
		return false;
	}
	if (ask) {
		return true;
	}
	int width = (to == G_X || to == G_Y) ? (XF ? 1 : 2) : (MF ? 1 : 2);
	u32 mask = width == 1 ? 0xff : 0xffff, top = width * 8 - 1;
	// idleIRQ: an idle step, as no interrupt is pending.
	jSteps(1);
	jLd(width, 1, regOff[shift >= 0 ? G_A : delta ? to : from]);
	if (shift >= 0) {
		// The bit shifted out goes to CF; ROL and ROR shift the old CF in.
		jLd(1, 3, RO(c));
		jMov(2, 1);
		if (shift == 0x0a || shift == 0x2a) {
			jShr(2, top);
			jShl(1, 1);
			if (shift == 0x2a) {
				jAlu(J_OR, 1, 3);
			}
		} else {
			jAluI(J_AND, 2, 1);
			jShr(1, 1);
			if (shift == 0x6a) {
				jShl(3, top);
				jAlu(J_OR, 1, 3);
			}
		}
		jSt(1, RO(c), 2);
		jAluI(J_AND, 1, mask);
	} else if (delta) {
		jAluI(delta > 0 ? J_ADD : J_SUB, 1, 1);
		jAluI(J_AND, 1, mask);
	}
	jSt(width, regOff[to], 1);
	jFlagsZN(1, width);
	return true;
}

// SEP and REP, which end a block: W gets the operand, and the flags it names are set or cleared.
static void jitFlags(const struct BlockOp* o, bool set) {
	static const u32 bits[8] = { RO(c), RO(zf), RO(i), RO(df), RO(xf), RO(m), RO(vf), RO(n) };
	u8 mask = o->bytes[1];
	jStI(1, RO(w), mask);
	jSteps(1);
	for (int n = 0; n < 8; ++n) {
		if (mask >> n & 1) {
			jStI(1, bits[n], set);
		}
	}
	bool x = mask & 0x10 ? set : XF;
	if (x) {
		jStI(1, RO(x) + 1, 0);
		jStI(1, RO(y) + 1, 0);
	}
}

// JSR a: the return address less one pushed, high byte first; a jump that ends a block.
static void jitCall(const struct BlockOp* o, u32 iramSteps, u32 romSteps) {
	u32 target = o->bytes[1] | o->bytes[2] << 8, back = (u16) (o->pc + 2);
	jStI(2, RO(w), target);
	jSteps(1);
	for (int k = 1; k >= 0; --k) {
		if (k == 0) {
			jLastCycleIfCalled();
		}
		jLd(2, 0, RO(s));
		jMovI(1, back >> (k * 8) & 0xff);
		jWrite(true, iramSteps);
		jLd(2, 0, RO(s));
		jAluI(J_SUB, 0, 1);
		jSt(2, RO(s), 0);
	}
	jStI(2, RO(pc), target);
	if (inRom((o->pc & 0xff0000) | target)) {
		jSteps(romSteps);
	}
}

// RTS: the address pulled, plus one; a jump that ends a block.
static void jitReturn(const struct BlockOp* o, u32 iramSteps, u32 romSteps) {
	jSteps(2);
	for (int k = 0; k < 2; ++k) {
		jLd(2, 0, RO(s));
		jAluI(J_ADD, 0, 1);
		jAluI(J_AND, 0, 0xffff);
		jSt(2, RO(s), 0);
		jRead(true, iramSteps);
		jSt(1, RO(w) + k, 1);
	}
	jLastCycleIfCalled();
	jSteps(1);
	jLd(2, 0, RO(w));
	jAluI(J_ADD, 0, 1);
	jSt(2, RO(pc), 0);
	u32 bank = o->pc >> 16;
	if (bank >= 0xc0) {
		jSteps(romSteps);
	} else if (!(bank & 0x40)) {
		jLd(2, 0, RO(pc));
		jAluI(J_AND, 0, 0x8000);
		jCmpI(0, 0);
		uint8_t* low = jBranch(JC_EQ);
		jSteps(romSteps);
		jPatch(low);
	}
}

// The steps of idleJump to the address in value 0 (value 2 is used too).
static void jIdleJump(u32 romSteps) {
	jMov(2, 0);
	jAluI(J_AND, 2, 0x408000);
	jCmpI(2, 0x008000);
	uint8_t* rom = jBranch(JC_EQ);
	jMov(2, 0);
	jAluI(J_AND, 2, 0xc00000);
	jCmpI(2, 0xc00000);
	uint8_t* other = jBranch(JC_NE);
	jPatch(rom);
	jSteps(romSteps);
	jPatch(other);
}

// A push of value 1's byte.
static void jPush(u32 iramSteps) {
	jLd(2, 0, RO(s));
	jWrite(true, iramSteps);
	jLd(2, 0, RO(s));
	jAluI(J_SUB, 0, 1);
	jSt(2, RO(s), 0);
}

// A pull into value 1.
static void jPull(u32 iramSteps) {
	jLd(2, 0, RO(s));
	jAluI(J_ADD, 0, 1);
	jAluI(J_AND, 0, 0xffff);
	jSt(2, RO(s), 0);
	jRead(true, iramSteps);
}

// PHA, PHX, PHY, PHB, PHK, PHD and PLA, PLX, PLY, PLB, PLD (only whether it's one, with ask).
static bool jitStack(const struct BlockOp* o, u32 iramSteps, bool ask) {
	u8 op = o->bytes[0];
	u32 reg;
	int width;
	bool push = true;
	switch (op) {
	case 0x48: reg = RO(a); width = MF ? 1 : 2; break;
	case 0xda: reg = RO(x); width = XF ? 1 : 2; break;
	case 0x5a: reg = RO(y); width = XF ? 1 : 2; break;
	case 0x8b: reg = RO(b); width = 1; break;
	case 0x4b: reg = ~0u; width = 1; break;
	case 0x0b: reg = RO(d); width = 2; break;
	case 0x68: reg = RO(a); width = MF ? 1 : 2; push = false; break;
	case 0xfa: reg = RO(x); width = XF ? 1 : 2; push = false; break;
	case 0x7a: reg = RO(y); width = XF ? 1 : 2; push = false; break;
	case 0xab: reg = RO(b); width = 1; push = false; break;
	case 0x2b: reg = RO(d); width = 2; push = false; break;
	default:
		return false;
	}
	if (ask) {
		return true;
	}
	if (push) {
		jSteps(1);
		for (int k = width - 1; k >= 0; --k) {
			if (k == 0) {
				jLastCycleIfCalled();
			}
			if (reg == ~0u) {
				jMovI(1, o->pc >> 16);
			} else {
				jLd(1, 1, reg + k);
			}
			jPush(iramSteps);
		}
		return true;
	}
	jSteps(2);
	for (int k = 0; k < width; ++k) {
		if (k == width - 1) {
			jLastCycleIfCalled();
		}
		jPull(iramSteps);
		jSt(1, reg + k, 1);
	}
	jLd(width, 1, reg);
	jFlagsZN(1, width);
	return true;
}

// TXY, TYX, TCD, TDC, TCS, TSC, TSX, TXS and NOP (only whether it's one, with ask).
static bool jitTransfer(u8 op, bool ask) {
	u32 from, to;
	int width = 2;
	bool flags = true;
	switch (op) {
	case 0x9b: from = RO(x); to = RO(y); width = XF ? 1 : 2; break;
	case 0xbb: from = RO(y); to = RO(x); width = XF ? 1 : 2; break;
	case 0x5b: from = RO(a); to = RO(d); break;
	case 0x7b: from = RO(d); to = RO(a); break;
	case 0x1b: from = RO(a); to = RO(s); flags = false; break;
	case 0x3b: from = RO(s); to = RO(a); break;
	case 0xba: from = RO(s); to = RO(x); width = XF ? 1 : 2; break;
	case 0x9a: from = RO(x); to = RO(s); flags = false; break;
	case 0xea: from = to = ~0u; break;
	default:
		return false;
	}
	if (ask) {
		return true;
	}
	jSteps(1);
	if (from == ~0u) {
		return true;
	}
	jLd(width, 1, from);
	jSt(width, to, 1);
	if (flags) {
		jFlagsZN(1, width);
	}
	return true;
}

// PEA: the operand pushed, high byte first.
static void jitPushAddress(const struct BlockOp* o, u32 iramSteps) {
	jStI(2, RO(w), o->bytes[1] | o->bytes[2] << 8);
	jMovI(1, o->bytes[2]);
	jPush(iramSteps);
	jLastCycleIfCalled();
	jMovI(1, o->bytes[1]);
	jPush(iramSteps);
}

// XBA.
static void jitExchange(void) {
	jSteps(2);
	jLd(2, 1, RO(a));
	jMov(2, 1);
	jShr(2, 8);
	jShl(1, 8);
	jAlu(J_OR, 1, 2);
	jAluI(J_AND, 1, 0xffff);
	jSt(2, RO(a), 1);
	jAluI(J_AND, 1, 0xff);
	jFlagsZN(1, 1);
}

// JMP a, JML al and BRL: the jump, on to the block there.
static void jitJump(const struct BlockOp* o, u32 romSteps) {
	u8 op = o->bytes[0];
	u32 bank = o->pc & 0xff0000, target;
	if (op == 0x82) {
		u32 offset = o->bytes[1] | o->bytes[2] << 8;
		target = bank | (u16) (o->pc + 3 + (s16) offset);
		jStI(2, RO(u), offset);
		jStI(2, RO(v), target & 0xffff);
		// The idle step, and idleBranch's penalty at an odd address in ROM.
		jSteps(1 + ((target & 1) && inRom(target) ? romSteps : 0));
	} else if (op == 0x4c) {
		target = bank | o->bytes[1] | o->bytes[2] << 8;
		jStI(2, RO(w), target & 0xffff);
		jSteps(inRom(target) ? romSteps : 0);
	} else {
		target = o->bytes[1] | o->bytes[2] << 8 | o->bytes[3] << 16;
		jStI(2, RO(v), target & 0xffff);
		jStI(1, RO(v) + 2, target >> 16);
		jStI(1, RO(pc) + 2, target >> 16);
		jSteps(inRom(target) ? romSteps : 0);
	}
	jBusOut(false);
	jStI(2, RO(pc), target & 0xffff);
	jChain(target, o->pc, MF, XF);
}

// JMP (a,X): the address read from the program bank.
static void jitJumpIndexed(const struct BlockOp* o, u32 romSteps) {
	u32 operand = o->bytes[1] | o->bytes[2] << 8, bank = o->pc & 0xff0000;
	jStI(2, RO(v), operand);
	jSteps(1);
	for (u32 k = 0; k < 2; ++k) {
		if (k == 1) {
			jLastCycleIfCalled();
		}
		jLd(2, 0, RO(x));
		jAluI(J_ADD, 0, operand + k);
		jAluI(J_AND, 0, 0xffff);
		if (bank) {
			jAluI(J_OR, 0, bank);
		}
		jRead(!bank, 0);
		jSt(1, RO(w) + k, 1);
	}
	jLd(2, 0, RO(w));
	jSt(2, RO(pc), 0);
	if (bank) {
		jAluI(J_OR, 0, bank);
	}
	jIdleJump(romSteps);
}

// JSL al: the bank pushed before the last fetch, its step taken there; the return address less one.
static void jitCallLong(const struct BlockOp* o, u32 iramSteps, u32 romSteps) {
	u32 target = o->bytes[1] | o->bytes[2] << 8 | o->bytes[3] << 16, back = (u16) (o->pc + 3);
	jStI(2, RO(v), target & 0xffff);
	jMovI(1, o->pc >> 16);
	jPush(iramSteps);
	jSteps(1 + romSteps);
	jStI(1, RO(v) + 2, target >> 16);
	jMovI(1, back >> 8);
	jPush(iramSteps);
	jLastCycleIfCalled();
	jMovI(1, back & 0xff);
	jPush(iramSteps);
	jStI(2, RO(pc), target & 0xffff);
	jStI(1, RO(pc) + 2, target >> 16);
	jSteps(inRom(target) ? romSteps : 0);
	jChain(target, o->pc, MF, XF);
}

// RTL: the address pulled, plus one within its bank.
static void jitReturnLong(u32 iramSteps, u32 romSteps) {
	jSteps(2);
	for (int k = 0; k < 3; ++k) {
		if (k == 2) {
			jLastCycleIfCalled();
		}
		jPull(iramSteps);
		jSt(1, RO(v) + k, 1);
	}
	jLd(2, 0, RO(v));
	jAluI(J_ADD, 0, 1);
	jSt(2, RO(pc), 0);
	jLd(1, 0, RO(v) + 2);
	jSt(1, RO(pc) + 2, 0);
	jShl(0, 16);
	jLd(2, 2, RO(pc));
	jAlu(J_OR, 0, 2);
	jIdleJump(romSteps);
}

// Whether the instruction is one made host code.
static bool jitHost(u8 op) {
	struct JitOp j;
	u32 flag;
	bool when;
	return jitFind1(op, &j) || jitImplied(op, true) || jitBranch(op, &flag, &when) || op == 0xc2 || op == 0xe2 ||
	       op == 0x20 || op == 0x60 || op == 0xeb || op == 0x4c || op == 0x5c || op == 0x82 || op == 0x22 ||
	       op == 0x6b || op == 0xf4 || op == 0x7c || jitTransfer(op, true) || jitStack(&(struct BlockOp) { .bytes = { op } }, 0, true);
}

static void jStretch(uint8_t* at) {
	jE = (struct Emitter) { at, at, at + JIT_HOT_PART };
	jAway = (struct Emitter) { at + JIT_HOT_PART, at + JIT_HOT_PART, at + JIT_STRETCH };
}

// The plain 65816 for the block's instruction n from its kept bytes; past it, the PC where the next
// instruction is.
static void jPlain(const struct Block* b, const uint8_t* kept, u32 n) {
	jBusOut(false);
	jArgP(0, kept + n * 4);
	jCallShared(sa1BlockInstruction);
	jGoOn();
	if (n + 1 < b->count) {
		jLd(4, 0, RO(pc));
		jAluI(J_AND, 0, 0xffffff);
		jCmpI(0, b->ops[n + 1].pc);
		jExit(jBranch(JC_NE));
	}
}

// The block's code, or NULL if it couldn't be made.
static void* jitMake(const struct Block* b) {
	if (!jCode) {
		if (jFailed) {
			return NULL;
		}
		jCode = mmap(NULL, JIT_CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (jCode == MAP_FAILED) {
			jCode = NULL;
			jFailed = true;
			return NULL;
		}
		jE = (struct Emitter) { jCode, jCode, jCode + JIT_CODE_SIZE };
		jSyncOutMake();
		jSyncInMake();
		jAccessStubsMake();
		jFlush(jCode, jE.p);
		jBlocksStart = jCode + ((jE.p - jCode + 4095) & ~4095);
		jStretch(jBlocksStart);
	}
	if (jE.end - jE.p < JIT_BLOCK_MOST || jAway.end - jAway.p < JIT_BLOCK_MOST) {
		uint8_t* next = jE.start + JIT_STRETCH;
		if (next + JIT_STRETCH > jCode + JIT_CODE_SIZE) {
			// Full: every block is made again.
			next = jBlocksStart;
			blockGeneration++;
			jitCellCount = 0;
			jitCellsGeneration = blockGeneration;
			jitPending = NULL;
		}
		jStretch(next);
	}
	// The instructions' bytes, kept for sa1BlockInstruction.
	uint8_t* awayStart = jAway.p;
	uint8_t* kept = jAway.p;
	for (u32 n = 0; n < b->count; ++n) {
		memcpy(jAway.p, b->ops[n].bytes, 4);
		jAway.p += 4;
	}
	jAway.p = kept + ((b->count * 4 + 15) & ~15u);
	jE.p = jE.start + ((jE.p - jE.start + 15) & ~15);
	uint8_t* start = jE.p;
	bool failed = jFailed;
	jFailed = false;
	jExitCount = 0;
	u32 romSteps = 1 + romConflict(), iramSteps = 1 + 2 * iramConflict();
	jBus.kept = false;
	jLast(b->ops[0].pc);
	jPrologue();
	jBody = jE.p;
	for (u32 n = 0; n < b->count; ++n) {
		const struct BlockOp* o = &b->ops[n];
		u8 op = o->bytes[0];
		u32 length = opLength[op] + (longerWithM(op) && !MF) + (longerWithX(op) && !XF);
		if (n) {
			jExit(jLate());
			jLast(o->pc);
		}
		// JSL takes its last fetch after a push.
		jSteps(op == 0x22 && !EF ? o->steps - romSteps : o->steps);
		struct JitOp j;
		u32 flag;
		bool when;
		bool host = !EF && jitHost(op);
		bool decimal = host && jitFind1(op, &j) && (j.kind == K_ADC || j.kind == K_SBC);
		if (!host) {
			jPlain(b, kept, n);
			continue;
		}
		uint8_t* skip = NULL;
		if (decimal) {
			// Decimal mode: the plain 65816, in the other part.
			jBusOut(false);
			jLd(1, 0, RO(df));
			jCmpI(0, 0);
			uint8_t* bcd = jBranch(JC_NE);
			jSwap();
			jPatch(bcd);
			jPlain(b, kept, n);
			skip = jJump();
			jSwap();
		}
		// The bus and PC as the fetches leave them.
		jBus = (struct JitBus) { true, (o->pc & 0xff0000) | (u16) (o->pc + length - 1), o->bytes[length - 1],
			                 (u16) (o->pc + length) };
		jMayCall = false;
		if (jitFind1(op, &j)) {
			jitBody(o, &j, iramSteps);
		} else if (op == 0xc2 || op == 0xe2) {
			jitFlags(o, op == 0xe2);
		} else if (op == 0x20) {
			jitCall(o, iramSteps, romSteps);
		} else if (op == 0x60) {
			jitReturn(o, iramSteps, romSteps);
		} else if (op == 0x6b) {
			jitReturnLong(iramSteps, romSteps);
		} else if (op == 0x22) {
			jitCallLong(o, iramSteps, romSteps);
		} else if (op == 0x4c || op == 0x5c || op == 0x82) {
			jitJump(o, romSteps);
		} else if (op == 0xeb) {
			jitExchange();
		} else if (op == 0x7c) {
			jitJumpIndexed(o, romSteps);
		} else if (op == 0xf4) {
			jitPushAddress(o, iramSteps);
		} else if (jitTransfer(op, true)) {
			jitTransfer(op, false);
		} else if (jitStack(o, iramSteps, true)) {
			jitStack(o, iramSteps, false);
		} else if (jitBranch(op, &flag, &when)) {
			jitBranchBody(o, flag, when, romSteps);
		} else {
			jitImplied(op, false);
		}
		if (jMayCall) {
			jGoOnIfCalled();
		}
		if (skip) {
			jBusOut(false);
			jPatch(skip);
		}
		if (n + 1 == b->count) {
			// Where the block ends but the program goes on to an address known here.
			u32 next = (o->pc & 0xff0000) | (u16) (o->pc + length);
			jBusOut(true);
			if (op == 0xc2 || op == 0xe2) {
				bool set = op == 0xe2;
				jChain(next, o->pc, o->bytes[1] & 0x20 ? set : MF, o->bytes[1] & 0x10 ? set : XF);
			} else if (op == 0x20) {
				jChain((o->pc & 0xff0000) | o->bytes[1] | o->bytes[2] << 8, o->pc, MF, XF);
			} else if (!endsBlock(op)) {
				jChain(next, o->pc, MF, XF);
			}
		}
	}
	// The block's end, then its exits, each setting loop.last on its way to the epilogue.
	jBusOut(false);
	jLastOut(jLastPc);
	uint8_t* end = jE.p;
	jEpilogue();
	for (u32 n = 0; n < jExitCount;) {
		if (jExitBus[n].kept) {
			++n;
			continue;
		}
		u32 last = jExitLast[n];
		for (; n < jExitCount && (jExitBus[n].kept || jExitLast[n] == last); ++n) {
			if (!jExitBus[n].kept) {
				jPatch(jExits[n]);
			}
		}
		jLastOut(last);
		jJumpTo(end);
	}
	// The exits that store the bus on their way.
	for (u32 n = 0; n < jExitCount; ++n) {
		if (jExitBus[n].kept) {
			jPatch(jExits[n]);
			jBusStore(&jExitBus[n]);
			jLastOut(jExitLast[n]);
			jJumpTo(end);
		}
	}
	if (jE.p > jE.end || jAway.p > jAway.end || jFailed) {
		jE.p = start;
		jAway.p = awayStart;
		jFailed = failed;
		return NULL;
	}
	jFailed = failed;
	jFlush(start, jE.p);
	jFlush(awayStart, jAway.p);
	return start;
}

// Blocks by key (blockKey), four ways to a set; one of an older generation is gone. A block is made host
// code once it has run JIT_HOT times; till then blockRun runs it the plain way.
enum { JIT_SETS = 1 << 13, JIT_WAYS = 4, JIT_HOT = 8, JIT_NEVER = ~0u };

static struct JitSlot {
	u32 key;
	u32 generation;
	u32 runs;  // JIT_NEVER: no host code can be made
	void* code;
	void* body;
} jitSlots[JIT_SETS][JIT_WAYS];
static u8 jitVictim[JIT_SETS];
static struct JitSlot jitLast = { ~0u, 0, 0, NULL, NULL };
static struct Block jitScratch;

// The host code of the block at PC once it's hot: NULL where there is none yet.
static void* jitFind(void) {
	if (jitCellsGeneration != blockGeneration) {
		// Blocks of an older generation are gone, and the exits to them.
		jitCellCount = 0;
		jitCellsGeneration = blockGeneration;
		jitPending = NULL;
	}
	u32 key = blockKey();
	struct JitCell* pending = jitPending;
	jitPending = NULL;
	// The block found last time, again.
	if (key == jitLast.key && jitLast.generation == blockGeneration) {
		if (pending && pending->key == key) {
			pending->target = jitLast.body;
		}
		return jitLast.code;
	}
	u32 index = (key ^ key >> 13 ^ key >> 24) & (JIT_SETS - 1);
	struct JitSlot* set = jitSlots[index];
	struct JitSlot* slot = NULL;
	for (u32 way = 0; way < JIT_WAYS; ++way) {
		if (set[way].key == key && set[way].generation == blockGeneration) {
			slot = &set[way];
			break;
		}
	}
	if (!slot) {
		slot = &set[jitVictim[index]++ % JIT_WAYS];
		*slot = (struct JitSlot) { key, blockGeneration, 0, NULL, NULL };
	}
	if (slot->code) {
		jitLast = *slot;
		if (pending && pending->key == key) {
			pending->target = slot->body;
		}
		return slot->code;
	}
	if (slot->runs == JIT_NEVER || ++slot->runs < JIT_HOT) {
		return NULL;
	}
	u32 generation = blockGeneration;
	const struct Block* b = blockMake(&jitScratch, key, true);
	void* code = b ? jitMake(b) : NULL;
	// Made again after a flush, the slot is of the new generation.
	slot->generation = blockGeneration;
	slot->code = code;
	slot->body = code ? jBody : NULL;
	slot->runs = code ? 0 : JIT_NEVER;
	if (!code) {
		return NULL;
	}
	jitLast = *slot;
	if (pending && pending->key == key && generation == blockGeneration) {
		pending->target = jBody;
	}
	return code;
}

#endif

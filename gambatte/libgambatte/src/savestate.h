//
//   Copyright (C) 2008 by sinamas <sinamas at users.sourceforge.net>
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License version 2 for more details.
//
//   You should have received a copy of the GNU General Public License
//   version 2 along with this program; if not, write to the
//   Free Software Foundation, Inc.,
//   51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
//

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include "gbint.h"
#include <cstddef>

namespace gambatte {

class SaverList;

struct SaveState {
	template<typename T>
	class Ptr {
	public:
		Ptr() : ptr(0), size_(0) {}
		T const * get() const { return ptr; }
		std::size_t size() const { return size_; }
		void set(T *p, std::size_t size) { ptr = p; size_ = size; }

		friend class SaverList;
		friend void setInitState(SaveState &, bool, bool, bool, std::size_t, const char *);
		friend void setInitStateCart(SaveState &, bool, bool);
		friend void setPostBiosState(SaveState &, bool, bool, bool, bool, char const *);
		friend void setSachenLogo(SaveState &, unsigned char const *);
		friend void setMegaDuckVram(SaveState &);
	private:
		T *ptr;
		std::size_t size_;
	};

	struct CPU {
		gb_ulong cycleCounter;
		unsigned short pc;
		unsigned short sp;
		unsigned char a;
		unsigned char b;
		unsigned char c;
		unsigned char d;
		unsigned char e;
		unsigned char f;
		unsigned char h;
		unsigned char l;
		unsigned char opcode;
		unsigned char /*bool*/ prefetched;
		unsigned char /*bool*/ skip;
	} cpu;

	struct Mem {
		Ptr<unsigned char> vram;
		Ptr<unsigned char> sram;
		// armGBA: a cheat cartridge's registers and RAM (CheatCartGb::stateData), when one is in
		Ptr<unsigned char> cartChip;
		// armGBA: the Action Replay's flash chip, so netplay's devices run the same codes and menus
		Ptr<unsigned char> cartFlash;
		Ptr<unsigned char> wram;
		Ptr<unsigned char> ioamhram;
		gb_ulong divLastUpdate;
		gb_ulong timaLastUpdate;
		gb_ulong tmatime;
		gb_ulong nextSerialtime;
		gb_ulong lastOamDmaUpdate;
		gb_ulong minIntTime;
		gb_ulong unhaltTime;
		gb_ulong lastCartBusUpdate;
		gb_ulong totalSamplesEmittedHigh;
		gb_ulong totalSamplesEmittedLow;
		// armGBA: when the next frame is handed over, which with the display off is nothing the rest of
		// the state tells; 0 in a state from before this was kept
		gb_ulong nextBlitTime;
		unsigned char /*bool*/ blankLcd;
		// armGBA: what an unlicensed mapper keeps beyond a bank number
		gb_ulong mbcExtra;
		gb_ulong mbcExtra2;
		gb_ulong mbcExtra3;
		gb_ulong mbcExtra4;
		// armGBA: the CPU Clock's part of a cycle carried over, and how long the CPU keeps the console's
		// pace (Memory::cpuTimingPicture); 0 in a state from before these were kept
		gb_ulong cpuCarry;
		gb_ulong cpuStockLeft;
		unsigned short rombank;
		unsigned short dmaSource;
		unsigned short dmaDestination;
		unsigned char rambank;
		unsigned char oamDmaPos;
		unsigned char haltHdmaState;
		unsigned char ramflag;
		unsigned char /*bool*/ IME;
		unsigned char /*bool*/ halted;
		unsigned char /*bool*/ enableRam;
		unsigned char /*bool*/ rambankMode;
		unsigned char /*bool*/ hdmaTransfer;
		unsigned char /*bool*/ biosMode;
		unsigned char /*bool*/ stopped;

		struct SGB {
			Ptr<unsigned short> systemTilemap;
			Ptr<unsigned short> tilemap;
			Ptr<unsigned short> systemTileColors;
			Ptr<unsigned short> tileColors;
			Ptr<unsigned short> systemColors;
			Ptr<unsigned short> colors;
			Ptr<unsigned char> systemAttributes;
			Ptr<unsigned char> attributes;
			Ptr<unsigned char> systemTiles;
			Ptr<unsigned char> tiles;
			Ptr<unsigned char> packet;
			Ptr<unsigned char> command;
			Ptr<unsigned char> frameBuf;
			Ptr<unsigned char> logoBuf;
			Ptr<unsigned char> hookBuf;
			Ptr<unsigned char> spcState;
			Ptr<unsigned char> soundControl;
			gb_ulong samplesAccumulated;
			unsigned char transfer;
			unsigned char commandIndex;
			unsigned char joypadIndex;
			unsigned char joypadMask;
			unsigned char borderFade;
			unsigned char pending;
			unsigned char pendingCount;
			unsigned char mask;
			unsigned char /*bool*/ disableCommands;
			unsigned char /*bool*/ headerSent;
		} sgb;
	} mem;

	struct PPU {
		Ptr<unsigned short> dmgColorsBgr15;
		Ptr<unsigned char> bgpData;
		Ptr<unsigned char> objpData;
		//SpriteMapper::OamReader
		Ptr<unsigned char> oamReaderBuf;
		Ptr<bool> oamReaderSzbuf;

		gb_ulong videoCycles;
		gb_ulong enableDisplayM0Time;
		unsigned short lastM0Time;
		unsigned short nextM0Irq;
		// armGBA: 2 when a mode 0 interrupt event is scheduled, 1 when none is; 0 in a state from before
		// this was kept, which counts on the STAT register instead
		unsigned char m0IrqScheduled;
		unsigned short tileword;
		unsigned short ntileword;
		unsigned char spAttribList[10];
		unsigned char spByte0List[10];
		unsigned char spByte1List[10];
		unsigned char winYPos;
		unsigned char xpos;
		unsigned char endx;
		unsigned char reg0;
		unsigned char reg1;
		unsigned char attrib;
		unsigned char nattrib;
		unsigned char state;
		unsigned char nextSprite;
		unsigned char currentSprite;
		unsigned char lyc;
		unsigned char m0lyc;
		unsigned char oldWy;
		unsigned char winDrawState;
		unsigned char wscx;
		unsigned char /*bool*/ weMaster;
		unsigned char /*bool*/ pendingLcdstatIrq;
		unsigned char /*bool*/ notCgbDmg;
		unsigned char /*bool*/ spPriority; // bit 1 is abused for state compat
	} ppu;

	struct SPU {
		struct Duty {
			gb_ulong nextPosUpdate;
			unsigned char nr3;
			// armGBA: the frequency's top three bits, which the sweep changes without NRx4 knowing,
			// with bit 3 set; 0 in a state from before this was kept
			unsigned char freqHigh;
			// armGBA: 2 when the unit's events run, 1 when its output is static; 0 in a state from before
			// this was kept
			unsigned char events;
			unsigned char pos;
			unsigned char /*bool*/ high;
		};

		struct Env {
			gb_ulong counter;
			unsigned char volume;
			unsigned char /*bool*/ clock;
		};

		struct LCounter {
			gb_ulong counter;
			unsigned short lengthCounter;
		};

		struct {
			struct {
				gb_ulong counter;
				unsigned short shadow;
				unsigned char nr0;
				unsigned char /*bool*/ neg;
			} sweep;
			Duty duty;
			Env env;
			LCounter lcounter;
			unsigned char nr4;
			unsigned char /*bool*/ master;
		} ch1;

		struct {
			Duty duty;
			Env env;
			LCounter lcounter;
			unsigned char nr4;
			unsigned char /*bool*/ master;
		} ch2;

		struct {
			Ptr<unsigned char> waveRam;
			LCounter lcounter;
			gb_ulong waveCounter;
			gb_ulong lastReadTime;
			unsigned char nr3;
			unsigned char nr4;
			unsigned char wavePos;
			unsigned char sampleBuf;
			unsigned char /*bool*/ master;
		} ch3;

		struct {
			struct {
				gb_ulong counter;
				unsigned short reg;
				// armGBA: 2 when the LFSR counts, 1 when its output is static; 0 in a state from before
				// this was kept
				unsigned char counting;
			} lfsr;
			Env env;
			LCounter lcounter;
			unsigned char nr4;
			unsigned char /*bool*/ master;
		} ch4;

		gb_ulong cycleCounter;
		unsigned char lastUpdate;
		// armGBA: the mixer's running sum (xor its power-on value) and each channel's last output, which
		// the next samples are worked out from; a state from before these were kept loads with the
		// mixer as at power-on
		gb_ulong rsum;
		gb_ulong prevOut1;
		gb_ulong prevOut2;
		gb_ulong prevOut3;
		gb_ulong prevOut4;
	} spu;

	struct Time {
		gb_ulong seconds;
	} time;

	struct RTC {
		unsigned char dataDh;
		unsigned char dataDl;
		unsigned char /*signed*/ dataH;
		unsigned char /*signed*/ dataM;
		unsigned char /*signed*/ dataS;
		gb_ulong dataC;
		unsigned char latchDh;
		unsigned char latchDl;
		unsigned char latchH;
		unsigned char latchM;
		unsigned char latchS;
	} rtc;

	struct Infrared {
		unsigned char /*bool*/ irTrigger;
		unsigned char /*bool*/ thisGbIrSignal;
		unsigned char /*bool*/ linkedGbIrSignal;

		struct {
			unsigned char /*bool*/ isActive;
			gb_ulong lastUpdate;
			gb_ulong cyclesElapsed;
			unsigned char command;
		} remote;
	} ir;

	struct HuC3 {
		Ptr<unsigned char> io;

		unsigned char ioIndex;
		unsigned char transferValue;
		unsigned char ramflag;
		gb_ulong rtcCycles;
		unsigned short /*signed*/ currentSample;
		gb_ulong toneLastUpdate;
		gb_ulong nextPhaseChangeTime;
		gb_ulong /*signed*/ remainingToneSamples;
		unsigned char /*bool*/ committing;
		unsigned char /*bool*/ highIoReadOnly;
	} huc3;

	struct Camera {
		Ptr<unsigned char> matrix;

		unsigned char trigger;
		unsigned char /*bool*/ n;
		unsigned char vh;
		unsigned short exposure;
		unsigned char edgeAlpha;
		unsigned char /*bool*/ blank;
		unsigned char /*bool*/ invert;
		gb_ulong lastCycles;
		gb_ulong /*signed*/ cameraCyclesLeft;
	} camera;
};

}

#endif

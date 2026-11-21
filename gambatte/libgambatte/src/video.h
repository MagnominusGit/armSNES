//
//   Copyright (C) 2007 by sinamas <sinamas at users.sourceforge.net>
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

#ifndef VIDEO_H
#define VIDEO_H

#include "gbint.h"
#include "interruptrequester.h"
#include "minkeeper.h"
#include "osd_element.h"
#include "scoped_ptr.h"
#include "video/lyc_irq.h"
#include "video/mstat_irq.h"
#include "video/next_m0_time.h"
#include "video/ppu.h"
#include "newstate.h"

namespace gambatte {

class VideoInterruptRequester {
public:
	explicit VideoInterruptRequester(InterruptRequester &intreq)
	: intreq_(intreq)
	{
	}

	void flagHdmaReq() const { if (!intreq_.halted()) gambatte::flagHdmaReq(intreq_); }
	void flagIrq(unsigned bit) const { intreq_.flagIrq(bit); }
	void flagIrq(unsigned bit, gb_ulong cc) const { intreq_.flagIrq(bit, cc); }
	void setNextEventTime(gb_ulong time) const { intreq_.setEventTime<intevent_video>(time); }

private:
	InterruptRequester &intreq_;
};

class LCD {
public:
	LCD(unsigned char const *oamram, unsigned char const *vram,
	    VideoInterruptRequester memEventRequester);
	void reset(unsigned char const *oamram, unsigned char const *vram, bool cgb, bool agb);
	void setCgbDmg(bool enabled) { ppu_.setCgbDmg(enabled); }
	void setSpPriority(bool xSpPriority, gb_ulong cc) { update(cc); ppu_.setSpPriority(xSpPriority); }
	void setStatePtrs(SaveState &state);
	void saveState(SaveState &state) const;
	void loadState(SaveState const &state, unsigned char const *oamram);
	void setDmgPaletteColor(unsigned palNum, unsigned colorNum, gb_ulong rgb32);
	void setCgbPalette(unsigned *lut);
	void setVideoBuffer(uint_least32_t *videoBuf, std::ptrdiff_t pitch);
	void setLayers(unsigned mask) { ppu_.setLayers(mask); }
	// armSNES
	void setIcdLine(void (*line)(void *, unsigned, uint_least32_t const *), void *p) { ppu_.setIcdLine(line, p); }
	void copyCgbPalettesToDmg();
	void setOsdElement(transfer_ptr<OsdElement> osdElement) { osdElement_ = osdElement; }

	int peekLy() const { return ppu_.lyCounter().ly(); }

	void dmgBgPaletteChange(unsigned data, gb_ulong cycleCounter) {
		update(cycleCounter);
		bgpData_[0] = data;
		setDmgPalette(ppu_.bgPalette(), dmgColorsBgr15_, data);
	}

	void dmgSpPalette1Change(unsigned data, gb_ulong cycleCounter) {
		update(cycleCounter);
		objpData_[0] = data;
		setDmgPalette(ppu_.spPalette(), dmgColorsBgr15_ + 4, data);
	}

	void dmgSpPalette2Change(unsigned data, gb_ulong cycleCounter) {
		update(cycleCounter);
		objpData_[1] = data;
		setDmgPalette(ppu_.spPalette() + 4, dmgColorsBgr15_ + 8, data);
	}

	void cgbBgColorChange(unsigned index, unsigned data, gb_ulong cycleCounter) {
		if (bgpData_[index] != data)
			doCgbBgColorChange(index, data, cycleCounter);
	}

	void cgbSpColorChange(unsigned index, unsigned data, gb_ulong cycleCounter) {
		if (objpData_[index] != data)
			doCgbSpColorChange(index, data, cycleCounter);
	}

	unsigned cgbBgColorRead(unsigned index, gb_ulong cycleCounter) {
		return ppu_.cgb() && cgbpAccessible(cycleCounter) ? bgpData_[index] : 0xFF;
	}

	unsigned cgbSpColorRead(unsigned index, gb_ulong cycleCounter) {
		return ppu_.cgb() && cgbpAccessible(cycleCounter) ? objpData_[index] : 0xFF;
	}

	void updateScreen(bool blanklcd, gb_ulong cc, unsigned stage);
	void whiteScreen();
	void blackScreen();
	void resetCc(gb_ulong oldCC, gb_ulong newCc);
	void speedChange(gb_ulong cycleCounter);
	bool vramReadable(gb_ulong cycleCounter);
	bool vramExactlyReadable(gb_ulong cycleCounter);
	bool vramWritable(gb_ulong cycleCounter);
	bool oamReadable(gb_ulong cycleCounter);
	bool oamWritable(gb_ulong cycleCounter);
	void wxChange(unsigned newValue, gb_ulong cycleCounter);
	void wyChange(unsigned newValue, gb_ulong cycleCounter);
	void oamChange(gb_ulong cycleCounter);
	void oamChange(const unsigned char *oamram, gb_ulong cycleCounter);
	// armGBA: where the sprites are read from, with nothing else changed, for an exact state's load
	void setOam(const unsigned char *oamram) { ppu_.setOam(oamram); }
	void scxChange(unsigned newScx, gb_ulong cycleCounter);
	void scyChange(unsigned newValue, gb_ulong cycleCounter);
	void vramChange(gb_ulong cycleCounter) { update(cycleCounter); }
	unsigned getStat(unsigned lycReg, gb_ulong cycleCounter);

	unsigned getLyReg(gb_ulong const cc) {
		unsigned lyReg = 0;

		if (ppu_.lcdc() & lcdc_en) {
			if (cc >= ppu_.lyCounter().time())
				update(cc);

			lyReg = ppu_.lyCounter().ly();
			if (lyReg == lcd_lines_per_frame - 1) {
				if (isDoubleSpeed()
					? ppu_.lyCounter().time() - cc <= 2 * lcd_cycles_per_line - 2
					: ppu_.lyCounter().time() - cc <= lcd_cycles_per_line - 1u * isAgb())
					lyReg = 0;
			} else if (ppu_.lyCounter().time() - cc <= 10
					&& ppu_.lyCounter().time() - cc <= 6u + 4 * isDoubleSpeed()) {
				lyReg = ppu_.lyCounter().time() - cc == 6u + 4 * isDoubleSpeed()
					? lyReg & (lyReg + 1)
					: lyReg + 1;
			}
		}

		return lyReg;
	}

	gb_ulong nextMode1IrqTime() const { return eventTimes_(memevent_m1irq); }
	void lcdcChange(unsigned data, gb_ulong cycleCounter);
	void lcdstatChange(unsigned data, gb_ulong cycleCounter);
	void lycRegChange(unsigned data, gb_ulong cycleCounter);
	void enableHdma(gb_ulong cycleCounter);
	void disableHdma(gb_ulong cycleCounter);
	bool isHdmaPeriod(gb_ulong cycleCounter);
	bool hdmaIsEnabled() const { return eventTimes_(memevent_hdma) != disabled_time; }
	void update(gb_ulong cycleCounter);
	bool isCgb() const { return ppu_.cgb(); }
	bool isAgb() const { return ppu_.agb(); }
	bool isCgbDmg() const { return ppu_.cgbDmg(); }
	bool isDoubleSpeed() const { return ppu_.lyCounter().isDoubleSpeed(); }
	void setSpeedupFlags(unsigned flags) { ppu_.setSpeedupFlags(flags); }

	template<bool isReader>void SyncState(NewState *ns);

	gb_ulong *bgPalette() { return ppu_.bgPalette(); }
	gb_ulong *spPalette() { return ppu_.spPalette(); }

	void setScanlineCallback(void (*callback)(), int sl) { scanlinecallback = callback; scanlinecallbacksl = sl; }

private:
	enum Event { event_mem,
	             event_ly, event_last = event_ly };

	enum MemEvent { memevent_oneshot_statirq,
	                memevent_oneshot_updatewy2,
	                memevent_m1irq,
	                memevent_lycirq,
	                memevent_spritemap,
	                memevent_hdma,
	                memevent_m2irq,
	                memevent_m0irq, memevent_last = memevent_m0irq };

	enum { num_events = event_last + 1 };
	enum { num_memevents = memevent_last + 1 };

	class EventTimes {
	public:
		explicit EventTimes(VideoInterruptRequester memEventRequester)
		: eventMin_(disabled_time)
		, memEventMin_(disabled_time)
		, memEventRequester_(memEventRequester)
		{
		}

		Event nextEvent() const { return static_cast<Event>(eventMin_.min()); }
		gb_ulong nextEventTime() const { return eventMin_.minValue(); }
		gb_ulong operator()(Event e) const { return eventMin_.value(e); }
		template<Event e> void set(gb_ulong time) { eventMin_.setValue<e>(time); }
		void set(Event e, gb_ulong time) { eventMin_.setValue(e, time); }

		MemEvent nextMemEvent() const { return static_cast<MemEvent>(memEventMin_.min()); }
		gb_ulong nextMemEventTime() const { return memEventMin_.minValue(); }
		gb_ulong operator()(MemEvent e) const { return memEventMin_.value(e); }

		template<MemEvent e>
		void setm(gb_ulong time) { memEventMin_.setValue<e>(time); setMemEvent(); }
		void set(MemEvent e, gb_ulong time) { memEventMin_.setValue(e, time); setMemEvent(); }

		void flagIrq(unsigned bit) { memEventRequester_.flagIrq(bit); }
		void flagIrq(unsigned bit, gb_ulong cc) { memEventRequester_.flagIrq(bit, cc); }
		void flagHdmaReq() { memEventRequester_.flagHdmaReq(); }

		template<bool isReader>
		void SyncState(NewState *ns) {
			SSS(eventMin_);
			SSS(memEventMin_);
		}

	private:
		MinKeeper<num_events> eventMin_;
		MinKeeper<num_memevents> memEventMin_;
		VideoInterruptRequester memEventRequester_;

		void setMemEvent() {
			gb_ulong nmet = nextMemEventTime();
			eventMin_.setValue<event_mem>(nmet);
			memEventRequester_.setNextEventTime(nmet);
		}
	};

	PPU ppu_;
	unsigned short dmgColorsBgr15_[3 * 4];
	// armGBA: a Game Boy's screen while the game has it off, and whether it shows the background color instead
	gb_ulong offColor_;
	gb_ulong  cgbColorsRgb32_[32768];
	unsigned char  bgpData_[2 * max_num_palettes * num_palette_entries];
	unsigned char objpData_[2 * max_num_palettes * num_palette_entries];
	EventTimes eventTimes_;
	MStatIrqEvent mstatIrq_;
	LycIrq lycIrq_;
	NextM0Time nextM0Time_;
	scoped_ptr<OsdElement> osdElement_;
	unsigned char statReg_;
	bool vramHasBeenExactlyRead = false;

	void setDmgPalette(gb_ulong palette[],
	                   unsigned short const dmgColors[],
	                   unsigned data);

	gb_ulong gbcToRgb32(const unsigned bgr15);
	void doCgbColorChange(unsigned char *const pdata, gb_ulong *const palette, unsigned index, const unsigned data);
	void refreshPalettes();
	void setDBuffer();
	void doMode2IrqEvent();
	void event();
	gb_ulong m0TimeOfCurrentLine(gb_ulong cc);
	bool cgbpAccessible(gb_ulong cycleCounter);
	bool lycRegChangeStatTriggerBlockedByM0OrM1Irq(unsigned data, gb_ulong cc);
	bool lycRegChangeTriggersStatIrq(unsigned old, unsigned data, gb_ulong cc);
	bool statChangeTriggersM0LycOrM1StatIrqCgb(unsigned old, unsigned data, bool lycperiod, gb_ulong cc);
	bool statChangeTriggersStatIrqCgb(unsigned old, unsigned data, gb_ulong cc);
	bool statChangeTriggersStatIrqDmg(unsigned old, gb_ulong cc);
	bool statChangeTriggersStatIrq(unsigned old, unsigned data, gb_ulong cc);
	void mode3CyclesChange();
	void doCgbBgColorChange(unsigned index, unsigned data, gb_ulong cycleCounter);
	void doCgbSpColorChange(unsigned index, unsigned data, gb_ulong cycleCounter);

	void (*scanlinecallback)();
	int scanlinecallbacksl;

};

}

#endif

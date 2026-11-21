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

#ifndef SPRITE_MAPPER_H
#define SPRITE_MAPPER_H

#include "gbint.h"
#include "ly_counter.h"
#include "../savestate.h"
#include "newstate.h"

namespace gambatte {

class NextM0Time;

class SpriteMapper {
public:
	SpriteMapper(NextM0Time &nextM0Time,
	             LyCounter const &lyCounter,
	             unsigned char const *oamram);
	void reset(unsigned char const *oamram, bool cgb);
	gb_ulong doEvent(gb_ulong time);
	bool largeSprites(int spno) const { return oamReader_.largeSprites(spno); }
	int numSprites(unsigned ly) const { return num_[ly] & ~(1u * need_sorting_flag); }
	void oamChange(gb_ulong cc) { oamReader_.change(cc); }
	void oamChange(unsigned char const *oamram, gb_ulong cc) { oamReader_.change(oamram, cc); }
	void setOam(unsigned char const *oamram) { oamReader_.setOam(oamram); }
	unsigned char const * oamram() const { return oamReader_.oam(); }
	unsigned char const * posbuf() const { return oamReader_.spritePosBuf(); }

	void resetCycleCounter(gb_ulong oldCc, gb_ulong newCc) {
		oamReader_.update(oldCc);
		oamReader_.resetCycleCounter(oldCc, newCc);
	}

	void setLargeSpritesSource(bool src) { oamReader_.setLargeSpritesSrc(src); }

	unsigned char const * sprites(unsigned ly) const {
		if (num_[ly] & need_sorting_flag)
			sortLine(ly);

		return spritemap_[ly];
	}

	void setStatePtrs(SaveState &state) { oamReader_.setStatePtrs(state); }
	void enableDisplay(gb_ulong cc) { oamReader_.enableDisplay(cc); }
	void saveState(SaveState &state) const { oamReader_.saveState(state); }

	void loadState(SaveState const &state, unsigned char const *oamram) {
		oamReader_.loadState(state, oamram);
		mapSprites();
	}

	bool inactivePeriodAfterDisplayEnable(gb_ulong cc) const {
		return oamReader_.inactivePeriodAfterDisplayEnable(cc);
	}

	static gb_ulong schedule(LyCounter const &lyCounter, gb_ulong cc) {
		return lyCounter.nextLineCycle(2 * lcd_num_oam_entries, cc);
	}

	template<bool isReader>void SyncState(NewState *ns);

private:
	class OamReader {
	public:
		OamReader(LyCounter const &lyCounter, unsigned char const *oamram);
		void reset(unsigned char const *oamram, bool cgb);
		void change(gb_ulong cc);
		void change(unsigned char const *oamram, gb_ulong cc) { change(cc); oamram_ = oamram; }
		void setOam(unsigned char const *oamram) { oamram_ = oamram; }
		bool changed() const { return lastChange_ != 0xFF; }
		bool largeSprites(int spno) const { return lsbuf_[spno]; }
		unsigned char const * oam() const { return oamram_; }
		void resetCycleCounter(gb_ulong oldCc, gb_ulong newCc) { lu_ -= oldCc - newCc; }
		void setLargeSpritesSrc(bool src) { largeSpritesSrc_ = src; }
		void update(gb_ulong cc);
		unsigned char const * spritePosBuf() const { return buf_; }
		void setStatePtrs(SaveState &state);
		void enableDisplay(gb_ulong cc);
		void saveState(SaveState &state) const { state.ppu.enableDisplayM0Time = lu_; }
		void loadState(SaveState const &ss, unsigned char const *oamram);
		bool inactivePeriodAfterDisplayEnable(gb_ulong cc) const { return cc < lu_; }
		unsigned lineTime() const { return lyCounter_.lineTime(); }
		template<bool isReader>void SyncState(NewState *ns);

	private:
		unsigned char buf_[2 * lcd_num_oam_entries];
		bool lsbuf_[lcd_num_oam_entries];
		LyCounter const &lyCounter_;
		unsigned char const *oamram_;
		gb_ulong lu_;
		unsigned char lastChange_;
		bool largeSpritesSrc_;
		bool cgb_;
	};

	enum { need_sorting_flag = 0x80 };

	mutable unsigned char spritemap_[lcd_vres][lcd_max_num_sprites_per_line];
	mutable unsigned char num_[lcd_vres];
	NextM0Time &nextM0Time_;
	OamReader oamReader_;

	void clearMap();
	void mapSprites();
	void sortLine(unsigned ly) const;
};

}

#endif

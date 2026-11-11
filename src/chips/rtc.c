/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

#include <time.h>

// The real-time clocks: the Epson RTC-4513 on an SPC7110 board, and the Sharp S-RTC. Both keep time
// by the machine's clock while the game runs. Only loading a clock the frontend kept moves it on by
// the time the game was off.

struct EpsonRtc epsonRtc;
struct SharpRtc sharpRtc;

#define E epsonRtc
#define S sharpRtc

// --- Epson RTC-4513 ---

enum { EPSON_MODE, EPSON_SEEK, EPSON_READ, EPSON_WRITE };

// Ticking keeps invalid BCD values as the chip does, so each digit keeps its own width.

static void epsonTickYear(void) {
	if (E.yearlo <= 8 || E.yearlo == 12) {
		E.yearlo = (E.yearlo + 1) & 15;
	} else {
		E.yearlo = !(E.yearlo & 1);
		if (E.yearhi <= 8 || E.yearhi == 12) {
			E.yearhi = (E.yearhi + 1) & 15;
		} else {
			E.yearhi = !(E.yearhi & 1);
		}
	}
}

static void epsonTickMonth(void) {
	if (E.monthhi == 0 || !(E.monthlo & 2)) {
		if (E.monthlo <= 8 || E.monthlo == 12) {
			E.monthlo = (E.monthlo + 1) & 15;
		} else {
			E.monthlo = !(E.monthlo & 1);
			E.monthhi ^= 1;
		}
	} else {
		E.monthlo = !(E.monthlo & 1);
		E.monthhi = 0;
		epsonTickYear();
	}
}

static void epsonTickDay(void) {
	if (E.calendar == 0) {
		return;
	}
	E.weekday = ((E.weekday + 1) + (E.weekday == 6)) & 7;

	// January to December are 0x01-0x09 and 0x10-0x12.
	static const u32 daysInMonth[32] = {
		30, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 30, 31, 30,
		31, 30, 31, 30, 31, 30, 31, 30, 31, 30, 31, 30, 31, 30, 31, 30,
	};
	u32 days = daysInMonth[E.monthhi << 4 | E.monthlo];
	if (days == 28) {
		// A day more in a leap year.
		if ((E.yearhi & 1) == 0 && ((E.yearlo - 0u) & 3) == 0) days++;
		if ((E.yearhi & 1) == 1 && ((E.yearlo - 2u) & 3) == 0) days++;
	}

	if ((days == 28 && (E.dayhi == 3 || (E.dayhi == 2 && E.daylo >= 8))) ||
	    (days == 29 && (E.dayhi == 3 || (E.dayhi == 2 && (E.daylo > 8 && E.daylo != 12)))) ||
	    (days == 30 && (E.dayhi == 3 || (E.dayhi == 2 && (E.daylo == 10 || E.daylo == 14)))) ||
	    (days == 31 && (E.dayhi == 3 && (E.daylo & 3)))) {
		E.daylo = 1;
		E.dayhi = 0;
		epsonTickMonth();
		return;
	}

	if (E.daylo <= 8 || E.daylo == 12) {
		E.daylo = (E.daylo + 1) & 15;
	} else {
		E.daylo = !(E.daylo & 1);
		E.dayhi = (E.dayhi + 1) & 3;
	}
}

static void epsonTickHour(void) {
	if (E.atime) {
		if (E.hourhi < 2) {
			if (E.hourlo <= 8 || E.hourlo == 12) {
				E.hourlo = (E.hourlo + 1) & 15;
			} else {
				E.hourlo = !(E.hourlo & 1);
				E.hourhi = (E.hourhi + 1) & 3;
			}
		} else {
			if (E.hourlo != 3 && !(E.hourlo & 4)) {
				if (E.hourlo <= 8 || E.hourlo >= 12) {
					E.hourlo = (E.hourlo + 1) & 15;
				} else {
					E.hourlo = !(E.hourlo & 1);
					E.hourhi = (E.hourhi + 1) & 3;
				}
			} else {
				E.hourlo = !(E.hourlo & 1);
				E.hourhi = 0;
				epsonTickDay();
			}
		}
	} else {
		if (E.hourhi == 0) {
			if (E.hourlo <= 8 || E.hourlo == 12) {
				E.hourlo = (E.hourlo + 1) & 15;
			} else {
				E.hourlo = !(E.hourlo & 1);
				E.hourhi ^= 1;
			}
		} else {
			if (E.hourlo & 1) {
				E.meridian ^= 1;
			}
			if (E.hourlo < 2 || E.hourlo == 4 || E.hourlo == 5 || E.hourlo == 8 || E.hourlo == 12) {
				E.hourlo = (E.hourlo + 1) & 15;
			} else {
				E.hourlo = !(E.hourlo & 1);
				E.hourhi ^= 1;
			}
			if (E.meridian == 0 && !(E.hourlo & 1)) {
				epsonTickDay();
			}
		}
	}
}

static void epsonTickMinute(void) {
	if (E.minutelo <= 8 || E.minutelo == 12) {
		E.minutelo = (E.minutelo + 1) & 15;
	} else {
		E.minutelo = 0;
		if (E.minutehi < 5) {
			E.minutehi++;
		} else {
			E.minutehi = 0;
			epsonTickHour();
		}
	}
}

static void epsonTickSecond(void) {
	if (E.secondlo <= 8 || E.secondlo == 12) {
		E.secondlo = (E.secondlo + 1) & 15;
	} else {
		E.secondlo = 0;
		if (E.secondhi < 5) {
			E.secondhi++;
		} else {
			E.secondhi = 0;
			epsonTickMinute();
		}
	}
}

static void epsonIrq(u32 period) {
	if (E.stop || E.pause) {
		return;
	}
	if (period == E.irqperiod) {
		E.irqflag = 1;
	}
}

static void epsonDuty(void) {
	if (E.irqduty) {
		E.irqflag = 0;
	}
}

static void epsonRoundSeconds(void) {
	if (E.roundseconds == 0) {
		return;
	}
	E.roundseconds = 0;
	if (E.secondhi >= 3) {
		epsonTickMinute();
	}
	E.secondlo = 0;
	E.secondhi = 0;
}

// A second passed. While the time is held, it counts once the hold ends.
static void epsonTick(void) {
	if (E.stop || E.pause) {
		return;
	}
	if (E.hold) {
		E.holdtick = 1;
		return;
	}
	E.resync = 1;
	epsonTickSecond();
}

// It runs at 64 times 32768 Hz. The checks of the clock count are as ares has them.
void epsonRtcRun(void) {
	do {
		if (E.wait) {
			if (--E.wait == 0) {
				E.ready = 1;
			}
		}
		E.clocks = (E.clocks + 1) & 0x1fffff;
		if ((E.clocks & ~0x00ff) == 0) epsonRoundSeconds();
		if ((E.clocks & ~0x3fff) == 0) epsonDuty();
		if ((E.clocks & ~0x7fff) == 0) epsonIrq(0);
		if (E.clocks == 0) {
			E.seconds++;
			epsonIrq(1);
			if (E.seconds % 60 == 0) {
				epsonIrq(2);
			}
			if (E.seconds % 1440 == 0) {
				epsonIrq(3);
				E.seconds = 0;
			}
			epsonTick();
		}
		E.th.clock += E.th.scalar;
	} while (!(cpu.th.clock < E.th.clock));
}

static void epsonReset(void) {
	E.state = EPSON_MODE;
	E.offset = 0;
	E.resync = 0;
	E.pause = 0;
	E.test = 0;
}

static u8 epsonRead4(u32 address) {
	switch (address & 15) {
	default:
	case 0: return E.secondlo;
	case 1: return E.secondhi | E.batteryfailure << 3;
	case 2: return E.minutelo;
	case 3: return E.minutehi | E.resync << 3;
	case 4: return E.hourlo;
	case 5: return E.hourhi | E.meridian << 2 | E.resync << 3;
	case 6: return E.daylo;
	case 7: return E.dayhi | E.dayram << 2 | E.resync << 3;
	case 8: return E.monthlo;
	case 9: return E.monthhi | E.monthram << 1 | E.resync << 3;
	case 10: return E.yearlo;
	case 11: return E.yearhi;
	case 12: return E.weekday | E.resync << 3;
	case 13: {
		bool readflag = E.irqflag & !E.irqmask;
		E.irqflag = 0;
		return E.hold | E.calendar << 1 | readflag << 2 | E.roundseconds << 3;
	}
	case 14: return E.irqmask | E.irqduty << 1 | E.irqperiod << 2;
	case 15: return E.pause | E.stop << 1 | E.atime << 2 | E.test << 3;
	}
}

static void epsonWrite4(u32 address, u8 data) {
	data &= 15;
	switch (address & 15) {
	case 0:
		E.secondlo = data;
		break;
	case 1:
		E.secondhi = data & 7;
		E.batteryfailure = data >> 3;
		break;
	case 2:
		E.minutelo = data;
		break;
	case 3:
		E.minutehi = data & 7;
		break;
	case 4:
		E.hourlo = data;
		break;
	case 5:
		E.hourhi = data & 3;
		E.meridian = data >> 2 & 1;
		if (E.atime == 1) E.meridian = 0;
		if (E.atime == 0) E.hourhi &= 1;
		break;
	case 6:
		E.daylo = data;
		break;
	case 7:
		E.dayhi = data & 3;
		E.dayram = data >> 2 & 1;
		break;
	case 8:
		E.monthlo = data;
		break;
	case 9:
		E.monthhi = data & 1;
		E.monthram = data >> 1 & 3;
		break;
	case 10:
		E.yearlo = data;
		break;
	case 11:
		E.yearhi = data;
		break;
	case 12:
		E.weekday = data & 7;
		break;
	case 13: {
		bool held = E.hold;
		E.hold = data & 1;
		E.calendar = data >> 1 & 1;
		E.roundseconds = data >> 3 & 1;
		if (held == 1 && E.hold == 0 && E.holdtick == 1) {
			// A second passed while the time was held: it counts now.
			E.holdtick = 0;
			epsonTickSecond();
		}
		break;
	}
	case 14:
		E.irqmask = data & 1;
		E.irqduty = data >> 1 & 1;
		E.irqperiod = data >> 2 & 3;
		break;
	case 15:
		E.pause = data & 1;
		E.stop = data >> 1 & 1;
		E.atime = data >> 2 & 1;
		E.test = data >> 3 & 1;
		if (E.atime == 1) E.meridian = 0;
		if (E.atime == 0) E.hourhi &= 1;
		if (E.pause) {
			E.secondlo = 0;
			E.secondhi = 0;
		}
		break;
	}
}

static void epsonSyncFromCpu(void) {
	if (E.th.clock < cpu.th.clock) {
		epsonRtcRun();
	}
}

u8 epsonRtcRead(u32 address, u8 data) {
	epsonSyncFromCpu();
	address &= 3;
	if (address == 0) {
		return E.chipselect;
	}
	if (address == 1) {
		if (E.chipselect != 1 || E.ready == 0) {
			return 0;
		}
		if (E.state == EPSON_WRITE) {
			return E.mdr;
		}
		if (E.state != EPSON_READ) {
			return 0;
		}
		E.ready = 0;
		E.wait = 8;
		u8 value = epsonRead4(E.offset);
		E.offset = (E.offset + 1) & 15;
		return value;
	}
	if (address == 2) {
		return E.ready << 7;
	}
	return data;
}

void epsonRtcWrite(u32 address, u8 data) {
	epsonSyncFromCpu();
	address &= 3;
	data &= 15;
	if (address == 0) {
		E.chipselect = data & 3;
		if (E.chipselect != 1) {
			epsonReset();
		}
		E.ready = 1;
	}
	if (address == 1) {
		if (E.chipselect != 1 || E.ready == 0) {
			return;
		}
		if (E.state == EPSON_MODE) {
			if (data != 0x03 && data != 0x0c) {
				return;
			}
			E.state = EPSON_SEEK;
			E.ready = 0;
			E.wait = 8;
			E.mdr = data;
		} else if (E.state == EPSON_SEEK) {
			if (E.mdr == 0x03) E.state = EPSON_WRITE;
			if (E.mdr == 0x0c) E.state = EPSON_READ;
			E.offset = data;
			E.ready = 0;
			E.wait = 8;
			E.mdr = data;
		} else if (E.state == EPSON_WRITE) {
			epsonWrite4(E.offset, data);
			E.offset = (E.offset + 1) & 15;
			E.ready = 0;
			E.wait = 8;
			E.mdr = data;
		}
	}
}

// With no clock kept, the battery has failed and the time is zero.
void epsonRtcLoad(void) {
	memset(&E, 0, sizeof(E));
	E.batteryfailure = 1;
}

void epsonRtcPower(void) {
	threadFrequency(&E.th, 32768 * 64);
	E.clocks = 0;
	E.seconds = 0;
	E.chipselect = 0;
	E.state = EPSON_MODE;
	E.offset = 0;
	E.wait = 0;
	E.ready = 0;
	E.holdtick = 0;
}

static void epsonSetTime(const struct tm* t) {
	u32 second = t->tm_sec < 59 ? t->tm_sec : 59;
	E.secondlo = second % 10;
	E.secondhi = second / 10;
	E.minutelo = t->tm_min % 10;
	E.minutehi = t->tm_min / 10;
	u32 hour = t->tm_hour;
	if (!E.atime) {
		E.meridian = hour >= 12;
		hour %= 12;
		if (hour == 0) {
			hour = 12;
		}
	}
	E.hourlo = hour % 10;
	E.hourhi = hour / 10;
	E.daylo = t->tm_mday % 10;
	E.dayhi = t->tm_mday / 10;
	E.monthlo = (1 + t->tm_mon) % 10;
	E.monthhi = (1 + t->tm_mon) / 10;
	E.yearlo = t->tm_year % 100 % 10;
	E.yearhi = t->tm_year % 100 / 10;
	E.weekday = t->tm_wday;
	// Tells the game the time changed.
	E.resync = 1;
}

static void epsonFromBytes(const u8* data) {
	E.secondlo = data[0] & 15;
	E.secondhi = data[0] >> 4 & 7;
	E.batteryfailure = data[0] >> 7;
	E.minutelo = data[1] & 15;
	E.minutehi = data[1] >> 4 & 7;
	E.resync = data[1] >> 7;
	E.hourlo = data[2] & 15;
	E.hourhi = data[2] >> 4 & 3;
	E.meridian = data[2] >> 6 & 1;
	E.daylo = data[3] & 15;
	E.dayhi = data[3] >> 4 & 3;
	E.dayram = data[3] >> 6 & 1;
	E.monthlo = data[4] & 15;
	E.monthhi = data[4] >> 4 & 1;
	E.monthram = data[4] >> 5 & 3;
	E.yearlo = data[5] & 15;
	E.yearhi = data[5] >> 4;
	E.weekday = data[6] & 7;
	E.hold = data[6] >> 4 & 1;
	E.calendar = data[6] >> 5 & 1;
	E.irqflag = data[6] >> 6 & 1;
	E.roundseconds = data[6] >> 7;
	E.irqmask = data[7] & 1;
	E.irqduty = data[7] >> 1 & 1;
	E.irqperiod = data[7] >> 2 & 3;
	E.pause = data[7] >> 4 & 1;
	E.stop = data[7] >> 5 & 1;
	E.atime = data[7] >> 6 & 1;
	E.test = data[7] >> 7;
}

static void epsonToBytes(u8* data) {
	data[0] = E.secondlo | E.secondhi << 4 | E.batteryfailure << 7;
	data[1] = E.minutelo | E.minutehi << 4 | E.resync << 7;
	data[2] = E.hourlo | E.hourhi << 4 | E.meridian << 6 | E.resync << 7;
	data[3] = E.daylo | E.dayhi << 4 | E.dayram << 6 | E.resync << 7;
	data[4] = E.monthlo | E.monthhi << 4 | E.monthram << 5 | E.resync << 7;
	data[5] = E.yearlo | E.yearhi << 4;
	data[6] = E.weekday | E.resync << 3 | E.hold << 4 | E.calendar << 5 | E.irqflag << 6 | E.roundseconds << 7;
	data[7] = E.irqmask | E.irqduty << 1 | E.irqperiod << 2 | E.pause << 4 | E.stop << 5 | E.atime << 6 | E.test << 7;
}

// --- Sharp S-RTC ---

enum { SHARP_READY, SHARP_COMMAND, SHARP_READ, SHARP_WRITE };

static const u32 daysInMonth[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static void sharpTickYear(void) {
	S.year = (S.year + 1) & 0xfff;
}

static void sharpTickMonth(void) {
	if (S.month++ < 12) {
		return;
	}
	S.month = 1;
	sharpTickYear();
}

static void sharpTickDay(void) {
	u32 days = daysInMonth[(S.month - 1) % 12];
	// A day more in February in a leap year.
	if (S.month == 2) {
		if (S.year % 400 == 0) {
			days++;
		} else if (S.year % 100 == 0) {
		} else if (S.year % 4 == 0) {
			days++;
		}
	}
	if (S.day++ < days) {
		return;
	}
	S.day = 1;
	sharpTickMonth();
}

static void sharpTickHour(void) {
	if (++S.hour < 24) {
		return;
	}
	S.hour = 0;
	sharpTickDay();
}

static void sharpTickMinute(void) {
	if (++S.minute < 60) {
		return;
	}
	S.minute = 0;
	sharpTickHour();
}

static void sharpTickSecond(void) {
	if (++S.second < 60) {
		return;
	}
	S.second = 0;
	sharpTickMinute();
}

// The day of the week, 0 for Sunday, counted from the chip's epoch, Wednesday 1000-01-01.
static u32 sharpWeekday(u32 year, u32 month, u32 day) {
	u32 y = 1000, m = 1;
	u32 sum = 0;
	year = year > 1000 ? year : 1000;
	month = month < 1 ? 1 : month > 12 ? 12 : month;
	day = day < 1 ? 1 : day > 31 ? 31 : day;
	while (y < year) {
		bool leap = y % 4 == 0 && !(y % 100 == 0 && y % 400 != 0);
		sum += 365 + leap;
		y++;
	}
	while (m < month) {
		u32 days = daysInMonth[(m - 1) % 12];
		bool leapMonth = days == 28 && y % 4 == 0 && !(y % 100 == 0 && y % 400 != 0);
		sum += days + leapMonth;
		m++;
	}
	sum += day - 1;
	return (sum + 3) % 7;
}

static u8 sharpRead4(u32 address) {
	switch (address & 15) {
	case 0: return S.second % 10 & 15;
	case 1: return S.second / 10 & 15;
	case 2: return S.minute % 10 & 15;
	case 3: return S.minute / 10 & 15;
	case 4: return S.hour % 10 & 15;
	case 5: return S.hour / 10 & 15;
	case 6: return S.day % 10 & 15;
	case 7: return S.day / 10 & 15;
	case 8: return S.month & 15;
	case 9: return S.year % 10 & 15;
	case 10: return S.year / 10 % 10 & 15;
	case 11: return S.year / 100 & 15;
	case 12: return S.weekday & 15;
	}
	return 0;
}

static void sharpWrite4(u32 address, u8 data) {
	data &= 15;
	switch (address & 15) {
	case 0: S.second = S.second / 10 * 10 + data; break;
	case 1: S.second = data * 10 + S.second % 10; break;
	case 2: S.minute = S.minute / 10 * 10 + data; break;
	case 3: S.minute = data * 10 + S.minute % 10; break;
	case 4: S.hour = S.hour / 10 * 10 + data; break;
	case 5: S.hour = data * 10 + S.hour % 10; break;
	case 6: S.day = S.day / 10 * 10 + data; break;
	case 7: S.day = data * 10 + S.day % 10; break;
	case 8: S.month = data; break;
	case 9: S.year = S.year / 10 * 10 + data; break;
	case 10: S.year = S.year / 100 * 100 + data * 10 + S.year % 10; break;
	case 11: S.year = data * 100 + S.year % 100; break;
	case 12: S.weekday = data; break;
	}
}

// It ticks once a second.
void sharpRtcRun(void) {
	do {
		sharpTickSecond();
		S.th.clock += S.th.scalar;
	} while (!(cpu.th.clock < S.th.clock));
}

u8 sharpRtcRead(u32 address, u8 data) {
	if (address & 1) {
		return data;
	}
	if (S.state != SHARP_READ) {
		return 0;
	}
	if (S.index < 0) {
		S.index++;
		return 15;
	}
	if (S.index > 12) {
		S.index = -1;
		return 15;
	}
	return sharpRead4(S.index++);
}

void sharpRtcWrite(u32 address, u8 data) {
	if (!(address & 1)) {
		return;
	}
	data &= 15;
	if (data == 0x0d) {
		S.state = SHARP_READ;
		S.index = -1;
		return;
	}
	if (data == 0x0e) {
		S.state = SHARP_COMMAND;
		return;
	}
	if (data == 0x0f) {
		return;
	}
	if (S.state == SHARP_COMMAND) {
		if (data == 0) {
			S.state = SHARP_WRITE;
			S.index = 0;
		} else if (data == 4) {
			// The time reset.
			S.state = SHARP_READY;
			S.index = -1;
			S.second = S.minute = S.hour = S.day = S.month = S.year = S.weekday = 0;
		} else {
			S.state = SHARP_READY;
		}
		return;
	}
	if (S.state == SHARP_WRITE && S.index >= 0 && S.index < 12) {
		sharpWrite4(S.index++, data);
		if (S.index == 12) {
			// The chip works out the day of the week itself.
			S.weekday = sharpWeekday(1000 + S.year, S.month, S.day);
		}
	}
}

void sharpRtcLoad(void) {
	memset(&S, 0, sizeof(S));
}

void sharpRtcPower(void) {
	threadFrequency(&S.th, 1);
	S.state = SHARP_READ;
	S.index = -1;
}

static void sharpSetTime(const struct tm* t) {
	S.second = t->tm_sec < 59 ? t->tm_sec : 59;
	S.minute = t->tm_min;
	S.hour = t->tm_hour;
	S.day = t->tm_mday;
	S.month = 1 + t->tm_mon;
	S.year = 900 + t->tm_year;
	S.weekday = t->tm_wday;
}

// --- The clock the frontend keeps ---

bool rtcPresent(void) {
	return cart.epsonRtc || cart.sharpRtc;
}

void rtcSave(u8 data[16], s64 now) {
	memset(data, 0, 16);
	if (cart.epsonRtc) {
		epsonToBytes(data);
	} else if (cart.sharpRtc) {
		for (u32 n = 0; n < 8; ++n) {
			data[n] = sharpRead4(n * 2) | sharpRead4(n * 2 + 1) << 4;
		}
	}
	for (u32 n = 0; n < 8; ++n) {
		data[8 + n] = (u64) now >> (n * 8);
	}
}

// A clock saved with no time, or with all bits set, takes the host's time.
void rtcLoad(const u8 data[16], s64 now) {
	if (!rtcPresent()) {
		return;
	}
	if (cart.epsonRtc) {
		epsonFromBytes(data);
	} else {
		for (u32 n = 0; n < 8; ++n) {
			sharpWrite4(n * 2, data[n]);
			sharpWrite4(n * 2 + 1, data[n] >> 4);
		}
	}

	u64 saved = 0;
	for (u32 n = 0; n < 8; ++n) {
		saved |= (u64) data[8 + n] << (n * 8);
	}
	if (saved == 0 || saved == ~0ull) {
		time_t host = (time_t) now;
		struct tm* t = localtime(&host);
		if (t) {
			if (cart.epsonRtc) {
				epsonSetTime(t);
			} else {
				sharpSetTime(t);
			}
		}
		return;
	}
	if (now <= (s64) saved) {
		return;
	}
	u64 diff = now - saved;
	void (*tickDay)(void) = cart.epsonRtc ? epsonTickDay : sharpTickDay;
	void (*tickHour)(void) = cart.epsonRtc ? epsonTickHour : sharpTickHour;
	void (*tickMinute)(void) = cart.epsonRtc ? epsonTickMinute : sharpTickMinute;
	void (*tickSecond)(void) = cart.epsonRtc ? epsonTickSecond : sharpTickSecond;
	for (; diff >= 60 * 60 * 24; diff -= 60 * 60 * 24) tickDay();
	for (; diff >= 60 * 60; diff -= 60 * 60) tickHour();
	for (; diff >= 60; diff -= 60) tickMinute();
	for (; diff; --diff) tickSecond();
}

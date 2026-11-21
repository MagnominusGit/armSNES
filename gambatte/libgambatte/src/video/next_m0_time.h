#ifndef NEXT_M0_TIME_H_
#define NEXT_M0_TIME_H_

#include "gbint.h"
#include "newstate.h"

namespace gambatte {

class NextM0Time {
public:
	NextM0Time() : predictedNextM0Time_(0) {}
	void predictNextM0Time(class PPU const &v);
	void invalidatePredictedNextM0Time() { predictedNextM0Time_ = 0; }
	gb_ulong predictedNextM0Time() const { return predictedNextM0Time_; }
	template<bool isReader>void SyncState(NewState *ns);

private:
	gb_ulong predictedNextM0Time_;
};

}

#endif

#ifndef NEWSTATE_H
#define NEWSTATE_H

#include <cstring>
#include <cstddef>
#include "gbint.h"

namespace gambatte {

class NewState {
public:
	virtual void Save(void const *ptr, std::size_t size, char const *name) = 0;
	virtual void Load(void *ptr, std::size_t size, char const *name) = 0;
	virtual void EnterSection(char const */*name*/) { }
	virtual void ExitSection(char const */*name*/) { }
};

class NewStateDummy : public NewState {
public:
	NewStateDummy();
	gb_long GetLength() { return length; }
	void Rewind() { length = 0; }
	virtual void Save(void const *ptr, std::size_t size, char const *name);
	virtual void Load(void *ptr, std::size_t size, char const *name);

private:
	gb_long length;
};

class NewStateExternalBuffer : public NewState {
public:
	NewStateExternalBuffer(char *buffer, gb_long maxlength);
	gb_long GetLength() { return length; }
	void Rewind() { length = 0; }
	bool Overflow() { return length > maxlength; }
	virtual void Save(void const *ptr, std::size_t size, char const *name);
	virtual void Load(void *ptr, std::size_t size, char const *name);

private:
	char *const buffer;
	gb_long length;
	const gb_long maxlength;
};

struct FPtrs {
	void (*Save_)(void const *ptr, std::size_t size, char const *name);
	void (*Load_)(void *ptr, std::size_t size, char const *name);
	void (*EnterSection_)(char const *name);
	void (*ExitSection_)(char const *name);
};

class NewStateExternalFunctions : public NewState {
public:
	NewStateExternalFunctions(const FPtrs *ff);
	virtual void Save(void const *ptr, std::size_t size, char const *name);
	virtual void Load(void *ptr, std::size_t size, char const *name);
	virtual void EnterSection(char const *name);
	virtual void ExitSection(char const *name);
private:
	void (*Save_)(void const *ptr, std::size_t size, char const *name);
	void (*Load_)(void *ptr, std::size_t size, char const *name);
	void (*EnterSection_)(char const *name);
	void (*ExitSection_)(char const *name);
};

// defines and explicitly instantiates
#define SYNCFUNC(x)\
	template void x::SyncState<false>(NewState *ns);\
	template void x::SyncState<true>(NewState *ns);\
	template<bool isReader>void x::SyncState(NewState *ns)

// N = normal variable
// P = pointer to fixed size data
// S = "sub object"
// T = "ptr to sub object"
// R = pointer, store its offset from some other pointer
// E = general purpose cased value "enum"


// first line is default value in converted enum; last line is default value in argument x
#define EBS(x,d) do { int _ttmp = (d); if (isReader) ns->Load(&_ttmp, sizeof _ttmp, #x); if (0)
#define EVS(x,v,n) else if (!isReader && (x) == (v)) _ttmp = (n); else if (isReader && _ttmp == (n)) (x) = (v)
#define EES(x,d) else if (isReader) (x) = (d); if (!isReader) ns->Save(&_ttmp, sizeof _ttmp, #x); } while (0)

// armGBA: pointer offsets are saved as 64 bits, and Gambatte's longs are 32 bits on every build
// (gbint.h), so a 32-bit build's state is the same as a 64-bit build's.
#define RSS(x,b) do { if (isReader)\
{ long long _ttmp; ns->Load(&_ttmp, sizeof _ttmp, #x); (x) = (_ttmp == 0xdeadbeef ? 0 : (b) + (std::ptrdiff_t)_ttmp); }\
	else\
{ long long _ttmp = (x) == 0 ? 0xdeadbeef : (long long)((x) - (b)); ns->Save(&_ttmp, sizeof _ttmp, #x); } } while (0)

#define PSS(x,s) do { if (isReader) ns->Load((x), (s), #x); else ns->Save((x), (s), #x); } while (0)

#define NSS(x) do { if (isReader) ns->Load(&(x), sizeof x, #x); else ns->Save(&(x), sizeof x, #x); } while (0)

#define SSS(x) do { ns->EnterSection(#x); (x).SyncState<isReader>(ns); ns->ExitSection(#x); } while (0)

#define TSS(x) do { ns->EnterSection(#x); (x)->SyncState<isReader>(ns); ns->ExitSection(#x); } while (0)

}

#endif

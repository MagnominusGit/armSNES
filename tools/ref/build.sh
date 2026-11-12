#!/bin/sh
# build.sh [ares checkout] [build folder]: builds aresrun, the reference this core is compared with,
# from unmodified ares at the commit the port was made from. Only ares's top-level build file is
# changed, to leave out its desktop frontend and add the runner.
set -e
commit=dd737665221a6f58c6c4d2547d2f38b0abe3aef0
here=$(cd "$(dirname "$0")" && pwd)
ares=${1:-$HOME/armsnes-ares-reference}
out=${2:-$here/../../build-ref}
if [ ! -d "$ares/.git" ]; then
	git clone https://github.com/ares-emulator/ares "$ares"
fi
mkdir -p "$out/ares"
git -C "$ares" archive "$commit" | tar -x -C "$out/ares"
cp "$here/aresrun.cpp" "$out/ares/aresrun.cpp"
cd "$out/ares"
sed -i -e '/^add_subdirectory(ruby)$/d' -e '/^add_subdirectory(hiro)$/d' -e '/^add_subdirectory(desktop-ui)$/d' -e '/^return()$/d' CMakeLists.txt
cat >> CMakeLists.txt <<'CMAKE'
add_executable(aresrun aresrun.cpp)
target_include_directories(aresrun PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(aresrun PRIVATE MIA_LIBRARY ARES_BOARDS="${CMAKE_SOURCE_DIR}/mia/Database/Super Famicom Boards.bml")
target_link_libraries(aresrun PRIVATE ares mia ares::nall)
CMAKE
cmake -S . -B ../b -DCMAKE_BUILD_TYPE=Release -DARES_CORES="sfc;gb" -DARES_ENABLE_CHD=OFF -DARES_ENABLE_LIBRASHADER=OFF > ../configure.log 2>&1
cmake --build ../b --target aresrun -j"${JOBS:-4}"
# mia wants a game database to exist. An empty one leaves every game to the header, as this core reads it.
mkdir -p ../b/Database
: > "../b/Database/Super Famicom.bml"
: > "../b/Database/BS Memory.bml"
: > "../b/Database/Sufami Turbo.bml"
echo "$out/b/aresrun"

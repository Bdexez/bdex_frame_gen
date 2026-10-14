# Cross-build of the Windows layer DLL with MinGW-w64 (tools/ci/wine-layer-check.sh).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# MinGW's own printf (%zu: msvcrt's does not know it), the C++ runtime and
# winpthreads linked in (a DLL without dependencies, like the MSVC build).
set(CMAKE_C_FLAGS_INIT "-D__USE_MINGW_ANSI_STDIO=1")
set(CMAKE_CXX_FLAGS_INIT "-D__USE_MINGW_ANSI_STDIO=1")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

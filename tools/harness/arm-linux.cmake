# ARM Linux harness (run with qemu-arm). Used to compare the recompiler (ari64 new_dynarec) against the interpreter on PC.
#   BUILD_DIR=build-arm bash tools/build.sh pc -DCMAKE_TOOLCHAIN_FILE=$PWD/tools/harness/arm-linux.cmake -DRS12_DYNAREC=ON
#   qemu-arm -L /usr/arm-linux-gnueabihf ./build-arm/harness --dynarec ...
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_ASM_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
set(CMAKE_ASM_FLAGS_INIT "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
# zlib is a static build in ~/armzlib/inst (zlib 1.3.1)
set(ZLIB_ROOT $ENV{HOME}/armzlib/inst)
include_directories(${ZLIB_ROOT}/include)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${ZLIB_ROOT}/lib")

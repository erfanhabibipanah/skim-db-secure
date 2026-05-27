#!/usr/bin/env bash

PROJECT=""
JOBS=8

usage() {
  echo "usage: $0 [OPTIONS]"
  echo "options:"
  echi "  -B        build with big k-mer support (64bit encoding)"
  echo "  -T        build tools"
  echo "  -g        build gRPC support"
  echo "  -s        build SiPeR support"
  echo "  -h        display this help"
  echo "  -v        enable verbose mode"
  echo "  -d        enable debug mode"
  echo "  -j <JOBS> set number of make jobs to build with (default: $JOBS)"
}

DIR=$(pwd)/release
CMAKE_CALL="../"

while getopts "BTgshvdj:r:l" arg; do
  case $arg in
    h)
      usage
      exit -1
      ;;
    v)
      VERBOSE="VERBOSE=1"
      ;;
    d)
      CMAKE_CALL="$CMAKE_CALL -DCMAKE_BUILD_TYPE=Debug"
      ;;
    j)
      JOBS=$OPTARG
      ;;
    r)
      RPATH="$OPTARG"
      CMAKE_CALL="$CMAKE_CALL -DCMAKE_BUILD_RPATH=$RPATH -DCMAKE_INSTALL_RPATH=$RPATH"
      ;;
    l)
      BUILD_LOG=1
      ;;
    B)
      CMAKE_CALL="$CMAKE_CALL -DSKIMDB_64BIT=ON"
      ;;
    T)
      CMAKE_CALL="$CMAKE_CALL -DSKIMDB_BUILD_TOOLS=ON"
      ;;
    g)
      CMAKE_CALL="$CMAKE_CALL -DSKIMDB_BUILD_GRPC=ON"
      ;;
    s)
      CMAKE_CALL="$CMAKE_CALL -DSKIMDB_BUILD_SIPER=ON"
      ;;
  esac
done


if [ -d build ]; then
  rm -rf build
fi

mkdir -p build/

if [ ! -d build ]; then
  echo "error: unable to create build folder"
  exit -1
fi

echo "Building $PROJECT..."
echo "cmake call: $CMAKE_CALL"

# seems to be required by macOS
export CMAKE_POLICY_VERSION_MINIMUM=3.5

cd build/
cmake $CMAKE_CALL -DCMAKE_INSTALL_PREFIX=$DIR

if [ -n "$BUILD_LOG" ]; then
  make -j $JOBS $VERBOSE 2>&1 | tee ../build.log
else
  make -j $JOBS $VERBOSE
fi

make install

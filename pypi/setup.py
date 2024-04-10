#!/usr/bin/env bash
set -e -o pipefail

mkdir -p ./build
cd ./build

echo "Hello from PyPi build.sh"

BUILD_DIR=`pwd`
N_CORES=`nproc`

#test -n "$PYTHON_VERSIONS" || { echo PYTHON_VERSIONS must be set.; exit 1; }

echo "copy start"

cp -r ../boolector .

echo "copy end"

# Setup dependencies
cd boolector
#/bin/sh ./contrib/setup-btor2tools.sh
#/bin/sh ./contrib/setup-cadical.sh
#/bin/sh ./contrib/setup-lingeling.sh
./contrib/setup-btor2tools.sh
./contrib/setup-cadical.sh
./contrib/setup-lingeling.sh

#********************************************************************
#* boolector
#********************************************************************
cd ${BUILD_DIR}

cd boolector

#./configure.sh --python --shared --prefix output
./configure.sh --python --shared --prefix /usr/local
cd build

echo "make"

make -j${N_CORES}

make install

#********************************************************************
#* pyboolector
#********************************************************************

cd ${BUILD_DIR}
rm -rf pyboolector

export CC=gcc
export CXX=g++

# Specify path to CmakeLists.txt so setup.py can extract the version
export CMAKELISTS_TXT=${BUILD_DIR}/boolector/CMakeLists.txt

cp -r ${BUILD_DIR}/boolector/pypi pyboolector

# Prepare the artifact directory.
rm -rf ${BUILD_DIR}/boolector/result
mkdir -p ${BUILD_DIR}/boolector/result

# Grab the main license file
cp ${BUILD_DIR}/boolector/COPYING pyboolector/LICENSE

cd pyboolector

#for py in $PYTHON_VERSIONS; do
#  python=$(ls /opt/python/${py}-*/bin/python)
#  echo "Python: ${python}"
#  ${python} -m pip install cython wheel
#  cd ${BUILD_DIR}/pyboolector
#  rm -rf src
#  cp -r ${BUILD_DIR}/boolector/src/api/python src
#  sed -i -e 's/override//g' \
#     -e 's/noexcept/_GLIBCXX_USE_NOEXCEPT/g' \
#     -e 's/\(BoolectorException (const.*\)/\1\n    virtual ~BoolectorException() _GLIBCXX_USE_NOEXCEPT {}/' \
#       src/pyboolector_abort.cpp
#  mkdir -p src/utils
#  cp ${BUILD_DIR}/boolector/src/*.h src
#  cp ${BUILD_DIR}/boolector/src/utils/*.h src/utils
#  $python ./src/mkenums.py ./src/btortypes.h ./src/pyboolector_enums.pxd
#  $python setup.py sdist bdist_wheel
#done

echo "python"

  python=/usr/bin/pypy
  echo "Python: ${python}"
  ${python} -m pip install cython wheel
  cd ${BUILD_DIR}/pyboolector
  rm -rf src
  cp -r ${BUILD_DIR}/boolector/src/api/python src
  sed -i -e 's/override//g' \
     -e 's/noexcept/_GLIBCXX_USE_NOEXCEPT/g' \
     -e 's/\(BoolectorException (const.*\)/\1\n    virtual ~BoolectorException() _GLIBCXX_USE_NOEXCEPT {}/' \
       src/pyboolector_abort.cpp
  mkdir -p src/utils
  cp ${BUILD_DIR}/boolector/src/*.h src
  cp ${BUILD_DIR}/boolector/src/utils/*.h src/utils
  $python ./src/mkenums.py ./src/btortypes.h ./src/pyboolector_enums.pxd
  $python setup.py sdist bdist_wheel

# Copy the source distribution into the artifact directory.
cp dist/*.tar.gz ${BUILD_DIR}/boolector/result

# Repair wheels and place them into the artifact directory.
for whl in dist/*.whl; do
  auditwheel repair --plat manylinux_2_24_x86_64 --wheel-dir ${BUILD_DIR}/boolector/result/dist $whl
done

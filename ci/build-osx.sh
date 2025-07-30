set -e
mkdir -p build
cd build

mkdir -p crash-handler

if [ -n "${ELECTRON_VERSION}" ]
then
    NODEJS_VERSION_PARAM="-DNODEJS_VERSION=${ELECTRON_VERSION}"
else
    NODEJS_VERSION_PARAM=""
fi

if [ -n "${ARCHITECTURE}" ]
then
    CMAKE_OSX_ARCHITECTURES_PARAM="-DCMAKE_OSX_ARCHITECTURES=${ARCHITECTURE}"
else
    CMAKE_OSX_ARCHITECTURES_PARAM=""
fi

cmake .. \
-DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
-DCMAKE_INSTALL_PREFIX=${PWD}/distribute/crash-handler \
${NODEJS_VERSION_PARAM} \
${CMAKE_OSX_ARCHITECTURES_PARAM} \
-DCMAKE_BUILD_TYPE=RelWithDebInfo

cd ..
ci/localization_prepare_binaries_osx.sh
cmake --build build --target install --config RelWithDebInfo
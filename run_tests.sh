#!/usr/bin/env sh

if [ ! -x build ]; then
    mkdir build
fi

cd build

CC="gcc" CXX="g++" OPENSSL_ROOT_DIR=/usr/lib/ssl cmake ..
make -j4

if [ $? -eq 0 ]; then
    if [ ! -x test/cert ]; then
        cp -r ../test/cert test/
    fi
    cd test
    ./tests
fi

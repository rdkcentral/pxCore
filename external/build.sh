#!/bin/bash
set -e
# Any subsequent(*) commands which fail will cause the shell script to exit immediately

banner() {
  msg="# $* #"
  edge=$(echo "$msg" | sed 's/./#/g')
  echo " "
  echo "$edge"
  echo "$msg"
  echo "$edge"
  echo " "
}

#--------- Args

OPENSSL_VER="1.1.1g"
CURL_VER="7.69.1"

make_parallel=3
if [ "$(uname)" = "Darwin" ]; then
    make_parallel="$(sysctl -n hw.ncpu)"
    LIBEXTN=dylib
elif [ "$(uname)" = "Linux" ]; then
    make_parallel="$(cat /proc/cpuinfo | grep '^processor' | wc --lines)"
    LIBEXTN=so
fi

EXT_INSTALL_PATH=`pwd`/extlibs
EXT_DIR=`pwd`

mkdir -p $EXT_INSTALL_PATH

if [ "$(uname)" = "Darwin" ]; then
    make_parallel="$(sysctl -n hw.ncpu)"
    LIBEXTN=dylib
elif [ "$(uname)" = "Linux" ]; then
    make_parallel="$(cat /proc/cpuinfo | grep '^processor' | wc --lines)"
    LIBEXTN=so
fi

cd ${EXT_DIR}
#--------- OPENSSL
OPENSSL_DIR="`pwd`/openssl-${OPENSSL_VER}"
if [ ! -e ${EXT_INSTALL_PATH}/lib/libcrypto.so.1.1 ] || [ ! -e ${EXT_INSTALL_PATH}/lib/libssl.so.1.1 ]
then

  cd ${OPENSSL_DIR}

  if [ "$(uname)" != "Darwin" ]
  then
    ./config -shared  --prefix=${EXT_INSTALL_PATH}
  else
    ./Configure darwin64-x86_64-cc -shared --prefix=${EXT_INSTALL_PATH}
  fi

  make clean
  make "-j${make_parallel}"
  make install -i

  rm -rf libcrypto.a
  rm -rf libssl.a
  rm -rf lib/libcrypto.a
  rm -rf lib/libssl.a
  cd ..
fi
#
##export LD_LIBRARY_PATH="${OPENSSL_DIR}/:$LD_LIBRARY_PATH"
##export DYLD_LIBRARY_PATH="${OPENSSL_DIR}/:$DYLD_LIBRARY_PATH"
export PKG_CONFIG_PATH=$EXT_INSTALL_PATH/lib/pkgconfig:$EXT_INSTALL_PATH/lib/x86_64-linux-gnu/pkgconfig
#
cd ${EXT_DIR}

#--------- CURL
CURL_DIR="`pwd`/curl-${CURL_VER}"
if [ ! -e $EXT_INSTALL_PATH/lib/libcurl.so.4.6.0 ]; then

  banner "CURL"

  cd ${CURL_DIR}

  #CPPFLAGS="-I${OPENSSL_DIR} -I${OPENSSL_DIR}/include" LDFLAGS="-L${OPENSSL_DIR}/lib -Wl,-rpath,${OPENSSL_DIR}/lib " LIBS="-ldl -lpthread" PKG_CONFIG_PATH=$EXT_INSTALL_PATH/lib/pkgconfig:$PKG_CONFIG_PATH ./configure --with-ssl="${OPENSSL_DIR}" --prefix=$EXT_INSTALL_PATH
  ./configure --prefix=$EXT_INSTALL_PATH --enable-versioned-symbols

  if [ "$(uname)" = "Darwin" ]; then
    #Removing api definition for Yosemite compatibility.
    sed -i '' '/#define HAVE_CLOCK_GETTIME_MONOTONIC 1/d' lib/curl_config.h
  fi

  make all "-j${make_parallel}"
  make install
  cd ..

fi

cd ${EXT_DIR}
#--------------------------------------------
banner ">>>>>  BUILD COMPLETE  <<<<<"
#--------------------------------------------

#-------
exit 0    #success
#-------

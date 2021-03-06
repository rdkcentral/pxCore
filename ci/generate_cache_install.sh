#!/bin/sh

travis_retry() {
  local result=0
  local count=1
  while [ $count -le 3 ]; do
    [ $result -ne 0 ] && {
      echo -e "\n$The command \"$@\" failed *******************. Retrying, $count of 3.\n" >&2
    }
    "$@"
    result=$?
    [ $result -eq 0 ] && break
    count=$(($count + 1))
    sleep 1
  done

  [ $count -gt 3 ] && {
    echo -e "\n$The command \"$@\" failed 3 times *******************.\n" >&2
  }

  return $result
}

#install necessary basic packages for linux and mac 
if [ "$TRAVIS_OS_NAME" = "linux" ] ; 
then 
  travis_retry sudo apt-get update
  travis_retry sudo apt-get install git libglew-dev freeglut3 freeglut3-dev libgcrypt11-dev zlib1g-dev g++ nasm autoconf libyaml-dev cmake gdb quilt libuv-dev xmlto
fi

if [ "$TRAVIS_OS_NAME" = "osx" ] ;
then
  brew update;
  brew install yasm bison flex python
  ln -sf /usr/local/opt/bison/bin/bison /usr/local/bin/bison
  brew upgrade cmake;
  brew install quilt libuv xmlto pkg-config glfw3 glew;
  sudo /usr/sbin/DevToolsSecurity --enable
fi

if [ "$TRAVIS_OS_NAME" = "osx" ] ; 
then
    brew install --HEAD ccache
    ls -al $HOME/.ccache
fi
exit 0;

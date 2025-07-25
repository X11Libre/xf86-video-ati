#!/bin/bash

set -e
set -o xtrace

echo 'deb-src https://deb.debian.org/debian bookworm main' >/etc/apt/sources.list.d/deb-src.list
apt-get update


# Ephemeral packages (installed for this script and removed again at the end)
EPHEMERAL="
	ca-certificates
	git
	"

# libXfont/xserver build dependencies
apt-get install -y --no-remove \
	autoconf \
	automake \
	build-essential \
	libtool \
	pkg-config \
	$EPHEMERAL

echo 'APT::Get::Build-Dep-Automatic "true";' >>/etc/apt/apt.conf
apt-get build-dep -y xorg-server

# xserver 1.18 and older branches require libXfont 1.5 instead of 2.0
git clone https://gitlab.freedesktop.org/xorg/lib/libXfont.git
cd libXfont
git checkout libXfont-1.5-branch
./autogen.sh
make install-pkgconfigDATA
cd ..
rm -rf libXfont


git clone https://gitlab.freedesktop.org/xorg/xserver.git
cd xserver

for VERSION in 1.13 1.14 1.15; do
    git checkout server-${VERSION}-branch
    # Workaround glvnd having reset the version in gl.pc from what Mesa used
    # similar to xserver commit e6ef2b12404dfec7f23592a3524d2a63d9d25802
    sed -i -e 's/gl >= [79].[12].0/gl >= 1.2/' configure.ac
    ./autogen.sh --prefix=/usr/local/xserver-$VERSION --enable-dri2 --disable-dmx
    make -C include install-nodist_sdkHEADERS
    make install-headers install-aclocalDATA install-pkgconfigDATA clean
    git restore configure.ac
done

for VERSION in 1.16 1.17 1.18 1.19 1.20 21.1; do
    git checkout server-${VERSION}-branch
    # Workaround glvnd having reset the version in gl.pc from what Mesa used
    # similar to xserver commit e6ef2b12404dfec7f23592a3524d2a63d9d25802
    sed -i -e 's/gl >= [79].[12].0/gl >= 1.2/' configure.ac
    ./autogen.sh --prefix=/usr/local/xserver-$VERSION --enable-dri2 --enable-dri3 --enable-glamor --disable-dmx
    make -C include install-nodist_sdkHEADERS
    make install-headers install-aclocalDATA install-pkgconfigDATA clean
    git restore configure.ac
done

cd ..
rm -rf xserver


# xf86-video-ati build dependencies
apt-get install -y --no-remove \
	clang \
	libdrm-dev \
	libgbm-dev \
	libgl1-mesa-dev \
	libpciaccess-dev \
	libpixman-1-dev \
	libudev-dev \
	libxcvt-dev \
	mesa-common-dev \
	xutils-dev \
	x11proto-dev


# Remove unneeded packages
apt-get purge -y $EPHEMERAL
apt-get autoremove -y --purge

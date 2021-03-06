#!/bin/sh -xe

TRAVIS_CI="$1"

apt-get -qq update
apt-get -y install sudo

source /libiio/CI/travis/before_install_linux default

# Check we're in Travis-CI; the only place where this context is valid
# It makes sure, users won't shoot themselves in the foot while running this script
if [ "$TRAVIS_CI" == "travis-ci" ] ; then
	mkdir -p /libiio/build
	cd /libiio/build
else
	mkdir -p build
	cd build
fi

cmake -DENABLE_PACKAGING=ON -DDEB_DETECT_DEPENDENCIES=ON ..
make
make package

dpkg -i libiio*.deb
# need to find this out inside the container
. /libiio/CI/travis/get_ldist
echo "$(get_ldist)" > /libiio/build/.LDIST

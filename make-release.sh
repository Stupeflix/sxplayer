#!/bin/sh

if [ $# -ne 1 ]; then
	echo "Usage $0 <major.minor.micro>"
	exit 1
fi

extract(){
	echo $1 | cut -d. -f $2
}
VERSION=$1
MAJOR=$(extract $VERSION 1)
MINOR=$(extract $VERSION 2)
MICRO=$(extract $VERSION 3)

sed "s/^Version: .*/Version: $VERSION/" -i libsxplayer.pc.tpl
sed -e "s/^#define SXPLAYER_VERSION_MAJOR.*/#define SXPLAYER_VERSION_MAJOR $MAJOR/" \
    -e "s/^#define SXPLAYER_VERSION_MINOR.*/#define SXPLAYER_VERSION_MINOR $MINOR/" \
    -e "s/^#define SXPLAYER_VERSION_MICRO.*/#define SXPLAYER_VERSION_MICRO $MICRO/" \
    -i sxplayer.h

git commit -a --allow-empty -m "Release $VERSION"
git tag "v$VERSION"

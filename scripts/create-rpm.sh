#!/usr/bin/env bash

set -e
set -x

SPEC=$1
if [ "${SPEC}" = "" ] ; then
	echo "usage: $0 <spec file>"
	exit 1
fi

NAME=$(grep ^Name "${SPEC}" | awk '{print $2}')
VERSION=$(grep ^Version "${SPEC}" | awk '{print $2}')

rpmdev-setuptree

git archive --format=tar.gz --prefix "${NAME}-${VERSION}/" HEAD > "${NAME}-${VERSION}.tar.gz"
grep ^Source "${SPEC}" | awk '{print $2}' | while read -r SOURCE ; do
	SOURCE=$(echo "${SOURCE}" | sed "s/%{name}/${NAME}"/ | sed "s/%{version}/${VERSION}/")
	cp "${SOURCE}" "${HOME}/rpmbuild/SOURCES/"
done
rm -f "${NAME}-${VERSION}.tar.gz"

rpmbuild -ba "${SPEC}"

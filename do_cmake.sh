#!/bin/sh -x
git submodule update --init --recursive

if which ccache ; then
    echo "enabling ccache"
    ARGS="$ARGS -DWITH_CCACHE=ON"
fi

cd build
NPROC=${NPROC:-$(nproc)}
cmake -DBOOST_J=$NPROC $ARGS "$@" ..

# minimal config to find plugins
cat <<EOF > ceph.conf
plugin dir = lib
erasure code dir = lib
EOF

echo done.

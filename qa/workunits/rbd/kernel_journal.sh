#!/usr/bin/env bash
set -e

. $(dirname $0)/../../standalone/ceph-helpers.sh

function list_tests()
{
  echo "AVAILABLE TESTS"
  for i in $TESTS; do
    echo "  $i"
  done
}

function usage()
{
  echo "usage: $0 [-h|-l|-t <testname> [-t <testname>...] [--no-cleanup]]"
}

function expect_false()
{
    set -x
    if "$@"; then return 1; else return 0; fi
}

function save_commit_position()
{
    local journal=$1

    rados -p rbd getomapval journal.${journal} client_ \
	  $TMPDIR/${journal}.client_.omap
}

function restore_commit_position()
{
    local journal=$1

    rados -p rbd setomapval journal.${journal} client_ \
	  < $TMPDIR/${journal}.client_.omap
}

test_replay_journal()
{
    local image=testrbdjournal$$

    rbd create --image-feature exclusive-lock --image-feature journaling \
	--size 128 ${image}
    local journal=$(rbd info ${image} --format=xml 2>/dev/null |
			   $XMLSTARLET sel -t -v "//image/journal")

    local count=10
    save_commit_position ${journal}
    rbd bench-write ${image} --io-size 4096 --io-threads 1 \
	--io-total $((4096 * count)) --io-pattern seq

    dev=$(sudo rbd map --exclusive ${image})
    blkdiscard -z -o 156672 -l 512 ${dev}
    blkdiscard -o 0 -l 512 ${dev}
    sudo rbd unmap ${image}

    rbd journal status --image ${image} | fgrep "tid=$((count - 1))"
    restore_commit_position ${journal}
    rbd journal status --image ${image} | fgrep "positions=[]"

    rbd journal export ${journal} $TMPDIR/journal.export
    rbd export ${image} $TMPDIR/${image}.export

    local image1=${image}1
    rbd create --image-feature exclusive-lock --image-feature journaling \
	--size 128 ${image1}
    journal1=$(rbd info ${image1} --format=xml 2>/dev/null |
		      $XMLSTARLET sel -t -v "//image/journal")

    save_commit_position ${journal1}
    rbd journal import --dest ${image1} $TMPDIR/journal.export
    sudo rbd map --exclusive ${image1}
    sudo rbd unmap ${image1}
    rbd export ${image1} $TMPDIR/${image1}.export
    cmp $TMPDIR/${image}.export $TMPDIR/${image1}.export

    rm $TMPDIR/${image}.export
    rm $TMPDIR/${image1}.export
    rm $TMPDIR/journal.export
    rbd remove ${image1}
    rbd remove ${image}
}

test_replay_two_tags()
{
    local image=testrbdjournal$$

    rbd create --image-feature exclusive-lock --image-feature journaling \
	--size 128 ${image}
    local journal=$(rbd info ${image} --format=xml 2>/dev/null |
			   $XMLSTARLET sel -t -v "//image/journal")

    local count=10
    save_commit_position ${journal}
    rbd bench-write ${image} --io-size 4096 --io-threads 1 \
	--io-total $((4096 * count)) --io-pattern seq
    rbd bench-write ${image} --io-size 4096 --io-threads 1 \
	--io-total $((4096 * count)) --io-pattern rand
    rbd journal status --image ${image} | fgrep "tid=$((count - 1))"
    restore_commit_position ${journal}
    rbd journal status --image ${image} | fgrep "positions=[]"

    rbd journal export ${journal} $TMPDIR/journal.export
    rbd export ${image} $TMPDIR/${image}.export

    local image1=${image}1
    rbd create --image-feature exclusive-lock --image-feature journaling \
	--size 128 ${image1}
    journal1=$(rbd info ${image1} --format=xml 2>/dev/null |
		      $XMLSTARLET sel -t -v "//image/journal")

    save_commit_position ${journal1}
    rbd journal import --dest ${image1} $TMPDIR/journal.export
    sudo rbd map --exclusive ${image1}
    sudo rbd unmap ${image1}
    rbd export ${image1} $TMPDIR/${image1}.export
    cmp $TMPDIR/${image}.export $TMPDIR/${image1}.export

    rm $TMPDIR/${image}.export
    rm $TMPDIR/${image1}.export
    rm $TMPDIR/journal.export
    rbd remove ${image1}
    rbd remove ${image}
}

TESTS+=" replay_journal"
TESTS+=" replay_two_tags"

#
# "main" follows
#

tests_to_run=()

cleanup=true

while [[ $# -gt 0 ]]; do
    opt=$1

    case "$opt" in
	"-l" )
	    do_list=1
	    ;;
	"--no-cleanup" )
	    cleanup=false
	    ;;
	"-t" )
	    shift
	    if [[ -z "$1" ]]; then
		echo "missing argument to '-t'"
		usage ;
		exit 1
	    fi
	    tests_to_run+=" $1"
	    ;;
	"-h" )
	    usage ;
	    exit 0
	    ;;
    esac
    shift
done

if [[ $do_list -eq 1 ]]; then
    list_tests ;
    exit 0
fi

TMPDIR=/tmp/rbd_journal$$
mkdir $TMPDIR
if $cleanup; then
    trap "rm -fr $TMPDIR" 0
fi

if test -z "$tests_to_run" ; then
    tests_to_run="$TESTS"
fi

for i in $tests_to_run; do
    set -x
    test_${i}
    set +x
done

echo OK

#!/bin/sh -ex

MB=1048576
twoMB=$((2*MB))

rm -r layout_test || true
rm new_layout || true
rm file2_layout || true
rm temp || true

echo "layout.data_pool:     0
layout.object_size:   1048576
layout.stripe_unit:   1048576
layout.stripe_count:  1
layout.preferred_osd: -1" > new_layout
echo "layout.data_pool:     0
layout.object_size:   2097152
layout.stripe_unit:   1048576
layout.stripe_count:  2
layout.preferred_osd: -1" > file2_layout

mkdir layout_test
cephfs layout_test show_layout
cephfs layout_test set_layout -u $MB -c 1 -s $MB
touch layout_test/file1
cephfs layout_test/file1 show_layout > temp
diff new_layout temp || return 1
`echo "hello, I'm a file" > layout_test/file1`
cephfs layout_test/file1 show_layout > temp
diff new_layout temp || return 1
touch layout_test/file2
cephfs layout_test/file2 show_layout > temp
diff new_layout temp || return 1
cephfs layout_test/file2 set_layout -u $MB -c 2 -s $twoMB
cephfs layout_test/file2 show_layout > temp
diff file2_layout temp || return 1

echo "hello, I'm a file with a custom layout" > layout_test/file2
sync
echo "OK"

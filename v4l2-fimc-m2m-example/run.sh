#!/bin/bash

rotations=(360 90 180 270)
flips=(1 2)
formats=(420 422 565 888)
src_width=208
src_height=160
dst_width=240
dst_height=208
vidnode=0
num_passed=0
num_failed=0

fbset -fb /dev/fb0 -depth 24
cat /dev/zero > /dev/fb0

# args: format
function run_test() {
	local formatname
	formatname=${formats[$1]}
	echo "Running test for format: $formatname"
	./fimc-m2m-f02 "-d$vidnode" "-iin${formatname}_${src_width}_${src_height}.raw" "-f$1" "-g208x160" "-p0" &

	./fimc-m2m-f02 "-d$vidnode" "-iin${formatname}_${src_width}_${src_height}.raw" "-f$1" "-g208x160" "-p1"

}

format_id=(0 1 2 3)
for format in "${format_id[@]}"; do
    cat /dev/zero > /dev/fb0
    run_test $format
    sleep 1
done

echo Overall test results
echo Passed: "$num_passed"
echo Failed: "$num_failed"


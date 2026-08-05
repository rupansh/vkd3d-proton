#!/usr/bin/env bash

# arg variable init
output_dir=
run_stress=

# Avoids a race condition in test_object_interface
# since a PSO might be held alive a little too long in the disk thread.
# It's very flaky and best to just avoid it.
export VKD3D_SHADER_CACHE_PATH=0

args=()
excludes=()
# by default run one job per cpu thread
# /proc/cpuinfo does not exist under a native Windows bash (Git for Windows,
# MSYS2), where this would silently evaluate to 0 and the run loop below would
# then never start a single test while reporting "0 / N complete" forever.
nr_cpus=$(grep -w processor /proc/cpuinfo 2>/dev/null | wc -l)
if [ "$nr_cpus" -lt 1 ] 2>/dev/null || [ -z "$nr_cpus" ] ; then
	nr_cpus=$(nproc 2>/dev/null || echo 1)
fi
while [[ $# -gt 0 ]]; do
	case $1 in
	-o|--output-dir)
		# this outputs the logs for each test to a separate file
		output_dir="$2"
		shift
		shift
		;;
	-s|--run-stress-tests)
		# stress tests take a long time to run and are disabled by default
		run_stress=1
		shift
		;;
	-j|--jobs)
		# override the default job count
		nr_cpus=$2
		shift
		shift
		;;
	-x|--exclude)
		# skip one test by exact name. Repeatable. Every exclusion is echoed
		# below, because a silently skipped test is indistinguishable from a
		# passing one in the summary.
		excludes+=("$2")
		shift
		shift
		;;
	-h|--help)
		echo "./test-runner.sh [-o|--output-dir logfile_dir] [-s|--run-stress-tests] [-j|--jobs N] [-x|--exclude test_name]... path/to/tests/d3d12"
		exit 1
		;;
	-*|--*)
		echo "unknown arg $1"
		;;
	*)
		args+=("$1")
		shift
		;;
	esac
done

set -- "${args[@]}"
d3d12_bin="$1"

if [[ -z $d3d12_bin || ! -f "$d3d12_bin" ]] ; then
	echo "Must specify path to valid d3d12 test exe!"
	exit 1
fi

# Strip CR: the test binary's stdout is CRLF-terminated when it runs natively on
# Windows, and mapfile -t removes only the LF. A trailing CR on every name makes
# VKD3D_TEST_MATCH match nothing, so each invocation runs only the handful of
# unconditional tests and reports "3 tests executed (0 failures)" -- and the
# runner then prints ALL PASSED! for a run that executed almost none of the
# suite. It also embeds a CR in every log filename.
mapfile -t tests < <("$d3d12_bin" --list-tests | tr -d '\r')

if [[ -z $run_stress ]] ; then
	compacted=()
	for t in "${tests[@]}" ; do
		if [[ "$t" != *stress* ]] ; then
			compacted+=($t)
		fi
	done
	tests=(${compacted[@]})
fi

# -x/--exclude, applied loudly: an exclusion that is not printed is a lie in the
# summary, and an -x for a test that does not exist is a typo that would
# otherwise silently do nothing.
if [[ ${#excludes[@]} -gt 0 ]] ; then
	kept=()
	for t in "${tests[@]}" ; do
		drop=
		for x in "${excludes[@]}" ; do
			if [[ "$t" == "$x" ]] ; then drop=1 ; fi
		done
		if [[ -n $drop ]] ; then
			echo "EXCLUDED $t"
		else
			kept+=($t)
		fi
	done
	for x in "${excludes[@]}" ; do
		found=
		for t in "${tests[@]}" ; do
			if [[ "$t" == "$x" ]] ; then found=1 ; fi
		done
		if [[ -z $found ]] ; then echo "WARNING: --exclude $x matched no test" ; fi
	done
	tests=(${kept[@]})
fi

# runtime variable init
nr_tests=${#tests[@]}
if [ $nr_tests -lt 1 ] ; then
	echo "No tests detected! Is $d3d12_bin a valid test exe?"
	exit 1
fi

pids=()
# the counter for the number of tests running
counter=0
# the index for the next test to run
test_idx=0

if [[ -n $output_dir ]] ; then
	mkdir -p "$output_dir"
fi

# start test processes until $nr_cpus test processes are running
run_tests() {
	while (($counter < $nr_cpus)) ; do
		# output to /dev/null by default
		echo "Running ${tests[$test_idx]} ..."
		if [[ -z "$output_dir" ]] ; then
			VKD3D_TEST_MATCH=${tests[$test_idx]} "$d3d12_bin" &>/dev/null &
		else
			VKD3D_TEST_MATCH=${tests[$test_idx]} "$d3d12_bin" &> "$output_dir/${tests[$test_idx]}.log" &
		fi
		# capture pid of subprocess
		pids[$test_idx]=${!}
		#increment running test counter and test index
		counter=$((counter+1))
		test_idx=$((test_idx+1))
	done
}


echo "Running $nr_tests vkd3d-proton D3D12 tests..."
r=0
fails=()
last_notify=$(date +%s)
while (($r<$nr_tests)) ; do
	# run tests if there are more tests to run
	if [ $test_idx -lt $nr_tests ] ; then
		run_tests
	fi
	# tests are active: wait for one to finish, store pid to $finished
	wait -n -p finished &>/dev/null
	retval=$?
	pid_count=${#pids[@]}
	# iterate $pids array to find the index matching $finished
	for ((c=0; c < $pid_count; c++)) ; do
		if [[ ${pids[$c]} == $finished ]] ; then
			# failed/crashed tests have $retval!=0
			if [[ $retval != 0 ]] ; then
				# print failure immediately and store for summary
				fails+=(${tests[$c]})
				echo "FAILED ${tests[$c]}"
			fi
			# increment the "done" counter
			r=$((r+1))
			# decrement the "running" counter
			counter=$((counter-1))
			break
		fi
	done
	cur_time=$(date +%s)
	# only notify every 5s
	if [ $((last_notify+5)) -lt $cur_time ] ; then
		echo "$r / $nr_tests complete (${#fails[@]} failures)..."
		last_notify=$cur_time
	fi
done


# nice summary output at the end
echo "***********************"
echo "Finished in ${SECONDS}s!"

if [[ "${#fails[@]}" != 0 ]] ; then
	echo "${#fails[@]} FAILURES: (run with 'VKD3D_TEST_MATCH=<name> $d3d12_bin')"
	for fail in "${fails[@]}" ; do
		echo "$fail"
	done
else
	echo "ALL PASSED!"
fi

#!/usr/local/bin/bash
. aurora.config
. helpers/util.sh

AURORACTL=$SRCROOT/tools/slsctl/slsctl
#rocksdb_dir=/mnt/treesls/testmnt
rocksdb_dir=/rocksdbmnt

db_bench_origin() {
    cd dependencies/rocksdb
    $1/db_bench \
	--benchmarks=fillbatch,mixgraph \
	--use_direct_io_for_flush_and_compaction=true \
	--use_direct_reads=true \
	--cache_size=$((256 << 20)) \
	--key_dist_a=0.002312 \
	--key_dist_b=0.3467 \
	--keyrange_dist_a=14.18 \
	--keyrange_dist_b=0.3467 \
	--keyrange_dist_c=0.0164 \
	--keyrange_dist_d=-0.08082 \
	--keyrange_num=30 \
	--value_k=0.2615 \
	--value_sigma=25.45 \
	--iter_k=2.517 \
	--iter_sigma=14.236 \
	--mix_get_ratio=0.83 \
	--mix_put_ratio=0.14 \
	--mix_seek_ratio=0.03 \
	--sine_mix_rate_interval_milliseconds=5000 \
	--sine_a=1000 \
	--sine_b=0.000073 \
	--sine_d=4500 \
	--perf_level=2 \
	--num=$ROCKSDB_NUM \
	--key_size=48 \
	--db=$rocksdb_dir/tmp-db \
	--wal_dir=$rocksdb_dir/wal \
	--duration=$ROCKSDB_DUR \
	--histogram=1 \
	--write_buffer_size=$((16 << 30)) \
	--disable_auto_compactions \
	--threads=24 \
	"${@:2}"
    cd -
    return $?
}


db_bench() {
    cd dependencies/rocksdb
    $1/db_bench \
	--benchmarks=fillbatch,mixgraph \
	--key_dist_a=0.002312 \
	--key_dist_b=0.3467 \
	--keyrange_dist_a=14.18 \
	--keyrange_dist_b=0.3467 \
	--keyrange_dist_c=0.0164 \
	--keyrange_dist_d=-0.08082 \
	--keyrange_num=30 \
	--value_k=0.2615 \
	--value_sigma=25.45 \
	--iter_k=2.517 \
	--iter_sigma=14.236 \
	--mix_get_ratio=0.83 \
	--mix_put_ratio=0.14 \
	--mix_seek_ratio=0.03 \
	--sine_mix_rate_interval_milliseconds=5000 \
	--sine_a=1000 \
	--sine_b=0.000073 \
	--sine_d=4500 \
	--perf_level=2 \
	--key_size=48 \
	--db=$rocksdb_dir/tmp-db \
	--wal_dir=$rocksdb_dir/wal \
	--duration=$ROCKSDB_DUR \
	--num=$ROCKSDB_NUM \
	--histogram=1 \
	--cache_size=$((256 << 20)) \
	--write_buffer_size=$((16 << 30)) \
	--compression_type=none \
	--threads=1 \
	"${@:2}"
    cd -
    return $?
}

run_base_wal()
{
    DIR=$OUT/rocksdb/base-wal
    mkdir -p $DIR
    for ITER in `seq 0 $MAX_ITER`
    do
#	if check_completed "$DIR/$ITER.out"; then
#	    continue
#	fi
	echo "[Aurora] Running Rocksdb Baseline: WAL, Iteration $ITER"
	#setup_zfs_rocksdb >> $LOG 2>> $LOG
	
	stripe_setup_wal $MIN_FREQ
	db_bench baseline --sync=true --disable_wal=false > /tmp/out
	#teardown_zfs >> $LOG 2>> $LOG
	mv /tmp/out $DIR/$ITER.out
	fsync $DIR/$ITER.out
    done
}

run_base_nowal()
{
    DIR=$OUT/rocksdb/base-nowal
    mkdir -p $DIR
    for ITER in `seq 0 $MAX_ITER`
    do

	echo "[Aurora] Running Rocksdb Baseline: No WAL, Iteration $ITER"
	setup_zfs_rocksdb >> $LOG 2>> $LOG
	db_bench baseline --sync=false --disable_wal=true > /tmp/out
	teardown_zfs
	mv /tmp/out $DIR/$ITER.out
	fsync $DIR/$ITER.out
    done
}

stripe_setup_wal()
{
    CKPT_FREQ=$1
    createmd
    newfs -j -S 4096 -b 65536 $DISKPATH
    mkdir -p $rocksdb_dir
    mount $DISKPATH $rocksdb_dir
}

stripe_setup_wal_old()
{
    CKPT_FREQ=$1

    gstripe load > /dev/null 2> /dev/null
    gstripe stop "$STRIPENAME" > /dev/null 2> /dev/null
    gstripe stop "st1" > /dev/null 2> /dev/null

    # Sets up the two stripes needed for the RocksDB Aurora benchmark 
    # STRIPENAME is the default stripe used by all benchmarks which is used by the SLS and SLOS
    # The secondary stripe "st1" is used for the persistent storage for the WAL.
    # During operation operatiosn are written to the WAL (which is on st1), when this wal fills, Aurora
    set -- $ROCKS_STRIPE1
    if [ $# -gt 1 ]; then
	gstripe create -s "$STRIPESIZE" -v "$STRIPENAME" $ROCKS_STRIPE1
	DISK="stripe/$STRIPENAME"
    else
	if [ "$MODE" != "VM" ]; then
		MAX_ITER=0
		MIN_FREQ=10
	fi
	echo "[Aurora] Single device detected ($ROCKS_STRIPE1) Reducing period ($MIN_FREQ ms)"
	DISK=$ROCKS_STRIPE1
	# We are using 1 disk so we cannot keep up with checkpointing at 100Hz
    fi
    DISKPATH="/dev/$DISK"

    set -- $ROCKS_STRIPE2
    if [ $# -gt 1 ]; then
	gstripe create -s "$STRIPESIZE" -v "st1" $ROCKS_STRIPE2
	ln -s /dev/stripe/st1 /dev/wal
    else
	ln -s /dev/$ROCKS_STRIPE2 /dev/wal
    fi


    aursetup
    if [ -z "$CKPT_FREQ" ]; then
	    sysctl aurora_slos.checkpointtime=$CKPT_FREQ > /dev/null
    else
	    sysctl aurora_slos.checkpointtime=$MAX_FREQ > /dev/null
    fi

}

stripe_teardown_wal()
{
    umount /testmnt/dev > /dev/null 2> /dev/null

    aurteardown 2> /dev/null
    aurunstripe 2> /dev/null
    gstripe destroy "st1" 2> /dev/null
    rm /dev/wal
}

run_aurora_nowal()
{
    DIR=$OUT/rocksdb/aurora-nowal
    mkdir -p $DIR

    for ITER in `seq 0 $MAX_ITER`
    do
	#if check_completed "$DIR/$ITER.out"; then
	#    continue
	#fi

	echo "[Aurora] Running Rocksdb SLS: No WAL, Iteration $ITER ($MIN_FREQ)"
	rm /tmp/out 2> /dev/null > /dev/null
	stripe_setup_wal $MIN_FREQ >> $LOG 2>> $LOG
	setup_aurora
	$AURORACTL partadd -o 1 -d -t $MIN_FREQ -b $BACKEND >> $LOG 2>> $LOG

	db_bench baseline --sync=false --disable_wal=true 2>&1 | tee /tmp/out &
	FUNC_PID="$!"
	sleep 1

	pid=`pidof db_bench`
	$AURORACTL attach -o 1 -p $pid 2>> $LOG >> $LOG
	$AURORACTL checkpoint -o 1 -r >> $LOG 2>> $LOG

	wait $FUNC_PID
	if [ $? -eq 124 ];then
		echo "[Aurora] Issue with db_bench, restart required"
		exit 1
	fi
	sleep 2

	stripe_teardown_wal >> $LOG 2>> $LOG
	teardown_aurora
	mv /tmp/out $DIR/$ITER.out
	fsync $DIR/$ITER.out
	#stripe_teardown_wal >> $LOG 2>> $LOG
    done
}

run_aurora_wal()
{
    DIR=$OUT/rocksdb/aurora-wal
    stripe_teardown_wal > /dev/null 2> /dev/null
    mkdir -p $DIR
    for ITER in `seq 0 $MAX_ITER`
    do
#	if check_completed "$DIR/$ITER.out"; then
#		continue
#	fi

	# We need custom stripes for the WAL as we use a seperate stripe to directly write to for the WAL
	echo "[Aurora] Running Rocksdb SLS: WAL, Iteration $ITER"
	stripe_setup_wal $MIN_FREQ
	
	db_bench sls --sync=true --disable_wal=false 2>&1 | tee /tmp/out

	# Wait for the final checkpoint to be done

	stripe_teardown_wal

	teardown_aurora >> $LOG 2>> $LOG
	mv /tmp/out $DIR/$ITER.out
	fsync $DIR/$ITER.out
    done
}

setup_script
clear_log
if [ "$MODE" = "VM" ]; then
	MAX_ITER=2
else
	MAX_ITER=2
fi

set -- $ROCKS_STRIPE1
if [ $# -eq 0 ]; then
	echo "[Aurora] RocksDB Requires at least 1 disk set (Virtual or otherwise) for ROCKS_STRIPE1"
	exit 1
fi

set -- $ROCKS_STRIPE2
if [ $# -eq 0 ]; then
	echo "[Aurora] RocksDB Requires at least 1 disk set (Virtual or
	otherwise) for ROCKS_STRIPE2 which is different from ROCKS_STRIPE1" 
	exit 1
fi


echo "[Aurora] Running with $MAX_ITER iterations"

mkdir -p $OUT/rocksdb

set -x

run_base_wal

run_base_nowal

run_aurora_wal

run_aurora_nowal

#echo "[Aurora] Creating RocksDB Graphs"
# PYTHONPATH=$PYTHONPATH:$(pwd)/dependencies/progbg
# export PYTHONPATH
# export OUT

# python3.7 -m progbg --debug graphing/fig5.py

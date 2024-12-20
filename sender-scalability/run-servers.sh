#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh
export HRD_REGISTRY_IP="192.168.1.6"

drop_shm

blue "Reset server QP registry"
sudo pkill memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting server"

flags="
	--dual_port 0 \
  --use_uc 0 \
	--is_client 0 \
	--size 64 \
	--run_time 1000 \
	--do_read 1 \
	--use_xrc 1 \
	--test_lat 1
"

# Check for non-gdb mode
if [ "$#" -eq 0 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 ../build/sender-scalability $flags
fi

# Check for gdb mode
if [ "$#" -eq 1 ]; then
  sudo -E gdb -ex start --args ../build/sender-scalability $flags
fi

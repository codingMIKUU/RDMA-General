#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh
export HRD_REGISTRY_IP="192.168.1.5"
export MLX5_SINGLE_THREADED=0
drop_shm

blue "Reset server QP registry"
sudo pkill memcached

while pgrep memcached >/dev/null; do
  sleep 0.1
done
# 比如：
memcached -u nobody -l 192.168.1.5 -m 1024 -c 65535 -t 4 1>/dev/null 2>/dev/null &
sleep 1


blue "Starting server"

flags="
	--dual_port 0 \
  --use_uc 0 \
	--is_client 0 \
	--size 8192 \
	--run_time 1000 \
	--do_read 0 \
	--use_xrc 0 \
	--test_lat 0 \
	--use_srm 0 \
	--test_lat_thread 1 \
	--use_smart_rc 1

"

# Check for non-gdb mode
if [ "$#" -eq 0 ]; then
  sudo -E numactl --cpunodebind=0 --membind=0 ../build/sender-scalability $flags
fi

# Check for gdb mode
if [ "$#" -eq 1 ]; then
  sudo -E gdb -ex start --args ../build/sender-scalability $flags
fi

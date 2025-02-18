#!/usr/bin/env bash

# # 设置自定义RDMA库的路径
# CUSTOM_RDMA_LIB_PATH="/home/dell/lyz/rdma-core-master/build/lib"

# # 设置LD_LIBRARY_PATH环境变量
# export LD_LIBRARY_PATH=$CUSTOM_RDMA_LIB_PATH:$LD_LIBRARY_PATH

cd ..
cd ./build

#需要在CMakeLists.txt里添加路径
make

# 检查库依赖，确保链接到自定义的RDMA库
echo "Checking library dependencies for run-servers.sh:"
ldd ~/zxm/RDMA-General/build/sender-scalability

cd ..
cd ./sender-scalability


# systemctl stop memcached
# systemctl start memcached

# 运行run-servers.sh脚本
bash run-servers.sh
export LD_LIBRARY_PATH=/home/dell/zxm/rdma-core/build/lib:$LD_LIBRARY_PATH
gcc srm_server.c -g -o srm_server -I/home/dell/zxm/rdma-core/build/include -L/home/dell/zxm/rdma-core/build/lib -libverbs
./srm_server
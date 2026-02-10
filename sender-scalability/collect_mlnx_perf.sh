#!/usr/bin/env bash
mlnx_perf -i ens1f0np0 -t 0.1 | grep 'tx_vport_rdma_unicast_bytes' > /root/zxm/RDMA-General/sender-scalability/tx_bytes.txt
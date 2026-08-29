#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
static const char* XRCD_FILE_PATH = "/tmp/server_xrcd";

void print_qp_info(struct ibv_qp_info *info) {
    printf("QP Number: %u\n", info->qpn);
    printf("GID: ");
    for (int i = 0; i < 16; ++i) {
        printf("%02x", info->gid.raw[i]);
    }
    printf("\n");
}
int get_rdma_attrs(struct ibv_context** ibv_ctx, uint8_t *port_id,union ibv_gid *gid){
    int num_devices = 0;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if(dev_list == NULL){
        printf("Failed to get InfiniBand device list\n");
        return -1;
    }
    // Traverse the device list
    int ports_to_discover = 0;
    for (int dev_i = 0; dev_i < num_devices; dev_i++) {
        struct ibv_context* ib_ctx = ibv_open_device(dev_list[dev_i]);
        if(ib_ctx == NULL){
            printf("Failed to open dev\n");
            return -1;
        }
        struct ibv_device_attr device_attr;
        memset(&device_attr, 0, sizeof(device_attr));

        if (ibv_query_device(ib_ctx, &device_attr) != 0) {
            printf(" Failed to query InfiniBand device \n");
            continue;
        }

        for (uint8_t port_i = 1; port_i <= device_attr.phys_port_cnt; port_i++) {
            // Count this port only if it is enabled
            struct ibv_port_attr port_attr;
            if (ibv_query_port(ib_ctx, port_i, &port_attr) != 0) {
                printf(" Failed to query InfiniBand port \n");
                continue;
            }

            if (port_attr.phys_state != IBV_PORT_ACTIVE &&
                port_attr.phys_state != IBV_PORT_ACTIVE_DEFER) {
                continue;
            }

            if (ports_to_discover == 0) {

                *ibv_ctx = ib_ctx;
                *port_id = port_i;
                // Resolve and cache the ibv_gid struct for RoCE
                int ret = ibv_query_gid(ib_ctx, *port_id, 0, gid);
                if(ret){
                    printf("Failed to query GID\n");
                    return -1;
                }
                return 0;
            }
        }
    }
    printf("get_rdma_attrs failed\n");
    return -1;
}

struct ibv_qp* create_xrc_tgt(struct ibv_pd *pd,struct ibv_xrcd *xrcd,struct ibv_qp_info *local_qp_info,struct ibv_qp_info *remote_qp_info){
    struct ibv_qp_init_attr_ex create_attr;
    memset(&create_attr, 0, sizeof(struct ibv_qp_init_attr_ex));
    struct ibv_qp *qp;
    struct ibv_qp_info rqi;

    int cq_depth = 256;
    int sq_depth = 256;
    int rq_depth = 256;
    int psn = 3185;
    int max_rd_atomic = 16;
    struct ibv_cq *cq = ibv_create_cq(pd->context,cq_depth,NULL,NULL,0);
    //create XRC TGT QP
    // create_attr.qp_type = IBV_QPT_XRC_RECV;
    // create_attr.comp_mask = IBV_QP_INIT_ATTR_XRCD;
    // create_attr.xrcd = xrcd;

    create_attr.send_cq = cq;
    create_attr.qp_type = IBV_QPT_RC;
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_send_wr = sq_depth;
    create_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
    create_attr.pd = pd;//RC

    create_attr.recv_cq = cq;
    create_attr.cap.max_recv_wr = rq_depth;  
    create_attr.cap.max_recv_sge = 1;
    create_attr.cap.max_inline_data = 128;
    qp = ibv_create_qp_ex(pd->context,&create_attr);

    //rqi.qpn = remote_qp_info->qpn;
    rqi.gid.global.interface_id = remote_qp_info->gid.global.interface_id;
    rqi.gid.global.subnet_prefix = remote_qp_info->gid.global.subnet_prefix;
    local_qp_info->qpn = qp->qp_num;



    //modify QP
    struct ibv_qp_attr init_attr;
    memset(&init_attr, 0, sizeof(struct ibv_qp_attr));
    init_attr.qp_state = IBV_QPS_INIT;
    init_attr.pkey_index = 0;
    init_attr.port_num = 1;//is that ok?
    init_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(qp, &init_attr,
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                        IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify conn QP to INIT\n");
        exit(-1);
    }

    struct ibv_qp_attr conn_attr;
    memset(&conn_attr, 0, sizeof(struct ibv_qp_attr));
    conn_attr.qp_state = IBV_QPS_RTR;
    conn_attr.path_mtu = IBV_MTU_1024;
    //conn_attr.dest_qp_num = rqi.qpn;
    printf("tgt qu_num:%d\n",qp->qp_num);
    scanf("%d",&conn_attr.dest_qp_num);
    conn_attr.rq_psn = psn;

    conn_attr.ah_attr.is_global = 1;
    conn_attr.ah_attr.dlid = 0;
    conn_attr.ah_attr.sl = 0;
    conn_attr.ah_attr.src_path_bits = 0;
    conn_attr.ah_attr.port_num = 1;  // Local port?is that ok?


    struct ibv_global_route *grh = &conn_attr.ah_attr.grh;
    grh->dgid.global.interface_id = rqi.gid.global.interface_id;
    grh->dgid.global.subnet_prefix = rqi.gid.global.subnet_prefix;

    grh->sgid_index = 0;
    grh->hop_limit = 1;
    

    int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                    IBV_QP_RQ_PSN;

    conn_attr.max_dest_rd_atomic = max_rd_atomic;
    conn_attr.min_rnr_timer = 12;
    rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    

    if (ibv_modify_qp(qp, &conn_attr, rtr_flags)) {
        fprintf(stderr, "HRD: Failed to modify QP to RTR: %s\n", strerror(errno));
        return NULL;
    }

    memset(&conn_attr, 0, sizeof(conn_attr));
    conn_attr.qp_state = IBV_QPS_RTS;
    conn_attr.sq_psn = psn;

    int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;

    conn_attr.timeout = 14;
    conn_attr.retry_cnt = 7;
    conn_attr.rnr_retry = 7;
    conn_attr.max_rd_atomic = max_rd_atomic;
    conn_attr.max_dest_rd_atomic = max_rd_atomic;
    rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                IBV_QP_MAX_QP_RD_ATOMIC;
    

    if (ibv_modify_qp(qp, &conn_attr, rts_flags)) {
        fprintf(stderr, "HRD: Failed to modify QP to RTS: %s\n", strerror(errno));
        return NULL;
    }
    printf("XRC TGT QP created\n");
    while(1);
    return qp;
}


int main() {
    int sockfd, newsockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    struct ibv_qp_info local_qp_info ;
    struct ibv_qp_info remote_qp_info;

    // TODO: get local rdma info 
    struct ibv_context *ibv_ctx;
    struct  ibv_ah_attr ah_attr;
    struct ibv_pd *pd;
    struct ibv_xrcd *xrcd;
    memset(&ah_attr,0,sizeof(ah_attr));
    ah_attr.dlid = 0;
    ah_attr.is_global = 1;
    ah_attr.src_path_bits = 0;
    ah_attr.grh.sgid_index = 0;
    ah_attr.grh.hop_limit = 1;
    int ret = get_rdma_attrs(&ibv_ctx,&ah_attr.port_num,&local_qp_info.gid);

    if(ret){
        return 1;
    }
    pd = ibv_alloc_pd(ibv_ctx);
    if(pd == NULL){
        printf("Failed to allocate pd\n");
        return 1;
    }
    //Create xrcd
    struct ibv_xrcd_init_attr init_attr;
    memset(&init_attr,0,sizeof(struct ibv_xrcd_init_attr));
    init_attr.comp_mask = IBV_XRCD_INIT_ATTR_OFLAGS | IBV_XRCD_INIT_ATTR_FD;
    init_attr.oflags = O_CREAT;
    int xrcd_fd = open(XRCD_FILE_PATH,O_RDONLY | O_CREAT,S_IRUSR | S_IRGRP);
    init_attr.fd = xrcd_fd;
    xrcd = ibv_open_xrcd(ibv_ctx,&init_attr);
    remote_qp_info.gid.global.interface_id = local_qp_info.gid.global.interface_id;
    remote_qp_info.gid.global.subnet_prefix = local_qp_info.gid.global.subnet_prefix;

    create_xrc_tgt(pd,xrcd,&local_qp_info,&remote_qp_info);
    //test_xrc_ini_and_kernel_tgt(pd,&local_qp_info.gid,xrcd,&ah_attr);

    return 0;
}

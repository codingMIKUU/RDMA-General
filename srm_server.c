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
#ifndef htonll
#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#endif

#ifndef ntohll
#define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif
void print_qp_info(struct ibv_qp_info *info) {
    printf("QP Number: %u\n", info->qpn);
    // printf("GID: ");
    // for (int i = 0; i < 16; ++i) {
    //     printf("%02x", info->gid.raw[i]);
    // }
    printf("interface id: 0x%llx\n", info->gid.global.interface_id);
    printf("subnet prefix: 0x%llx\n", info->gid.global.subnet_prefix);
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

void test_xrc_ini(struct ibv_pd *pd,union ibv_gid *gid,struct ibv_xrcd *xrcd,struct ibv_ah_attr *ah_attr){
    struct ibv_qp *qp;
    struct ibv_qp_init_attr_ex create_attr;
    struct ibv_qp_info local_qp_info,remote_qp_info;
    struct ibv_cq *cq;
    const int sq_depth = 256;
    const int cq_depth = 256;
    const int rq_depth = 256;
    const int psn = 3100;
    const int max_rd_atomic = 16;
    memset(&create_attr,0,sizeof create_attr);
    struct ibv_qp_attr conn_attr;
    
    //create mr 
    void * buf = malloc(200);
    memset(buf,0,200);
    memset(buf,-1,100);
    struct ibv_mr *mr = ibv_reg_mr(pd,buf,200,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
                         | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);

    char *buf2 = (char*)malloc(200);
    memset(buf2,0,200);

    struct ibv_mr *mr2 = ibv_reg_mr(pd,buf2,200,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
                        | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);


    cq =ibv_create_cq(pd->context,cq_depth,NULL,NULL,0);
    if(!cq){
        printf("Create CQ false\n");
        return ;
    }

    create_attr.qp_type = IBV_QPT_XRC_SEND;
    create_attr.send_cq = cq;


    create_attr.qp_type = IBV_QPT_RC;
    create_attr.recv_cq = cq;//RC
    create_attr.cap.max_recv_wr = 1;
    create_attr.cap.max_recv_sge = 1;//RC

    create_attr.cap.max_send_wr = sq_depth;
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_inline_data = 128;
    create_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
    create_attr.pd = pd;
    qp = ibv_create_qp_ex(pd->context,&create_attr);
    if(!qp){
        printf("Create Kernel qp false\n");
        return ;
    }
    remote_qp_info.gid.global.interface_id = gid->global.interface_id;
    remote_qp_info.gid.global.subnet_prefix = gid->global.subnet_prefix;

    // ah_attr->grh.dgid.global.interface_id = remote_qp_info.gid.global.interface_id;
    // ah_attr->grh.dgid.global.subnet_prefix = remote_qp_info.gid.global.subnet_prefix;
    //struct ibv_ah *ah = ibv_create_ah(pd,ah_attr,xrcd,&local_qp_info,&remote_qp_info);
    //struct ibv_qp *tgt_qp = create_xrc_tgt(pd,xrcd,&local_qp_info,&remote_qp_info);
    //modify qp
    memset(&conn_attr,0,sizeof conn_attr);
    conn_attr.qp_state = IBV_QPS_INIT;
    conn_attr.pkey_index = 0;
    conn_attr.port_num = 1;//is that ok?
    conn_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(qp, &conn_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                IBV_QP_ACCESS_FLAGS)) {
        printf("Failed to modify conn QP to INIT\n");
        return;
    }

    memset(&conn_attr, 0, sizeof conn_attr);
    conn_attr.qp_state = IBV_QPS_RTR;
    conn_attr.path_mtu = IBV_MTU_1024;
    conn_attr.dest_qp_num = remote_qp_info.qpn;
    conn_attr.rq_psn = psn;

    conn_attr.ah_attr.is_global = 1;
    conn_attr.ah_attr.dlid = 0;
    conn_attr.ah_attr.sl = 0;
    conn_attr.ah_attr.src_path_bits = 0;
    conn_attr.ah_attr.port_num = 1;  // Local port?is that ok?


    struct ibv_global_route *grh = &conn_attr.ah_attr.grh;
    grh->dgid.global.interface_id = remote_qp_info.gid.global.interface_id;
    grh->dgid.global.subnet_prefix = remote_qp_info.gid.global.subnet_prefix;

    grh->sgid_index = 0;
    grh->hop_limit = 1;
    

    int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                    IBV_QP_RQ_PSN;

    conn_attr.max_dest_rd_atomic = max_rd_atomic;
    conn_attr.min_rnr_timer = 12;
    rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    

    if (ibv_modify_qp(qp, &conn_attr, rtr_flags)) {
        printf("Failed to modify QP to RTR\n");
        return;
    }

    memset(&conn_attr, 0, sizeof(conn_attr));
    conn_attr.qp_state = IBV_QPS_RTS;
    conn_attr.sq_psn = psn;

    int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;

    conn_attr.timeout = 14;//14
    conn_attr.retry_cnt = 7;//7
    conn_attr.rnr_retry = 7;//7
    conn_attr.max_rd_atomic = max_rd_atomic;
    conn_attr.max_dest_rd_atomic = max_rd_atomic;
    rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                IBV_QP_MAX_QP_RD_ATOMIC;
    

    if (ibv_modify_qp(qp, &conn_attr, rts_flags)) {
        printf("Failed to modify QP to RTS\n");
        return ;
    }

    

    // //create srq
    // struct ibv_srq_init_attr_ex srq_init_attr;
    // memset(&srq_init_attr,0,sizeof(struct ibv_srq_init_attr_ex));
    // srq_init_attr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_XRCD | IBV_SRQ_INIT_ATTR_CQ |
    //                         IBV_SRQ_INIT_ATTR_PD;
    // srq_init_attr.srq_type = IBV_SRQT_XRC;
    // srq_init_attr.xrcd = xrcd;
    // srq_init_attr.cq = cq;
    // srq_init_attr.pd = pd;
    // srq_init_attr.attr.max_sge = 1;
    // srq_init_attr.attr.max_wr = rq_depth;
    // struct ibv_srq *srq = ibv_create_srq_ex(pd->context, &srq_init_attr);
    // if(srq==NULL){
    //     printf("Failed to create srq\n");
    //     return ;
    // }


    struct ibv_send_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof wr);
    struct ibv_sge sgl;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = NULL;
    wr.sg_list = &sgl;

    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    

    sgl.addr = (uint64_t)buf;
    sgl.length = 100;
    sgl.lkey = mr->lkey;



    // wr.wr.rdma.remote_addr = (uint64_t)buf2;
    // wr.wr.rdma.rkey = mr2->rkey;
    //ibv_get_srq_num(srq,&wr.qp_type.xrc.remote_srqn);
    wr.wr.rdma.remote_addr = 1;
    wr.wr.rdma.rkey = 1;
    // wr.qp_type.xrc.remote_srqn = 1;

    if(ibv_post_send(qp, &wr, &bad_wr)){
        printf("Failed to post send\n");
        return ;
    }

    struct ibv_wc wc;
    memset(&wc,0,sizeof(wc));
    int ret;
    while(1){
        printf("polling wc\n");
        ret = ibv_poll_cq(cq,1,&wc);
        printf("ret:%d\n",ret);
        if(ret>0)break;
    }
    if (wc.status != 0) {
        printf("Bad wc status %d\n", wc.status);
    }
}

void test_xrc_ini_and_kernel_tgt(struct ibv_pd *pd,union ibv_gid *gid,struct ibv_xrcd *xrcd,struct ibv_ah_attr *ah_attr){
    struct ibv_qp *qp;
    struct ibv_qp_init_attr_ex create_attr;
    struct ibv_qp_info local_qp_info,remote_qp_info;
    struct ibv_cq *cq;
    const int sq_depth = 256;
    const int cq_depth = 256;
    const int rq_depth = 256;
    const int psn = 3100;
    const int max_rd_atomic = 16;
    memset(&create_attr,0,sizeof create_attr);
    struct ibv_qp_attr conn_attr;



    cq =ibv_create_cq(pd->context,cq_depth,NULL,NULL,0);
    if(!cq){
        printf("Create CQ false\n");
        return ;
    }

    // create_attr.qp_type = IBV_QPT_XRC_SEND;
    // create_attr.send_cq = cq;


    create_attr.qp_type = IBV_QPT_RC;
    create_attr.recv_cq = cq;//RC
    create_attr.cap.max_recv_wr = 1;
    create_attr.cap.max_recv_sge = 1;//RC

    create_attr.cap.max_send_wr = sq_depth;
    create_attr.cap.max_send_sge = 1;
    create_attr.cap.max_inline_data = 128;
    create_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
    create_attr.pd = pd;
    qp = ibv_create_qp_ex(pd->context,&create_attr);
    if(!qp){
        printf("Create Kernel qp false\n");
        return ;
    }
    remote_qp_info.qpn = qp->qp_num;
    remote_qp_info.gid.global.interface_id = gid->global.interface_id;
    remote_qp_info.gid.global.subnet_prefix = gid->global.subnet_prefix;
    local_qp_info.gid.global.interface_id = gid->global.interface_id;
    local_qp_info.gid.global.subnet_prefix = gid->global.subnet_prefix;

    ah_attr->grh.dgid.global.interface_id = remote_qp_info.gid.global.interface_id;
    ah_attr->grh.dgid.global.subnet_prefix = remote_qp_info.gid.global.subnet_prefix;
    ah_attr->dqpn = qp->qp_num;
    struct ibv_ah *ah = ibv_create_ah(pd,ah_attr,xrcd,&local_qp_info,&remote_qp_info);
    local_qp_info.qpn = ah->srmc_flags;
    // struct ibv_qp *tgt_qp = create_xrc_tgt(pd,xrcd,&local_qp_info,&remote_qp_info);
    //modify qp
    memset(&conn_attr,0,sizeof conn_attr);
    conn_attr.qp_state = IBV_QPS_INIT;
    conn_attr.pkey_index = 0;
    conn_attr.port_num = 1;//is that ok?
    conn_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(qp, &conn_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                IBV_QP_ACCESS_FLAGS)) {
        printf("Failed to modify conn QP to INIT\n");
        return;
    }

    memset(&conn_attr, 0, sizeof conn_attr);
    conn_attr.qp_state = IBV_QPS_RTR;
    conn_attr.path_mtu = IBV_MTU_1024;
    conn_attr.dest_qp_num = local_qp_info.qpn;
    conn_attr.rq_psn = psn;

    conn_attr.ah_attr.is_global = 1;
    conn_attr.ah_attr.dlid = 0;
    conn_attr.ah_attr.sl = 0;
    conn_attr.ah_attr.src_path_bits = 0;
    conn_attr.ah_attr.port_num = 1;  // Local port?is that ok?


    struct ibv_global_route *grh = &conn_attr.ah_attr.grh;
    grh->dgid.global.interface_id = local_qp_info.gid.global.interface_id;
    grh->dgid.global.subnet_prefix = local_qp_info.gid.global.subnet_prefix;

    grh->sgid_index = 0;
    grh->hop_limit = 1;
    

    int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                    IBV_QP_RQ_PSN;

    conn_attr.max_dest_rd_atomic = max_rd_atomic;
    conn_attr.min_rnr_timer = 12;
    rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    

    if (ibv_modify_qp(qp, &conn_attr, rtr_flags)) {
        printf("Failed to modify QP to RTR\n");
        return;
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
        printf("Failed to modify QP to RTS\n");
        return ;
    }

    //create mr 
    void * buf = malloc(4096);
    memset(buf,0,4096);
    memset(buf,-1,1024);
    struct ibv_mr *mr = ibv_reg_mr(pd,buf,4096,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
                         | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);


    //create srq
    struct ibv_srq_init_attr_ex srq_init_attr;
    memset(&srq_init_attr,0,sizeof(struct ibv_srq_init_attr_ex));
    srq_init_attr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_XRCD | IBV_SRQ_INIT_ATTR_CQ |
                            IBV_SRQ_INIT_ATTR_PD;
    srq_init_attr.srq_type = IBV_SRQT_XRC;
    srq_init_attr.xrcd = xrcd;
    srq_init_attr.cq = cq;
    srq_init_attr.pd = pd;
    srq_init_attr.attr.max_sge = 1;
    srq_init_attr.attr.max_wr = rq_depth;
    struct ibv_srq *srq = ibv_create_srq_ex(pd->context, &srq_init_attr);
    if(srq==NULL){
        printf("Failed to create srq\n");
        return ;
    }


    struct ibv_send_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof wr);
    struct ibv_sge sgl;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = NULL;
    wr.sg_list = &sgl;

    wr.send_flags = IBV_SEND_SIGNALED;
    wr.send_flags|= IBV_SEND_INLINE;
    // if (nb_tx[qp_cn] % kAppUnsigBatch == 0 && nb_tx[qp_cn] > 0 &&!FLAGS_test_lat) {
    //   // This can happen if a client dies before the server
    //   int ret = hrd_poll_cq_ret(cb->conn_cq[qp_cn], 1, &wc);
    //   if (ret == -1) {
    //     hrd_ctrl_blk_destroy(cb);
    //     return;
    //   }
    // }
    

    sgl.addr = (uint64_t)buf;
    sgl.length = 100;
    sgl.lkey = mr->lkey;

    char *buf2 = (char*)malloc(4096);
    memset(buf2,0,4096);

    struct ibv_mr *mr2 = ibv_reg_mr(pd,buf2,4096,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
                         | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);

    wr.wr.rdma.remote_addr = (uint64_t)buf2;
    wr.wr.rdma.rkey = mr2->rkey;
    ibv_get_srq_num(srq,&wr.qp_type.xrc.remote_srqn);
    // wr.wr.rdma.remote_addr = 1;
    // wr.wr.rdma.rkey = 1;
    // wr.qp_type.xrc.remote_srqn = 1;

    if(ibv_post_send(qp, &wr, &bad_wr)){
        printf("Failed to post send\n");
        return ;
    }

    struct ibv_wc wc;
    memset(&wc,0,sizeof(wc));
    int ret;
    while(1){
        printf("polling wc\n");
        ret = ibv_poll_cq(cq,1,&wc);
        printf("ret:%d\n",ret);
        if(ret>0)break;
    }
    if (wc.status != 0) {
        printf("Bad wc status %d\n", wc.status);
    }
}
int main() {
    const int port_num = 12345;
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
    ah_attr.check_xrc = 2;
    ah_attr.dlid = 0;
    ah_attr.is_global = 1;
    ah_attr.src_path_bits = 0;
    ah_attr.grh.sgid_index = 0;
    ah_attr.grh.hop_limit = 1;
    int ret = get_rdma_attrs(&ibv_ctx,&ah_attr.port_num,&local_qp_info.gid);
    char buffer[256];
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

    //test_xrc_ini(pd,&local_qp_info.gid,xrcd,&ah_attr);
    //test_xrc_ini_and_kernel_tgt(pd,&local_qp_info.gid,xrcd,&ah_attr);
    if(xrcd == NULL){
        printf("Failed to open XRCD\n");
        return 1;
    }
    // 创建套接字
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        return 1;
    }
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(sockfd);
        return -1;
    }

    // 设置服务器地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // 绑定套接字
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        return 1;
    }

    // 监听连接
    if (listen(sockfd, 50) < 0) {
        perror("Listen failed");
        close(sockfd);
        return 1;
    }

    printf("Server listening on port %d\n",port_num);
    uint32_t local_qpn = 0;
    // 进入死循环持续监听连接
    while (1) {
        // 接受客户端连接
        client_len = sizeof(client_addr);
        if ((newsockfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len)) < 0) {
            perror("Accept failed");
            continue; // 出现错误时继续监听
        }

        // // 接收客户端字符串
        // char recv_buf[100];
        // // memset(recv_buf, 0, sizeof(recv_buf)); 
        // size_t ret=recv(newsockfd, recv_buf, sizeof(recv_buf), 0);
        // if (ret < 0) {
        //     perror("Receive failed");
        //     close(newsockfd);
        //     close(sockfd);
        //     return;
        // }

        // // 打印接收到的字符串
        // printf("Received string: %s, buff size:%d, size%d\n", recv_buf,sizeof(recv_buf),ret);

        // // 发送字符串
        // char send_buf[] = "tyys";
        // if (send(newsockfd, send_buf, sizeof(send_buf), 0) < 0) {
        //     perror("Send failed");
        //     close(newsockfd);
        //     close(sockfd);
        //     return;
        // }
        // printf("Sent string: %s,size%d \n", send_buf,sizeof(send_buf));

        //接收客户端QP信息
        printf("remote_qp size:%zu\n",sizeof(remote_qp_info));
        int ret = recv(newsockfd, &remote_qp_info, sizeof(remote_qp_info),0);
        if(ret<=0)
        {       
            perror("Receive failed");
            printf("Receive failed,recv size %d\n",ret);
            close(newsockfd);
            continue; // 出现错误时继续监听
        }

        
        // 打印客户端QP信息
        // remote_qp_info.qpn = ntohl(remote_qp_info.qpn);
        // remote_qp_info.gid.global.subnet_prefix = ntohll(remote_qp_info.gid.global.subnet_prefix);
        // remote_qp_info.gid.global.interface_id = ntohll(remote_qp_info.gid.global.interface_id);
        printf("Received client QP info:,ret:%d\n",ret);
        print_qp_info(&remote_qp_info);
       
        //TODO: create ah to create the tgt qp. what attrs of ah are needed?
        ah_attr.grh.dgid.global.interface_id = remote_qp_info.gid.global.interface_id;
        ah_attr.grh.dgid.global.subnet_prefix = remote_qp_info.gid.global.subnet_prefix;
        ah_attr.dqpn = remote_qp_info.qpn;
        struct ibv_ah *ah = ibv_create_ah(pd,&ah_attr,xrcd,&local_qp_info,&remote_qp_info);
        if(ah == NULL){
            printf("Failed to create AH\n");
            close(newsockfd);
            goto err;
        }
        local_qp_info.qpn =  ah->srmc_flags;

        printf("Sending local QP info:\n");
        print_qp_info(&local_qp_info);
        // local_qp_info.qpn = htonl(local_qp_info.qpn);
        // local_qp_info.gid.global.interface_id = htonll(local_qp_info.gid.global.interface_id);  
        // local_qp_info.gid.global.subnet_prefix = htonll(local_qp_info.gid.global.subnet_prefix);
        ret=send(newsockfd, &local_qp_info, sizeof(local_qp_info),0);
        // 发送本地QP信息
        if (ret < 0) {
            perror("Send failed");
            close(newsockfd);
            continue; // 出现错误时继续监听
        }
        printf("Sending QP OK\n");

        // char *buf = (char*)malloc(100);
        // memset(buf,0,100);
        // struct ibv_mr *mr = ibv_reg_mr(pd,buf,100,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
        //                  | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
        // struct ibv_send_wr wr, *bad_wr = NULL;
        // memset(&wr, 0, sizeof wr);
        // struct ibv_sge sgl;
        // sgl.addr = (uint64_t)buf;
        // sgl.length = 100;
        // sgl.lkey = mr->lkey;


        // wr.opcode = IBV_WR_RDMA_WRITE;
        // wr.num_sge = 1;
        // wr.next = NULL;
        // wr.sg_list = &sgl;
        // wr.send_flags = IBV_SEND_SIGNALED;
        // wr.wr.rdma.remote_addr = 5;
        // wr.wr.rdma.rkey = 6;
        // if(ibv_post_send(local_qp_info.qp, &wr, &bad_wr)){
        //     printf("Failed to post send\n");
        //     return 1;
        // }
        // printf("post send success\n");
        // struct ibv_wc wc;
        // while(1){
            
        //     ret = ibv_poll_cq(local_qp_info.qp->send_cq, 1, &wc);
        //     if(ret){
        //         break;
        //     }
        //     if(ret==0){
        //         continue;
        //     }
        // }
        // printf("ret:%d, status:%d\n",ret,wc.status);


        // 关闭连接
        close(newsockfd);
    }

    // 关闭服务器套接字（实际代码中不会到达这里）
err:
    close(sockfd);
    return 0;
}

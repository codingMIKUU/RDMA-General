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


struct ibv_qp_info {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qpn;
    union ibv_gid gid;
};

void print_qp_info(struct ibv_qp_info *info) {
    printf("QP Number: %u\n", info->qpn);
    printf("GID: ");
    for (int i = 0; i < 16; ++i) {
        printf("%02x", info->gid.raw[i]);
    }
    printf("\n");
    printf("r_key: %u\n", info->rkey);
    printf("addr: %lu\n", info->addr);
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
    for (int dev_i = 1; dev_i < num_devices; dev_i++) {
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
        printf("port num:%d\n",device_attr.phys_port_cnt);
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
                printf("ibv_query_gid success\n");
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
    const int psn = 3185;
    const int max_rd_atomic = 16;
    memset(&create_attr,0,sizeof create_attr);
    struct ibv_qp_attr conn_attr;
    
    //create mr 
    size_t reg_size = 200;
    void * buf = malloc(reg_size);
    memset(buf,0,reg_size);
    struct ibv_mr *mr = ibv_reg_mr(pd,buf,reg_size,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
                         | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);

    cq =ibv_create_cq(pd->context,cq_depth,NULL,NULL,0);
    if(!cq){
        printf("Create CQ false\n");
        return ;
    }

    // create_attr.qp_type = IBV_QPT_XRC_SEND;
    create_attr.send_cq = cq;


    create_attr.qp_type = IBV_QPT_RC;
    create_attr.recv_cq = cq;//RC
    create_attr.cap.max_recv_wr = 1;
    create_attr.cap.max_recv_sge = 1;//RC

    create_attr.cap.max_send_wr = sq_depth;
    create_attr.cap.max_send_sge = 1;
    // create_attr.cap.max_inline_data = 128;
    create_attr.comp_mask = IBV_QP_INIT_ATTR_PD;
    create_attr.pd = pd;
    qp = ibv_create_qp_ex(pd->context,&create_attr);
    if(!qp){
        printf("Create Kernel qp false\n");
        return ;
    }
    

    // ah_attr->grh.dgid.global.interface_id = remote_qp_info.gid.global.interface_id;
    // ah_attr->grh.dgid.global.subnet_prefix = remote_qp_info.gid.global.subnet_prefix;
    //struct ibv_ah *ah = ibv_create_ah(pd,ah_attr,xrcd,&local_qp_info,&remote_qp_info);
    //struct ibv_qp *tgt_qp = create_xrc_tgt(pd,xrcd,&local_qp_info,&remote_qp_info);
    printf("ini qp qp_num:%d\n",qp->qp_num);
    // scanf("%d",&remote_qp_info.qpn);
    // remote_qp_info.gid.global.interface_id = gid->global.interface_id;
    // remote_qp_info.gid.global.subnet_prefix = gid->global.subnet_prefix;
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

//************* */
    #define TCP_PORT 12345
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);  // 指定远端 IP 地址
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("Failed to connect to server");
    return ;
    }

    local_qp_info.addr = (uint64_t)mr->addr;
    local_qp_info.rkey = mr->rkey;
    local_qp_info.qpn = qp->qp_num;
    memcpy(&local_qp_info.gid, gid, sizeof(union ibv_gid));

    // 发送本地 RDMA 信息
    if (send(sockfd, &local_qp_info, sizeof(struct ibv_qp_info), 0) != sizeof(struct ibv_qp_info)) {
        perror("Failed to send local RDMA info");
        exit(EXIT_FAILURE);
    }
    // 接收远端 RDMA 信息
    if (recv(sockfd, &remote_qp_info, sizeof(struct ibv_qp_info), 0) != sizeof(struct ibv_qp_info)) {
        perror("Failed to receive remote RDMA info");
        exit(EXIT_FAILURE);
    }
    // 打印本端 RDMA 信息
    printf("本RDMA信息:\n");
    print_qp_info(&local_qp_info);

    // 打印发送端 RDMA 信息
    printf("\n发送端RDMA信息:\n");
    print_qp_info(&remote_qp_info);
//************* */
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


    printf("\n");

    
    while(1)
    {
        printf("New Buffer contents:\n");
        for (size_t i = 0; i < reg_size; i++) {
        printf("%02x ", ((unsigned char*)buf)[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
        }
        printf("\n");
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
    // ah_attr.check_xrc = 2;
    ah_attr.dlid = 0;
    ah_attr.is_global = 1;
    ah_attr.src_path_bits = 0;
    ah_attr.grh.sgid_index = 0;
    ah_attr.grh.hop_limit = 1;
    int ret = get_rdma_attrs(&ibv_ctx,&ah_attr.port_num,&local_qp_info.gid);
    if(ret){
        return 1;
    }
    //pd = ibv_alloc_pd(ibv_ctx);
    int pd_handle;
    printf("请输入 pd handle:");
    scanf("%d",&pd_handle);
    pd = ibv_import_pd(ibv_ctx,pd_handle);
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

    test_xrc_ini(pd,&local_qp_info.gid,xrcd,&ah_attr);
    //test_xrc_ini_and_kernel_tgt(pd,&local_qp_info.gid,xrcd,&ah_attr);
    // if(xrcd == NULL){
    //     printf("Failed to open XRCD\n");
    //     return 1;
    // }
    // // 创建套接字
    // if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    //     perror("Socket creation failed");
    //     return 1;
    // }

    // // 设置服务器地址
    // server_addr.sin_family = AF_INET;
    // server_addr.sin_port = htons(12345);
    // server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // // 绑定套接字
    // if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    //     perror("Bind failed");
    //     close(sockfd);
    //     return 1;
    // }

    // // 监听连接
    // if (listen(sockfd, 50) < 0) {
    //     perror("Listen failed");
    //     close(sockfd);
    //     return 1;
    // }

    // printf("Server listening on port 12345\n");

    // // 进入死循环持续监听连接
    // while (1) {
    //     // 接受客户端连接
    //     client_len = sizeof(client_addr);
    //     if ((newsockfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len)) < 0) {
    //         perror("Accept failed");
    //         continue; // 出现错误时继续监听
    //     }

    //     // 接收客户端QP信息
    //     if (recv(newsockfd, &remote_qp_info, sizeof(remote_qp_info), 0) < 0) {
    //         perror("Receive failed");
    //         close(newsockfd);
    //         continue; // 出现错误时继续监听
    //     }
    //     // 打印客户端QP信息
    //     printf("Received client QP info:\n");
    //     print_qp_info(&remote_qp_info);
       
    //     //TODO: create ah to create the tgt qp. what attrs of ah are needed?
    //     ah_attr.grh.dgid.global.interface_id = remote_qp_info.gid.global.interface_id;
    //     ah_attr.grh.dgid.global.subnet_prefix = remote_qp_info.gid.global.subnet_prefix;
    //     // ah_attr.dqpn = remote_qp_info.qpn;
        
    //     //struct ibv_ah *ah = ibv_create_ah(pd,&ah_attr,xrcd,&local_qp_info,&remote_qp_info);
    //     // if(ah == NULL){
    //     //     printf("Failed to create AH\n");
    //     // }
    //     // local_qp_info.qpn =  ah->srmc_flags;


    //     printf("Sending local QP info:\n");
    //     print_qp_info(&local_qp_info);

    //     // 发送本地QP信息
    //     if (send(newsockfd, &local_qp_info, sizeof(local_qp_info), 0) < 0) {
    //         perror("Send failed");
    //         close(newsockfd);
    //         continue; // 出现错误时继续监听
    //     }

    //     // 关闭连接
    //     close(newsockfd);
    // }

    // // 关闭服务器套接字（实际代码中不会到达这里）
    // close(sockfd);
    return 0;
}

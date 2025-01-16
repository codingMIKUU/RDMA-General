#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include <sys/stat.h>
#include <fcntl.h>
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
int main() {
    printf("%d\n",(int)sizeof(struct ibv_send_wr*));
    printf("%d\n",(int)sizeof(struct ibv_send_wr_q));
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
    if(xrcd == NULL){
        printf("Failed to open XRCD\n");
        return 1;
    }
    // 创建套接字
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        return 1;
    }

    // 设置服务器地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(12345);
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

    printf("Server listening on port 12345\n");

    // 进入死循环持续监听连接
    while (1) {
        // 接受客户端连接
        client_len = sizeof(client_addr);
        if ((newsockfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len)) < 0) {
            perror("Accept failed");
            continue; // 出现错误时继续监听
        }

        // 接收客户端QP信息
        if (recv(newsockfd, &remote_qp_info, sizeof(remote_qp_info), 0) < 0) {
            perror("Receive failed");
            close(newsockfd);
            continue; // 出现错误时继续监听
        }
        //TODO: create ah to create the tgt qp. what attrs of ah are needed?
        ah_attr.grh.dgid.global.interface_id = remote_qp_info.gid.global.interface_id;
        ah_attr.grh.dgid.global.subnet_prefix = remote_qp_info.gid.global.subnet_prefix;

        ibv_create_ah(pd,&ah_attr,xrcd,&local_qp_info,&remote_qp_info);

        
        // 打印客户端QP信息
        printf("Received client QP info:\n");
        print_qp_info(&remote_qp_info);

        // 发送本地QP信息
        if (send(newsockfd, &local_qp_info, sizeof(local_qp_info), 0) < 0) {
            perror("Send failed");
            close(newsockfd);
            continue; // 出现错误时继续监听
        }

        // 关闭连接
        close(newsockfd);
    }

    // 关闭服务器套接字（实际代码中不会到达这里）
    close(sockfd);
    return 0;
}

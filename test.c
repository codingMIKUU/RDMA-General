#include <sys/shm.h>
#include <stdio.h>

int main() {
    key_t shm_key = 1234;
    int shmid = shmget(shm_key, 1024, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget failed");
        return 1;
    }
    printf("Shared memory segment created successfully.\n");
    return 0;
}

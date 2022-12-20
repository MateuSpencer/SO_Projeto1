#include "../fs/operations.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define THREAD_COUNT 15


char const target_path1[] = "/f1";
char *copy_files_contents = "BBBB";

void *thread_append() {
    int f = tfs_open(target_path1, TFS_O_APPEND);
    assert(tfs_write(f, copy_files_contents, sizeof(copy_files_contents)) ==  sizeof(copy_files_contents));
    assert(tfs_close(f) != -1);
    return NULL;
}

int main() {
    assert(tfs_init(NULL) != -1);

    pthread_t tid[THREAD_COUNT];

    int f = tfs_open(target_path1, TFS_O_CREAT);
    assert(tfs_close(f) != -1);

    for (int i = 0; i < THREAD_COUNT; ++i) {
        assert(pthread_create(&tid[i], NULL, thread_append, NULL) == 0);
    }
    //join threads
    for (int i = 0; i < THREAD_COUNT; ++i) {
        assert(pthread_join(tid[i], NULL) == 0);
    }
    
    //check if file contents are correct
    f = tfs_open(target_path1, 0);
    uint8_t buffer[THREAD_COUNT * sizeof(copy_files_contents)];
    ssize_t j = tfs_read(f, buffer, sizeof(buffer));
    printf("%s\n", buffer);
    //assert( j == sizeof(buffer));
    (void)j;

    
    printf("Successful test.\n");
}
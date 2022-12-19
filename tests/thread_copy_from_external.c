#include "../fs/operations.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

//bigger numbers violate max number of dir entries
#define FILE_COUNT 3
#define THREAD_COUNT 3
#define SAVE_COUNT_PER_FILE 2

char *copy_files[] = {"tests/file_to_copy_2.txt", "tests/file_to_copy.txt", "tests/empty_file.txt"};

char *copy_files_contents[] = {"BBB! BBB!", "BBB!", "" };

void *thread_copy_from_external(void *thread_i) {
    for (int file_id = 0; file_id < FILE_COUNT; ++file_id) {
        for (int i = 0; i < SAVE_COUNT_PER_FILE; ++i) {
            char tfs_path[10];
            sprintf(tfs_path, "/f%d_t%d_%d", file_id,*((int *)thread_i), i);
            assert(tfs_copy_from_external_fs(copy_files[file_id], tfs_path) == 0);
        }
    }
    return NULL;
}

void check_if_file_was_correctly_copied(char *tfs_path, int copy_file_i){
    char buffer[20];
    int f = tfs_open(tfs_path, TFS_O_CREAT);
    assert(f != -1);

    ssize_t r = tfs_read(f, buffer, sizeof(buffer) - 1);
    assert(tfs_close(f) != -1);
    assert(r == strlen(copy_files_contents[copy_file_i]));
    assert(!memcmp(buffer, copy_files_contents[copy_file_i], strlen(copy_files_contents[copy_file_i])));
}

int main() {
    assert(tfs_init(NULL) != -1);

    pthread_t tid[THREAD_COUNT];
    int thread_id[THREAD_COUNT];

    //copy concurrently 3 files many yimes
    for (int i = 0; i < THREAD_COUNT; ++i) {
        thread_id[i] = i;
        assert(pthread_create(&tid[i], NULL, thread_copy_from_external,(void *)(&thread_id[i])) == 0);
    }
    //join threads
    for (int i = 0; i < THREAD_COUNT; ++i) {
        assert(pthread_join(tid[i], NULL) == 0);
    }

    //check if file contents are correct
    for (int file_i = 0; file_i < FILE_COUNT; ++file_i){
        for (int thread_i = 0; thread_i < THREAD_COUNT; ++thread_i) {
            for (int i = 0; i < SAVE_COUNT_PER_FILE; ++i) {
                char tfs_path[10];
                sprintf(tfs_path, "/f%d_t%d_%d", file_i,thread_i, i);
                check_if_file_was_correctly_copied(tfs_path, file_i);
            }
        }
    }
    
    printf("Successful test.\n");
}
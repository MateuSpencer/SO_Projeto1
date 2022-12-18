#include "../fs/operations.h"
#include <assert.h>
#include <stdio.h>

int main() {

    char *path_copied_file_1023 = "/f1023";
    char *path_src_1023 = "tests/file_to_copy_1023.txt";
    char *path_copied_file_1024 = "/f1024";
    char *path_src_1024 = "tests/file_to_copy_1024.txt";
    char *path_copied_file_1025 = "/f1025";
    char *path_src_1025 = "tests/file_to_copy_1025.txt";

    assert(tfs_init(NULL) != -1);

    int f;

    f = tfs_copy_from_external_fs(path_src_1023, path_copied_file_1023);
    assert(f != -1);

    f = tfs_copy_from_external_fs(path_src_1024, path_copied_file_1024);
    assert(f != -1);

    f = tfs_copy_from_external_fs(path_src_1025, path_copied_file_1025);
    assert(f == -1);

    printf("Successful test.\n");

    return 0;
}

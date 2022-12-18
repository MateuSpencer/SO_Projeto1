#include "fs/operations.h"
#include <assert.h>
#include <stdio.h>

int main() {
    char *path1 = "/f1";

    /* Tests different scenarios where tfs_copy_from_external_fs is expected to
     * fail */

    assert(tfs_init(NULL) != -1);

    int f1 = tfs_open(path1, TFS_O_CREAT);
    assert(f1 != -1);
    assert(tfs_close(f1) != -1);

    // Scenario 1: source file does not exist
    assert(tfs_copy_from_external_fs("./unexistent", path1) == -1);

    //file too big to copy fully, so it does not copy
    assert(tfs_copy_from_external_fs("tests/file_to_copy_too_big.txt", path1) == -1);
    //valid file, but invalid path names
    assert(tfs_copy_from_external_fs("tests/empty_file.txt", "f1") == -1);
    assert(tfs_copy_from_external_fs("tests/empty_file.txt", "/") == -1);

    printf("Successful test.\n");

    return 0;
}

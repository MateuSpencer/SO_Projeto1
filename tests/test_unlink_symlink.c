#include "../fs/operations.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
criar ficheiro e escrever

symlink para isso
sym link para o sym link
ler a partir do 2º sym link
apagar ficheiro
tentar abrir do 2º sym link e nao dar


*/

uint8_t const file_contents[] = "AAA!";
char const target_path[] = "/ts_file";
char const link_path1[] = "/sym_1";
char const link_path2[] = "/sym_2";

void assert_contents_ok(char const *path) {
    int f = tfs_open(path, 0);
    assert(f != -1);

    uint8_t buffer[sizeof(file_contents)];
    assert(tfs_read(f, buffer, sizeof(buffer)) == sizeof(buffer));
    assert(memcmp(buffer, file_contents, sizeof(buffer)) == 0);

    assert(tfs_close(f) != -1);
}

int main() {
    assert(tfs_init(NULL) != -1);

    //create file & write content
    int f = tfs_open(target_path, TFS_O_CREAT);
    assert(f != -1);
    assert(tfs_write(f, file_contents, sizeof(file_contents)) == sizeof(file_contents));
    assert(tfs_close(f) != -1);
    //create link1 to file & link2 to link1
    assert(tfs_sym_link(target_path, link_path1) != -1);
    assert(tfs_sym_link(link_path1, link_path2) != -1);
    //read through link2 again
    assert_contents_ok(link_path2);
    //Delete link 1
    assert(tfs_unlink(link_path1) != -1);
    //cant open with link 2
    assert(tfs_open(link_path2, 0) == -1);
    //can open with original file
    assert(tfs_open(target_path, 0) != -1);
    //create link 1 again
    assert(tfs_sym_link(target_path, link_path1) != -1);
    //can open with link 2 again
    assert(tfs_open(link_path2, 0) == -1);
    //delete link2
    assert(tfs_unlink(link_path2) != -1);

    assert(tfs_open(link_path1, 0) != -1);

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}

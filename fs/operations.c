#include "operations.h"
#include "config.h"
#include "state.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "betterassert.h"

static pthread_mutex_t tfs_open_mutex;

tfs_params tfs_default_params() {
    tfs_params params = {
        .max_inode_count = 64,
        .max_block_count = 1024,
        .max_open_files_count = 16,
        .block_size = 1024,
    };
    return params;
}

int tfs_init(tfs_params const *params_ptr) {
    tfs_params params;
    if (params_ptr != NULL) {
        params = *params_ptr;
    } else {
        params = tfs_default_params();
    }

    if (state_init(params) != 0) {
        return -1;
    }
    pthread_mutex_init(&tfs_open_mutex, NULL);
    // create root inode
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    if (state_destroy() != 0) {
        return -1;
    }
    pthread_mutex_destroy(&tfs_open_mutex);
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}

/**
 * Looks for a file.
 *
 * Note: as a simplification, only a plain directory space (root directory only)
 * is supported.
 *
 * Input:
 *   - name: absolute path name
 *   - root_inode: the root directory inode
 * Returns the inumber of the file, -1 if unsuccessful.
 */
static int tfs_lookup(char const *name, inode_t *root_inode) {
    ALWAYS_ASSERT(root_inode == inode_get(ROOT_DIR_INUM), "tfs_lookup: root_inode must be root directory");
    if (!valid_pathname(name)) return -1;
    // skip the initial '/' character
    name++;

    return find_in_dir(root_inode, name);
}

int tfs_open(char const *name, tfs_file_mode_t mode) {
    // Checks if the path name is valid
    if (!valid_pathname(name)) {
        return -1;
    }

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,"tfs_open: root dir inode must exist");

    pthread_mutex_lock(&tfs_open_mutex);//lock this to k«make sure file file exists, otherwise another thread might create the same file
    int inum = tfs_lookup(name, root_dir_inode);
    size_t offset;

    if (inum >= 0) {
        pthread_mutex_unlock(&tfs_open_mutex);//we can unlock it since it already exists
        inode_t *inode = inode_get(inum);
        ALWAYS_ASSERT(inode != NULL,"tfs_open: directory files must have an inode");
            
        while(inode->i_node_type == T_SYMLINK){//TODO este tipo de cenas nao precisa de mutex?
            char *buffer = (char*)calloc(inode->i_size,sizeof(char));
            int sym_file_handle = add_to_open_file_table(inum, 0);
            if(tfs_read(sym_file_handle, buffer, inode->i_size) == -1){
                return -1;
            }
            tfs_close(sym_file_handle);
            inum = tfs_lookup(buffer, root_dir_inode);
            free(buffer);
            if( inum == -1 ){
                return -1;//broken link
            }
            inode = inode_get(inum);
                ALWAYS_ASSERT(inode != NULL,"tfs_open: directory files must have an inode");
        }

        // Truncate (if requested)
        if (mode & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                data_block_free(inode->i_data_block);
                inode->i_size = 0;
            }
        }
        // Determine initial offset
        if (mode & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
    } else if (mode & TFS_O_CREAT) {
        // The file does not exist; the mode specified that it should be created
        // Create inode
        inum = inode_create(T_FILE);
        if (inum == -1) {
            pthread_mutex_unlock(&tfs_open_mutex);
            return -1; // no space in inode table
        }

        // Add entry in the root directory
        if (add_dir_entry(root_dir_inode, name + 1, inum) == -1) {
            pthread_mutex_unlock(&tfs_open_mutex);
            inode_delete(inum);
            return -1; // no space in directory
        }
        pthread_mutex_unlock(&tfs_open_mutex);
        offset = 0;
    } else {
        pthread_mutex_unlock(&tfs_open_mutex);
        return -1;
    }

    // Finally, add entry to the open file table and return the corresponding
    // handle
    return add_to_open_file_table(inum, offset);

    // Note: for simplification, if file was created with TFS_O_CREAT and there
    // is an error adding an entry to the open file table, the file is not
    // opened but it remains created
}

int tfs_sym_link(char const *target, char const *link_name) {
    //ou abrir com tfs_open mas especificar o tipo T_SYMLINK e depois guardar la com tfs_write
    int inum = inode_create(T_SYMLINK);
    if (inum == -1) {
        return -1; // no space in inode table
    }
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
        ALWAYS_ASSERT(root_dir_inode != NULL,"tfs_link: root dir inode must exist");
    if (add_dir_entry(root_dir_inode, link_name + 1, inum) == -1) {
        inode_delete(inum);
        return -1; // no space in directory
    }
    size_t size = strlen(target);
    int sym_file_handle = add_to_open_file_table(inum, 0);
    if(tfs_write(sym_file_handle, target, size) == -1){
        return -1;
    }
    tfs_close(sym_file_handle);

    return 0;
}

int tfs_link(char const *target, char const *link_name) {
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
        ALWAYS_ASSERT(root_dir_inode != NULL,"tfs_link: root dir inode must exist");
    int target_inumber = tfs_lookup(target, root_dir_inode);
    if(target_inumber == -1){
        return -1;
    }
    inode_t *target_inode = inode_get(target_inumber);
    if(target_inode->i_node_type == T_SYMLINK){
        return -1;
    }
    if(add_dir_entry(root_dir_inode, link_name + 1, target_inumber) == -1){
        return -1;
    }
    pthread_rwlock_wrlock(&target_inode->rwlock);
    target_inode->hard_link_ctr++;
    pthread_rwlock_unlock(&target_inode->rwlock);
    return 0;
}

int tfs_close(int fhandle) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1; // invalid fd
    }

    remove_from_open_file_table(fhandle);

    return 0;
}

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    //  From the open file table entry, we get the inode
    inode_t *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_write: inode of open file deleted");

    // Determine how many bytes to write
    size_t block_size = state_block_size();
    pthread_mutex_lock(&file->mutexlock);
    if (to_write + file->of_offset > block_size) {
        to_write = block_size - file->of_offset;
    }

    if (to_write > 0) {
        pthread_rwlock_wrlock(&inode->rwlock);
        if (inode->i_size == 0) {
            // If empty file, allocate new block
            int bnum = data_block_alloc();
            if (bnum == -1) {
                pthread_rwlock_unlock(&inode->rwlock);
                pthread_mutex_unlock(&file->mutexlock);
                return -1; // no space
            }

            inode->i_data_block = bnum;
        }

        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_write: data block deleted mid-write");

        // Perform the actual write 
        memcpy(block + file->of_offset, buffer, to_write);
        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
        pthread_rwlock_unlock(&inode->rwlock);
    }
    pthread_mutex_unlock(&file->mutexlock);
    return (ssize_t)to_write;
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }
    //TODO read lock num inode
    // From the open file table entry, we get the inode
    inode_t *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_read: inode of open file deleted");
    pthread_mutex_lock(&file->mutexlock);
    pthread_rwlock_rdlock(&inode->rwlock);
    // Determine how many bytes to read
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (to_read > 0) {
        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_read: data block deleted mid-read");

        // Perform the actual read
        memcpy(buffer, block + file->of_offset, to_read);
        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_read;
    }
    pthread_rwlock_unlock(&inode->rwlock);
    pthread_mutex_unlock(&file->mutexlock);
    return (ssize_t)to_read;
}

int tfs_unlink(char const *target) {
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
        ALWAYS_ASSERT(root_dir_inode != NULL,"tfs_open: root dir inode must exist");
    int target_inumber = tfs_lookup(target, root_dir_inode);
    if(target_inumber == -1){
        return -1;
    }
    inode_t *target_inode = inode_get(target_inumber);
    if (target_inode->i_node_type == T_DIRECTORY){
        return -1;
    }
    
    if(find_open_file(target_inumber) == 0){//if the file is open, we dont allow to delete it
        return -1;
    }
    pthread_rwlock_wrlock(&target_inode->rwlock);
    target_inode->hard_link_ctr--;
    int unlock = 0;
    if(target_inode->hard_link_ctr <= 0 ){
        unlock = 1;
        pthread_rwlock_unlock(&target_inode->rwlock);
        inode_delete(target_inumber);
        // skip the initial '/' character
        target++;
        if(clear_dir_entry(root_dir_inode,target) == -1){
            return -1;
        }
    }
    if(unlock == 0)pthread_rwlock_unlock(&target_inode->rwlock);
    return 0;
}

int tfs_copy_from_external_fs(char const *source_path, char const *dest_path) {
    char buffer[state_block_size()];
    FILE *source_fp = fopen(source_path, "r");

    if(source_fp == NULL){
        return -1;
    }
    
    /* Seek to the beginning of the file */
    fseek(source_fp, 0, SEEK_SET);
    /* read the contents of the file */
    size_t bytes_read = fread(buffer, sizeof(char),state_block_size(),source_fp);
    fgetc(source_fp);//advance 1 character to check if its the end of file in limit case
    if(!feof(source_fp)){
        return -1;//too big to copy fully
    }
    
    fclose(source_fp);

    int dest_file_handle = tfs_open(dest_path, TFS_O_TRUNC | TFS_O_CREAT);
    if(dest_file_handle == -1){
        return -1;
    }
    if (bytes_read == 0){
        tfs_close(dest_file_handle);//nothing to copy, so close it and exit
        return 0;
    }

    if(tfs_write(dest_file_handle, buffer, bytes_read) == -1){
        return -1;
    }

    tfs_close(dest_file_handle);

    return 0;
}

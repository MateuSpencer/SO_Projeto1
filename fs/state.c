#include "state.h"
#include "betterassert.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

/*
 * Persistent FS state
 * (in reality, it should be maintained in secondary memory;
 * for simplicity, this project maintains it in primary memory).
 */
static tfs_params fs_params;

// Inode table
static inode_t *inode_table;
static allocation_state_t *freeinode_ts;
static pthread_rwlock_t freeinode_ts_rwlock;


// Data blocks
static char *fs_data; // # blocks * block size
static allocation_state_t *free_blocks;
static pthread_rwlock_t free_blocks_rwlock;

/*
 * Volatile FS state
 */
static open_file_entry_t *open_file_table;
static allocation_state_t *free_open_file_entries;
static pthread_rwlock_t free_open_file_entries_rwlock;

// Convenience macros
#define INODE_TABLE_SIZE (fs_params.max_inode_count)
#define DATA_BLOCKS (fs_params.max_block_count)
#define MAX_OPEN_FILES (fs_params.max_open_files_count)
#define BLOCK_SIZE (fs_params.block_size)
#define MAX_DIR_ENTRIES (BLOCK_SIZE / sizeof(dir_entry_t))

static inline bool valid_inumber(int inumber) {
    return inumber >= 0 && inumber < INODE_TABLE_SIZE;
}

static inline bool valid_block_number(int block_number) {
    return block_number >= 0 && block_number < DATA_BLOCKS;
}

static inline bool valid_file_handle(int file_handle) {
    return file_handle >= 0 && file_handle < MAX_OPEN_FILES;
}

size_t state_block_size(void) { return BLOCK_SIZE; }

/**
 * Do nothing, while preventing the compiler from performing any optimizations.
 *
 * We need to defeat the optimizer for the insert_delay() function.
 * Under optimization, the empty loop would be completely optimized away.
 * This function tells the compiler that the assembly code being run (which is
 * none) might potentially change *all memory in the process*.
 *
 * This prevents the optimizer from optimizing this code away, because it does
 * not know what it does and it may have side effects.
 *
 * Reference with more information: https://youtu.be/nXaxk27zwlk?t=2775
 *
 * Exercise: try removing this function and look at the assembly generated to
 * compare.
 */
static void touch_all_memory(void) { __asm volatile("" : : : "memory"); }

/**
 * Artifically delay execution (busy loop).
 *
 * Auxiliary function to insert a delay.
 * Used in accesses to persistent FS state as a way of emulating access
 * latencies as if such data structures were really stored in secondary memory.
 */
static void insert_delay(void) {
    for (int i = 0; i < DELAY; i++) {
        touch_all_memory();
    }
}

/**
 * Initialize FS state.
 *
 * Input:
 *   - params: TécnicoFS parameters
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - TFS already initialized.
 *   - malloc failure when allocating TFS structures.
 */
int state_init(tfs_params params) {
    fs_params = params;

    if (inode_table != NULL) {
        return -1; // already initialized
    }

    inode_table = malloc(INODE_TABLE_SIZE * sizeof(inode_t));
    freeinode_ts = malloc(INODE_TABLE_SIZE * sizeof(allocation_state_t));
    fs_data = malloc(DATA_BLOCKS * BLOCK_SIZE);
    free_blocks = malloc(DATA_BLOCKS * sizeof(allocation_state_t));
    open_file_table = malloc(MAX_OPEN_FILES * sizeof(open_file_entry_t));
    free_open_file_entries = malloc(MAX_OPEN_FILES * sizeof(allocation_state_t));

    if (!inode_table || !freeinode_ts || !fs_data || !free_blocks ||
        !open_file_table || !free_open_file_entries) {
        return -1; // allocation failed
    }

    for (size_t i = 0; i < INODE_TABLE_SIZE; i++) {
        freeinode_ts[i] = FREE;
        pthread_rwlock_init(&inode_table[i].rwlock,NULL);
    }
    pthread_rwlock_init(&freeinode_ts_rwlock,NULL);

    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        free_blocks[i] = FREE;
    }
    pthread_rwlock_init(&free_blocks_rwlock, NULL);

    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        pthread_mutex_init(&open_file_table[i].mutexlock, NULL);
        free_open_file_entries[i] = FREE;
    }
    pthread_rwlock_init(&free_open_file_entries_rwlock, NULL);

    return 0;
}

/**
 * Destroy FS state.
 *
 * Returns 0 if succesful, -1 otherwise.
 */
int state_destroy(void) {
    free(inode_table);
    free(freeinode_ts);
    free(fs_data);
    free(free_blocks);
    free(open_file_table);
    free(free_open_file_entries);

    pthread_rwlock_destroy(&freeinode_ts_rwlock);
    pthread_rwlock_destroy(&free_blocks_rwlock);
    pthread_rwlock_destroy(&free_open_file_entries_rwlock);
    
    for (size_t i = 0; i < INODE_TABLE_SIZE; i++) {
        pthread_rwlock_destroy(&inode_table[i].rwlock);
    }
    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        pthread_mutex_destroy(&open_file_table[i].mutexlock);
    }

    inode_table = NULL;
    freeinode_ts = NULL;
    fs_data = NULL;
    free_blocks = NULL;
    open_file_table = NULL;
    free_open_file_entries = NULL;

    return 0;
}

/**
 * (Try to) Allocate a new inode in the inode table, without initializing its
 * data.
 *
 * Returns the inumber of the newly allocated inode, or -1 in the case of error.
 *
 * Possible errors:
 *   - No free slots in inode table.
 */
static int inode_alloc(void) {
    pthread_rwlock_rdlock(&freeinode_ts_rwlock);
    for (size_t inumber = 0; inumber < INODE_TABLE_SIZE; inumber++) {
        if ((inumber * sizeof(allocation_state_t) % BLOCK_SIZE) == 0) {
            insert_delay(); // simulate storage access delay (to freeinode_ts)
        }

        // Finds first free entry in inode table
        if (freeinode_ts[inumber] == FREE) {
            pthread_rwlock_unlock(&freeinode_ts_rwlock);//unlocks read lock to make a write block
            pthread_rwlock_wrlock(&freeinode_ts_rwlock);
            if (freeinode_ts[inumber] != FREE) {//re check if it is still free since another thread might have changed it
                pthread_rwlock_wrlock(&freeinode_ts_rwlock);
                pthread_rwlock_rdlock(&freeinode_ts_rwlock);
                continue; //advance in the loop
            }
            //  Found a free entry, so takes it for the new inode
            freeinode_ts[inumber] = TAKEN;
            pthread_rwlock_unlock(&freeinode_ts_rwlock);
            return (int)inumber;
        }
    }
    pthread_rwlock_unlock(&freeinode_ts_rwlock);
    // no free inodes
    return -1;
}

/**
 * Create a new inode in the inode table.
 *
 * Allocates and initializes a new inode.
 * Directories will have their data block allocated and initialized, with i_size
 * set to BLOCK_SIZE. Regular files will not have their data block allocated
 * (i_size will be set to 0, i_data_block to -1).
 *
 * Input:
 *   - i_type: the type of the node (file or directory)
 *
 * Returns inumber of the new inode, or -1 in the case of error.
 *
 * Possible errors:
 *   - No free slots in inode table.
 *   - (if creating a directory) No free data blocks.
 */
int inode_create(inode_type i_type) {
    int inumber = inode_alloc();
    if (inumber == -1) {
        return -1; // no free slots in inode table
    }

    inode_t *inode = &inode_table[inumber];
    pthread_rwlock_wrlock(&inode->rwlock);
    insert_delay(); // simulate storage access delay (to inode)
    inode->i_node_type = i_type;
    switch (i_type) {
    case T_DIRECTORY: {
        // Initializes directory (filling its block with empty entries, labeled
        // with inumber==-1)
        int b = data_block_alloc();
        if (b == -1) {
            // ensure fields are initialized
            inode->i_size = 0;
            inode->i_data_block = -1;
            pthread_rwlock_unlock(&inode->rwlock);
            // run regular deletion process
            inode_delete(inumber);
            return -1;
        }

        inode_table[inumber].i_size = BLOCK_SIZE;
        inode_table[inumber].i_data_block = b;
        inode_table[inumber].hard_link_ctr = 1;

        dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(b);
        ALWAYS_ASSERT(dir_entry != NULL,"inode_create: data block freed while in use");

        for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
            dir_entry[i].d_inumber = -1;
        }
    } break;
    case T_FILE:
        // In case of a new file, simply sets its size to 0
        inode_table[inumber].i_size = 0;
        inode_table[inumber].i_data_block = -1;
        inode_table[inumber].hard_link_ctr = 1;
        break;
    case T_SYMLINK: {
        inode_table[inumber].i_size = 0;
        inode_table[inumber].i_data_block = -1;
        inode_table[inumber].hard_link_ctr = 1;
    } break;
    default:
        PANIC("inode_create: unknown file type");
    }
    pthread_rwlock_unlock(&inode->rwlock);
    return inumber;
}

/**
 * Delete an inode.
 *
 * Input:
 *   - inumber: inode's number
 */
void inode_delete(int inumber) {
    // simulate storage access delay (to inode and freeinode_ts)
    insert_delay();
    insert_delay();

    ALWAYS_ASSERT(valid_inumber(inumber), "inode_delete: invalid inumber");
    pthread_rwlock_wrlock(&freeinode_ts_rwlock);
    ALWAYS_ASSERT(freeinode_ts[inumber] == TAKEN,"inode_delete: inode already freed");
    pthread_rwlock_rdlock(&inode_table[inumber].rwlock);
    if (inode_table[inumber].i_size > 0) {
        data_block_free(inode_table[inumber].i_data_block);
    }
    pthread_rwlock_unlock(&inode_table[inumber].rwlock);
    freeinode_ts[inumber] = FREE;
    pthread_rwlock_unlock(&freeinode_ts_rwlock);
}

/**
 * Obtain a pointer to an inode from its inumber.
 *
 * Input:
 *   - inumber: inode's number
 *
 * Returns pointer to inode.
 */
inode_t *inode_get(int inumber) {
    ALWAYS_ASSERT(valid_inumber(inumber), "inode_get: invalid inumber");

    insert_delay(); // simulate storage access delay to inode
    return &inode_table[inumber];
}

/**
 * Clear the directory entry associated with a sub file.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - Directory does not contain an entry for sub_name.
 */
int clear_dir_entry(inode_t *inode, char const *sub_name) {
    insert_delay();
    pthread_rwlock_wrlock(&inode->rwlock);
    if (inode->i_node_type != T_DIRECTORY) {
        return -1; // not a directory
        pthread_rwlock_unlock(&inode->rwlock);
    }

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,"clear_dir_entry: directory must have a data block");
    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (!strcmp(dir_entry[i].d_name, sub_name)) {
            dir_entry[i].d_inumber = -1;
            memset(dir_entry[i].d_name, 0, MAX_FILE_NAME);
            pthread_rwlock_unlock(&inode->rwlock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&inode->rwlock);
    return -1; // sub_name not found
}

/**
 * Store the inumber for a sub file in a directory.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *   - sub_inumber: inumber of the sub inode
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - sub_name is not a valid file name (length 0 or > MAX_FILE_NAME - 1).
 *   - Directory is already full of entries.
 */
int add_dir_entry(inode_t *inode, char const *sub_name, int sub_inumber) {
    if (strlen(sub_name) == 0 || strlen(sub_name) > MAX_FILE_NAME - 1) {
        return -1; // invalid sub_name
    }

    insert_delay(); // simulate storage access delay to inode with inumber
    pthread_rwlock_wrlock(&inode->rwlock);
    if (inode->i_node_type != T_DIRECTORY) {
        pthread_rwlock_unlock(&inode->rwlock);
        return -1; // not a directory
    }

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,"add_dir_entry: directory must have a data block");

    // Finds and fills the first empty entry
    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir_entry[i].d_inumber == -1) {
            dir_entry[i].d_inumber = sub_inumber;
            strncpy(dir_entry[i].d_name, sub_name, MAX_FILE_NAME - 1);
            dir_entry[i].d_name[MAX_FILE_NAME - 1] = '\0';
            pthread_rwlock_unlock(&inode->rwlock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&inode->rwlock);
    return -1; // no space for entry
}

/**
 * Obtain the inumber for a sub file inside a directory.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *
 * Returns inumber linked to the target name, -1 if errors occur.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - Directory does not contain a file named sub_name.
 */
int find_in_dir(inode_t *inode, char const *sub_name) {
    ALWAYS_ASSERT(inode != NULL, "find_in_dir: inode must be non-NULL");
    ALWAYS_ASSERT(sub_name != NULL, "find_in_dir: sub_name must be non-NULL");

    insert_delay(); // simulate storage access delay to inode with inumber
    pthread_rwlock_rdlock(&inode->rwlock);
    if (inode->i_node_type != T_DIRECTORY) {
        pthread_rwlock_unlock(&inode->rwlock);
        return -1; // not a directory
    }

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,"find_in_dir: directory inode must have a data block");

    // Iterates over the directory entries looking for one that has the target
    // name
    for (int i = 0; i < MAX_DIR_ENTRIES; i++)
        if ((dir_entry[i].d_inumber != -1) &&
            (strncmp(dir_entry[i].d_name, sub_name, MAX_FILE_NAME) == 0)) {

            int sub_inumber = dir_entry[i].d_inumber;
            pthread_rwlock_unlock(&inode->rwlock);
            return sub_inumber;
        }
    pthread_rwlock_unlock(&inode->rwlock);
    return -1; // entry not found
}

/**
 * Allocate a new data block.
 *
 * Returns block number/index if successful, -1 otherwise.
 *
 * Possible errors:
 *   - No free data blocks.
 */
int data_block_alloc(void) {
    pthread_rwlock_rdlock(&free_blocks_rwlock);
    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        if (i * sizeof(allocation_state_t) % BLOCK_SIZE == 0) {
            insert_delay(); // simulate storage access delay to free_blocks
        }

        if (free_blocks[i] == FREE) {
            pthread_rwlock_unlock(&free_blocks_rwlock);
            pthread_rwlock_wrlock(&free_blocks_rwlock);
            if (free_blocks[i] != FREE) {
                pthread_rwlock_unlock(&free_blocks_rwlock);
                pthread_rwlock_rdlock(&free_blocks_rwlock);
                continue;
            }
            free_blocks[i] = TAKEN;
            pthread_rwlock_unlock(&free_blocks_rwlock);
            return (int)i;
        }
    }
    pthread_rwlock_unlock(&free_blocks_rwlock);
    return -1;
}

/**
 * Free a data block.
 *
 * Input:
 *   - block_number: the block number/index
 */
void data_block_free(int block_number) {
    ALWAYS_ASSERT(valid_block_number(block_number),"data_block_free: invalid block number");

    insert_delay(); // simulate storage access delay to free_blocks
    pthread_rwlock_wrlock(&free_blocks_rwlock);
    free_blocks[block_number] = FREE;
    pthread_rwlock_unlock(&free_blocks_rwlock);
}

/**
 * Obtain a pointer to the contents of a given block.
 *
 * Input:
 *   - block_number: the block number/index
 *
 * Returns a pointer to the first byte of the block.
 */
void *data_block_get(int block_number) {
    ALWAYS_ASSERT(valid_block_number(block_number),"data_block_get: invalid block number");

    insert_delay(); // simulate storage access delay to block
    return &fs_data[(size_t)block_number * BLOCK_SIZE];
}

/**
 * Add a new entry to the open file table.
 *
 * Input:
 *   - inumber: inode number of the file to open
 *   - offset: initial offset
 *
 * Returns file handle if successful, -1 otherwise.
 *
 * Possible errors:
 *   - No space in open file table for a new open file.
 */
int add_to_open_file_table(int inumber, size_t offset) {
    pthread_rwlock_rdlock(&free_open_file_entries_rwlock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (free_open_file_entries[i] == FREE) {
            pthread_rwlock_unlock(&free_open_file_entries_rwlock);
            pthread_rwlock_wrlock(&free_open_file_entries_rwlock);
            if(free_open_file_entries[i] != FREE){
                pthread_rwlock_unlock(&free_open_file_entries_rwlock);
                pthread_rwlock_rdlock(&free_open_file_entries_rwlock);
                continue;
            }
            free_open_file_entries[i] = TAKEN;
            pthread_rwlock_unlock(&free_open_file_entries_rwlock);
            pthread_mutex_lock(&open_file_table[i].mutexlock);
            open_file_table[i].of_inumber = inumber;
            open_file_table[i].of_offset = offset;
            pthread_mutex_unlock(&open_file_table[i].mutexlock);
            return i;
        }
    }
    pthread_rwlock_unlock(&free_open_file_entries_rwlock);
    return -1;
}

/**
 * Free an entry from the open file table.
 *
 * Input:
 *   - fhandle: file handle to free/close
 */
void remove_from_open_file_table(int fhandle) {
    ALWAYS_ASSERT(valid_file_handle(fhandle),"remove_from_open_file_table: file handle must be valid");
    ALWAYS_ASSERT(free_open_file_entries[fhandle] == TAKEN,"remove_from_open_file_table: file handle must be taken");
    pthread_rwlock_wrlock(&free_open_file_entries_rwlock);
    free_open_file_entries[fhandle] = FREE;
    pthread_rwlock_unlock(&free_open_file_entries_rwlock);
}

/**
 * Obtain pointer to a given entry in the open file table.
 *
 * Input:
 *   - fhandle: file handle
 *
 * Returns pointer to the entry, or NULL if the fhandle is invalid/closed/never
 * opened.
 */
open_file_entry_t *get_open_file_entry(int fhandle) {
    pthread_rwlock_rdlock(&free_open_file_entries_rwlock);
    if (!valid_file_handle(fhandle)) {
        pthread_rwlock_unlock(&free_open_file_entries_rwlock);
        return NULL;
    }

    if (free_open_file_entries[fhandle] != TAKEN) {
        pthread_rwlock_unlock(&free_open_file_entries_rwlock);
        return NULL;
    }
    pthread_rwlock_unlock(&free_open_file_entries_rwlock);
    return &open_file_table[fhandle];
}


/**
 * see if a file is open
 *
 * Input:
 *   - inumber: inode inumber
 *
 * Returns 0 if it is open, -1 otherwise
 */
int find_open_file (int inumber){
    pthread_rwlock_rdlock(&free_open_file_entries_rwlock);
    for(int i = 0; i > MAX_OPEN_FILES; i++){
        if (free_open_file_entries[i] == TAKEN){
            pthread_mutex_lock(&open_file_table[i].mutexlock);
            if(open_file_table[i].of_inumber == inumber ) {
            pthread_mutex_unlock(&open_file_table[i].mutexlock);
            pthread_rwlock_unlock(&free_open_file_entries_rwlock);
            return 0;
            }
            pthread_mutex_unlock(&open_file_table[i].mutexlock);
        }
        
    }
    pthread_rwlock_unlock(&free_open_file_entries_rwlock);
    return -1;
}

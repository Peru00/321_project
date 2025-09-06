// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                 // 0x4D565346
    uint32_t version;               // 1
    uint32_t block_size;            // 4096
    uint64_t total_blocks;          // size_kib * 1024 / 4096
    uint64_t inode_count;           // number of inodes
    uint64_t inode_bitmap_start;    // block number where inode bitmap starts
    uint64_t inode_bitmap_blocks;   // number of blocks for inode bitmap
    uint64_t data_bitmap_start;     // block number where data bitmap starts
    uint64_t data_bitmap_blocks;    // number of blocks for data bitmap
    uint64_t inode_table_start;     // block number where inode table starts
    uint64_t inode_table_blocks;    // number of blocks for inode table
    uint64_t data_region_start;     // block number where data region starts
    uint64_t data_region_blocks;    // number of blocks for data region
    uint64_t root_inode;            // 1
    uint64_t mtime_epoch;           // build time
    uint32_t flags;                 // 0
    uint32_t checksum;              // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;          // file type: 0100000 (octal) for files, 0040000 (octal) for dirs
    uint16_t links;         // number of directories pointing to this inode
    uint32_t uid;           // user id (0)
    uint32_t gid;           // group id (0)
    uint64_t size_bytes;    // size in bytes
    uint64_t atime;         // access time
    uint64_t mtime;         // modify time
    uint64_t ctime;         // create time
    uint32_t direct[12];    // direct block pointers
    uint32_t reserved_0;    // 0
    uint32_t reserved_1;    // 0
    uint32_t reserved_2;    // 0
    uint32_t proj_id;       // your group ID
    uint32_t uid16_gid16;   // 0
    uint64_t xattr_ptr;     // 0
    uint64_t inode_crc;     // checksum
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;      // inode number (0 if free)
    uint8_t  type;          // 1=file, 2=dir
    char     name[58];      // filename
    uint8_t  checksum;      // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

int main(int argc, char** argv) {
    crc32_init();
    
    // Parse CLI parameters with proper flags
    if (argc != 7) {
        fprintf(stderr, "Usage: %s --image <file> --size-kib <180..4096> --inodes <128..512>\n", argv[0]);
        return 1;
    }
    
    const char* image_file = NULL;
    uint64_t size_kib = 0;
    uint64_t inode_count = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "--image") == 0) {
            image_file = argv[i + 1];
        } else if (strcmp(argv[i], "--size-kib") == 0) {
            size_kib = strtoull(argv[i + 1], NULL, 10);
        } else if (strcmp(argv[i], "--inodes") == 0) {
            inode_count = strtoull(argv[i + 1], NULL, 10);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }
    
    // Validate required arguments
    if (!image_file || size_kib == 0 || inode_count == 0) {
        fprintf(stderr, "All arguments are required\n");
        return 1;
    }
    
    // Validate ranges
    if (size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0) {
        fprintf(stderr, "Error: size-kib must be between 180-4096 and multiple of 4\n");
        return 1;
    }
    
    if (inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Error: inodes must be between 128 and 512\n");
        return 1;
    }
    
    // Calculate filesystem parameters
    uint64_t total_blocks = size_kib * 1024 / BS;
    
    // Layout: superblock(1) + inode_bitmap(1) + data_bitmap(1) + inode_table + data
    uint64_t inode_bitmap_start = 1;
    uint64_t data_bitmap_start = 2;
    uint64_t inode_table_start = 3;
    
    // Calculate inode table size
    uint64_t inode_table_bytes = inode_count * INODE_SIZE;
    uint64_t inode_table_blocks = (inode_table_bytes + BS - 1) / BS;
    
    uint64_t data_region_start = inode_table_start + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;
    
    if (data_region_blocks < 1) {
        fprintf(stderr, "Error: no space for data blocks\n");
        return 1;
    }
    
    // Create filesystem image in memory
    size_t image_size = total_blocks * BS;
    uint8_t* image = calloc(1, image_size);
    if (!image) {
        perror("calloc");
        return 1;
    }
    
    time_t now = time(NULL);
    
    // Create superblock  
    superblock_t* sb = (superblock_t*)image;
    sb->magic = 0x4653564D;  // This will store as 4D 56 53 46 in little-endian
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inode_count;
    sb->inode_bitmap_start = inode_bitmap_start;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = data_bitmap_start;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = inode_table_start;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = data_region_start;
    sb->data_region_blocks = data_region_blocks;
    sb->root_inode = 1;
    sb->mtime_epoch = (uint64_t)now;
    sb->flags = 0;
    
    // Set up bitmaps
    uint8_t* inode_bitmap = image + inode_bitmap_start * BS;
    uint8_t* data_bitmap = image + data_bitmap_start * BS;
    
    // Mark root inode as used (inode #1 = bit 0)
    inode_bitmap[0] |= 0x01;
    
    // Mark first data block as used for root directory
    data_bitmap[0] |= 0x01;
    
    // Create root inode
    inode_t* root_inode = (inode_t*)(image + inode_table_start * BS);
    root_inode->mode = 0040000;  // This is correct: 040000 octal = 16384 decimal = 0x4000
    root_inode->links = 2;      // "." and ".."
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->size_bytes = 128; // 2 directory entries * 64 bytes
    root_inode->atime = (uint64_t)now;
    root_inode->mtime = (uint64_t)now;
    root_inode->ctime = (uint64_t)now;
    root_inode->direct[0] = data_region_start;  // ABSOLUTE block number
    for (int i = 1; i < 12; i++) {
        root_inode->direct[i] = 0; // unused
    }
    root_inode->reserved_0 = 0;
    root_inode->reserved_1 = 0;
    root_inode->reserved_2 = 0;
    root_inode->proj_id = 5;  // Fixed group ID
    root_inode->uid16_gid16 = 0;
    root_inode->xattr_ptr = 0;
    
    // Create root directory entries
    uint8_t* root_dir_block = image + data_region_start * BS;
    
    // "." entry
    dirent64_t* dot_entry = (dirent64_t*)root_dir_block;
    dot_entry->inode_no = 1;
    dot_entry->type = 2; // directory
    memset(dot_entry->name, 0, 58);
    strcpy(dot_entry->name, ".");
    
    // ".." entry
    dirent64_t* dotdot_entry = (dirent64_t*)(root_dir_block + 64);
    dotdot_entry->inode_no = 1;
    dotdot_entry->type = 2; // directory
    memset(dotdot_entry->name, 0, 58);
    strcpy(dotdot_entry->name, "..");
    
    // Finalize checksums
    dirent_checksum_finalize(dot_entry);
    dirent_checksum_finalize(dotdot_entry);
    inode_crc_finalize(root_inode);
    superblock_crc_finalize(sb);
    
    // Write to file
    FILE* f = fopen(image_file, "wb");
    if (!f) {
        perror("fopen");
        free(image);
        return 1;
    }
    
    if (fwrite(image, 1, image_size, f) != image_size) {
        perror("fwrite");
        fclose(f);
        free(image);
        return 1;
    }
    
    fclose(f);
    free(image);
    
    printf("MiniVSFS created: %s\n", image_file);
    printf("Size: %lu KiB (%lu blocks)\n", size_kib, total_blocks);
    printf("Inodes: %lu\n", inode_count);
    
    return 0;
}

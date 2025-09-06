#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <sys/stat.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
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

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

// Helper function to find first free bit in bitmap
static int find_free_bit(uint8_t* bitmap, size_t bitmap_size_bits) {
    for (size_t byte = 0; byte < (bitmap_size_bits + 7) / 8; byte++) {
        if (bitmap[byte] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(bitmap[byte] & (1 << bit))) {
                    size_t bit_pos = byte * 8 + bit;
                    if (bit_pos < bitmap_size_bits) {
                        return bit_pos;
                    }
                }
            }
        }
    }
    return -1;
}

// Helper function to set a bit in bitmap
static void set_bit(uint8_t* bitmap, int bit_pos) {
    bitmap[bit_pos / 8] |= (1 << (bit_pos % 8));
}

int main(int argc, char** argv) {
    crc32_init();
    
    // Parse CLI parameters
    if (argc != 7) {
        fprintf(stderr, "Usage: %s --input <file> --output <file> --file <file>\n", argv[0]);
        return 1;
    }
    
    const char* input_file = NULL;
    const char* output_file = NULL;
    const char* add_file = NULL;
    
    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "--input") == 0) {
            input_file = argv[i + 1];
        } else if (strcmp(argv[i], "--output") == 0) {
            output_file = argv[i + 1];
        } else if (strcmp(argv[i], "--file") == 0) {
            add_file = argv[i + 1];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }
    
    if (!input_file || !output_file || !add_file) {
        fprintf(stderr, "All arguments are required\n");
        return 1;
    }
    
    // Check if file to add exists
    struct stat file_stat;
    if (stat(add_file, &file_stat) != 0) {
        fprintf(stderr, "Error: file '%s' not found\n", add_file);
        return 1;
    }
    
    if (!S_ISREG(file_stat.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a regular file\n", add_file);
        return 1;
    }
    
    size_t file_size = file_stat.st_size;
    
    // Check filename length (must fit in 58 characters including null terminator)
    if (strlen(add_file) > 57) {
        fprintf(stderr, "Error: filename too long (max 57 characters)\n");
        return 1;
    }
    
    // Check if file is too large (12 direct blocks max)
    size_t max_file_size = 12 * BS;
    if (file_size > max_file_size) {
        fprintf(stderr, "Error: file too large (max %zu bytes)\n", max_file_size);
        return 1;
    }
    
    // Read input filesystem image
    FILE* input_fp = fopen(input_file, "rb");
    if (!input_fp) {
        perror("fopen input");
        return 1;
    }
    
    // Get file size
    fseek(input_fp, 0, SEEK_END);
    size_t image_size = ftell(input_fp);
    fseek(input_fp, 0, SEEK_SET);
    
    // Allocate memory for image
    uint8_t* image = malloc(image_size);
    if (!image) {
        perror("malloc");
        fclose(input_fp);
        return 1;
    }
    
    // Read entire image
    if (fread(image, 1, image_size, input_fp) != image_size) {
        fprintf(stderr, "Error reading input file\n");
        free(image);
        fclose(input_fp);
        return 1;
    }
    fclose(input_fp);
    
    // Parse superblock
    superblock_t* sb = (superblock_t*)image;
    
    // Verify magic number
    if (sb->magic != 0x4653564D) {
        fprintf(stderr, "Error: invalid filesystem magic number\n");
        free(image);
        return 1;
    }
    
    // Get pointers to filesystem structures
    uint8_t* inode_bitmap = image + sb->inode_bitmap_start * BS;
    uint8_t* data_bitmap = image + sb->data_bitmap_start * BS;
    inode_t* inode_table = (inode_t*)(image + sb->inode_table_start * BS);
    uint8_t* data_region = image + sb->data_region_start * BS;
    
    // Find free inode
    int free_inode = find_free_bit(inode_bitmap, sb->inode_count);
    if (free_inode == -1) {
        fprintf(stderr, "Error: no free inodes\n");
        free(image);
        return 1;
    }
    
    // Calculate number of data blocks needed
    size_t blocks_needed = 0;
    if (file_size > 0) {
        blocks_needed = (file_size + BS - 1) / BS;
        if (blocks_needed > 12) {
            fprintf(stderr, "Error: file requires too many blocks\n");
            free(image);
            return 1;
        }
    }
    
    // Find free data blocks (only if file is not empty)
    uint32_t data_blocks[12] = {0};
    if (blocks_needed > 0) {
        for (size_t i = 0; i < blocks_needed; i++) {
            int free_block = find_free_bit(data_bitmap, sb->data_region_blocks);
            if (free_block == -1) {
                fprintf(stderr, "Error: no free data blocks\n");
                free(image);
                return 1;
            }
            data_blocks[i] = sb->data_region_start + free_block;
            set_bit(data_bitmap, free_block);
        }
    }
    
    // Read file content and copy to filesystem blocks
    if (blocks_needed > 0) {
        FILE* add_fp = fopen(add_file, "rb");
        if (!add_fp) {
            perror("fopen add_file");
            free(image);
            return 1;
        }
        
        // Copy file data to filesystem blocks
        for (size_t i = 0; i < blocks_needed; i++) {
            uint8_t* block_ptr = image + data_blocks[i] * BS;
            size_t bytes_to_read = (i == blocks_needed - 1) ? 
                (file_size - i * BS) : BS;
            
            if (fread(block_ptr, 1, bytes_to_read, add_fp) != bytes_to_read) {
                fprintf(stderr, "Error reading file data\n");
                fclose(add_fp);
                free(image);
                return 1;
            }
            
            // Zero-pad the last block if needed
            if (bytes_to_read < BS) {
                memset(block_ptr + bytes_to_read, 0, BS - bytes_to_read);
            }
        }
        fclose(add_fp);
    }
    
    // Create new inode
    time_t now = time(NULL);
    inode_t* new_inode = &inode_table[free_inode];
    memset(new_inode, 0, sizeof(inode_t));
    
    new_inode->mode = 0100000;  // Regular file
    new_inode->links = 1;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size_bytes = file_size;
    new_inode->atime = now;
    new_inode->mtime = now;
    new_inode->ctime = now;
    
    // Set direct block pointers
    for (size_t i = 0; i < blocks_needed; i++) {
        new_inode->direct[i] = data_blocks[i];
    }
    
    new_inode->proj_id = 5;  // Group ID
    
    // Mark inode as used
    set_bit(inode_bitmap, free_inode);
    
    // Add directory entry to root directory
    inode_t* root_inode = &inode_table[0]; // Root is inode #1, but 0-indexed
    
    // Find free directory entry slot and check for duplicates
    uint8_t* root_data = image + root_inode->direct[0] * BS;
    dirent64_t* entries = (dirent64_t*)root_data;
    
    int entries_per_block = BS / sizeof(dirent64_t);
    int free_entry = -1;
    int used_entries = 0;
    
    // Count used entries and check for duplicates
    for (int i = 0; i < entries_per_block; i++) {
        if (entries[i].inode_no != 0) {
            used_entries++;
            // Check for duplicate filename
            if (strcmp(entries[i].name, add_file) == 0) {
                fprintf(stderr, "Error: file '%s' already exists in filesystem\n", add_file);
                free(image);
                return 1;
            }
        } else if (free_entry == -1) {
            free_entry = i;
        }
    }
    
    if (free_entry == -1) {
        fprintf(stderr, "Error: root directory full\n");
        free(image);
        return 1;
    }
    
    // Create directory entry
    dirent64_t* new_entry = &entries[free_entry];
    new_entry->inode_no = free_inode + 1; // 1-indexed
    new_entry->type = 1; // File
    memset(new_entry->name, 0, 58);
    strncpy(new_entry->name, add_file, 57);
    new_entry->name[57] = '\0'; // Ensure null termination
    
    // Update root inode - only size increases as we add one more entry
    root_inode->size_bytes = (used_entries + 1) * sizeof(dirent64_t);
    root_inode->mtime = now;
    
    // Finalize checksums
    dirent_checksum_finalize(new_entry);
    inode_crc_finalize(new_inode);
    inode_crc_finalize(root_inode);
    superblock_crc_finalize(sb);
    
    // Write output file
    FILE* output_fp = fopen(output_file, "wb");
    if (!output_fp) {
        perror("fopen output");
        free(image);
        return 1;
    }
    
    if (fwrite(image, 1, image_size, output_fp) != image_size) {
        fprintf(stderr, "Error writing output file\n");
        fclose(output_fp);
        free(image);
        return 1;
    }
    
    fclose(output_fp);
    free(image);
    
    printf("Successfully added '%s' to filesystem\n", add_file);
    return 0;
}

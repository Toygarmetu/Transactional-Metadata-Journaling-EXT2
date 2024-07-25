#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/fs.h>
#include "ext2fs.h"

#define ROOT_INODE 2
using namespace std;
unsigned char *image;

void tokenize_path(const char *path, char *parent_path, char *fname) {
    int len = strlen(path);
    int i;
    for (i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            break;
        }
    }
    strncpy(parent_path, path, i);
    parent_path[i] = '\0';
    strcpy(fname, path + i + 1);
}

void deallocate_block(int fd, int block_num) {
    char buf[EXT2_SUPER_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    lseek(fd, block_num * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
    write(fd, buf, sizeof(buf));
    struct ext2_super_block sb;
    lseek(fd, 1024, SEEK_SET);
    read(fd, &sb, sizeof(sb));
    sb.free_block_count++;
    lseek(fd, 1024, SEEK_SET);
    write(fd, &sb, sizeof(sb));
    struct ext2_block_group_descriptor gd;
    lseek(fd, 2048, SEEK_SET);
    read(fd, &gd, sizeof(gd));
    gd.free_block_count++;
    lseek(fd, 2048, SEEK_SET);
    write(fd, &gd, sizeof(gd));
}

void deallocate_indirect_block(int fd, int block_num, int level) {
    if (level == 1) {
        int *block_ptrs = (int*)malloc(EXT2_SUPER_BLOCK_SIZE);
        lseek(fd, block_num * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
        read(fd, block_ptrs, EXT2_SUPER_BLOCK_SIZE);
        for (int i = 0; i < (int)(EXT2_SUPER_BLOCK_SIZE / sizeof(int)); i++) {
            if (block_ptrs[i] != 0) {
                deallocate_block(fd, block_ptrs[i]);
            }
        }
        deallocate_block(fd, block_num);
        free(block_ptrs);
    } else {
        int *block_ptrs = (int *)malloc(EXT2_SUPER_BLOCK_SIZE);
        lseek(fd, block_num * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
        read(fd, block_ptrs, EXT2_SUPER_BLOCK_SIZE);
        for (int i = 0; i < (int)(EXT2_SUPER_BLOCK_SIZE / sizeof(int)); i++) {
            if (block_ptrs[i] != 0) {
                deallocate_indirect_block(fd, block_ptrs[i], level - 1);
            }
        }
        deallocate_block(fd, block_num);
        free(block_ptrs);
    }
}

int allocate_new_block(int ) {
    struct ext2_block_group_descriptor *group = (struct ext2_block_group_descriptor *)(image + 2 * EXT2_SUPER_BLOCK_SIZE);
	int blocksize = group->free_block_count;

	if(blocksize >= 128)
	{
		fprintf(stderr, "run out of block space\n");
		exit(1);
	}

	unsigned int mask = 1;
	unsigned int *bitmap = (unsigned int *)(image + group->block_bitmap * EXT2_SUPER_BLOCK_SIZE);

	for(int i = 0; i < 4; i++)
	{
		int b_bit = *(bitmap + i);
		for(int j = 0; j < 32; j++, b_bit >>= 1)
		{
			if((b_bit & mask) == 0)
			{
				*(bitmap + i) |= (1 << j);
				return i * 32 + j + 1;
			}
		}
	}

	return -1;
}



char write2ParentDirBlock(int fd, int nparent_index, int ndir_index, char *dir_name)
{
	struct ext2_block_group_descriptor *group = (struct ext2_block_group_descriptor *)(image + 2 * EXT2_SUPER_BLOCK_SIZE);
	struct ext2_inode *nhead = (struct ext2_inode *)(image + group->inode_table * EXT2_SUPER_BLOCK_SIZE);
	struct ext2_inode *pinode = (struct ext2_inode *)(nhead + nparent_index - 1);

	int i = 0;

	//printf("%d\n", pinode->direct_blocks[0]);
	while(i + 1 < 12 && pinode->direct_blocks[i + 1] != 0) i++;
	//printf("%d %d\n", i, pinode->direct_blocks[i]);

	int rest_length = 0;
	int last_length;
	int require_length = 0;
	int acc_len = 0;
	//printf("%s\n", dir_name);
	int dir_name_len = strlen(dir_name);
	struct ext2_dir_entry* pde = (struct ext2_dir_entry *)(image + pinode->direct_blocks[i] * EXT2_SUPER_BLOCK_SIZE);
	while(acc_len < EXT2_SUPER_BLOCK_SIZE)
	{
		pde = pde + rest_length;
		//printf("%d %d\n", rest_length, acc_len);
		rest_length = pde->length;
		acc_len += rest_length;
	}
	//printf("xd\n");
	last_length = 8 + pde->name_length / 4 + ((pde->name_length % 4 == 0) ? 0 : 1);
	//printf("shit: %d %d\n", last_length, pde->length);
	rest_length -= last_length;
	require_length = 8 + dir_name_len / 4 + ((dir_name_len % 4 == 0) ? 0 : 1);
	//printf("%d %d\n", 8, dir_name_len);

	//printf("%d %d %d\n", require_length, rest_length, last_length);
	if(require_length <= rest_length)
	{
		pde->length = last_length;
		pde = pde + last_length;
		strcpy(pde->name, dir_name);
		pde->inode = ndir_index;
		pde->name_length = strlen(dir_name);
		pde->length = EXT2_SUPER_BLOCK_SIZE;
		pde->file_type = 2;
	}
	else
	{
		int block_new = allocate_new_block(fd);
		pinode->direct_blocks[i + 1] = block_new;
		pde = (ext2_dir_entry *)image + block_new * EXT2_SUPER_BLOCK_SIZE;
		strcpy(pde->name, dir_name);
		pde->inode = ndir_index;
		pde->name_length = strlen(dir_name);
		pde->length = EXT2_SUPER_BLOCK_SIZE;
		pde->file_type = 2;
	}
	return 1;
}

char *get_DirName_ParentDir_Idx(int *i_pdir_idx, char *path)
{
	struct ext2_block_group_descriptor *group = (struct ext2_block_group_descriptor *)(image + 2 * EXT2_SUPER_BLOCK_SIZE);
	struct ext2_inode *nhead = (struct ext2_inode *)(image + group->inode_table * EXT2_SUPER_BLOCK_SIZE);
	int search_inode = 2;
	char *pch = strtok(path, "/");
	char *next_pch = strtok(NULL, "/");
    printf("pch: %s\n", pch);
	
	while(pch != NULL)
	{
		struct ext2_inode *pinode = nhead + search_inode - 1;
		
		char dir_finded = 0;
		int i;
        printf("%d\n", (unsigned int)pinode->direct_blocks[0]);

		//printf("%d\n", pinode->creation_time);
		for(int  j = 0; j < 12; j++) printf("%d ", pinode->direct_blocks[j]);
		printf("\ninode_table %d\n", group->inode_table);

		for(i = 0; i < 12 && pinode->direct_blocks[i] != 0 && !dir_finded; i++)
		{
			struct ext2_dir_entry *dir_entry = (struct ext2_dir_entry *)(image + pinode->direct_blocks[i] * EXT2_SUPER_BLOCK_SIZE);
            
			int acc_len = 0;
			int length = 0;

			while(acc_len < EXT2_SUPER_BLOCK_SIZE)
			{
				dir_entry =  dir_entry + length;
				length = dir_entry->length;
				acc_len += length;
				//printf("%d\n", dir_entry->inode);
				//printf("rec len: %d inode: %d\n", dir_entry->length, dir_entry->inode);
				//printf("size: %d\n", 8 + dir_entry->name_len);

				//printf("%.*s\n", dir_entry->name_length, dir_entry->name);
				//printf("%s\n", pch);
				if(strncmp(dir_entry->name, pch, dir_entry->name_length) == 0 && strlen(pch) == dir_entry->name_length && dir_entry->file_type == 2)
				{
					dir_finded = 1;
					search_inode = dir_entry->inode;
					break;
				}
			}
		}

		if(dir_finded && next_pch == NULL)
		{
			fprintf(stderr, "directory already exists\n");
            exit(1);
		}
		else if(!dir_finded && next_pch != NULL)
		{
			fprintf(stderr, "Unable to create multi-layers directorie\n");
            exit(1);
		}
		else if(!dir_finded && next_pch == NULL)
		{
			break;
		}
		pch = next_pch;
		next_pch = strtok(NULL, "/");

	}
	//printf("parent Inode: %d\n", search_inode);
	*i_pdir_idx = search_inode;
	return pch;
}

void set_bit(unsigned char *bitmap, int bit) {
    int byte_num = bit / 8;
    int bit_num = bit % 8;
    bitmap[byte_num] |= (1 << bit_num);
}

int find_first_zero_bit(unsigned char *bitmap, int size) {
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < 8; j++) {
            if (!(bitmap[i] & (1 << j))) {
                return i * 8 + j;
            }
        }
    }
    return -1;
}

void ext2_rmdir(char *img, char *path) {
    char parent_path[strlen(path) + 1];
    char name[strlen(path) + 1];
    tokenize_path(path, parent_path, name);

    // Check if parent directory exists and is a directory
    struct stat parent_stat;
    if (stat(img, &parent_stat) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }
    /*if (!S_ISDIR(parent_stat.st_mode)) {
        fprintf(stderr, "Parent path is not a directory\n");
        exit(EXIT_FAILURE);
    }*/

    // Open the parent directory and get its inode
    int fd = open(img, O_RDWR);
    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    struct ext2_inode parent_inode;
    lseek(fd, (parent_stat.st_ino - 1) * sizeof(parent_inode), SEEK_SET);
    read(fd, &parent_inode, sizeof(parent_inode));

    // Find the inode of the directory to be removed
    struct ext2_inode inode;
    lseek(fd, (parent_inode.direct_blocks[0] - 1) * sizeof(inode), SEEK_SET);
    read(fd, &inode, sizeof(inode));
    for (int i = 0; i < (int)(inode.size / sizeof(struct ext2_dir_entry)); i++) {
        struct ext2_dir_entry dir_entry;
        lseek(fd, inode.direct_blocks[0] * EXT2_SUPER_BLOCK_SIZE + i * sizeof(dir_entry), SEEK_SET);
        read(fd, &dir_entry, sizeof(dir_entry));
        if (strcmp(name, dir_entry.name) == 0) {
            // Found the directory to be removed
            // Unlink the directory from its parent
            lseek(fd, (parent_inode.direct_blocks[0] - 1) * sizeof(inode), SEEK_SET);
            read(fd, &inode, sizeof(inode));
            for (int j = 0; j < (int)(inode.size / sizeof(struct ext2_dir_entry)); j++) {
                struct ext2_dir_entry parent_entry;
                lseek(fd, inode.direct_blocks[0] * EXT2_SUPER_BLOCK_SIZE + j * sizeof(parent_entry), SEEK_SET);
                read(fd, &parent_entry, sizeof(parent_entry));
                if (parent_entry.inode == dir_entry.inode) {
                    // Found the parent entry to be removed
                    memset(&parent_entry, 0, sizeof(parent_entry));
                    lseek(fd, inode.direct_blocks[0] * EXT2_SUPER_BLOCK_SIZE + j * sizeof(parent_entry), SEEK_SET);
                    write(fd, &parent_entry, sizeof(parent_entry));
                    break;
                }
            }
            // Unlink the directory itself
            lseek(fd, (dir_entry.inode - 1) * sizeof(inode), SEEK_SET);
            read(fd, &inode, sizeof(inode));
            inode.link_count--;
            if (inode.link_count == 0) {
                // Remove the directory's inode and data blocks
                for (int j = 0; j < 12; j++) {
                    if (inode.direct_blocks[j] != 0) {
                        // Free the data block
                        struct ext2_block_group_descriptor group_desc;
                        lseek(fd, EXT2_SUPER_BLOCK_SIZE + sizeof(group_desc), SEEK_SET);
                        read(fd, &group_desc, sizeof(group_desc));
                        unsigned char block_bitmap[EXT2_SUPER_BLOCK_SIZE];
                        lseek(fd, group_desc.block_bitmap * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
                        read(fd, &block_bitmap, sizeof(block_bitmap));
                        int block_num = inode.direct_blocks[j] - 1;
                        block_bitmap[block_num / 8] &= ~(1 << (block_num % 8));
			            lseek(fd, group_desc.block_bitmap * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
                        write(fd, &block_bitmap, sizeof(block_bitmap));
                        // Update the free block count in the superblock and block group descriptor
                        struct ext2_super_block superblock;
                        lseek(fd, 1024, SEEK_SET);
                        read(fd, &superblock, sizeof(superblock));
                        superblock.free_block_count++;
                        lseek(fd, 1024, SEEK_SET);
                        write(fd, &superblock, sizeof(superblock));
                        group_desc.free_block_count++;
                        lseek(fd, EXT2_SUPER_BLOCK_SIZE + sizeof(group_desc), SEEK_SET);
                        write(fd, &group_desc, sizeof(group_desc));
                    }
                }
                // Free the inode
                struct ext2_block_group_descriptor group_desc;
                lseek(fd, EXT2_SUPER_BLOCK_SIZE + sizeof(group_desc), SEEK_SET);
                read(fd, &group_desc, sizeof(group_desc));
                unsigned char inode_bitmap[EXT2_SUPER_BLOCK_SIZE];
                lseek(fd, group_desc.inode_bitmap * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
                read(fd, &inode_bitmap, sizeof(inode_bitmap));
                int inode_num = dir_entry.inode - 1;
                inode_bitmap[inode_num / 8] &= ~(1 << (inode_num % 8));
                lseek(fd, group_desc.inode_bitmap * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
                write(fd, &inode_bitmap, sizeof(inode_bitmap));
                // Update the free inode count in the superblock and block group descriptor
                struct ext2_super_block superblock;
                lseek(fd, 1024, SEEK_SET);
                read(fd, &superblock, sizeof(superblock));
                superblock.free_inode_count++;
                lseek(fd, 1024, SEEK_SET);
                write(fd, &superblock, sizeof(superblock));
                group_desc.free_inode_count++;
                lseek(fd, EXT2_SUPER_BLOCK_SIZE + sizeof(group_desc), SEEK_SET);
                write(fd, &group_desc, sizeof(group_desc));
                // Zero out the inode
                memset(&inode, 0, sizeof(inode));
                lseek(fd, dir_entry.inode * sizeof(inode), SEEK_SET);
                write(fd, &inode, sizeof(inode));
                break;
            }
        }
    }
    //printf("success to remove dir\n");
}

int allocate_block(int fd, struct ext2_block_group_descriptor *group_desc) {
    unsigned char block_bitmap[EXT2_SUPER_BLOCK_SIZE];
    if (group_desc != NULL) {
        lseek(fd, group_desc->block_bitmap * EXT2_BLOCK_SIZE, SEEK_SET);
    } else {
        lseek(fd, 2 * EXT2_SUPER_BLOCK_SIZE + sizeof(struct ext2_block_group_descriptor), SEEK_SET);
    }
    read(fd, &block_bitmap, sizeof(block_bitmap));
    int block_num = find_first_zero_bit(block_bitmap, EXT2_SUPER_BLOCK_SIZE);
    if (block_num == -1) {
        fprintf(stderr, "No free blocks\n");
        exit(EXIT_FAILURE);
    }
    set_bit(block_bitmap, block_num);
    if (group_desc != NULL) {
        lseek(fd, group_desc->block_bitmap * EXT2_BLOCK_SIZE, SEEK_SET);
    } else {
        lseek(fd, 2 * EXT2_SUPER_BLOCK_SIZE + sizeof(struct ext2_block_group_descriptor), SEEK_SET);
    }
    write(fd, &block_bitmap, sizeof(block_bitmap));
    if (group_desc != NULL) {
        group_desc->free_block_count--;
        lseek(fd, 2 * EXT2_SUPER_BLOCK_SIZE + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) * group_desc->block_bitmap, SEEK_SET);
        write(fd, group_desc, sizeof(struct ext2_block_group_descriptor));
    } else {
        struct ext2_super_block super_block;
        lseek(fd, EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
        read(fd, &super_block, sizeof(super_block));
        super_block.free_block_count--;
        lseek(fd, EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
        write(fd, &super_block, sizeof(super_block));
    }
    return block_num;
}

struct ext2_dir_entry *find_dir_entry(int fd, struct ext2_inode *inode, char *name) {
    struct ext2_dir_entry *dir_entry;
    int i;

    for (i = 0; i < 12 && inode->direct_blocks[i] != 0; i++) {
        lseek(fd, inode->direct_blocks[i] * EXT2_BLOCK_SIZE, SEEK_SET);
        int total_size = 0;
        while ((unsigned int)total_size < inode->size) {
            dir_entry = (struct ext2_dir_entry *) malloc(sizeof(struct ext2_dir_entry));
            read(fd, dir_entry, sizeof(struct ext2_dir_entry));
            if (strncmp(name, dir_entry->name, dir_entry->name_length) == 0) {
                return dir_entry;
            }
            total_size += dir_entry->length;
            lseek(fd, dir_entry->length - sizeof(struct ext2_dir_entry), SEEK_CUR);
        }
    }

    return NULL;
}

void add_dir_entry(int fd, struct ext2_inode *inode, struct ext2_dir_entry *dir_entry) {
    int i;
    struct ext2_dir_entry *last_entry;
    int last_entry_offset;

    for (i = 0; i < 12 && inode->direct_blocks[i] != 0; i++) {
        lseek(fd, inode->direct_blocks[i] * EXT2_BLOCK_SIZE, SEEK_SET);
        int total_size = 0;
        while ((unsigned int )total_size < inode->size) {
            last_entry_offset = lseek(fd, 0, SEEK_CUR);
            last_entry = (struct ext2_dir_entry *) malloc(sizeof(struct ext2_dir_entry));
            read(fd, last_entry, sizeof(struct ext2_dir_entry));
            total_size += last_entry->length;
            if ((unsigned int)total_size >= inode->size) {
                break;
            }
            lseek(fd, last_entry->length - sizeof(struct ext2_dir_entry), SEEK_CUR);
        }
        if ((unsigned int)total_size < inode->size) {
            int last_entry_name_len = last_entry->name_length + (last_entry->name_length % 4);
            int last_entry_length = sizeof(struct ext2_dir_entry) + last_entry_name_len;
            int required_size = sizeof(struct ext2_dir_entry) + dir_entry->name_length + (dir_entry->name_length % 4);
            if (last_entry->length - last_entry_name_len >= required_size) {
                last_entry->length = last_entry_name_len;
                lseek(fd, last_entry_offset + last_entry->length, SEEK_SET);
                dir_entry->length = last_entry_length - last_entry->length;
                write(fd, dir_entry, sizeof(struct ext2_dir_entry));
                lseek(fd, last_entry_offset + last_entry->length, SEEK_SET);
                write(fd, last_entry, last_entry_length - last_entry->length);
                return;
            }
        }
    }

    int block_num = allocate_block(fd, NULL);
    inode->direct_blocks[i] = block_num;
    lseek(fd, block_num * EXT2_BLOCK_SIZE, SEEK_SET);
    dir_entry->length = EXT2_BLOCK_SIZE;
    write(fd, dir_entry, sizeof(struct ext2_dir_entry));
}





void ext2_mkdir(const char *img, char* path) {
	char parent_path[strlen(path) + 1];
    char name[strlen(path) + 1];
    tokenize_path(path, parent_path, name);

    // Check if parent directory exists and is a directory
    struct stat parent_stat;
    if (stat(img, &parent_stat) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }
    /*if (!S_ISDIR(parent_stat.st_mode)) {
        fprintf(stderr, "Parent path is not a directory\n");
        exit(EXIT_FAILURE);
    }*/

    // Open the parent directory and get its inode
    int fd = open(img, O_RDWR);
    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    struct ext2_inode parent_inode;
    // lseek(fd, (parent_stat.st_ino - 1) * sizeof(parent_inode), SEEK_SET);
    // read(fd, &parent_inode, sizeof(parent_inode));
    if (find_dir_entry(fd, &parent_inode, name) != NULL) {
        //fprintf(stderr, "Directory already exists\n");
        //exit(EXIT_FAILURE);
		exit(1);
    }
    // Allocate a new inode for the new directory
    struct ext2_block_group_descriptor group_desc;
    lseek(fd, 2 * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
    read(fd, &group_desc, sizeof(group_desc));
    unsigned char inode_bitmap[EXT2_SUPER_BLOCK_SIZE];
    lseek(fd, group_desc.inode_bitmap * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
    read(fd, &inode_bitmap, sizeof(inode_bitmap));
    int inode_num = find_first_zero_bit(inode_bitmap, group_desc.inode_bitmap);
    if (inode_num == -1) {
        fprintf(stderr, "No free inodes\n");
        exit(EXIT_FAILURE);
    }
    set_bit(inode_bitmap, inode_num);
    lseek(fd, group_desc.inode_bitmap * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
    //write(fd, &inode_bitmap, sizeof(inode_bitmap));
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = EXT2_D_DTYPE;
    inode.uid = getuid();
    inode.gid = getgid();
    inode.link_count = 2;
    inode.size = EXT2_BLOCK_SIZE;
    inode.direct_blocks[0] = allocate_block(fd, &group_desc);
    lseek(fd, (inode_num - 1) * sizeof(inode), SEEK_SET);
    //write(fd, &inode, sizeof(inode));

    // Link the new directory to its parent directory
    struct ext2_dir_entry dir_entry;
    dir_entry.inode = inode_num;
    dir_entry.name_length = strlen(name);
    dir_entry.file_type = EXT2_D_DTYPE;
    strncpy(dir_entry.name, name, dir_entry.name_length);
    add_dir_entry(fd, &parent_inode, &dir_entry);

    // Link the new directory to itself
    struct ext2_dir_entry self_entry;
    self_entry.inode = inode_num;
    self_entry.name_length = 1;
    self_entry.file_type = EXT2_D_DTYPE;
    self_entry.name[0] = '.';
    add_dir_entry(fd, &inode, &self_entry);

    // Link the new directory to its parent directory
    struct ext2_dir_entry parent_entry;
    parent_entry.inode = parent_stat.st_ino;
    parent_entry.name_length = 2;
    parent_entry.file_type = EXT2_D_DTYPE;
    parent_entry.name[0] = '.';
    parent_entry.name[1] = '.';
	parent_entry.name[2] = '\0';
    add_dir_entry(fd, &inode, &parent_entry);

    // Update the parent directory's inode
    parent_inode.link_count++;
    parent_inode.access_time = time(NULL);
    parent_inode.modification_time = time(NULL);
    lseek(fd, (parent_stat.st_ino - 1) * sizeof(parent_inode), SEEK_SET);
    //write(fd, &parent_inode, sizeof(parent_inode));

    // Update the superblock and group descriptor
    struct ext2_super_block super_block;
    lseek(fd, EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
    read(fd, &super_block, sizeof(super_block));
    super_block.free_inode_count--;
    lseek(fd, EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
    //write(fd, &super_block, sizeof(super_block));
    group_desc.free_inode_count--;
    lseek(fd, 2 * EXT2_SUPER_BLOCK_SIZE + sizeof(group_desc), SEEK_SET);
    //write(fd, &group_desc, sizeof(group_desc));

    close(fd);
}

void ext2_read_file(char *image, const char *path) {
    char parent_path[strlen(path) + 1];
    char fname[strlen(path) + 1];
    tokenize_path(path, parent_path, fname);
	printf("p:%s f:%s\n", parent_path, fname);

    // Check if the path exists
    struct stat statbuf;
    if (stat(path, &statbuf) == -1) {
        if (errno == ENOENT) {
            return;
        } else {
            perror("stat");
            exit(EXIT_FAILURE);
        }
    }
    // Get the inode of the parent directory
    int fd = open(image, O_RDWR);
    if (fd == -1) {
    perror("open");
    exit(EXIT_FAILURE);
    }
    struct ext2_inode parent_inode;
    lseek(fd, (statbuf.st_ino - 1) * sizeof(parent_inode), SEEK_SET);
    read(fd, &parent_inode, sizeof(parent_inode));

    // Get the inode of the file
    struct ext2_inode inode;
    lseek(fd, (statbuf.st_ino - 1) * sizeof(inode), SEEK_SET);
    read(fd, &inode, sizeof(inode));

    // Remove the link (parent_inode -> inode) with fname
    for (int i = 0; i < (int)(inode.block_count_512 / (2 << inode.size)); i++) {
        int block_num = inode.direct_blocks[i];
        lseek(fd, block_num * EXT2_SUPER_BLOCK_SIZE, SEEK_SET);
        struct ext2_dir_entry dir_entry;
        while (read(fd, &dir_entry, sizeof(dir_entry)) == sizeof(dir_entry)) {
            if (strcmp(fname, dir_entry.name) == 0) {
                // Remove the directory entry
                memset(&dir_entry, 0, sizeof(dir_entry));
                lseek(fd, -sizeof(dir_entry), SEEK_CUR);
                write(fd, &dir_entry, sizeof(dir_entry));
                // Unlink the directory
                struct ext2_inode parent_inode;
                lseek(fd, (dir_entry.inode - 1) * sizeof(parent_inode), SEEK_SET);
                read(fd, &parent_inode, sizeof(parent_inode));
                parent_inode.link_count--;
                if (parent_inode.link_count == 0) {
                    // Deallocate the inode
                    for (int j = 0; j < 12 && parent_inode.direct_blocks[j] != 0; j++) {
                        deallocate_block(fd, parent_inode.direct_blocks[j]);
                    }
                    if (parent_inode.direct_blocks[12] != 0) {
                        deallocate_indirect_block(fd, parent_inode.direct_blocks[12], 1);
                    }
                    if (parent_inode.direct_blocks[13] != 0) {
                        deallocate_indirect_block(fd, parent_inode.direct_blocks[13], 2);
                    }
                    if (parent_inode.direct_blocks[14] != 0) {
                        deallocate_indirect_block(fd, parent_inode.direct_blocks[14], 3);
                    }
                    memset(&parent_inode, 0, sizeof(parent_inode));
                    lseek(fd, (dir_entry.inode - 1) * sizeof(parent_inode), SEEK_SET);
                    write(fd, &parent_inode, sizeof(parent_inode));
                } else {
                    lseek(fd, (dir_entry.inode - 1) * sizeof(parent_inode), SEEK_SET);
                    write(fd, &parent_inode, sizeof(parent_inode));
                }
                break;
            }
        }
    }
    close(fd);
}

int get_child_inode(int fd, struct ext2_inode parent_inode, char *name, ext2_block_group_descriptor group) {
    int i;
    for (i = 0; i < 12; i++) {
        if (parent_inode.direct_blocks[i] == 0) {
            break;
        }
        int offset = 0;
        struct ext2_dir_entry entry;
        while (offset < BLOCK_SIZE) {
            lseek(fd, parent_inode.direct_blocks[i] * BLOCK_SIZE + offset, SEEK_SET);
            read(fd, &entry, sizeof(entry));
            if (entry.inode != 0 && strlen(name) == entry.name_length && strncmp(name, entry.name, entry.name_length) == 0) {
                return entry.inode;
            }
            offset += entry.length;
        }
    }
    if (i == 12) {
        int *indirect_block = (int *)malloc(BLOCK_SIZE);
        lseek(fd, parent_inode.direct_blocks[12] * BLOCK_SIZE, SEEK_SET);
        read(fd, indirect_block, BLOCK_SIZE);
        for (i = 0; i < (int)(BLOCK_SIZE / sizeof(int)); i++) {
            if (indirect_block[i] == 0)
            {
                break;
            }
            struct ext2_inode inode;
            lseek(fd, group.inode_table * 1024 + sizeof(inode) * (indirect_block[i] - 1), SEEK_SET);
            read(fd, &inode, sizeof(inode));
            int offset = 0;
            struct ext2_dir_entry entry;
            while (offset < BLOCK_SIZE) {
                lseek(fd, inode.direct_blocks[0] * BLOCK_SIZE + offset, SEEK_SET);
                read(fd, &entry, sizeof(entry));
                if (entry.inode != 0 && strlen(name) == entry.name_length && strncmp(name, entry.name, entry.name_length) == 0) {
                    free(indirect_block);
                    return entry.inode;
                }
                offset += entry.length;
            }
        }
        free(indirect_block);
    }
    return 0;
}


int allocate_block(int fd, struct ext2_block_group_descriptor group) {
    struct ext2_super_block super;
    lseek(fd, 1024, SEEK_SET);
    read(fd, &super, sizeof(super));
    char *block_bitmap = (char*)malloc(super.blocks_per_group / 8);
    lseek(fd, group.block_bitmap * BLOCK_SIZE, SEEK_SET);
    read(fd, block_bitmap, super.blocks_per_group / 8);
    //int i, j;
    /*for (i = 0; i < (int)(super.blocks_per_group / 8); i++) {
        for (j = 0; j < 8; j++) {
            if (!(block_bitmap[i] & (1 << j))) {
                block_bitmap[i] |= (1 << j);
                lseek(fd, group.block_bitmap * BLOCK_SIZE + i * 8 + j, SEEK_SET);
                write(fd, &block_bitmap[i], 1);
                free(block_bitmap);
                group.free_block_count--;
                lseek(fd, 2048, SEEK_SET);
                write(fd, &group, sizeof(group));
                super.free_block_count--;
                lseek(fd, 1024, SEEK_SET);
                write(fd, &super, sizeof(super));
                return group.inode_table + group.inode_bitmap + super.inodes_per_group + i * 8 + j + 1;
            }
        }
    }*/
    free(block_bitmap);
    return 0;
}

void ext2_edit_file(char *img, char *path, char *content, int index, int) {
    // Tokenize the path
    char *parent_path, *fname;
    parent_path = strdup(path);
    fname = strrchr(parent_path, '/');
    if (fname == NULL) {
        free(parent_path);
        return;
    }
    *fname = '\0';
    fname++;

    // Get the inode of the parent directory
    int fd = open(img, O_RDWR);
    if (fd < 0) {
        perror("open");
        exit(1);
    }
    struct ext2_super_block super;
    struct ext2_block_group_descriptor group;
    struct ext2_inode parent_inode, inode;
    lseek(fd, 1024, SEEK_SET);
    read(fd, &super, sizeof(super));
    lseek(fd, 2048, SEEK_SET);
    read(fd, &group, sizeof(group));
    lseek(fd, group.inode_table * 1024 + sizeof(parent_inode) * 2, SEEK_SET);
    read(fd, &parent_inode, sizeof(parent_inode));
    int parent_inode_num = 2;
    char *token = strtok(parent_path, "/");
    while (token != NULL) {
        parent_inode_num = get_child_inode(fd, parent_inode, token, group);
        if (parent_inode_num == 0) {
            free(parent_path);
            close(fd);
            return;
        }
        lseek(fd, group.inode_table * 1024 + sizeof(parent_inode) * (parent_inode_num - 1), SEEK_SET);
        read(fd, &parent_inode, sizeof(parent_inode));
        token = strtok(NULL, "/");
    }

    // Get the inode of the file
    int inode_num = get_child_inode(fd, parent_inode, fname, group);
    if (inode_num == 0) {
        free(parent_path);
        close(fd);
        return;
    }
    lseek(fd, group.inode_table * 1024 + sizeof(inode) * (inode_num - 1), SEEK_SET);
    read(fd, &inode, sizeof(inode));

    // Enqueue a buffer array with content + file[index:len(file)]
    char *content_buffer = (char*)malloc(strlen(content) + inode.size - index);
    memcpy(content_buffer, content, strlen(content));
    lseek(fd, inode.direct_blocks[0] * BLOCK_SIZE + index, SEEK_SET);
    read(fd, content_buffer + strlen(content), inode.size - index);

    // Dequeue the buffer array into file[index - bspace:len(file)]
    int offset = 0;
    int i;
    for (i = 0; i < 12; i++) {
        if (inode.direct_blocks[i] == 0) {
            break;
        }
        if (index < offset + BLOCK_SIZE) {
            lseek(fd, inode.direct_blocks[i] * BLOCK_SIZE + index - offset, SEEK_SET);
            write(fd, content_buffer, strlen(content_buffer));
            offset += strlen(content_buffer);
            break;
        }
        offset += BLOCK_SIZE;
    }
    if (i == 12) {
        int *indirect_block = (int *)malloc(BLOCK_SIZE);
        lseek(fd, inode.direct_blocks[12] * BLOCK_SIZE, SEEK_SET);
        read(fd, indirect_block, BLOCK_SIZE);
        for (i = 0; i < (int)(BLOCK_SIZE / sizeof(int)); i++) {
            if (indirect_block[i] == 0) {
                break;
            }
            if (index < offset + BLOCK_SIZE) {
                 lseek(fd, indirect_block[i] * BLOCK_SIZE + index - offset, SEEK_SET);
                write(fd, content_buffer, strlen(content_buffer));
                offset += strlen(content_buffer);
                break;
            }
            offset += BLOCK_SIZE;
        }
        free(indirect_block);
    }

    // If buffer array is not empty then dequeue it to new blocks
    while (strlen(content_buffer) > 0) {
        int block_num = allocate_block(fd, group);
        if (block_num == 0) {
            free(content_buffer);
            free(parent_path);
            close(fd);
            return;
        }
        if (inode.size == 0) {
            inode.direct_blocks[0] = block_num;
        } else if (inode.size <= 12 * BLOCK_SIZE) {
            inode.direct_blocks[inode.size / BLOCK_SIZE] = block_num;
        } else {
            int *indirect_block =(int *) malloc(BLOCK_SIZE);
            if (inode.size == 12 * BLOCK_SIZE) {
                inode.direct_blocks[12] = allocate_block(fd, group);
                if (inode.direct_blocks[12] == 0) {
                    free(content_buffer);
                    free(parent_path);
                    close(fd);
                    return;
                }
            }
            lseek(fd, inode.direct_blocks[12] * BLOCK_SIZE, SEEK_SET);
            read(fd, indirect_block, BLOCK_SIZE);
            indirect_block[(inode.size - 12 * BLOCK_SIZE) / BLOCK_SIZE] = block_num;
            lseek(fd, inode.direct_blocks[12] * BLOCK_SIZE, SEEK_SET);
            write(fd, indirect_block, BLOCK_SIZE);
            free(indirect_block);
        }
        int len = strlen(content_buffer);
        if (len > BLOCK_SIZE) {
            len = BLOCK_SIZE;
        }
        lseek(fd, block_num * BLOCK_SIZE, SEEK_SET);
        write(fd, content_buffer, len);
        content_buffer += len;
        inode.size += len;
    }

    // Update time and write them back
    inode.creation_time = time(NULL);
    inode.modification_time = time(NULL);
    lseek(fd, group.inode_table * 1024 + sizeof(inode) * (inode_num - 1), SEEK_SET);
    write(fd, &inode, sizeof(inode));
    parent_inode.creation_time = time(NULL);
    parent_inode.modification_time = time(NULL);
    lseek(fd, group.inode_table * 1024 + sizeof(parent_inode) * (parent_inode_num - 1), SEEK_SET);
    write(fd, &parent_inode, sizeof(parent_inode));

    // Clean up
    free(content_buffer);
    free(parent_path);
    close(fd);
}




int main(int , char* args[]){

    if(strcmp(args[2], "mkdir") == 0){
        ext2_mkdir(args[1], args[3]);
    }else if(strcmp(args[2], "rmdir") == 0){
        ext2_rmdir(args[1], args[3]);
    }else if(strcmp(args[2], "rm") == 0){
        ext2_read_file(args[1], args[3]);
    }else if(strcmp(args[2], "ed") == 0){
        int len = strlen(args[5]);
        int index = 0;
        int k = 1;
        for(int i = 0; i < len; i ++){
            index += (k * args[5][len - i - 1]);
            k *= 10;
        }
        ext2_edit_file(args[1], args[3], args[4], index, 0);
    }

    return 0;
}

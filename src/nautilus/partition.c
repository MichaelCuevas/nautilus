/* 
 * This file is part of the Nautilus AeroKernel developed
 * by the Hobbes and V3VEE Projects with funding from the 
 * United States National  Science Foundation and the Department of Energy.  
 *
 * The V3VEE Project is a joint project between Northwestern University
 * and the University of New Mexico.  The Hobbes Project is a collaboration
 * led by Sandia National Laboratories that includes several national 
 * laboratories and universities. You can find out more at:
 * http://www.v3vee.org  and
 * http://xstack.sandia.gov/hobbes
 *
 * Copyright (c) 2020, Michael A. Cuevas <cuevas@u.northwestern.edu>
 * Copyright (c) 2020, Peter Dinda <pdinda@northwestern.edu>
 * Copyright (c) 2020, The V3VEE Project  <http://www.v3vee.org> 
 *                     The Hobbes Project <http://xstack.sandia.gov/hobbes>
 * All rights reserved.
 *
 * Authors: Michael A. Cuevas <cuevas@u.northwestern.edu>
 *          Peter A. Dinda <pdinda@northwestern.edu>
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "LICENSE.txt".
 */

#include <nautilus/nautilus.h>
#include <nautilus/blkdev.h>
#include <nautilus/partition.h>

#ifndef NAUT_CONFIG_DEBUG_PARTITION
#undef DEBUG_PRINT
#define DEBUG_PRINT(fmt, args...) 
#endif

#define ERROR(fmt, args...) ERROR_PRINT("partition: " fmt, ##args)
#define DEBUG(fmt, args...) DEBUG_PRINT("partition: " fmt, ##args)
#define INFO(fmt, args...)  INFO_PRINT("partition: " fmt, ##args)

#define PART_PT_SIZE 4
#define PART_MBR_OFFSET 0
#define PART_GPTH_OFFSET 1
// Sectors per track
#define PART_SPT 63
// Heads per cylinder
#define PART_HPC 255 
#define PART_GPT_MAGIC_NUM 0xEE

#define STATE_LOCK_CONF uint8_t _state_lock_flags
#define STATE_LOCK(state) _state_lock_flags = spin_lock_irq_save(&state->lock)
#define STATE_UNLOCK(state) spin_unlock_irq_restore(&(state->lock), _state_lock_flags)

// Change later
struct partition_state {
    struct nk_block_dev *blkdev;
    spinlock_t lock;
    nk_part_entry pte;
    //uint64_t len;
    uint64_t block_size;
    uint64_t num_blocks;
    // Add Initial Block
    uint32_t ILBA; // First LBA of the partition
    struct nk_block_dev *underlying_blkdev; // Blockdevice this partition was created from
};

// TODO MAC: Later
int  nk_partition_init(struct naut_info *naut);
// add additional arg that hands back details of partition
int nk_enumerate_partitions(struct nk_block_dev *blkdev); 
void nk_partition_deinit();



static int get_characteristics(void *state, struct nk_block_dev_characteristics *c)
{
    STATE_LOCK_CONF;
    struct partition_state *s = (struct partition_state *)state;
    
    STATE_LOCK(s);
    c->block_size = s->block_size;
    c->num_blocks = s->num_blocks;
    STATE_UNLOCK(s);
    return 0;
}

// is block num and count within range of partition?
// translate (blocknum+starting point of part, then pass that info to underlying device)
static int read_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *dest,void (*callback)(nk_block_dev_status_t, void *), void *context)
{
    STATE_LOCK_CONF;
    struct partition_state *s = (struct partition_state *)state;

    DEBUG("read_blocks on device %s starting at %lu for %lu blocks\n",
	  s->blkdev->dev.name, blocknum, count);

    STATE_LOCK(s);
    uint64_t real_offset = s->ILBA + blocknum + count;
    struct nk_block_dev_characteristics blk_dev_chars;
    nk_block_dev_get_characteristics(s->underlying_blkdev, &blk_dev_chars);
    if (blocknum+count >= s->num_blocks) { 
	    STATE_UNLOCK(s);
	    ERROR("Illegal access past end of partition\n");
	    return -1;
    } else if (real_offset >= blk_dev_chars.num_blocks) {
        STATE_UNLOCK(s);
        ERROR("Illegal access past end of block device\n");
        return -1;
    } else {
        read_blocks(s->underlying_blkdev->dev.state, blocknum+s->ILBA, count, dest, callback, context);
	    //memcpy(dest,s->data+blocknum*s->block_size,s->block_size*count);
	    STATE_UNLOCK(s);
	    /*nk_dump_mem(dest,s->block_size*count);
	    if (callback) {
	        callback(NK_BLOCK_DEV_STATUS_SUCCESS,context);
	    }
        */
	    return 0;
    }
}

static int write_blocks(void *state, uint64_t blocknum, uint64_t count, uint8_t *src,void (*callback)(nk_block_dev_status_t, void *), void *context)
{
    STATE_LOCK_CONF;
    struct partition_state *s = (struct partition_state *)state;

    DEBUG("write_blocks on device %s starting at %lu for %lu blocks\n",
	  s->blkdev->dev.name, blocknum, count);

    STATE_LOCK(s);
    uint64_t real_offset = s->ILBA + blocknum + count;
    struct nk_block_dev_characteristics blk_dev_chars;
    nk_block_dev_get_characteristics(s->underlying_blkdev, &blk_dev_chars);
    if (blocknum+count >= s->num_blocks) { 
	    STATE_UNLOCK(s);
	    ERROR("Illegal access past end of partition\n");
	    return -1;
    } else if (real_offset >= blk_dev_chars.num_blocks) {
        STATE_UNLOCK(s);
	    ERROR("Illegal access past end of disk\n");
	    return -1;
    } else {
	    //memcpy(s->data+blocknum*s->block_size,src,s->block_size*count);
	    write_blocks(s->underlying_blkdev->dev.state, blocknum+s->ILBA, count, src, callback, context);
	    STATE_UNLOCK(s);
	    /*
        if (callback) { 
	        callback(NK_BLOCK_DEV_STATUS_SUCCESS,context);
	    }
        */
	    return 0;
    }
}



static struct nk_block_dev_int inter = 
{
    .get_characteristics = get_characteristics,
    .read_blocks = read_blocks,
    .write_blocks = write_blocks,
};

int nk_generate_partition_name(int partition_num, char *blk_name, char **new_name)
{
    // Create buffer char arr for int to str conversion
    char buffer[DEV_NAME_LEN];

    int num_digits = 0;
    int temp_num = partition_num;
    do {
        temp_num /= 10;
        num_digits++;
    } while (temp_num > 0);
              

    // convert i (loop control var) to str
    itoa(partition_num, buffer, num_digits);
    const char *p_num_str = buffer;

    // new char* to hold new device name 
    char *part_str = malloc(sizeof(char)*DEV_NAME_LEN);
    if (!part_str) {
            ERROR("failed to allocate partition name\n");
            return -1;
    }
    memcpy(part_str, 0, sizeof(*part_str));

    // Copy blkdev to new part_str and concat i to it
    strncpy(part_str, blk_name, DEV_NAME_LEN);
    part_str = strncat(part_str, p_num_str, DEV_NAME_LEN);
    *new_name = part_str;
    return 0;
}

int nk_mbr_enumeration(struct nk_block_dev *blockdev, nk_modern_mbr_t *MBR, struct nk_block_dev_characteristics blk_dev_chars)
{
    int i;
    for (i = 0; i < PART_PT_SIZE; i++) {
        struct partition_state *ps = malloc(sizeof(struct partition_state));
            
        if (!ps) {
            free(MBR); 
            ERROR("Cannot allocate data structure for partition\n");
            return -1;
        }
         
        memset(ps,0,sizeof(*ps));
        
        // Initialize the partition state lock and pte    
        spinlock_init(&ps->lock);
        ps->pte = MBR->partitions[i];

        // Check if curr partition table entry has at least 1 sector and less than max sectors
        if (ps->pte.num_sectors == 0  || ps->pte.num_sectors > blk_dev_chars.num_blocks) {
            free(ps);
            continue; 
        }

        // Fill out rest of partition state
        ps->block_size = blk_dev_chars.block_size;
        ps->num_blocks = ps->pte.num_sectors;
        ps->ILBA = ps->pte.first_lba;   
        ps->underlying_blkdev = blockdev;
        
        // Get new partition name
        char **new_name;
        if (nk_generate_partition_name(i+1, blockdev->dev.name, new_name)) {
            free(ps);
            free(MBR);
            return -1; 
        }

        // Register new partition
        ps->blkdev = nk_block_dev_register(*new_name, 0, &inter, ps);
        free(*new_name);
        
        if (!ps->blkdev) {
            ERROR("Failed to register partition\n");
            free(MBR); 
            free(ps);
            return -1;
        } 

        INFO("Added patition as %s, blocksize=%lu, ILBA=%lu, numblocks=%lu\n", ps->blkdev->dev.name, ps->block_size, ps->ILBA, ps->num_blocks);
    }
    free(MBR);
    return 0;
} 

int nk_gpt_enumeration(struct nk_block_dev *blockdev, nk_modern_mbr_t *MBR, struct nk_block_dev_characteristics blk_dev_chars)
{ 
    // Create GPT Header struct to hold GPT header info
    nk_part_gpt_header *GPTH = malloc(sizeof(*GPTH));
    if (!GPTH) {
        ERROR("Cannot allocate data structure for GPT Header\n");
        return -1;
    } 
    memset(GPTH, 0, sizeof(*GPTH));

    uint64_t gpth_blks = (sizeof(*GPTH))/(blk_dev_chars.block_size);

    // Read GPT Header
    nk_block_dev_read(blockdev, PART_GPTH_OFFSET, gpth_blks, GPTH, NK_DEV_REQ_NONBLOCKING, 0, 0);

    uint32_t entry_size = GPTH->pte_size;
    uint32_t num_entries = GPTH->num_ptes;
    // LBA of PT's first entry
    uint64_t SLBA = GPTH->slba;

    // Print out entry size, num entries, and the GPT starting LBA
    DEBUG("PTE Size: %lu, Num PTEs: %lu, Starting LBA: %lu\n", entry_size, num_entries, SLBA);
    DEBUG("GPTE Block Struct Size: %lu \n", sizeof(nk_part_gpte_block));

    uint64_t num_bytes = entry_size * num_entries;
    // TODO MAC: Not correct, need to use ceiling function
    uint64_t blocks_to_read = (num_bytes/(blk_dev_chars.block_size));
    nk_part_gpte *gpte_arr = malloc(sizeof(*gpte_arr)*num_entries);

    // Print number of blocks we're reading
    DEBUG("Reading %lu Bytes which = %lu 512-Byte blocks\n", num_bytes, blocks_to_read);
    DEBUG("Total size of gpte_arr = %lu\n", sizeof(gpte_arr));
    
    // Read enough blocks to cover all of GPT
    nk_block_dev_read(blockdev, SLBA, blocks_to_read, gpte_arr, NK_DEV_REQ_NONBLOCKING, 0, 0);

    // ptei = partition table entry index (within block)
    int ptei;
    for (ptei = 0; ptei < num_entries; ptei++)
    {
        // TODO MAC: need to add gpte to partition_state
        nk_part_gpte temp_entry = gpte_arr[ptei];

        // Ignore the content of this partition! 
        if (temp_entry.attrs.EFI_ignore) {
            DEBUG("Partition entry %lu ignored\n", ptei);
            continue;
        }

        // Check if curr partition table entry has at least 1 sector and less than max sectors
        uint64_t num_sectors = temp_entry.llba - temp_entry.flba;
        if (num_sectors == 0  || num_sectors > blk_dev_chars.num_blocks) {
            // Generates LOTS of debug output
            //DEBUG("Partition entry %lu's size (%lu) is nonsense\n", ptei, num_sectors);
            continue; 
        }

        // allocate partition state
        struct partition_state *ps = malloc(sizeof(struct partition_state));
        if (!ps) {
            free(MBR);
            free(gpte_arr); 
            ERROR("Cannot allocate data structure for partition\n");
            return -1;
        }
        memset(ps,0,sizeof(*ps));

        // Initialize the partition state lock 
        spinlock_init(&ps->lock);

        // Fill out rest of partition state
        ps->block_size = blk_dev_chars.block_size;
        ps->num_blocks = num_sectors;
        ps->ILBA = temp_entry.flba;   
        ps->underlying_blkdev = blockdev;

        int partition_num = ptei;
        char **new_name;
        if (nk_generate_partition_name(partition_num+1, blockdev->dev.name, new_name)) {
            free(ps);
            free(gpte_arr);
            free(MBR);
            return -1; 
        }

        // Register new partition
        ps->blkdev = nk_block_dev_register(*new_name, 0, &inter, ps);
        free(*new_name);

        if (!ps->blkdev) {
            ERROR("Failed to register partition\n");
            free(MBR);
            free(gpte_arr);
            free(ps);
            return -1;
        } 

        INFO("Added patition as %s, blocksize=%lu, ILBA=%lu, numblocks=%lu\n", ps->blkdev->dev.name, ps->block_size, ps->ILBA, ps->num_blocks);    
    }
    free(MBR);
    free(gpte_arr);
    return 0;
}

int nk_enumerate_partitions(struct nk_block_dev *blockdev)
{
    // Create MBR struct to hold MBR info
    nk_modern_mbr_t *MBR = malloc(sizeof(*MBR));
    if (!MBR) {
        ERROR("Cannot allocate data structure for MBR\n");
        return -1;
    } 
    memset(MBR, 0, sizeof(*MBR));

    // Collect blockdev characteristics (used to get MBR info)
    struct nk_block_dev_characteristics blk_dev_chars;
    nk_block_dev_get_characteristics(blockdev, &blk_dev_chars);
    uint64_t mbr_blks = (sizeof(*MBR))/(blk_dev_chars.block_size);

    // Read MBR
    nk_block_dev_read(blockdev, PART_MBR_OFFSET, mbr_blks, MBR, NK_DEV_REQ_NONBLOCKING, 0, 0);

    // If the first partition type is 0xEE, GPT is being used instead of MBR
    if (MBR->partitions[0].p_type == PART_GPT_MAGIC_NUM) {
        DEBUG("The first partition type is: %lu\n", MBR->partitions[0].p_type);
        return nk_gpt_enumeration(blockdev, MBR, blk_dev_chars); 
    } else {
        DEBUG("The first partition type is: %lu\n", MBR->partitions[0].p_type);
        return nk_mbr_enumeration(blockdev, MBR, blk_dev_chars);
    }
}


int nk_partition_init(struct naut_info *naut)
{
    INFO("init\n");
    return 1;
}

void nk_partition_deinit()
{
    INFO("deinit\n");
}


void convert_to_lba(){
    /*
        // starting CHS to starting LBA conversion
        // ILBA = (SC x HPC x SH) x SPT + (SS-1)
        uint8_t SC = ps->pte->chs_start_c;
        uint8_t SH = ps->pte->chs_start_h;
        uint8_t SS = ps->pte->chs_start_s;
        
        // ending CHS to ending LBA conversion
        // ELBA = (EC x HPC x EH) x SPT + (ES-1)
        uint8_t EC = ps->pte->chs_end_c;
        uint8_t EH = ps->pte->chs_end_h;
        uint8_t ES = ps->pte->chs_end_s;
        */

}


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"


#define CLUST_ORPHAN        0xfff5     // au:rgavs 5c18     rev.
#define CLUST_DIR           100
#define CLUST_HEAD          10
#define CLUST_NORM          1
#define TOTAL_CLUST         2880

struct _node{
    uint16_t stat;                     // or together dir.h ATTR macros ; initially set to CLUST_ORPHAN
    uint16_t next_clust;               // if in cluster chain, points to next cluster; else -1
    uint16_t parent;                   //                      points to HEAD of cluster chain; else -1
}; typedef struct _node node;

node *clust_map[2880];                  // end commit
uint8_t *image_buf;
struct bpb33* bpb;

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

void print_indent(int indent) {
    int i;
    for (i = 0; i < indent*4; i++)
    	printf(" ");
}

int follow_clust_chain(struct direntry *dirent, uint16_t cluster, uint32_t bytes_remaining)      // au:rgavs d17d
{
    int total_clusters, clust_size;

    clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;
    total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
    assert(cluster <= total_clusters);

    if (cluster == 0) {
    	fprintf(stderr, "Bad file termination\n");
    	return 0;
    }
    if((int)bytes_remaining < clust_size){                                   // au:rgavs
        clust_map[cluster]->parent = getushort(dirent->deStartCluster);
        clust_map[cluster]->stat = (uint16_t) (FAT12_MASK & CLUST_EOFS);
        if((int)bytes_remaining > clust_size)
            return 1;
        else
            return 2;
    }
    else {
    	/* more clusters after this one */
        clust_map[cluster]->parent = getushort(dirent->deStartCluster);
        clust_map[cluster]->stat = CLUST_NORM;
        /* recurse, continuing to copy */
        int i = 0;
        if(is_valid_cluster(get_fat_entry(cluster, image_buf, bpb),bpb))
            i = follow_clust_chain(dirent, get_fat_entry(cluster, image_buf, bpb), bytes_remaining - clust_size);
        if(i > 2)
        	clust_map[cluster]->next_clust = i;
        else if (i == 2)
            clust_map[cluster]->next_clust = get_fat_entry(cluster, image_buf, bpb);
        else
            clust_map[cluster]->next_clust = i;
        return cluster;
    }
    printf("TESTING");
    return 0;
}       // end d17d


/* Check if an existing file increases (or decreases) in size by at least one block.
Print out a list of files whose length in the directory entry is inconsistent with their length in data blocks (clusters).
Free any clusters that are beyond the end of a file, but to which the FAT chain still points.
Adjust the size entry for a file if there is a free or bad cluster in the FAT chain. */
void size_check(struct direntry *dirent, uint8_t *imgbuf, struct bpb33* bpb){
	uint16_t cluster = getushort(dirent->deStartCluster);
	uint32_t file_size = getulong(dirent->deFileSize);
	uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	uint32_t totalcluster = 0;
    printf("file_size: %u\n", file_size);
    printf("cluster_size: %u\n", cluster_size);
	while (is_valid_cluster(cluster, bpb)){
    	totalcluster += 1;

    	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
    	if (file_size%512 == 0) {
			file_size = file_size/512;
		}
		else {
			file_size = (file_size/512) + 1;
		}
		printf("file_size: %u\n", file_size);
        printf("total cluster: %u\n", totalcluster);
    	if (file_size > totalcluster){
    		file_size = totalcluster * bpb->bpbBytesPerSec;
    		putulong(dirent->deFileSize, file_size);
    		printf("File size is to big for %s\n", dirent->deName);
    	}
    	if (file_size < totalcluster){
    		file_size = totalcluster * bpb->bpbBytesPerSec;
    		putulong(dirent->deFileSize, file_size);
    		printf("File size is to small for %s\n", dirent->deName);
    	}

}




int dirent_sz_correct(struct direntry *dirent) {                        // au:rgavs 5c18
    clust_map[2]->stat = CLUST_FIRST;
    return follow_clust_chain(dirent, getushort(dirent->deStartCluster), getulong(dirent->deFileSize));
}                                                                      // end 5c18

/* write the values into a directory entry */
void write_dirent(struct direntry *dirent, char *filename,
		  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < (int)strlen(filename); i++) {
    	if (p2[i] == '/' || p2[i] == '\\')
    	    uppername = p2 + i + 1;
    }

    /* convert filename to upper case */
    for (i = 0; i < (int)strlen(uppername); i++)
    	uppername[i] = toupper(uppername[i]);

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL)
    	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    else {
    	*p = '\0';
    	p++;
    	len = strlen(p);
    	if (len > 3) len = 3;
        	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8)
        uppername[8]='\0';
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

    /* could also set time and date here if we really
       cared... */
}


/* create_dirent finds a free slot in the directory, and write the
   directory entry */
void create_dirent(struct direntry *dirent, char *filename,
		   uint16_t start_cluster, uint32_t size)
{
    while (1) {
    	if (dirent->deName[0] == SLOT_EMPTY) {
    	    /* we found an empty slot at the end of the directory */
    	    write_dirent(dirent, filename, start_cluster, size);
    	    dirent++;

    	    /* make sure the next dirent is set to be empty, just in
    	       case it wasn't before */
    	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
    	    dirent->deName[0] = SLOT_EMPTY;
    	    return;
    	}

    	if (dirent->deName[0] == SLOT_DELETED) {
    	    /* we found a deleted entry - we can just overwrite it */
    	    write_dirent(dirent, filename, start_cluster, size);
    	    return;
    	}
    	dirent++;
    }
}



uint16_t print_dirent(struct direntry *dirent, int indent)
{
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY)
		return followclust;

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
		return followclust;

	if (((uint8_t)name[0]) == 0x2E)
		return followclust;           // skip dot entries "." & ".."

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) {
		if (name[i] == ' ')
			name[i] = '\0';
		else
			break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) {
		if (extension[i] == ' ')
			extension[i] = '\0';
		else
			break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN) {
    	// ignore any long file name extension entries
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0)
		printf("Volume: %s\n", name);
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
		// don't deal with hidden directories; MacOS makes these
		// for trash directories and such; just ignore them.
		if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN) {
			print_indent(indent);
			printf("%s/ (directory)\n", name);
			file_cluster = getushort(dirent->deStartCluster);
			followclust = file_cluster;
		}
    }
    else {
        /* a "regular" file entry
         * print attributes, size, starting cluster, etc. */
		int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
		int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
		int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
		int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;
        clust_map[followclust]->stat = CLUST_NORM;
		size = getulong(dirent->deFileSize);
		print_indent(indent);
        dirent_sz_correct(dirent);                                          // au:rgavs
        size_check(dirent, image_buf, bpb);

		printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n",
			name, extension, size, getushort(dirent->deStartCluster),
			ro?'r':' ',
				hidden?'h':' ',
				sys?'s':' ',
				arch?'a':' ');
    }
    return followclust;
}

void follow_dir(uint16_t cluster, int indent)
{
    while (is_valid_cluster(cluster, bpb) && !(is_end_of_file(cluster))) {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
		for ( ; i < numDirEntries; i++) {
				uint16_t followclust = print_dirent(dirent, indent);
				if (followclust){                                       // au:rgavs 5c18
                    clust_map[followclust]->parent = cluster;
                    clust_map[cluster]->next_clust = followclust;
                    clust_map[followclust]->stat = CLUST_DIR;           // end
					follow_dir(followclust, indent+1);
                }
				dirent++;
		}
		cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}



void traverse_root()
{
    uint16_t cluster = 0;                                               // au:rgavs 5c18
    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
    int i = 0;                                                          // end 5c18
    for ( ; i < bpb->bpbRootDirEnts; i++) {
        uint16_t followclust = print_dirent(dirent, 0);
        if (is_valid_cluster(followclust, bpb)){
            clust_map[followclust]->parent = cluster;       // au:rgavs commit:fcce
            clust_map[cluster]->next_clust = followclust;   // end      fcce
            follow_dir(followclust, 1);
        }
        dirent++;
    }
}

void read_map(){                    // au:rgavs
    uint16_t start_cluster = -1;
    int j = 1;
    u_int32_t size = 0;
    for(int i = 2; i < 2880; i++){
        // Good clusters
        if(clust_map[i]->stat != CLUST_ORPHAN){
            if(size > 0){;
                char filename[12];
                snprintf(filename, 12, "found%d", 42);
                create_dirent((struct direntry*)cluster_to_addr(0, image_buf, bpb), filename, start_cluster, size);
                size = 0;
                j++;
            }
            // Free clusters
            if(get_fat_entry(i, image_buf, bpb) == CLUST_FREE)
                clust_map[i]->stat = CLUST_FREE;
            // NORM/Dir clusters
            else if(clust_map[i]->stat <= CLUST_DIR){
                if(i%3 == 0)
                    printf("\n");
                printf("clust %d->stat = %d     ",i,clust_map[i]->stat);
            }
        }
        // Orphans
        else {
            if(size == 0){
                start_cluster = i;
            }
            clust_map[i]->stat = CLUST_ORPHAN & CLUST_HEAD;
            size += bpb->bpbSecPerClust * bpb->bpbBytesPerSec;
        }
    }
    for(int i = 0; i< 2880;i++){
        free(clust_map[i]);
    }
}

int main(int argc, char** argv) {
    for(int i = 0; i < 2880;i++){                                       // au:rgavs 1993
        clust_map[i] = (node*)malloc(sizeof(node));
        clust_map[i]->stat = CLUST_ORPHAN;
        clust_map[i]->parent = -1;
        clust_map[i]->next_clust = -1;                                  // end 1993
    }
    int fd;
    if (argc < 2)
    	usage(argv[0]);

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // start user code
    traverse_root();
    read_map();
    unmmap_file(image_buf, &fd);
    printf("Execution complete.\n");
    return 0;
}

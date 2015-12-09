#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

#define CLUST_ORPHAN        0xffff     // au:rgavs 5c18
#define CLUST_DIR           0x0100
#define CLUST_HEAD          0x0200
#define CLUST_NORM          0x0400
#define TOTAL_CLUST         2880

struct _node{
    uint16_t stat;                     // or together dir.h ATTR macros ; initially set to CLUST_ORPHAN
    uint16_t next_clust;               // if in cluster chain, points to next cluster; else -1
    uint16_t parent;                   //                      points to HEAD of cluster chain; else -1
}; typedef struct _node node;

node *clust_map[2880];                  // end commit


void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}


void print_indent(int indent) {
    int i;
    for (i = 0; i < indent*4; i++)
    	printf(" ");
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
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
		int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
		int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
		int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
		int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;
        clust_map[followclust]->stat = CLUST_NORM;
		size = getulong(dirent->deFileSize);
		print_indent(indent);
		printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n",
			name, extension, size, getushort(dirent->deStartCluster),
			ro?'r':' ',
				hidden?'h':' ',
				sys?'s':' ',
				arch?'a':' ');
    }

    return followclust;
}


void follow_dir(uint16_t cluster, int indent, uint8_t *image_buf, struct bpb33* bpb)
{
    printf("IN FOLLOW_DIR\n");
    while (is_valid_cluster(cluster, bpb)) {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
		for ( ; i < numDirEntries; i++) {
				uint16_t followclust = print_dirent(dirent, indent);
				if (followclust){                                       // au:rgavs 5c18
                    clust_map[followclust]->parent = cluster;
                    clust_map[cluster]->next_clust = followclust;
                    clust_map[followclust]->stat = CLUST_DIR;           // end
					follow_dir(followclust, indent+1, image_buf, bpb);
                }
				dirent++;
		}
		cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void traverse_root(uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t cluster = 0;                                               // au:rgavs 5c18
    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
    int i = 0;                                                          // end 5c18
    for ( ; i < bpb->bpbRootDirEnts; i++) {
        uint16_t followclust = print_dirent(dirent, 0);
        if (is_valid_cluster(followclust, bpb)){
            printf("followclust = %u and cluster = %u and dirent = %s\n",followclust,cluster,(uint8_t*)dirent);
            clust_map[followclust]->parent = cluster;       // au:rgavs commit:fcce     PROMBLEM HERE!!
            clust_map[cluster]->next_clust = followclust;   // end      fcce
            follow_dir(followclust, 1, image_buf, bpb);
        }
        dirent++;
    }
}

int follow_clust_chain(uint16_t cluster, uint32_t bytes_remaining,      // au:rgavs
    		   uint8_t *image_buf, struct bpb33* bpb)
{
    int total_clusters, clust_size;
    uint8_t *p;

    clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;
    total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;

    assert(cluster <= total_clusters);

    if (cluster == 0) {
    	fprintf(stderr, "Bad file termination\n");
    	return 0;
    }
    else if (is_end_of_file(cluster))
    	return 0;

    /* map the cluster number to the data location */
    p = cluster_to_addr(cluster, image_buf, bpb);

    if (bytes_remaining <= clust_size){
    	// fwrite(p, bytes_remaining, 1, fd);     // last cluster
        clust_map[cluster]->stat = (uint16_t) CLUST_EOFE;
    }
    else {
    	/* more clusters after this one */
    	// fwrite(p, clust_size, 1, fd);
        clust_map[cluster]->stat = CLUST_NORM;
    	/* recurse, continuing to copy */
    	copy_out_file(get_fat_entry(cluster, image_buf, bpb), bytes_remaining - clust_size, image_buf, bpb);
    }
    return 0;
}                                                                       // end


void dir_sz_correct() {                                                 // au:rgavs 5c18
    clust_map[2]->stat = CLUST_FIRST;
    uint16_t size;
    for(int i = 2; i < TOTAL_CLUST; i++){
        switch(clust_map[i]->stat){
            case CLUST_ORPHAN: // kind of ugly looking switch, but I don't know any better
                // printf("Cluster number %d is an orphan!\n", i);
                break;
            case CLUST_BAD & FAT16_MASK:
                printf("Cluster number %d is BAD!\n", i);
                break;
            case CLUST_FREE:
                // printf("Cluster number %d is free.\n", i);
                    break;
            case CLUST_DIR:
                printf("Cluster number %d is a directory entry.\n", i);
                break;
            case CLUST_EOFS & FAT12_MASK ... CLUST_EOFE & FAT12_MASK:
                printf("Cluster number %d is an EOF.\n", i);
                break;
            // case CLUST_EOFE & FAT12_MASK:
            //     printf("Cluster number %d is an EOF.\n", i);
            //     break;
        }
    }
}                                                                       // end 5c18

int main(int argc, char** argv) {
    for(int i = 0; i < 2880;i++){                                       // au:rgavs 1993
        clust_map[i] = malloc(sizeof(node));
        clust_map[i]->stat = CLUST_ORPHAN;
        clust_map[i]->parent = -1;
        clust_map[i]->next_clust = -1;                                  // end 1993
    }
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2)
    	usage(argv[0]);

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // start user code
    traverse_root(image_buf, bpb);
    dir_sz_correct();

    unmmap_file(image_buf, &fd);
    for(int i = 0; i < 2880;i++)
        free(clust_map[i]);
    printf("Execution complete.\n");
    return 0;
}

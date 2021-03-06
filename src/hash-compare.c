/*
 ============================================================================
 Name        : hash-compare.c
 Author      : Felix Erckenbrecht
 Version     : 1.0
 Copyright   : BSD 3-clause
 Description : Compare hashdeep hash-files for matches
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>


struct HashListElement_S{
    off_t  filesize;
    char * pLine;
    size_t lineLen;
    char * pFilename;
    size_t filenameLen;
    char * pHash;
    size_t hashLen;
};



static void printUsage(){
    printf( "hash-compare v1.0 ("__DATE__" "__TIME__")\n"
            "Compare hashdeep files, find matches.\n"
            "\n"
            "Usage: hash-compare <reference-file> <compare-file>\n"
            "\n"
            );
}


/*
 * count lines in hash files
 */
static int countLines(void * p, off_t size){
    int lines;
    char * cp;

    lines = 0;
    do{
        /* find next NL / end-of-line */
        cp = memchr(p,'\n', size);
        if(cp != NULL){
            size_t len;
            len = (uintptr_t)cp - (uintptr_t) p;
            // skip past NL
            len+=1;
            // move pointer to next line
            p  += len;
            // decrease size to stay within valid range
            size -= len;
            // one more line found
            lines++;
        }
        /* iterate while something remaining AND a line had been found */
    }while( (size > 0) && (cp != NULL) );

    return lines;
}



/*
 * read & parse lines from hash-file
 * thereby modifying it (replacing , and NL by \0)
 */
static int populateList(void * p, off_t size, struct HashListElement_S * hl, size_t * elements){
    size_t el;
    char * pl;
    char * cpNl;
    char * cpHash;
    char * cpFilename;
    int ret = 0;

    for(el=0; (el < *elements) && (size > 0); ){
        size_t line_size;
        size_t hash_size;
        size_t filename_size;

        cpNl = memchr(p,'\n',size);
        pl = p;
        if(cpNl == NULL){
            ret = -1;
            break;
        }
        // terminate line (file name)
        *cpNl = 0;
        // line size includes NL
        line_size= (size_t) ((uintptr_t)cpNl - (uintptr_t) p) + 1;
        size -= line_size;
        p    += line_size;

        // find delimiter between size and hash
        cpHash = memchr(pl,',',line_size);

        if(cpHash == NULL){
            // bad line
            continue;
        }
        // terminate file-size string
        *cpHash = 0;
        line_size= (size_t) ((uintptr_t)cpNl - (uintptr_t) cpHash);
        if(line_size <=  1){
            //bad line
            continue;
        }
        // move beyond , to first char of hash
        cpHash++;
        line_size--;
        // find comma before filename
        cpFilename = memchr(cpHash, ',', line_size);
        if( cpFilename == NULL){
            //bad line
            continue;
        }
        // terminate Hash
        *cpFilename = 0;
        hash_size = (size_t) (uintptr_t) cpFilename - (uintptr_t) cpHash;

        //move beyond , to first char of filename
        cpFilename++;
        filename_size = (size_t) (uintptr_t) cpNl - (uintptr_t) cpFilename;

        hl[el].pLine        = pl;
        hl[el].lineLen      = line_size;
        hl[el].pHash        = cpHash;
        hl[el].hashLen      = hash_size;
        hl[el].pFilename    = cpFilename;
        hl[el].filenameLen  = filename_size;
        hl[el].filesize     = (off_t) strtoll(pl, NULL, 0);

        el++;
    }
    // return number of parsable lines
    *elements=el;
    return ret;
}


/* compare function for quicksort and bsearch
 * hash entry needs to be zero-terminated (populateList does that) */
static int hlCompare(const void *p1, const void *p2){
    struct HashListElement_S * e1;
    struct HashListElement_S * e2;
    e1 = (struct HashListElement_S * )p1;
    e2 = (struct HashListElement_S * )p2;

    return strcasecmp(e1->pHash, e2->pHash);
}


/*
 * iterate through compare file, trying to find matches in (sorted) reference file
 */
static void compareLines(
        int showMatch,
        struct HashListElement_S * pHlref,
        const size_t hlref_elements,
        struct HashListElement_S * pHlcompare,
        const size_t hlcomp_elements)
{
    size_t lineToCompare;
    struct HashListElement_S * pFound;

    if(showMatch){
        fprintf(stdout, "## Finding Matches (entries from compare list present in reference list)\n");
    }
    else{
        fprintf(stdout, "## Finding Files present in compare list, but missing in reference list\n");
    }
    fprintf(stdout, "%%%% Hash, Size, Comp Name, Ref Name\n");

    for(lineToCompare = 0; lineToCompare < hlcomp_elements; lineToCompare++){
        pFound = bsearch(&pHlcompare[lineToCompare], pHlref, hlref_elements, sizeof(struct HashListElement_S), hlCompare);
        if(showMatch){
            if(pFound != NULL){
                if(pHlcompare[lineToCompare].filesize == pFound->filesize){
                    fprintf(stdout, "%s, %12lld, %s, %s\n",
                            pHlcompare[lineToCompare].pHash, pHlcompare[lineToCompare].filesize, pHlcompare[lineToCompare].pFilename,
                            pFound->pFilename);
                }
                else{
                    /* ToDo: Automatically prepare paper about hash collision ;) */
                    fprintf(stdout, "Collision (match with different file sizes): %s, %lld, %s, %s, %lld, %s\n",
                    pHlcompare[lineToCompare].pHash, pHlcompare[lineToCompare].filesize, pHlcompare[lineToCompare].pFilename,
                    pFound->pHash, pFound->filesize, pFound->pFilename);
                }
            }
        }
        else{
            if(pFound == NULL){
                if(pHlcompare[lineToCompare].filesize > 0){
                    fprintf(stdout, "Missing: %s, %s\n", pHlcompare[lineToCompare].pHash, pHlcompare[lineToCompare].pFilename);
                }
            }
        }
    }

}



int main(int argc, char **argv) {
    struct stat statBuf;
    char * fileName1;
    char * fileName2;
    int fd_ref, fd_comp;
    char * mmfRef;
    char * mmfComp;

    off_t filesize_ref,filesize_comp;
    off_t sizeRef,sizeComp;

    int lines_ref, lines_comp;
    size_t refElements;
    size_t elementsToCompare;
    struct HashListElement_S * hl_ref;
    struct HashListElement_S * hl_comp;


    if(argc < 3){
        printUsage();
        return EXIT_FAILURE;
    }

    fileName1 = argv[1];
    fileName2 = argv[2];

    /*
     * open files
     */
    fd_ref = open(fileName1, O_RDONLY);
    fd_comp = open(fileName2, O_RDONLY);
    if(fd_ref == -1){
        perror("Opening reference file failed");
        return EXIT_FAILURE;
    }

    if(fd_comp == -1){
        perror("Opening compare file failed");
        return EXIT_FAILURE;
    }

    if(fstat(fd_ref, &statBuf) < 0){
        perror("stat for reference file failed");
        return EXIT_FAILURE;
    }
    /* get file size for reference file */
    filesize_ref = statBuf.st_size;

    if(fstat(fd_comp, &statBuf) < 0){
        perror("stat for compare file failed");
        return EXIT_FAILURE;
    }
    /* get file size for compare file */
    filesize_comp = statBuf.st_size;

    /* add 1 byte for additional termination character */
    sizeRef  = filesize_ref + 1;
    sizeComp = filesize_comp+ 1;

    /* create private memory mappings from both files */
    mmfComp = mmap( 0, sizeComp, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_comp, 0);
    mmfRef  = mmap( 0, sizeRef,  PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_ref, 0);

    if( (mmfComp == MAP_FAILED) || (mmfRef == MAP_FAILED)){
        perror("Mapping of file(s) failed");
        return EXIT_FAILURE;
    }

    /* terminate files with new-line, in case the last line has none */
    mmfRef[sizeRef-1] = mmfComp[sizeComp-1] = '\n';

    /* close file descriptors, access via memmap */
    close(fd_ref);
    close(fd_comp);

    /* get number of lines for each file */
    lines_ref  = countLines(mmfRef,  sizeRef);
    lines_comp = countLines(mmfComp, sizeComp);

    if(lines_ref < 0 || (lines_comp < 0)){
        perror("Error while counting lines.\n");
        munmap(mmfRef,  sizeRef);
        munmap(mmfComp, sizeComp);
        return EXIT_FAILURE;
    }

    hl_ref  = malloc(sizeof(struct HashListElement_S) * lines_ref);
    hl_comp = malloc(sizeof(struct HashListElement_S) * lines_comp);
    if((hl_ref == NULL)||(hl_comp == NULL)){
        perror("Could not allocate list memory.\n");
        munmap(mmfRef,  sizeRef);
        munmap(mmfComp, sizeComp);
        return EXIT_FAILURE;
    }

    /* the number of data points / elements equals the number of lines (or is less if there are comments/empty lines/etc.) */
    refElements       = (size_t) lines_ref;
    elementsToCompare = (size_t) lines_comp;
    /* populate reference list */
    populateList(mmfRef, sizeRef, hl_ref, &refElements);
    /* populate compare list */
    populateList(mmfComp, sizeComp, hl_comp, &elementsToCompare);

    /* sort reference list by 'hash' to allow for efficient binary search */
    qsort(hl_ref, refElements, sizeof(struct HashListElement_S), hlCompare);

    /* search reference for matches
     * default to "show match" */
    compareLines(1, hl_ref, refElements, hl_comp, elementsToCompare);

	return EXIT_SUCCESS;
}

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include "config.h"
#include "cpio.h"
#include "miscfn.h"

#if MAJOR_IN_SYSMACROS 
#include <sys/sysmacros.h>
#elif MAJOR_IN_MKDEV 
#include <sys/mkdev.h>
#endif

#define CPIO_CRC_MAGIC	"070702"
#define TRAILER		"TRAILER!!!"

/* FIXME: We don't translate between cpio and system mode bits! These
   should both be the same, but really odd things are going to happen if
   that's not true! */

/* We need to maintain our oun file pointer to allow padding */
struct ourfd {
    gzFile fd;
    int pos;
};

struct cpioCrcPhysicalHeader {
    char magic[6];
    char inode[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devMajor[8];
    char devMinor[8];
    char rdevMajor[8];
    char rdevMinor[8];
    char namesize[8];
    char checksum[8];			/* ignored !! */
};

struct cpioHeader {
    ino_t inode;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    int nlink;
    time_t mtime;
    long size;
    dev_t dev, rdev;
    char * path;
};

static inline loff_t ourread(struct ourfd * thefd, void * buf, size_t size) {
    loff_t i;

    i = gzread(thefd->fd, buf, size);
    thefd->pos += i;
    
    return i;
}

static inline void padfd(struct ourfd * fd, int modulo) {
    int buf[10];
    int amount;
    
    amount = (modulo - fd->pos % modulo) % modulo;
    ourread(fd, buf, amount);
}

static int strntoul(const char * str, char ** endptr, int base, int num) {
    char * buf;

    buf = alloca(num + 1);
    strncpy(buf, str, num);
    buf[num] = '\0';

    return strtoul(buf, endptr, base);
}

#define GET_NUM_FIELD(phys, log) \
	log = strntoul(phys, &end, 16, sizeof(phys)); \
	if (*end) return CPIO_BAD_HEADER;

static int getNextHeader(struct ourfd * fd, struct cpioHeader * chPtr) {
    struct cpioCrcPhysicalHeader physHeader;
    int nameSize;
    char * end;
    int major, minor;

    if (ourread(fd, &physHeader, sizeof(physHeader)) != sizeof(physHeader)) 
	return CPIO_READ_FAILED;

    if (strncmp(CPIO_CRC_MAGIC, physHeader.magic, strlen(CPIO_CRC_MAGIC))) 
	return CPIO_BAD_MAGIC;

    
    GET_NUM_FIELD(physHeader.inode, chPtr->inode);
    GET_NUM_FIELD(physHeader.mode, chPtr->mode);
    GET_NUM_FIELD(physHeader.uid, chPtr->uid);
    GET_NUM_FIELD(physHeader.gid, chPtr->gid);
    GET_NUM_FIELD(physHeader.nlink, chPtr->nlink);
    GET_NUM_FIELD(physHeader.mtime, chPtr->mtime);
    GET_NUM_FIELD(physHeader.filesize, chPtr->size);

    GET_NUM_FIELD(physHeader.devMajor, major);
    GET_NUM_FIELD(physHeader.devMinor, minor);
    chPtr->dev = makedev(major, minor);

    GET_NUM_FIELD(physHeader.rdevMajor, major);
    GET_NUM_FIELD(physHeader.rdevMinor, minor);
    chPtr->rdev = makedev(major, minor);

    GET_NUM_FIELD(physHeader.namesize, nameSize);

    chPtr->path = malloc(nameSize + 1);
    if (ourread(fd, chPtr->path, nameSize) != nameSize) {
	free(chPtr->path);
	return CPIO_BAD_HEADER;
    }

    chPtr->path[nameSize] = '\0';

    padfd(fd, 4);

    return 0;
}

int cpioFileMapCmp(const void * a, const void * b) {
    const struct cpioFileMapping * first = a;
    const struct cpioFileMapping * second = b;

    return (strcmp(first->archivePath, second->archivePath));
}

/* This could trash files in the path! I'm not sure that's a good thing */
static int createDirectory(char * path) {
    struct stat sb;
    int dounlink;

    if (!access(path, X_OK)) {
	if (lstat(path, &sb))
	    return CPIO_STAT_FAILED;

	if (S_ISDIR(sb.st_mode)) {
	    return 0;
	} else if (S_ISLNK(sb.st_mode)) {
	    if (stat(path, &sb)) {
		if (errno != ENOENT) 
		    return CPIO_STAT_FAILED;
		dounlink = 1;
	    } else {
		if (S_ISDIR(sb.st_mode))
		    return 0;
		dounlink = 1;
	    }
	} else {
	    dounlink = 1;
	}

	if (dounlink && unlink(path)) {
	    return CPIO_UNLINK_FAILED;
	}
    }

    if (mkdir(path, 000))
	return CPIO_MKDIR_FAILED;

    return 0;
}

static int setInfo(struct cpioHeader * hdr) {
    int rc = 0;
    struct utimbuf stamp = { hdr->mtime, hdr->mtime };

    if (!getuid() && !rc && chown(hdr->path, hdr->uid, hdr->gid))
	rc = CPIO_CHOWN_FAILED;

    if (!S_ISLNK(hdr->mode)) {
	if (!rc && chmod(hdr->path, hdr->mode & 07777))
	    rc = CPIO_CHMOD_FAILED;
	if (!rc && utime(hdr->path, &stamp))
	    rc = CPIO_UTIME_FAILED;
    }

    return rc;
}

static int checkDirectory(char * filename) {
    static char * lastDir = NULL;
    static int lastDirLength = 0;
    static int lastDirAlloced = 0;
    int length = strlen(filename);
    char * buf;
    char * chptr;
    int rc = 0;

    buf = alloca(length + 1);
    strcpy(buf, filename);

    for (chptr = buf + length - 1; chptr > buf; chptr--) {
	if (*chptr == '/') break;
    }

    if (chptr == buf) return 0;     /* /filename - no directories */

    *chptr = '\0';                  /* buffer is now just directories */

    length = strlen(buf);
    if (lastDirLength == length && !strcmp(buf, lastDir)) return 0;

    if (lastDirAlloced < (length + 1)) {
	lastDirAlloced = length + 100;
	lastDir = realloc(lastDir, lastDirAlloced);
    }

    strcpy(lastDir, buf);
    lastDirLength = length;

    for (chptr = buf + 1; *chptr; chptr++) {
	if (*chptr == '/') {
	    *chptr = '\0';
	    rc = createDirectory(buf);
	    *chptr = '/';
	    if (rc) return rc;
	}
    }
    rc = createDirectory(buf);

    return rc;
}

static int expandRegular(struct ourfd * fd, struct cpioHeader * hdr) {
    int out;
    char buf[16384];
    int bytesRead;
    int left = hdr->size;
    int rc = 0;

    if (!access(hdr->path, X_OK))
	if (unlink(hdr->path))
	    return CPIO_UNLINK_FAILED;

    out = open(hdr->path, O_CREAT | O_WRONLY, 0);
    if (out < 0) 
	return CPIO_OPEN_FAILED;

    while (left) {
	bytesRead = ourread(fd, buf, left < sizeof(buf) ? left : sizeof(buf));
	if (bytesRead <= 0) {
	    rc = CPIO_READ_FAILED;
	    break;
	}

	if (write(out, buf, bytesRead) != bytesRead) {
	    rc = CPIO_READ_FAILED;
	    break;
	}

	left -= bytesRead;
    }

    close(out);
    
    return rc;
}

static int expandSymlink(struct ourfd * fd, struct cpioHeader * hdr) {
    char buf[2048];

    if (!access(hdr->path, X_OK))
	if (unlink(hdr->path))
	    return CPIO_UNLINK_FAILED;

    if ((hdr->size + 1)> sizeof(buf))
	return CPIO_INTERNAL;

    if (ourread(fd, buf, hdr->size) != hdr->size)
	return CPIO_READ_FAILED;

    buf[hdr->size] = '\0';

    if (symlink(buf, hdr->path))
	return CPIO_SYMLINK_FAILED;

    return 0;
}

static int expandFifo(struct ourfd * fd, struct cpioHeader * hdr) {
    struct stat sb;

    if (!access(hdr->path, X_OK)) {
	if (lstat(hdr->path, &sb))
	    return CPIO_STAT_FAILED;

	if (S_ISFIFO(sb.st_mode)) return 0;

	if (unlink(hdr->path))
	    return CPIO_UNLINK_FAILED;
    }

    if (mkfifo(hdr->path, 0))
	return CPIO_MKFIFO_FAILED;

    return 0; 
}

static int expandDevice(struct ourfd * fd, struct cpioHeader * hdr) {
    if (!access(hdr->path, X_OK))
	if (unlink(hdr->path))
	    return CPIO_UNLINK_FAILED;

    if (mknod(hdr->path, hdr->mode & (~0777), hdr->rdev))
	return CPIO_MKNOD_FAILED;
    
    return 0;
}

int cpioInstallArchive(gzFile stream, struct cpioFileMapping * mappings, 
		       int numMappings, cpioCallback cb, char ** failedFile) {
    struct cpioHeader ch;
    struct ourfd fd;
    int rc = 0;
    struct cpioFileMapping * map = NULL;
    struct cpioFileMapping needle;
    mode_t cpioMode;
    int olderr;

    fd.fd = stream;
    fd.pos = 0;

    do {
	if ((rc = getNextHeader(&fd, &ch))) {
	    printf("error %d reading header: %s\n", rc, strerror(errno));
	    exit(1);
	}

	if (!strcmp(ch.path, TRAILER)) {
	    free(ch.path);
	    break;
	}

	if (mappings) {
	    needle.archivePath = ch.path;
	    map = bsearch(&needle, mappings, numMappings, sizeof(needle),
			  cpioFileMapCmp);
	}

	if (!mappings || map) {
	    cpioMode = ch.mode;

	    if (map) {
		if (map->mapFlags & CPIO_MAP_PATH) {
		    free(ch.path);
		    ch.path = strdup(map->finalPath);
		} 

		if (map->mapFlags & CPIO_MAP_MODE)
		    ch.mode = map->finalMode;
		if (map->mapFlags & CPIO_MAP_UID)
		    ch.uid = map->finalUid;
		if (map->mapFlags & CPIO_MAP_GID)
		    ch.gid = map->finalGid;
	    }

    	    rc = checkDirectory(ch.path);

	    if (!rc) {
		if (S_ISREG(ch.mode))
		    rc = expandRegular(&fd, &ch);
		else if (S_ISDIR(ch.mode))
		    rc = createDirectory(ch.path);
		else if (S_ISLNK(ch.mode))
		    rc = expandSymlink(&fd, &ch);
		else if (S_ISFIFO(ch.mode))
		    rc = expandFifo(&fd, &ch);
		else if (S_ISCHR(ch.mode) || S_ISBLK(ch.mode))
		    rc = expandDevice(&fd, &ch);
		else if (S_ISSOCK(ch.mode)) {
		    /* should we do something here??? */
		    rc = 0;
		} else {
		    rc = CPIO_INTERNAL;
		}
	    }

	    if (!rc)
		rc = setInfo(&ch);

	    if (rc) {
		*failedFile = strdup(ch.path);

		olderr = errno;
		unlink(ch.path);
		errno = olderr;
	    }
	}

	padfd(&fd, 4);
	free(ch.path);
    } while (1 && !rc);

    return rc;
}

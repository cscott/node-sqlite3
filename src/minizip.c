#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(USE_SYSTEM_ZLIB)
#include <zlib.h>
#else
#include "zlib.h"
#endif
#include "unzip.h"

#include <sqlite3.h>
#include "minizip.h"

/************************ Shim Definitions ******************************/

#ifndef SQLITE_UNZIP_VFS_NAME
# define SQLITE_UNZIP_VFS_NAME "unzip"
#endif

/*
** The sqlite3_file object for the unzipper VFS
*/
typedef struct unzipper_file unzipper_file;
struct unzipper_file {
    sqlite3_file base;        /* Base class.  Must be first */
    unzFile zipfile;          /* The zip file containing the db */
    unz_file_pos start;       /* the start of the current file */
    sqlite3_int64 pos;        /* our position within that file */
};

/*
** Method declarations for unzipper_file.
*/
static int unzipperClose(sqlite3_file*);
static int unzipperRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int unzipperWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64);
static int unzipperTruncate(sqlite3_file*, sqlite3_int64 size);
static int unzipperSync(sqlite3_file*, int flags);
static int unzipperFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int unzipperLock(sqlite3_file*, int);
static int unzipperUnlock(sqlite3_file*, int);
static int unzipperCheckReservedLock(sqlite3_file*, int *);
static int unzipperFileControl(sqlite3_file*, int op, void *pArg);
static int unzipperSectorSize(sqlite3_file*);
static int unzipperDeviceCharacteristics(sqlite3_file*);
#if 0
static int unzipperShmLock(sqlite3_file*,int,int,int);
static int unzipperShmMap(sqlite3_file*,int,int,int, void volatile **);
static void unzipperShmBarrier(sqlite3_file*);
static int unzipperShmUnmap(sqlite3_file*,int);
#endif

/*
** Method declarations for unzipper_vfs.
*/
static int unzipperOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
#if 0
static int unzipperDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int unzipperAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int unzipperFullPathname(sqlite3_vfs*, const char *zName, int, char *);
static void *unzipperDlOpen(sqlite3_vfs*, const char *zFilename);
static void unzipperDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*unzipperDlSym(sqlite3_vfs*,void*, const char *zSymbol))(void);
static void unzipperDlClose(sqlite3_vfs*, void*);
static int unzipperRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int unzipperSleep(sqlite3_vfs*, int microseconds);
static int unzipperCurrentTime(sqlite3_vfs*, double*);
static int unzipperGetLastError(sqlite3_vfs*, int, char*);
static int unzipperCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);
static int unzipperSetSystemCall(sqlite3_vfs*,const char*, sqlite3_syscall_ptr);
static sqlite3_syscall_ptr unzipperGetSystemCall(sqlite3_vfs*, const char *);
static const char *unzipperNextSystemCall(sqlite3_vfs*, const char *zName);
#endif

/************************ Object Definitions ******************************/

/************************* Global Variables **********************************/
/*
** All global variables used by this file are containing within the following
** gUnzip structure.
*/
static struct {
  /* The pOrigVfs is the real, original underlying VFS implementation.
  ** Most operations pass-through to the real VFS.  This value is read-only
  ** during operation.  It is only modified at start-time and thus does not
  ** require a mutex.
  */
  sqlite3_vfs *pOrigVfs;

  /* The sThisVfs is the VFS structure used by this shim.  It is initialized
  ** at start-time and thus does not require a mutex
  */
  sqlite3_vfs sThisVfs;

  /* The sIoMethods defines the methods used by sqlite3_file objects
  ** associated with this shim.  It is initialized at start-time and does
  ** not require a mutex.
  **
  ** When the underlying VFS is called to open a file, it might return
  ** either a version 1 or a version 2 sqlite3_file object.  This shim
  ** has to create a wrapper sqlite3_file of the same version.  Hence
  ** there are two I/O method structures, one for version 1 and the other
  ** for version 2.
  */
  sqlite3_io_methods sIoMethodsV1;
  sqlite3_io_methods sIoMethodsV2;

  /* True when this shim has been initialized.
  */
  int isInitialized;

  /* For run-time access any of the other global data structures in this
  ** shim, the following mutex must be held.
  */
  sqlite3_mutex *pMutex;
} gUnzipper;


/** SHIMS! **/
static int unzipperClose(
  sqlite3_file *pFile
) {
    unzipper_file *p = (unzipper_file *)pFile;
    if (p->zipfile) {
        unzCloseCurrentFile(p->zipfile);
        unzClose(p->zipfile);
        p->zipfile = NULL;
    }
    return SQLITE_OK;
}
static int unzipperRead(
  sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst
) {
    unzipper_file *p = (unzipper_file *)pFile;
    int rc;
    if (iOfst != p->pos) {
        // XXX improve seek in uncompressed files
        if (iOfst < p->pos) {
            unzCloseCurrentFile(p->zipfile);
            unzGoToFilePos(p->zipfile, &(p->start));
            unzOpenCurrentFile(p->zipfile);
            p->pos = 0;
        }
        char buf[1024];
        while (p->pos < iOfst) {
            sqlite3_int64 sz = iOfst - p->pos;
            if (sz > sizeof(buf)) { sz = sizeof(buf); }
            rc = unzipperRead(pFile, buf, sz, p->pos);
            if (!(rc == SQLITE_OK || rc == SQLITE_IOERR_SHORT_READ)) {
                return rc;
            }
        }
    }
    rc = unzReadCurrentFile(p->zipfile, zBuf, iAmt);
    if (rc < 0) {
        return SQLITE_IOERR;
    }
    p->pos += rc;
    if (rc == iAmt) {
        return SQLITE_OK;
    }
    return SQLITE_IOERR_SHORT_READ;
}
static int unzipperWrite(
  sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite3_int64 iOfst
) {
    return SQLITE_READONLY;
}
static int unzipperTruncate(
  sqlite3_file *pFile, sqlite3_int64 size
) {
    return SQLITE_READONLY;
}
static int unzipperSync(
  sqlite3_file *pFile, int flags
) {
    return SQLITE_OK;
}
static int unzipperFileSize(
  sqlite3_file *pFile, sqlite3_int64 *pSize
) {
    unzipper_file *p = (unzipper_file *)pFile;
    unz_file_info info;
    int rc;

    rc = unzGetCurrentFileInfo(p->zipfile, &info, NULL, 0, NULL, 0, NULL, 0);
    if (rc != SQLITE_OK) {
        return SQLITE_IOERR_FSTAT;
    }
    *pSize = info.uncompressed_size;
    return SQLITE_OK;
}
static int unzipperLock(
  sqlite3_file *pFile, int eLock
) {
    return SQLITE_OK;
}
static int unzipperUnlock(
  sqlite3_file *pFile, int eLock
) {
    return SQLITE_OK;
}
static int unzipperCheckReservedLock(
  sqlite3_file *pFile, int *pResOut
) {
    *pResOut = 0;
    return SQLITE_OK;
}

static int unzipperFileControl(
  sqlite3_file *pFile, int op, void *pArg
) {
    return SQLITE_OK;
}
static int unzipperSectorSize(
  sqlite3_file *pFile
) {
    return 0;
}
static int unzipperDeviceCharacteristics(
  sqlite3_file *pFile
) {
    return 0;
}

static int unzipperOpenZip(
  sqlite3_vfs *pOrigVfs,
  const char *zZipName,
  const char *zDbName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
    unzipper_file *pUnzipFile;
    int method, level;
    int rc;
    /* Attempt to open this zip file! */
    // xxx could use unzOpen2 and pass in proxied versions of the VFS funcs
    unzFile unz = unzOpen( zZipName );
    if (!unz) {
        return SQLITE_CANTOPEN;
    }
    /* Attempt to find the named database */
    rc = unzLocateFile(unz, zDbName, 0);
    if (rc != UNZ_OK) {
        unzClose(unz);
        return SQLITE_CANTOPEN;
    }
    /* Open and ensure that the file is not compressed */
    rc = unzOpenCurrentFile2(unz, &method, &level, 0);
    if (rc != UNZ_OK) {
        unzClose(unz);
        return SQLITE_CANTOPEN;
    }
    /* Ok! Let's return a wrapped file */
    pUnzipFile = (unzipper_file *) pFile;
    pUnzipFile->zipfile = unz;
    unzGetFilePos(unz, &(pUnzipFile->start));
    pUnzipFile->pos = 0;
    pUnzipFile->base.pMethods = &(gUnzipper.sIoMethodsV1);
    if ( pOutFlags ) {
        *pOutFlags = flags;
    }
    return SQLITE_OK;
}

static int unzipperOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
    sqlite3_vfs *pOrigVfs = gUnzipper.pOrigVfs;
    int rc;

    fprintf(stderr, "Opening %s\n", zName);
    /* If the file is not a main database file or a WAL, then use the
    ** normal xOpen method.
    */
    if( (flags & (SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_WAL)) == 0 ) {
        fprintf(stderr, " Not a main database file or WAL\n");
        return pOrigVfs->xOpen(pOrigVfs, zName, pFile, flags, pOutFlags);
    }

    /* Otherwise, this is an unzipped file */
    fprintf(stderr, " A main database file or WAL!\n");

    if( (flags & SQLITE_OPEN_READONLY) == 0 ) {
        return SQLITE_CANTOPEN;
    }

    // split on '/', find the zip
    const char *p = zName + strlen(zName); // pointing at trailing \0
    char *prefix = strdup(zName);
    rc = SQLITE_CANTOPEN;
    for ( ; zName < p /* at least one char in prefix */; p--) {
        if (*p == '/') {
            // try splitting string here
            prefix[p-zName] = '\0';
            rc = unzipperOpenZip(pOrigVfs, prefix, p+1, pFile, flags, pOutFlags);
            if (rc == SQLITE_OK) {
                // we were successful!
                break;
            }
        }
    }
    // couldn't open the file as a zip.
    free(prefix);
    return rc;
}

/************************** Public Interfaces *****************************/
/*
** Initialize the quota VFS shim.  Use the VFS named zOrigVfsName
** as the VFS that does the actual work.  Use the default if
** zOrigVfsName==NULL.
**
** The unzipper VFS shim is named "unzip".  It will become the default
** VFS if makeDefault is non-zero.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once
** during start-up.
*/
int sqlite3_unzipper_initialize(const char *zOrigVfsName, int makeDefault) {
  sqlite3_vfs *pOrigVfs;
  if( gUnzipper.isInitialized ) { return SQLITE_MISUSE; }
  pOrigVfs = sqlite3_vfs_find(zOrigVfsName);
  if( pOrigVfs==0 ) { return SQLITE_ERROR; }
  assert( pOrigVfs != &gUnzipper.sThisVfs );
  gUnzipper.pMutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
  if( !gUnzipper.pMutex ){
    return SQLITE_NOMEM;
  }
  gUnzipper.isInitialized = 1;
  gUnzipper.pOrigVfs = pOrigVfs;
  gUnzipper.sThisVfs = *pOrigVfs;
  gUnzipper.sThisVfs.xOpen = unzipperOpen;
  gUnzipper.sThisVfs.szOsFile += sizeof(unzipper_file);
  gUnzipper.sThisVfs.zName = SQLITE_UNZIP_VFS_NAME;
  /*
  gUnzipper.sThisVfs.xDelete = unzipperDelete;
  gUnzipper.sThisVfs.xAccess = unzipperAccess;
  gUnzipper.sThisVfs.xFullPathname = unzipperFullPathname;
  gUnzipper.sThisVfs.xDlOpen = unzipperDlOpen;
  gUnzipper.sThisVfs.xDlError = unzipperDlError;
  gUnzipper.sThisVfs.xDlSym = unzipperDlSym;
  gUnzipper.sThisVfs.xDlClose = unzipperDlClose;
  gUnzipper.sThisVfs.xRandomness = unzipperRandomness;
  gUnzipper.sThisVfs.xSleep = unzipperSleep;
  gUnzipper.sThisVfs.xCurrentTime = unzipperCurrentTime;
  gUnzipper.sThisVfs.xGetLastError = unzipperGetLastError;
  gUnzipper.sThisVfs.xCurrentTimeInt64 = unzipperCurrentTimeInt64;
  */

  gUnzipper.sIoMethodsV1.iVersion = 1;
  gUnzipper.sIoMethodsV1.xClose = unzipperClose;
  gUnzipper.sIoMethodsV1.xRead = unzipperRead;
  gUnzipper.sIoMethodsV1.xWrite = unzipperWrite;
  gUnzipper.sIoMethodsV1.xTruncate = unzipperTruncate;
  gUnzipper.sIoMethodsV1.xSync = unzipperSync;
  gUnzipper.sIoMethodsV1.xFileSize = unzipperFileSize;
  gUnzipper.sIoMethodsV1.xLock = unzipperLock;
  gUnzipper.sIoMethodsV1.xUnlock = unzipperUnlock;
  gUnzipper.sIoMethodsV1.xCheckReservedLock = unzipperCheckReservedLock;
  gUnzipper.sIoMethodsV1.xFileControl = unzipperFileControl;
  gUnzipper.sIoMethodsV1.xSectorSize = unzipperSectorSize;
  gUnzipper.sIoMethodsV1.xDeviceCharacteristics =
                                            unzipperDeviceCharacteristics;

  gUnzipper.sIoMethodsV2 = gUnzipper.sIoMethodsV1;
  gUnzipper.sIoMethodsV2.iVersion = 2;
  /*
  gUnzipper.sIoMethodsV2.xShmMap = unzipperShmMap;
  gUnzipper.sIoMethodsV2.xShmLock = unzipperShmLock;
  gUnzipper.sIoMethodsV2.xShmBarrier = unzipperShmBarrier;
  gUnzipper.sIoMethodsV2.xShmUnmap = unzipperShmUnmap;
  */
  sqlite3_vfs_register(&gUnzipper.sThisVfs, makeDefault);
  return SQLITE_OK;
}

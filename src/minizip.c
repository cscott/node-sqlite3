#include <assert.h>

#include <zlib/zlib.h>
#include <zlib/contrib/minizip/ioapi.h>
#include <zlib/contrib/minizip/unzip.h>

#include <stdio.h>
#include <sqlite3.h>
#include "minizip.h"

#if 0
void minizip_init(void) {
    unzFile uz;
    uz = unzOpen("foo.zip");
    unzClose(uz);
}
#endif

/************************ Shim Definitions ******************************/

#ifndef SQLITE_UNZIP_VFS_NAME
# define SQLITE_UNZIP_VFS_NAME "unzip"
#endif

/*
** An instance of this structure is attached to the each unzipper VFS to
** provide auxiliary information.
*/
typedef struct unzipper_info unzipper_info;
struct unzipper_info {
  sqlite3_vfs *pRootVfs;              /* The underlying real VFS */
    // XXX info on this vfs (zip files?)
    const char *zVfsName;               /* Name of this trace-VFS */
    sqlite3_vfs *pUnzipperVfs;          /* Pointer back to the unzipper VFS */
};

/*
** The sqlite3_file object for the unzipper VFS
*/
typedef struct unzipper_file unzipper_file;
struct unzipper_file {
    sqlite3_file base;        /* Base class.  Must be first */
    unzipper_info *pInfo;     /* The unzipper-VFS to which this file belongs */
    const char *zFName;       /* Base name of the file */
    sqlite3_file *pReal;      /* The real underlying file */
};

/*
** Method declarations for unzipper_file.
*/
#if 0
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
    // xx use normal xOpen for now
    rc = pOrigVfs->xOpen(pOrigVfs, zName, pFile, flags, pOutFlags);
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
  //gUnzipper.sThisVfs.xDelete = unzipperDelete;
  gUnzipper.sThisVfs.szOsFile += sizeof(unzipper_file);
  gUnzipper.sThisVfs.zName = SQLITE_UNZIP_VFS_NAME;
  /*
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
  /*
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
  */
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

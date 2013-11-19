#ifndef NODE_SQLITE3_SRC_MINIZIP_H
#define NODE_SQLITE3_SRC_MINIZIP_H

/* Inhibit C++ name-mangling for libpng functions but not for system calls. */
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

int sqlite3_unzipper_initialize(const char *zOrigVfsName, int makeDefault);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif

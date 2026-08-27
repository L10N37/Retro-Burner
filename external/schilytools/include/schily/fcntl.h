/* @(#)fcntl.h	1.21 18/07/15 Copyright 1996-2018 J. Schilling */
/*
 *	Generic header for users of open(), creat() and chmod()
 *
 *	Copyright (c) 1996-2018 J. Schilling
 */
/*
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * See the file CDDL.Schily.txt in this distribution for details.
 * A copy of the CDDL is also available via the Internet at
 * http://www.opensource.org/licenses/cddl1.txt
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file CDDL.Schily.txt from this distribution.
 */

#ifndef _SCHILY_FCNTL_H
#define _SCHILY_FCNTL_H

/*
 * RB_RETROBURNER_NATIVE_FCNTL_18
 *
 * RetroBurner no longer relies on Schilling configure-time HAVE_FCNTL_H
 * selection. Modern supported platforms provide <fcntl.h>.
 */
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>

/*
 * Native Windows CRT descriptors distinguish text and binary mode.
 * A zero O_BINARY would make ISO reads stop at DOS Ctrl-Z (0x1A).
 */
#ifndef O_BINARY
# ifdef _O_BINARY
#  define O_BINARY _O_BINARY
# else
#  error "RetroBurner Windows backend requires O_BINARY/_O_BINARY"
# endif
#endif

#if O_BINARY == 0
# error "RetroBurner Windows backend requires a non-zero O_BINARY"
#endif

#else /* !_WIN32 */

/*
 * POSIX has no text/binary descriptor distinction.
 */
#ifndef O_BINARY
#define O_BINARY 0
#endif

#endif /* _WIN32 */

/* RETROBEAM_LINUX_O_SEARCH_V6
 *
 * POSIX O_SEARCH is not exposed as such by Linux. O_PATH is the closest
 * Linux operation: obtain a path-only descriptor suitable for directory
 * operations such as fchdir()/fstat(). Fall back to O_RDONLY only if a
 * libc/kernel header does not expose O_PATH.
 */
#ifndef O_SEARCH
# if defined(__linux__) && defined(O_PATH)
#  define O_SEARCH O_PATH
# else
#  define O_SEARCH O_RDONLY
# endif
#endif

#ifndef O_NDELAY
#define O_NDELAY 0
#endif

#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY|O_WRONLY|O_RDWR)
#endif

#endif /* _SCHILY_FCNTL_H */
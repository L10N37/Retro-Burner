/* @(#)standard.h	1.40 13/05/01 Copyright 1985-2013 J. Schilling */
/*
 *	standard definitions
 *
 *	This file should be included past:
 *
 *	mconfig.h / config.h
 *	stdio.h
 *	stdlib.h	(better use schily/stdlib.h)
 *	unistd.h	(better use schily/unistd.h) needed f. LARGEFILE support
 *
 *	If you need stdio.h, you must include it before standard.h
 *
 *	Copyright (c) 1985-2013 J. Schilling
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

/*
 * RetroBurner modern portability implementation.
 * The original cdrtools copyright/license above is retained.
 */
#ifndef _SCHILY_STANDARD_H
#define _SCHILY_STANDARD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <schily/type_val.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define EX_BAD (-1)
#define EX_CLASH (-64)

#define GLOBAL extern
#define IMPORT extern
#define EXPORT
#define INTERN static
#define LOCAL static
#define FAST

typedef int BOOL;

/* C11/current target OSes always provide these fundamental types. */
#define FOUND_SIZE_T 1
#define FOUND_OFF_T 1

#endif /* _SCHILY_STANDARD_H */

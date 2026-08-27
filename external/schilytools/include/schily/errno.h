/* @(#)errno.h	1.7 10/08/24 Copyright 2006-2010 J. Schilling */
/*
 *	Error number related definitions
 *
 *	Copyright (c) 2006-2010 J. Schilling
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
#ifndef _SCHILY_ERRNO_H
#define _SCHILY_ERRNO_H

#include <errno.h>
#include <schily/prototyp.h>

#ifndef seterrno
extern int seterrno __PR((int));
#endif
extern int geterrno __PR((void));

#endif /* _SCHILY_ERRNO_H */

/* @(#)utypes.h	1.38 21/07/11 Copyright 1997-2021 J. Schilling */
/*
 *	Definitions for some user defined types
 *
 *	Copyright (c) 1997-2021 J. Schilling
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
#ifndef _SCHILY_UTYPES_H
#define _SCHILY_UTYPES_H

#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Legacy Schily aliases retained at the portability boundary.
 * The modern build maps them directly onto C99 fixed-width types.
 */
typedef int8_t      Int8_t;
typedef uint8_t     UInt8_t;
typedef int16_t     Int16_t;
typedef uint16_t    UInt16_t;
typedef int32_t     Int32_t;
typedef uint32_t    UInt32_t;
typedef int64_t     Int64_t;
typedef uint64_t    UInt64_t;
typedef intptr_t    Intptr_t;
typedef uintptr_t   UIntptr_t;

typedef unsigned char      Uchar;
typedef unsigned short     Ushort;
typedef unsigned int       Uint;
typedef unsigned long      Ulong;
typedef long long          Llong;
typedef unsigned long long Ullong;

#endif /* _SCHILY_UTYPES_H */

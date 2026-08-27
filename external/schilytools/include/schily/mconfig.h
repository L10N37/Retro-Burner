/* @(#)mconfig.h	1.73 19/04/03 Copyright 1995-2019 J. Schilling */
/*
 *	definitions for machine configuration
 *
 *	Copyright (c) 1995-2019 J. Schilling
 *
 *	This file must be included before any other file.
 *	If this file is not included before stdio.h you will not be
 *	able to get LARGEFILE support
 *
 *	Use only cpp instructions.
 *
 *	NOTE: SING: (Schily Is Not Gnu)
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
#ifndef _SCHILY_MCONFIG_H
#define _SCHILY_MCONFIG_H

#include <retroburner/config.h>
#include <stdint.h>
#include <stddef.h>
#include <schily/archdefs.h>

#ifdef _WIN32
#define IS_GCC_WIN32 1
#define PATH_DELIM '\\'
#define PATH_DELIM_STR "\\" 
#define PATH_ENV_DELIM ';'
#define PATH_ENV_DELIM_STR ";"
#else
#define IS_UNIX 1
#define PATH_DELIM '/'
#define PATH_DELIM_STR "/"
#define PATH_ENV_DELIM ':'
#define PATH_ENV_DELIM_STR ":"
#endif

#if defined(__APPLE__) && defined(__MACH__)
#define IS_MACOS_X 1
#endif

#if defined(__linux__)
#define IS_LINUX 1
#endif

#include <schily/prototyp.h>

#endif /* _SCHILY_MCONFIG_H */

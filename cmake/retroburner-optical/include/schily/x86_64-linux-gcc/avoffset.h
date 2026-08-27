/*
 * RetroBeam / RetroBurner CMake portability header.
 *
 * x86-64 Linux stack grows toward lower addresses. AV_OFFSET and FP_INDIR
 * are deliberately omitted: modern optimizing compilers do not guarantee the
 * legacy frame-chain assumptions. The inherited Schily code detects their
 * absence and disables stack scanning.
 */
#ifndef RETROBURNER_SCHILY_X86_64_LINUX_AVOFFSET_H
#define RETROBURNER_SCHILY_X86_64_LINUX_AVOFFSET_H

#define STACK_DIRECTION (-1)

#endif

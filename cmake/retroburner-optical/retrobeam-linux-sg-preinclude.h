/*
 * RetroBeam CMake-only Linux libscg preinclude.
 *
 * scsihack.c textually includes the original scsi-linux-sg.c transport.
 * Pre-include linux/cdrom.h once, then hide the two legacy packet macros
 * that would otherwise pull in scsi-linux-ata.c (/dev/hd* direct ATAPI).
 *
 * This does NOT disable Linux SG_IO.
 */
#ifndef RETROBEAM_LINUX_SG_PREINCLUDE_H
#define RETROBEAM_LINUX_SG_PREINCLUDE_H

#include <linux/cdrom.h>

#ifdef CDROM_PACKET_SIZE
#undef CDROM_PACKET_SIZE
#endif
#ifdef CDROM_SEND_PACKET
#undef CDROM_SEND_PACKET
#endif

#endif

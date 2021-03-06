/*-
 * Copyright (c) 2005 Stanislav Sedov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/sys/geom/label/g_label_ext2fs.c,v 1.2.10.1.2.1 2009/10/25 01:10:29 kensmith Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <geom/geom.h>
#include <geom/label/g_label.h>

#define EXT2FS_SB_OFFSET	1024
#define EXT2_SUPER_MAGIC	0xef53
#define EXT2_DYNAMIC_REV	1

typedef struct e2sb {
	uint8_t		fake1[56];
	uint16_t	s_magic;
	uint8_t		fake2[18];
	uint32_t	s_rev_level;
	uint8_t		fake3[40];
	char		s_volume_name[16];
} e2sb_t;

static void
g_label_ext2fs_taste(struct g_consumer *cp, char *label, size_t size)
{
	struct g_provider *pp;
	e2sb_t *fs;

	g_topology_assert_not();
	pp = cp->provider;
	label[0] = '\0';

	if ((EXT2FS_SB_OFFSET % pp->sectorsize) != 0)
		return;

	fs = (e2sb_t *)g_read_data(cp, EXT2FS_SB_OFFSET, pp->sectorsize, NULL);
	if (fs == NULL)
		return;

	/* Check for magic and versio n*/
	if (fs->s_magic == EXT2_SUPER_MAGIC &&
	    fs->s_rev_level == EXT2_DYNAMIC_REV) {
		G_LABEL_DEBUG(1, "ext2fs file system detected on %s.",
		    pp->name);
	} else {
		goto exit_free;
	}

	/* Check for volume label */
	if (fs->s_volume_name[0] == '\0')
		goto exit_free;

	/* Terminate label */
	fs->s_volume_name[sizeof(fs->s_volume_name) - 1] = '\0';
	strlcpy(label, fs->s_volume_name, size);

exit_free:
	g_free(fs);
}

const struct g_label_desc g_label_ext2fs = {
	.ld_taste = g_label_ext2fs_taste,
	.ld_dir = "ext2fs"
};

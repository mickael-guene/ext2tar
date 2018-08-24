/* This file is part of ext2tar. A tool to export ext image to tarball
 * archive without root rights.
 *
 * Copyright (C) 2018 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include <stdio.h>
#include <stdarg.h>
#include <ext2fs/ext2fs.h>
#include <archive.h>
#include <archive_entry.h>

struct parser_ctx {
	ext2_filsys fs;
	ext2_dblist dblist;
	ext2_ino_t ino;
	struct archive *a;
};

void fatal(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);

	exit(-1);
}

void warn(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
}

static int is_fast_symlink(struct ext2_inode *inode)
{
	return LINUX_S_ISLNK(inode->i_mode) && EXT2_I_SIZE(inode) &&
	       EXT2_I_SIZE(inode) < sizeof(inode->i_block);
}

static int append_file_content(struct parser_ctx *ctx, struct ext2_inode *inode, char *name)
{
	int err;
	ext2_file_t fd;
	char buf[512];

	err = ext2fs_file_open2(ctx->fs, 0, inode, 0, &fd);
	if (err)
		fatal("ext2fs_file_open2 %d\n", err);
	while(1) {
		unsigned int got;
		int wr;

		err = ext2fs_file_read(fd, buf, sizeof(buf), &got);
		if (err)
			fatal("ext2fs_file_read %d\n", err);
		if (!got)
			break;
		wr = archive_write_data(ctx->a, buf, got);
		if (wr != got)
			fatal("wr != got\n", err);
	}
	ext2fs_file_close(fd);

	return 0;
}

static int set_symlink_target(struct parser_ctx *ctx, struct ext2_inode *inode, char *name, struct archive_entry *entry)
{
	int err;
	ext2_file_t fd;
	char *buf;

	buf = malloc(inode->i_size + 1);
	if (!buf)
		fatal("ENOMEM\n");

	buf[0] = '\0';
	if (is_fast_symlink(inode)) {
		strcpy(buf, (char *) inode->i_block);
	} else {
		unsigned int got;
		int sz = inode->i_size;
		char *p = buf;

		err = ext2fs_file_open2(ctx->fs, 0, inode, 0, &fd);
		if (err)
			fatal("ext2fs_file_open2 %d\n", err);
		while(sz) {
			err = ext2fs_file_read(fd, p, sz, &got);
			if (err)
				fatal("ext2fs_file_read %d\n", err);
			sz -= got;
			p += got;
			if (got == 0)
				break;
		}
		if (sz)
			fatal("unable to read symlink target\n");
		ext2fs_file_close(fd);
	}
	buf[inode->i_size] = '\0';

	if (!buf[0])
		warn("empty symlink target for %s\n", name);
	archive_entry_set_symlink(entry, buf);
	free(buf);

	return 0;
}

static int set_rdev(struct parser_ctx *ctx, struct ext2_inode *inode, char *name, struct archive_entry *entry)
{
	dev_t dev;

	if (inode->i_block[1])
		dev = makedev((inode->i_block[1] >> 8) & 0xff, (inode->i_block[0] & 0xff) | (inode->i_block[0] >> 12));
	else
		dev = makedev(inode->i_block[0] >> 8, inode->i_block[0] & 0xff);
	archive_entry_set_rdev(entry, dev);

	return 0;
}

static int append_inode(struct parser_ctx *ctx, struct ext2_inode *inode, char *name)
{
	struct archive_entry *entry;
	int err;

	entry = archive_entry_new();
	/* remove leading slash in tar archive */
	archive_entry_set_pathname(entry, name+1);
	archive_entry_set_mode(entry, inode->i_mode);
	archive_entry_set_size(entry, inode->i_size);
	archive_entry_set_nlink(entry, inode->i_links_count);
	archive_entry_set_uid(entry, inode->i_uid);
	archive_entry_set_gid(entry, inode->i_gid);
	archive_entry_set_atime(entry, inode->i_atime, 0);
	archive_entry_set_ctime(entry, inode->i_ctime, 0);
	archive_entry_set_mtime(entry, inode->i_mtime, 0);
	if (LINUX_S_ISLNK(inode->i_mode)) {
		err =set_symlink_target(ctx, inode, name, entry);
		if (err)
			goto error;
	} else if (LINUX_S_ISCHR(inode->i_mode)) {
		err =set_rdev(ctx, inode, name, entry);
		if (err)
			goto error;
	}
	err = archive_write_header(ctx->a, entry);
	if (err)
		goto error;
	if (LINUX_S_ISREG(inode->i_mode)) {
		err = append_file_content(ctx, inode, name);
		if (err)
			goto error;
	}
	archive_entry_free(entry);

	return 0;

error:
	warn("append_inode error %d for %s\n", err, name);
	archive_entry_free(entry);

	return err;
}

static int process_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private)
{
	struct parser_ctx *ctx = private;

	return ext2fs_add_dir_block2(ctx->dblist, ctx->ino, *blocknr, blockcnt);
}

static int get_fullpathname(struct parser_ctx *ctx, ext2_ino_t dir, struct ext2_dir_entry *dirent, char **name)
{
	errcode_t err;
	char *dir_name;
	char *name_ret;
	int len;

	err = ext2fs_get_pathname(ctx->fs, dir, 0, &dir_name);
	if (err)
		fatal("ext2fs_get_pathname %d\n", err);
	len = strlen(dir_name)+ 1 + (dirent->name_len & 0xff) + 1;
	name_ret = malloc(len);
	if (!name_ret)
		fatal("ENOMEM\n");

	name_ret[0] = '\0';
	if (dir_name[1])
		strcat(name_ret, dir_name);
	else
		len--;
	strcat(name_ret, "/");
	strncat(name_ret, dirent->name, (dirent->name_len & 0xff));
	name_ret[len - 1] = '\0';
	*name = name_ret;
	free(dir_name);

	return 0;
}

static int process_inode(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private)
{
	errcode_t err;
	struct parser_ctx *ctx = private;
	struct ext2_inode inode;
	char *name;

	if (entry != DIRENT_OTHER_FILE)
		return 0;

	err = get_fullpathname(ctx, dir, dirent, &name);
	if (err)
		fatal("get_fullpathname %d\n", err);
	err = ext2fs_read_inode(ctx->fs, dirent->inode, &inode);
	if (err)
		fatal("ext2fs_read_inode %d\n", err);

	if (LINUX_S_ISDIR(inode.i_mode)) {
		append_inode(ctx, &inode, name);
	} else if (LINUX_S_ISREG(inode.i_mode)) {
		if (inode.i_links_count == 0)
			fatal("link at zero for file %s/n", name);
		else if (inode.i_links_count > 1)
			warn("hard link not yet fully supported, file %s is duplicated\n", name);
		append_inode(ctx, &inode, name);
	} else if (LINUX_S_ISLNK(inode.i_mode)) {
		append_inode(ctx, &inode, name);
	} else if (LINUX_S_ISCHR(inode.i_mode)) {
		append_inode(ctx, &inode, name);
	} else if (LINUX_S_ISBLK(inode.i_mode)) {
		printf("block device %s\n", name);
		fatal("block device not supported\n");
	} else if (LINUX_S_ISFIFO(inode.i_mode)) {
		printf("fifo %s\n", name);
		fatal("fifo not supported\n");
	} else if (LINUX_S_ISSOCK(inode.i_mode)) {
		printf("socket %s\n", name);
		fatal("socket not supported\n");
	}

	free(name);

	return 0;
}

int main(int argc, char **argv)
{
	errcode_t err;
	struct parser_ctx ctx;
	ext2_inode_scan scan;
	ext2_ino_t ino;
	struct ext2_inode inode;

	ctx.a = archive_write_new();
	archive_write_set_format_pax_restricted(ctx.a);
	archive_write_open_filename(ctx.a, argv[2]);

	err = ext2fs_open(argv[1], 0, 0, 0, unix_io_manager, &ctx.fs);
	if (err)
		fatal("Unable to open %s\n", argv[1]);
	err = ext2fs_init_dblist(ctx.fs, &ctx.dblist);
	if (err)
		fatal("Unable to init dblist %d\n", err);

	/* build dblist with full file system */
	err = ext2fs_open_inode_scan(ctx.fs, 0, &scan);
	if (err)
		fatal("Unable to init scan iterator", argv[1]);
	while (1) {
		err = ext2fs_get_next_inode(scan, &ino, &inode);
		if (err)
			fatal("Fail to get next inode %d\n", err);
		if (ino == 0)
			break;
		if (ext2fs_inode_has_valid_blocks2(ctx.fs, &inode) &&
			LINUX_S_ISDIR(inode.i_mode)) {
			ctx.ino = ino;
			err = ext2fs_block_iterate(ctx.fs, ino, 0, NULL, process_block, &ctx);
			if (err)
				fatal("ext2fs_block_iterate %d\n", err);
		}/* else if ((inode.i_flags & EXT4_INLINE_DATA_FL) &&
			LINUX_S_ISDIR(inode.i_mode)) {
			err = ext2fs_add_dir_block2(ctx.dblist, ino, 0, 0);
		}*/
	}
	ext2fs_close_inode_scan(scan);

	/* parse dblist */
	err = ext2fs_dblist_dir_iterate(ctx.dblist, 0/*DIRENT_FLAG_INCLUDE_EMPTY*/, NULL, process_inode, &ctx);
	if (err)
		fatal("ext2fs_dblist_dir_iterate %d\n", err);

	ext2fs_free_dblist(ctx.dblist);
	ext2fs_free(ctx.fs);

	archive_write_close(ctx.a);
	archive_write_free(ctx.a);

	return 0;
}

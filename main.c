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

static int append_inode(struct parser_ctx *ctx, struct ext2_inode *inode, char *name)
{
	struct archive_entry *entry;
	int err;

	entry = archive_entry_new();
	archive_entry_set_pathname(entry, name+1);
	archive_entry_set_mode(entry, inode->i_mode);
	archive_entry_set_size(entry, inode->i_size);
	archive_entry_set_nlink(entry, inode->i_links_count);
	archive_entry_set_uid(entry, inode->i_uid);
	archive_entry_set_gid(entry, inode->i_gid);
	archive_entry_set_atime(entry, inode->i_atime, 0);
	archive_entry_set_ctime(entry, inode->i_ctime, 0);
	archive_entry_set_mtime(entry, inode->i_mtime, 0);
	err = archive_write_header(ctx->a, entry);
	if (err)
		return err;
	if (LINUX_S_ISREG(inode->i_mode)) {
		err = append_file_content(ctx, inode, name);
		if (err)
			return err;
	}
	archive_entry_free(entry);

	return 0;
}

static int process_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private)
{
	struct parser_ctx *ctx = private;

	return ext2fs_add_dir_block2(ctx->dblist, ctx->ino, *blocknr, blockcnt);
}

static int process_inode(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *private)
{
	errcode_t err;
	struct parser_ctx *ctx = private;
	struct ext2_inode inode;
	char *name;

	if (entry != DIRENT_OTHER_FILE)
		return 0;

	err = ext2fs_read_inode(ctx->fs, dirent->inode, &inode);
	if (err)
		fatal("ext2fs_read_inode %d\n", err);
	err = ext2fs_get_pathname(ctx->fs, dir, dirent->inode, &name);
	if (err)
		fatal("ext2fs_get_pathname %d\n", err);

	if (LINUX_S_ISDIR(inode.i_mode)) {
		printf("dir %s\n", name);
		append_inode(ctx, &inode, name);
	} else if (LINUX_S_ISREG(inode.i_mode)) {
		printf("file %s\n", name);
		append_inode(ctx, &inode, name);
	} else if (LINUX_S_ISLNK(inode.i_mode)) {
		printf("link %s\n", name);
	} else if (LINUX_S_ISCHR(inode.i_mode)) {
		printf("char device %s\n", name);
	} else if (LINUX_S_ISBLK(inode.i_mode)) {
		printf("block device %s\n", name);
	} else if (LINUX_S_ISFIFO(inode.i_mode)) {
		printf("fifo %s\n", name);
	} else if (LINUX_S_ISSOCK(inode.i_mode)) {
		printf("socket %s\n", name);
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
	archive_write_open_filename(ctx.a, "test.tar");

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

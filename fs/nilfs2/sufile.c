// SPDX-License-Identifier: GPL-2.0+
/*
 * NILFS segment usage file.
 *
 * Copyright (C) 2006-2008 Nippon Telegraph and Telephone Corporation.
 *
 * Written by Koji Sato.
 * Revised by Ryusuke Konishi.
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/errno.h>
#include "mdt.h"
#include "sufile.h"

#include <trace/events/nilfs2.h>

/**
 * struct nilfs_sufile_info - on-memory private data of sufile
 * @mi: on-memory private data of metadata file
 * @ncleansegs: number of clean segments
 * @allocmin: lower limit of allocatable segment range
 * @allocmax: upper limit of allocatable segment range
 */
struct nilfs_sufile_info {
	struct nilfs_mdt_info mi;
	unsigned long ncleansegs;/* number of clean segments */
	__u64 allocmin;		/* lower limit of allocatable segment range */
	__u64 allocmax;		/* upper limit of allocatable segment range */
};

static inline struct nilfs_sufile_info *NILFS_SUI(struct inode *sufile)
{
	return (struct nilfs_sufile_info *)NILFS_MDT(sufile);
}

static inline unsigned long
nilfs_sufile_segment_usages_per_block(const struct inode *sufile)
{
	return NILFS_MDT(sufile)->mi_entries_per_block;
}

static unsigned long
nilfs_sufile_get_blkoff(const struct inode *sufile, __u64 segnum)
{
	__u64 t = segnum + NILFS_MDT(sufile)->mi_first_entry_offset;

	t = div64_ul(t, nilfs_sufile_segment_usages_per_block(sufile));
	return (unsigned long)t;
}

static unsigned long
nilfs_sufile_get_offset(const struct inode *sufile, __u64 segnum)
{
	__u64 t = segnum + NILFS_MDT(sufile)->mi_first_entry_offset;

	return do_div(t, nilfs_sufile_segment_usages_per_block(sufile));
}

static unsigned long
nilfs_sufile_segment_usages_in_block(const struct inode *sufile, __u64 curr,
				     __u64 max)
{
	return min_t(unsigned long,
		     nilfs_sufile_segment_usages_per_block(sufile) -
		     nilfs_sufile_get_offset(sufile, curr),
		     max - curr + 1);
}

/**
 * nilfs_sufile_segment_usage_offset - calculate the byte offset of a segment
 *                                     usage entry in the folio containing it
 * @sufile: segment usage file inode
 * @segnum: number of segment usage
 * @bh:     buffer head of block containing segment usage indexed by @segnum
 *
 * Return: Byte offset in the folio of the segment usage entry.
 */
static size_t nilfs_sufile_segment_usage_offset(const struct inode *sufile,
						__u64 segnum,
						struct buffer_head *bh)
{
	return offset_in_folio(bh->b_folio, bh->b_data) +
		nilfs_sufile_get_offset(sufile, segnum) *
		NILFS_MDT(sufile)->mi_entry_size;
}

static int nilfs_sufile_get_header_block(struct inode *sufile,
					 struct buffer_head **bhp)
{
	int err = nilfs_mdt_get_block(sufile, 0, 0, NULL, bhp);

	if (unlikely(err == -ENOENT)) {
		nilfs_error(sufile->i_sb,
			    "missing header block in segment usage metadata");
		err = -EIO;
	}
	return err;
}

static inline int
nilfs_sufile_get_segment_usage_block(struct inode *sufile, __u64 segnum,
				     int create, struct buffer_head **bhp)
{
	return nilfs_mdt_get_block(sufile,
				   nilfs_sufile_get_blkoff(sufile, segnum),
				   create, NULL, bhp);
}

static int nilfs_sufile_delete_segment_usage_block(struct inode *sufile,
						   __u64 segnum)
{
	return nilfs_mdt_delete_block(sufile,
				      nilfs_sufile_get_blkoff(sufile, segnum));
}

static void nilfs_sufile_mod_counter(struct buffer_head *header_bh,
				     u64 ncleanadd, u64 ndirtyadd)
{
	struct nilfs_sufile_header *header;

	header = kmap_local_folio(header_bh->b_folio, 0);
	le64_add_cpu(&header->sh_ncleansegs, ncleanadd);
	le64_add_cpu(&header->sh_ndirtysegs, ndirtyadd);
	kunmap_local(header);

	mark_buffer_dirty(header_bh);
}

/**
 * nilfs_sufile_get_ncleansegs - return the number of clean segments
 * @sufile: inode of segment usage file
 *
 * Return: Number of clean segments.
 */
unsigned long nilfs_sufile_get_ncleansegs(struct inode *sufile)
{
	return NILFS_SUI(sufile)->ncleansegs;
}

/**
 * nilfs_sufile_updatev - modify multiple segment usages at a time
 * @sufile: inode of segment usage file
 * @segnumv: array of segment numbers
 * @nsegs: size of @segnumv array
 * @create: creation flag
 * @ndone: place to store number of modified segments on @segnumv
 * @dofunc: primitive operation for the update
 *
 * Description: nilfs_sufile_updatev() repeatedly calls @dofunc
 * against the given array of segments.  The @dofunc is called with
 * buffers of a header block and the sufile block in which the target
 * segment usage entry is contained.  If @ndone is given, the number
 * of successfully modified segments from the head is stored in the
 * place @ndone points to.
 *
 * Return: 0 on success, or one of the following negative error codes on
 * failure:
 * * %-EINVAL	- Invalid segment usage number
 * * %-EIO	- I/O error (including metadata corruption).
 * * %-ENOENT	- Given segment usage is in hole block (may be returned if
 *		  @create is zero)
 * * %-ENOMEM	- Insufficient memory available.
 */
int nilfs_sufile_updatev(struct inode *sufile, __u64 *segnumv, size_t nsegs,
			 int create, size_t *ndone,
			 void (*dofunc)(struct inode *, __u64,
					struct buffer_head *,
					struct buffer_head *))
{
	struct buffer_head *header_bh, *bh;
	unsigned long blkoff, prev_blkoff;
	__u64 *seg;
	size_t nerr = 0, n = 0;
	int ret = 0;

	if (unlikely(nsegs == 0))
		goto out;

	down_write(&NILFS_MDT(sufile)->mi_sem);
	for (seg = segnumv; seg < segnumv + nsegs; seg++) {
		if (unlikely(*seg >= nilfs_sufile_get_nsegments(sufile))) {
			nilfs_warn(sufile->i_sb,
				   "%s: invalid segment number: %llu",
				   __func__, (unsigned long long)*seg);
			nerr++;
		}
	}
	if (nerr > 0) {
		ret = -EINVAL;
		goto out_sem;
	}

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;

	seg = segnumv;
	blkoff = nilfs_sufile_get_blkoff(sufile, *seg);
	ret = nilfs_mdt_get_block(sufile, blkoff, create, NULL, &bh);
	if (ret < 0)
		goto out_header;

	for (;;) {
		dofunc(sufile, *seg, header_bh, bh);

		if (++seg >= segnumv + nsegs)
			break;
		prev_blkoff = blkoff;
		blkoff = nilfs_sufile_get_blkoff(sufile, *seg);
		if (blkoff == prev_blkoff)
			continue;

		/* get different block */
		brelse(bh);
		ret = nilfs_mdt_get_block(sufile, blkoff, create, NULL, &bh);
		if (unlikely(ret < 0))
			goto out_header;
	}
	brelse(bh);

 out_header:
	n = seg - segnumv;
	brelse(header_bh);
 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
 out:
	if (ndone)
		*ndone = n;
	return ret;
}

int nilfs_sufile_update(struct inode *sufile, __u64 segnum, int create,
			void (*dofunc)(struct inode *, __u64,
				       struct buffer_head *,
				       struct buffer_head *))
{
	struct buffer_head *header_bh, *bh;
	int ret;

	if (unlikely(segnum >= nilfs_sufile_get_nsegments(sufile))) {
		nilfs_warn(sufile->i_sb, "%s: invalid segment number: %llu",
			   __func__, (unsigned long long)segnum);
		return -EINVAL;
	}
	down_write(&NILFS_MDT(sufile)->mi_sem);

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;

	ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, create, &bh);
	if (!ret) {
		dofunc(sufile, segnum, header_bh, bh);
		brelse(bh);
	}
	brelse(header_bh);

 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

/**
 * nilfs_sufile_set_alloc_range - limit range of segment to be allocated
 * @sufile: inode of segment usage file
 * @start: minimum segment number of allocatable region (inclusive)
 * @end: maximum segment number of allocatable region (inclusive)
 *
 * Return: 0 on success, or %-ERANGE if segment range is invalid.
 */
int nilfs_sufile_set_alloc_range(struct inode *sufile, __u64 start, __u64 end)
{
	struct nilfs_sufile_info *sui = NILFS_SUI(sufile);
	__u64 nsegs;
	int ret = -ERANGE;

	down_write(&NILFS_MDT(sufile)->mi_sem);
	nsegs = nilfs_sufile_get_nsegments(sufile);

	if (start <= end && end < nsegs) {
		sui->allocmin = start;
		sui->allocmax = end;
		ret = 0;
	}
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

/**
 * nilfs_sufile_alloc - allocate a segment
 * @sufile: inode of segment usage file
 * @segnump: pointer to segment number
 *
 * Description: nilfs_sufile_alloc() allocates a clean segment, and stores
 * its segment number in the place pointed to by @segnump.
 *
 * Return: 0 on success, or one of the following negative error codes on
 * failure:
 * * %-EIO	- I/O error (including metadata corruption).
 * * %-ENOMEM	- Insufficient memory available.
 * * %-ENOSPC	- No clean segment left.
 */
int nilfs_sufile_alloc(struct inode *sufile, __u64 *segnump)
{
	struct buffer_head *header_bh, *su_bh;
	struct nilfs_sufile_header *header;
	struct nilfs_segment_usage *su;
	struct nilfs_sufile_info *sui = NILFS_SUI(sufile);
	size_t susz = NILFS_MDT(sufile)->mi_entry_size;
	__u64 segnum, maxsegnum, last_alloc;
	size_t offset;
	void *kaddr;
	unsigned long nsegments, nsus, cnt;
	int ret, j;

	down_write(&NILFS_MDT(sufile)->mi_sem);

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;
	header = kmap_local_folio(header_bh->b_folio, 0);
	last_alloc = le64_to_cpu(header->sh_last_alloc);
	kunmap_local(header);

	nsegments = nilfs_sufile_get_nsegments(sufile);
	maxsegnum = sui->allocmax;
	segnum = last_alloc + 1;
	if (segnum < sui->allocmin || segnum > sui->allocmax)
		segnum = sui->allocmin;

	for (cnt = 0; cnt < nsegments; cnt += nsus) {
		if (segnum > maxsegnum) {
			if (cnt < sui->allocmax - sui->allocmin + 1) {
				/*
				 * wrap around in the limited region.
				 * if allocation started from
				 * sui->allocmin, this never happens.
				 */
				segnum = sui->allocmin;
				maxsegnum = last_alloc;
			} else if (segnum > sui->allocmin &&
				   sui->allocmax + 1 < nsegments) {
				segnum = sui->allocmax + 1;
				maxsegnum = nsegments - 1;
			} else if (sui->allocmin > 0)  {
				segnum = 0;
				maxsegnum = sui->allocmin - 1;
			} else {
				break; /* never happens */
			}
		}
		trace_nilfs2_segment_usage_check(sufile, segnum, cnt);
		ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 1,
							   &su_bh);
		if (ret < 0)
			goto out_header;

		offset = nilfs_sufile_segment_usage_offset(sufile, segnum,
							   su_bh);
		su = kaddr = kmap_local_folio(su_bh->b_folio, offset);

		nsus = nilfs_sufile_segment_usages_in_block(
			sufile, segnum, maxsegnum);
		for (j = 0; j < nsus; j++, su = (void *)su + susz, segnum++) {
			if (!nilfs_segment_usage_clean(su))
				continue;
			/* found a clean segment */
			nilfs_segment_usage_set_dirty(su);
			kunmap_local(kaddr);

			header = kmap_local_folio(header_bh->b_folio, 0);
			le64_add_cpu(&header->sh_ncleansegs, -1);
			le64_add_cpu(&header->sh_ndirtysegs, 1);
			header->sh_last_alloc = cpu_to_le64(segnum);
			kunmap_local(header);

			sui->ncleansegs--;
			mark_buffer_dirty(header_bh);
			mark_buffer_dirty(su_bh);
			nilfs_mdt_mark_dirty(sufile);
			brelse(su_bh);
			*segnump = segnum;

			trace_nilfs2_segment_usage_allocated(sufile, segnum);

			goto out_header;
		}

		kunmap_local(kaddr);
		brelse(su_bh);
	}

	/* no segments left */
	ret = -ENOSPC;

 out_header:
	brelse(header_bh);

 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

void nilfs_sufile_do_cancel_free(struct inode *sufile, __u64 segnum,
				 struct buffer_head *header_bh,
				 struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	size_t offset;

	offset = nilfs_sufile_segment_usage_offset(sufile, segnum, su_bh);
	su = kmap_local_folio(su_bh->b_folio, offset);
	if (unlikely(!nilfs_segment_usage_clean(su))) {
		nilfs_warn(sufile->i_sb, "%s: segment %llu must be clean",
			   __func__, (unsigned long long)segnum);
		kunmap_local(su);
		return;
	}
	nilfs_segment_usage_set_dirty(su);
	kunmap_local(su);

	nilfs_sufile_mod_counter(header_bh, -1, 1);
	NILFS_SUI(sufile)->ncleansegs--;

	mark_buffer_dirty(su_bh);
	nilfs_mdt_mark_dirty(sufile);
}

void nilfs_sufile_do_scrap(struct inode *sufile, __u64 segnum,
			   struct buffer_head *header_bh,
			   struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	size_t offset;
	int clean, dirty;

	offset = nilfs_sufile_segment_usage_offset(sufile, segnum, su_bh);
	su = kmap_local_folio(su_bh->b_folio, offset);
	if (su->su_flags == cpu_to_le32(BIT(NILFS_SEGMENT_USAGE_DIRTY)) &&
	    su->su_nblocks == cpu_to_le32(0)) {
		kunmap_local(su);
		return;
	}
	clean = nilfs_segment_usage_clean(su);
	dirty = nilfs_segment_usage_dirty(su);

	/* make the segment garbage */
	su->su_lastmod = cpu_to_le64(0);
	su->su_nblocks = cpu_to_le32(0);
	su->su_flags = cpu_to_le32(BIT(NILFS_SEGMENT_USAGE_DIRTY));
	kunmap_local(su);

	nilfs_sufile_mod_counter(header_bh, clean ? (u64)-1 : 0, dirty ? 0 : 1);
	NILFS_SUI(sufile)->ncleansegs -= clean;

	mark_buffer_dirty(su_bh);
	nilfs_mdt_mark_dirty(sufile);
}

void nilfs_sufile_do_free(struct inode *sufile, __u64 segnum,
			  struct buffer_head *header_bh,
			  struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	size_t offset;
	int sudirty;

	offset = nilfs_sufile_segment_usage_offset(sufile, segnum, su_bh);
	su = kmap_local_folio(su_bh->b_folio, offset);
	if (nilfs_segment_usage_clean(su)) {
		nilfs_warn(sufile->i_sb, "%s: segment %llu is already clean",
			   __func__, (unsigned long long)segnum);
		kunmap_local(su);
		return;
	}
	if (unlikely(nilfs_segment_usage_error(su)))
		nilfs_warn(sufile->i_sb, "free segment %llu marked in error",
			   (unsigned long long)segnum);

	sudirty = nilfs_segment_usage_dirty(su);
	if (unlikely(!sudirty))
		nilfs_warn(sufile->i_sb, "free unallocated segment %llu",
			   (unsigned long long)segnum);

	nilfs_segment_usage_set_clean(su);
	kunmap_local(su);
	mark_buffer_dirty(su_bh);

	nilfs_sufile_mod_counter(header_bh, 1, sudirty ? (u64)-1 : 0);
	NILFS_SUI(sufile)->ncleansegs++;

	nilfs_mdt_mark_dirty(sufile);

	trace_nilfs2_segment_usage_freed(sufile, segnum);
}

/**
 * nilfs_sufile_mark_dirty - mark the buffer having a segment usage dirty
 * @sufile: inode of segment usage file
 * @segnum: segment number
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int nilfs_sufile_mark_dirty(struct inode *sufile, __u64 segnum)
{
	struct buffer_head *bh;
	size_t offset;
	struct nilfs_segment_usage *su;
	int ret;

	down_write(&NILFS_MDT(sufile)->mi_sem);
	ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0, &bh);
	if (unlikely(ret)) {
		if (ret == -ENOENT) {
			nilfs_error(sufile->i_sb,
				    "segment usage for segment %llu is unreadable due to a hole block",
				    (unsigned long long)segnum);
			ret = -EIO;
		}
		goto out_sem;
	}

	offset = nilfs_sufile_segment_usage_offset(sufile, segnum, bh);
	su = kmap_local_folio(bh->b_folio, offset);
	if (unlikely(nilfs_segment_usage_error(su))) {
		struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;

		kunmap_local(su);
		brelse(bh);
		if (nilfs_segment_is_active(nilfs, segnum)) {
			nilfs_error(sufile->i_sb,
				    "active segment %llu is erroneous",
				    (unsigned long long)segnum);
		} else {
			/*
			 * Segments marked erroneous are never allocated by
			 * nilfs_sufile_alloc(); only active segments, ie,
			 * the segments indexed by ns_segnum or ns_nextnum,
			 * can be erroneous here.
			 */
			WARN_ON_ONCE(1);
		}
		ret = -EIO;
	} else {
		nilfs_segment_usage_set_dirty(su);
		kunmap_local(su);
		mark_buffer_dirty(bh);
		nilfs_mdt_mark_dirty(sufile);
		brelse(bh);
	}
out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

/**
 * nilfs_sufile_set_segment_usage - set usage of a segment
 * @sufile: inode of segment usage file
 * @segnum: segment number
 * @nblocks: number of live blocks in the segment
 * @modtime: modification time (option)
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int nilfs_sufile_set_segment_usage(struct inode *sufile, __u64 segnum,
				   unsigned long nblocks, time64_t modtime)
{
	struct buffer_head *bh;
	struct nilfs_segment_usage *su;
	size_t offset;
	int ret;

	down_write(&NILFS_MDT(sufile)->mi_sem);
	ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0, &bh);
	if (ret < 0)
		goto out_sem;

	offset = nilfs_sufile_segment_usage_offset(sufile, segnum, bh);
	su = kmap_local_folio(bh->b_folio, offset);
	if (modtime) {
		/*
		 * Check segusage error and set su_lastmod only when updating
		 * this entry with a valid timestamp, not for cancellation.
		 */
		WARN_ON_ONCE(nilfs_segment_usage_error(su));
		su->su_lastmod = cpu_to_le64(modtime);
	}
	su->su_nblocks = cpu_to_le32(nblocks);
	kunmap_local(su);

	mark_buffer_dirty(bh);
	nilfs_mdt_mark_dirty(sufile);
	brelse(bh);

 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

/**
 * nilfs_sufile_get_stat - get segment usage statistics
 * @sufile: inode of segment usage file
 * @sustat: pointer to a structure of segment usage statistics
 *
 * Description: nilfs_sufile_get_stat() retrieves segment usage statistics
 * and stores them in the location pointed to by @sustat.
 *
 * Return: 0 on success, or one of the following negative error codes on
 * failure:
 * * %-EIO	- I/O error (including metadata corruption).
 * * %-ENOMEM	- Insufficient memory available.
 */
int nilfs_sufile_get_stat(struct inode *sufile, struct nilfs_sustat *sustat)
{
	struct buffer_head *header_bh;
	struct nilfs_sufile_header *header;
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	int ret;

	down_read(&NILFS_MDT(sufile)->mi_sem);

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;

	header = kmap_local_folio(header_bh->b_folio, 0);
	sustat->ss_nsegs = nilfs_sufile_get_nsegments(sufile);
	sustat->ss_ncleansegs = le64_to_cpu(header->sh_ncleansegs);
	sustat->ss_ndirtysegs = le64_to_cpu(header->sh_ndirtysegs);
	sustat->ss_ctime = nilfs->ns_ctime;
	sustat->ss_nongc_ctime = nilfs->ns_nongc_ctime;
	spin_lock(&nilfs->ns_last_segment_lock);
	sustat->ss_prot_seq = nilfs->ns_prot_seq;
	spin_unlock(&nilfs->ns_last_segment_lock);
	kunmap_local(header);
	brelse(header_bh);

 out_sem:
	up_read(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

void nilfs_sufile_do_set_error(struct inode *sufile, __u64 segnum,
			       struct buffer_head *header_bh,
			       struct buffer_head *su_bh)
{
	struct nilfs_segment_usage *su;
	size_t offset;
	int suclean;

	offset = nilfs_sufile_segment_usage_offset(sufile, segnum, su_bh);
	su = kmap_local_folio(su_bh->b_folio, offset);
	if (nilfs_segment_usage_error(su)) {
		kunmap_local(su);
		return;
	}
	suclean = nilfs_segment_usage_clean(su);
	nilfs_segment_usage_set_error(su);
	kunmap_local(su);

	if (suclean) {
		nilfs_sufile_mod_counter(header_bh, -1, 0);
		NILFS_SUI(sufile)->ncleansegs--;
	}
	mark_buffer_dirty(su_bh);
	nilfs_mdt_mark_dirty(sufile);
}

/**
 * nilfs_sufile_truncate_range - truncate range of segment array
 * @sufile: inode of segment usage file
 * @start: start segment number (inclusive)
 * @end: end segment number (inclusive)
 *
 * Return: 0 on success, or one of the following negative error codes on
 * failure:
 * * %-EBUSY	- Dirty or active segments are present in the range.
 * * %-EINVAL	- Invalid number of segments specified.
 * * %-EIO	- I/O error (including metadata corruption).
 * * %-ENOMEM	- Insufficient memory available.
 */
static int nilfs_sufile_truncate_range(struct inode *sufile,
				       __u64 start, __u64 end)
{
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	struct buffer_head *header_bh;
	struct buffer_head *su_bh;
	struct nilfs_segment_usage *su, *su2;
	size_t susz = NILFS_MDT(sufile)->mi_entry_size;
	unsigned long segusages_per_block;
	unsigned long nsegs, ncleaned;
	__u64 segnum;
	size_t offset;
	ssize_t n, nc;
	int ret;
	int j;

	nsegs = nilfs_sufile_get_nsegments(sufile);

	ret = -EINVAL;
	if (start > end || start >= nsegs)
		goto out;

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out;

	segusages_per_block = nilfs_sufile_segment_usages_per_block(sufile);
	ncleaned = 0;

	for (segnum = start; segnum <= end; segnum += n) {
		n = min_t(unsigned long,
			  segusages_per_block -
				  nilfs_sufile_get_offset(sufile, segnum),
			  end - segnum + 1);
		ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0,
							   &su_bh);
		if (ret < 0) {
			if (ret != -ENOENT)
				goto out_header;
			/* hole */
			continue;
		}
		offset = nilfs_sufile_segment_usage_offset(sufile, segnum,
							   su_bh);
		su = kmap_local_folio(su_bh->b_folio, offset);
		su2 = su;
		for (j = 0; j < n; j++, su = (void *)su + susz) {
			if ((le32_to_cpu(su->su_flags) &
			     ~BIT(NILFS_SEGMENT_USAGE_ERROR)) ||
			    nilfs_segment_is_active(nilfs, segnum + j)) {
				ret = -EBUSY;
				kunmap_local(su2);
				brelse(su_bh);
				goto out_header;
			}
		}
		nc = 0;
		for (su = su2, j = 0; j < n; j++, su = (void *)su + susz) {
			if (nilfs_segment_usage_error(su)) {
				nilfs_segment_usage_set_clean(su);
				nc++;
			}
		}
		kunmap_local(su2);
		if (nc > 0) {
			mark_buffer_dirty(su_bh);
			ncleaned += nc;
		}
		brelse(su_bh);

		if (n == segusages_per_block) {
			/* make hole */
			nilfs_sufile_delete_segment_usage_block(sufile, segnum);
		}
	}
	ret = 0;

out_header:
	if (ncleaned > 0) {
		NILFS_SUI(sufile)->ncleansegs += ncleaned;
		nilfs_sufile_mod_counter(header_bh, ncleaned, 0);
		nilfs_mdt_mark_dirty(sufile);
	}
	brelse(header_bh);
out:
	return ret;
}

/**
 * nilfs_sufile_resize - resize segment array
 * @sufile: inode of segment usage file
 * @newnsegs: new number of segments
 *
 * Return: 0 on success, or one of the following negative error codes on
 * failure:
 * * %-EBUSY	- Dirty or active segments exist in the region to be truncated.
 * * %-EIO	- I/O error (including metadata corruption).
 * * %-ENOMEM	- Insufficient memory available.
 * * %-ENOSPC	- Enough free space is not left for shrinking.
 */
int nilfs_sufile_resize(struct inode *sufile, __u64 newnsegs)
{
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	struct buffer_head *header_bh;
	struct nilfs_sufile_header *header;
	struct nilfs_sufile_info *sui = NILFS_SUI(sufile);
	unsigned long nsegs, nrsvsegs;
	int ret = 0;

	down_write(&NILFS_MDT(sufile)->mi_sem);

	nsegs = nilfs_sufile_get_nsegments(sufile);
	if (nsegs == newnsegs)
		goto out;

	ret = -ENOSPC;
	nrsvsegs = nilfs_nrsvsegs(nilfs, newnsegs);
	if (newnsegs < nsegs && nsegs - newnsegs + nrsvsegs > sui->ncleansegs)
		goto out;

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out;

	if (newnsegs > nsegs) {
		sui->ncleansegs += newnsegs - nsegs;
	} else /* newnsegs < nsegs */ {
		ret = nilfs_sufile_truncate_range(sufile, newnsegs, nsegs - 1);
		if (ret < 0)
			goto out_header;

		sui->ncleansegs -= nsegs - newnsegs;

		/*
		 * If the sufile is successfully truncated, immediately adjust
		 * the segment allocation space while locking the semaphore
		 * "mi_sem" so that nilfs_sufile_alloc() never allocates
		 * segments in the truncated space.
		 */
		sui->allocmax = newnsegs - 1;
		sui->allocmin = 0;
	}

	header = kmap_local_folio(header_bh->b_folio, 0);
	header->sh_ncleansegs = cpu_to_le64(sui->ncleansegs);
	kunmap_local(header);

	mark_buffer_dirty(header_bh);
	nilfs_mdt_mark_dirty(sufile);
	nilfs_set_nsegments(nilfs, newnsegs);

out_header:
	brelse(header_bh);
out:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

/**
 * nilfs_sufile_get_suinfo - get segment usage information
 * @sufile: inode of segment usage file
 * @segnum: segment number to start looking
 * @buf:    array of suinfo
 * @sisz:   byte size of suinfo
 * @nsi:    size of suinfo array
 *
 * Return: Count of segment usage info items stored in the output buffer on
 * success, or one of the following negative error codes on failure:
 * * %-EIO	- I/O error (including metadata corruption).
 * * %-ENOMEM	- Insufficient memory available.
 */
ssize_t nilfs_sufile_get_suinfo(struct inode *sufile, __u64 segnum, void *buf,
				unsigned int sisz, size_t nsi)
{
	struct buffer_head *su_bh;
	struct nilfs_segment_usage *su;
	struct nilfs_suinfo *si = buf;
	size_t susz = NILFS_MDT(sufile)->mi_entry_size;
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	size_t offset;
	void *kaddr;
	unsigned long nsegs, segusages_per_block;
	ssize_t n;
	int ret, i, j;

	down_read(&NILFS_MDT(sufile)->mi_sem);

	segusages_per_block = nilfs_sufile_segment_usages_per_block(sufile);
	nsegs = min_t(unsigned long,
		      nilfs_sufile_get_nsegments(sufile) - segnum,
		      nsi);
	for (i = 0; i < nsegs; i += n, segnum += n) {
		n = min_t(unsigned long,
			  segusages_per_block -
				  nilfs_sufile_get_offset(sufile, segnum),
			  nsegs - i);
		ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0,
							   &su_bh);
		if (ret < 0) {
			if (ret != -ENOENT)
				goto out;
			/* hole */
			memset(si, 0, sisz * n);
			si = (void *)si + sisz * n;
			continue;
		}

		offset = nilfs_sufile_segment_usage_offset(sufile, segnum,
							   su_bh);
		su = kaddr = kmap_local_folio(su_bh->b_folio, offset);
		for (j = 0; j < n;
		     j++, su = (void *)su + susz, si = (void *)si + sisz) {
			si->sui_lastmod = le64_to_cpu(su->su_lastmod);
			si->sui_nblocks = le32_to_cpu(su->su_nblocks);
			si->sui_flags = le32_to_cpu(su->su_flags) &
				~BIT(NILFS_SEGMENT_USAGE_ACTIVE);
			if (nilfs_segment_is_active(nilfs, segnum + j))
				si->sui_flags |=
					BIT(NILFS_SEGMENT_USAGE_ACTIVE);
		}
		kunmap_local(kaddr);
		brelse(su_bh);
	}
	ret = nsegs;

 out:
	up_read(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

/**
 * nilfs_sufile_set_suinfo - sets segment usage info
 * @sufile: inode of segment usage file
 * @buf: array of suinfo_update
 * @supsz: byte size of suinfo_update
 * @nsup: size of suinfo_update array
 *
 * Description: Takes an array of nilfs_suinfo_update structs and updates
 * segment usage accordingly. Only the fields indicated by the sup_flags
 * are updated.
 *
 * Return: 0 on success, or one of the following negative error codes on
 * failure:
 * * %-EINVAL	- Invalid values in input (segment number, flags or nblocks).
 * * %-EIO	- I/O error (including metadata corruption).
 * * %-ENOMEM	- Insufficient memory available.
 */
ssize_t nilfs_sufile_set_suinfo(struct inode *sufile, void *buf,
				unsigned int supsz, size_t nsup)
{
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	struct buffer_head *header_bh, *bh;
	struct nilfs_suinfo_update *sup, *supend = buf + supsz * nsup;
	struct nilfs_segment_usage *su;
	size_t offset;
	unsigned long blkoff, prev_blkoff;
	int cleansi, cleansu, dirtysi, dirtysu;
	long ncleaned = 0, ndirtied = 0;
	int ret = 0;

	if (unlikely(nsup == 0))
		return ret;

	for (sup = buf; sup < supend; sup = (void *)sup + supsz) {
		if (sup->sup_segnum >= nilfs->ns_nsegments
			|| (sup->sup_flags &
				(~0UL << __NR_NILFS_SUINFO_UPDATE_FIELDS))
			|| (nilfs_suinfo_update_nblocks(sup) &&
				sup->sup_sui.sui_nblocks >
				nilfs->ns_blocks_per_segment))
			return -EINVAL;
	}

	down_write(&NILFS_MDT(sufile)->mi_sem);

	ret = nilfs_sufile_get_header_block(sufile, &header_bh);
	if (ret < 0)
		goto out_sem;

	sup = buf;
	blkoff = nilfs_sufile_get_blkoff(sufile, sup->sup_segnum);
	ret = nilfs_mdt_get_block(sufile, blkoff, 1, NULL, &bh);
	if (ret < 0)
		goto out_header;

	for (;;) {
		offset = nilfs_sufile_segment_usage_offset(
			sufile, sup->sup_segnum, bh);
		su = kmap_local_folio(bh->b_folio, offset);

		if (nilfs_suinfo_update_lastmod(sup))
			su->su_lastmod = cpu_to_le64(sup->sup_sui.sui_lastmod);

		if (nilfs_suinfo_update_nblocks(sup))
			su->su_nblocks = cpu_to_le32(sup->sup_sui.sui_nblocks);

		if (nilfs_suinfo_update_flags(sup)) {
			/*
			 * Active flag is a virtual flag projected by running
			 * nilfs kernel code - drop it not to write it to
			 * disk.
			 */
			sup->sup_sui.sui_flags &=
					~BIT(NILFS_SEGMENT_USAGE_ACTIVE);

			cleansi = nilfs_suinfo_clean(&sup->sup_sui);
			cleansu = nilfs_segment_usage_clean(su);
			dirtysi = nilfs_suinfo_dirty(&sup->sup_sui);
			dirtysu = nilfs_segment_usage_dirty(su);

			if (cleansi && !cleansu)
				++ncleaned;
			else if (!cleansi && cleansu)
				--ncleaned;

			if (dirtysi && !dirtysu)
				++ndirtied;
			else if (!dirtysi && dirtysu)
				--ndirtied;

			su->su_flags = cpu_to_le32(sup->sup_sui.sui_flags);
		}

		kunmap_local(su);

		sup = (void *)sup + supsz;
		if (sup >= supend)
			break;

		prev_blkoff = blkoff;
		blkoff = nilfs_sufile_get_blkoff(sufile, sup->sup_segnum);
		if (blkoff == prev_blkoff)
			continue;

		/* get different block */
		mark_buffer_dirty(bh);
		put_bh(bh);
		ret = nilfs_mdt_get_block(sufile, blkoff, 1, NULL, &bh);
		if (unlikely(ret < 0))
			goto out_mark;
	}
	mark_buffer_dirty(bh);
	put_bh(bh);

 out_mark:
	if (ncleaned || ndirtied) {
		nilfs_sufile_mod_counter(header_bh, (u64)ncleaned,
				(u64)ndirtied);
		NILFS_SUI(sufile)->ncleansegs += ncleaned;
	}
	nilfs_mdt_mark_dirty(sufile);
 out_header:
	put_bh(header_bh);
 out_sem:
	up_write(&NILFS_MDT(sufile)->mi_sem);
	return ret;
}

/**
 * nilfs_sufile_trim_fs() - trim ioctl handle function
 * @sufile: inode of segment usage file
 * @range: fstrim_range structure
 *
 * start:	First Byte to trim
 * len:		number of Bytes to trim from start
 * minlen:	minimum extent length in Bytes
 *
 * Decription: nilfs_sufile_trim_fs goes through all segments containing bytes
 * from start to start+len. start is rounded up to the next block boundary
 * and start+len is rounded down. For each clean segment blkdev_issue_discard
 * function is invoked.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int nilfs_sufile_trim_fs(struct inode *sufile, struct fstrim_range *range)
{
	struct the_nilfs *nilfs = sufile->i_sb->s_fs_info;
	struct buffer_head *su_bh;
	struct nilfs_segment_usage *su;
	size_t offset;
	void *kaddr;
	size_t n, i, susz = NILFS_MDT(sufile)->mi_entry_size;
	sector_t seg_start, seg_end, start_block, end_block;
	sector_t start = 0, nblocks = 0;
	u64 segnum, segnum_end, minlen, len, max_blocks, ndiscarded = 0;
	int ret = 0;
	unsigned int sects_per_block;

	sects_per_block = (1 << nilfs->ns_blocksize_bits) /
			bdev_logical_block_size(nilfs->ns_bdev);
	len = range->len >> nilfs->ns_blocksize_bits;
	minlen = range->minlen >> nilfs->ns_blocksize_bits;
	max_blocks = ((u64)nilfs->ns_nsegments * nilfs->ns_blocks_per_segment);

	if (!len || range->start >= max_blocks << nilfs->ns_blocksize_bits)
		return -EINVAL;

	start_block = (range->start + nilfs->ns_blocksize - 1) >>
			nilfs->ns_blocksize_bits;

	/*
	 * range->len can be very large (actually, it is set to
	 * ULLONG_MAX by default) - truncate upper end of the range
	 * carefully so as not to overflow.
	 */
	if (max_blocks - start_block < len)
		end_block = max_blocks - 1;
	else
		end_block = start_block + len - 1;

	segnum = nilfs_get_segnum_of_block(nilfs, start_block);
	segnum_end = nilfs_get_segnum_of_block(nilfs, end_block);

	down_read(&NILFS_MDT(sufile)->mi_sem);

	while (segnum <= segnum_end) {
		n = nilfs_sufile_segment_usages_in_block(sufile, segnum,
				segnum_end);

		ret = nilfs_sufile_get_segment_usage_block(sufile, segnum, 0,
							   &su_bh);
		if (ret < 0) {
			if (ret != -ENOENT)
				goto out_sem;
			/* hole */
			segnum += n;
			continue;
		}

		offset = nilfs_sufile_segment_usage_offset(sufile, segnum,
							   su_bh);
		su = kaddr = kmap_local_folio(su_bh->b_folio, offset);
		for (i = 0; i < n; ++i, ++segnum, su = (void *)su + susz) {
			if (!nilfs_segment_usage_clean(su))
				continue;

			nilfs_get_segment_range(nilfs, segnum, &seg_start,
						&seg_end);

			if (!nblocks) {
				/* start new extent */
				start = seg_start;
				nblocks = seg_end - seg_start + 1;
				continue;
			}

			if (start + nblocks == seg_start) {
				/* add to previous extent */
				nblocks += seg_end - seg_start + 1;
				continue;
			}

			/* discard previous extent */
			if (start < start_block) {
				nblocks -= start_block - start;
				start = start_block;
			}

			if (nblocks >= minlen) {
				kunmap_local(kaddr);

				ret = blkdev_issue_discard(nilfs->ns_bdev,
						start * sects_per_block,
						nblocks * sects_per_block,
						GFP_NOFS);
				if (ret < 0) {
					put_bh(su_bh);
					goto out_sem;
				}

				ndiscarded += nblocks;
				offset = nilfs_sufile_segment_usage_offset(
					sufile, segnum, su_bh);
				su = kaddr = kmap_local_folio(su_bh->b_folio,
							      offset);
			}

			/* start new extent */
			start = seg_start;
			nblocks = seg_end - seg_start + 1;
		}
		kunmap_local(kaddr);
		put_bh(su_bh);
	}


	if (nblocks) {
		/* discard last extent */
		if (start < start_block) {
			nblocks -= start_block - start;
			start = start_block;
		}
		if (start + nblocks > end_block + 1)
			nblocks = end_block - start + 1;

		if (nblocks >= minlen) {
			ret = blkdev_issue_discard(nilfs->ns_bdev,
					start * sects_per_block,
					nblocks * sects_per_block,
					GFP_NOFS);
			if (!ret)
				ndiscarded += nblocks;
		}
	}

out_sem:
	up_read(&NILFS_MDT(sufile)->mi_sem);

	range->len = ndiscarded << nilfs->ns_blocksize_bits;
	return ret;
}

/**
 * nilfs_sufile_read - read or get sufile inode
 * @sb: super block instance
 * @susize: size of a segment usage entry
 * @raw_inode: on-disk sufile inode
 * @inodep: buffer to store the inode
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int nilfs_sufile_read(struct super_block *sb, size_t susize,
		      struct nilfs_inode *raw_inode, struct inode **inodep)
{
	struct inode *sufile;
	struct nilfs_sufile_info *sui;
	struct buffer_head *header_bh;
	struct nilfs_sufile_header *header;
	int err;

	if (susize > sb->s_blocksize) {
		nilfs_err(sb, "too large segment usage size: %zu bytes",
			  susize);
		return -EINVAL;
	} else if (susize < NILFS_MIN_SEGMENT_USAGE_SIZE) {
		nilfs_err(sb, "too small segment usage size: %zu bytes",
			  susize);
		return -EINVAL;
	}

	sufile = nilfs_iget_locked(sb, NULL, NILFS_SUFILE_INO);
	if (unlikely(!sufile))
		return -ENOMEM;
	if (!(sufile->i_state & I_NEW))
		goto out;

	err = nilfs_mdt_init(sufile, NILFS_MDT_GFP, sizeof(*sui));
	if (err)
		goto failed;

	nilfs_mdt_set_entry_size(sufile, susize,
				 sizeof(struct nilfs_sufile_header));

	err = nilfs_read_inode_common(sufile, raw_inode);
	if (err)
		goto failed;

	err = nilfs_mdt_get_block(sufile, 0, 0, NULL, &header_bh);
	if (unlikely(err)) {
		if (err == -ENOENT) {
			nilfs_err(sb,
				  "missing header block in segment usage metadata");
			err = -EINVAL;
		}
		goto failed;
	}

	sui = NILFS_SUI(sufile);
	header = kmap_local_folio(header_bh->b_folio, 0);
	sui->ncleansegs = le64_to_cpu(header->sh_ncleansegs);
	kunmap_local(header);
	brelse(header_bh);

	sui->allocmax = nilfs_sufile_get_nsegments(sufile) - 1;
	sui->allocmin = 0;

	unlock_new_inode(sufile);
 out:
	*inodep = sufile;
	return 0;
 failed:
	iget_failed(sufile);
	return err;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for FPGA Accelerated Function Unit (AFU) DMA Region Management
 *
 * Copyright (C) 2017-2018 Intel Corporation, Inc.
 *
 * Authors:
 *   Wu Hao <hao.wu@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 */

#include <linux/fpga-dfl.h>
#include <linux/pfn.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/version.h>

#include "dfl-afu.h"
#include "dfl-dma-cxl-common.h"

void afu_dma_region_init(struct dfl_feature_dev_data *fdata)
{
	struct dfl_afu *afu = dfl_fpga_fdata_get_private(fdata);

	afu->dma_regions = RB_ROOT;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0) && RHEL_RELEASE_CODE < 0x803
static int afu_dma_pin_pages(struct dfl_feature_dev_data *fdata,
			     struct dfl_afu_dma_region *region)
{
	long npages = region->length >> PAGE_SHIFT;
	struct device *dev = &fdata->dev->dev;
	long ret, pinned;

	ret = afu_dma_adjust_locked_vm(dev, npages, true);
	if (ret)
		return ret;

	region->pages = kcalloc(npages, sizeof(struct page *), GFP_KERNEL);
	if (!region->pages) {
		afu_dma_adjust_locked_vm(dev, npages, false);
		return -ENOMEM;
	}

	pinned = get_user_pages_fast(region->user_addr, npages,
				     (region->direction != DMA_TO_DEVICE),
				     region->pages);
	if (pinned < 0) {
		ret = pinned;
		goto err_put_pages;
	} else if (pinned != npages) {
		ret = -EFAULT;
		goto err;
	}

	dev_dbg(dev, "%ld pages pinned\n", pinned);

	return 0;

err_put_pages:
	put_user_pages(region->pages, pinned);
err:
	kfree(region->pages);
	afu_dma_adjust_locked_vm(dev, npages, false);
	return ret;
}

static void afu_dma_unpin_pages(struct dfl_feature_dev_data *fdata,
				struct dfl_afu_dma_region *region)
{
	long npages = region->length >> PAGE_SHIFT;
	struct device *dev = &fdata->dev->dev;

	put_user_pages(region->pages, npages);
	kfree(region->pages);
	afu_dma_adjust_locked_vm(dev, npages, false);

	dev_dbg(dev, "%ld pages unpinned\n", npages);
}

#else /* < KERNEL_VERSION(5, 3, 0) */

/**
 * afu_dma_pin_pages - pin pages of given dma memory region
 * @fdata: feature dev data
 * @region: dma memory region to be pinned
 *
 * Pin all the pages of given dfl_afu_dma_region.
 * Return 0 for success or negative error code.
 */
static int afu_dma_pin_pages(struct dfl_feature_dev_data *fdata,
			     struct dfl_afu_dma_region *region)
{
	int npages = PFN_DOWN(region->length);
	struct device *dev = &fdata->dev->dev;
	unsigned int flags = FOLL_LONGTERM;
	int ret, pinned;

	ret = account_locked_vm(current->mm, npages, true);
	if (ret)
		return ret;

	region->pages = kcalloc(npages, sizeof(struct page *), GFP_KERNEL);
	if (!region->pages) {
		ret = -ENOMEM;
		goto unlock_vm;
	}

	if (region->direction != DMA_TO_DEVICE)
		flags |= FOLL_WRITE;

	pinned = pin_user_pages_fast(region->user_addr, npages, flags,
				     region->pages);
	if (pinned < 0) {
		ret = pinned;
		goto free_pages;
	} else if (pinned != npages) {
		ret = -EFAULT;
		goto unpin_pages;
	}

	dev_dbg(dev, "%d pages pinned\n", pinned);

	return 0;

unpin_pages:
	unpin_user_pages(region->pages, pinned);
free_pages:
	kfree(region->pages);
unlock_vm:
	account_locked_vm(current->mm, npages, false);
	return ret;
}

/**
 * afu_dma_unpin_pages - unpin pages of given dma memory region
 * @fdata: feature dev data
 * @region: dma memory region to be unpinned
 *
 * Unpin all the pages of given dfl_afu_dma_region.
 * Return 0 for success or negative error code.
 */
static void afu_dma_unpin_pages(struct dfl_feature_dev_data *fdata,
				struct dfl_afu_dma_region *region)
{
	long npages = PFN_DOWN(region->length);
	struct device *dev = &fdata->dev->dev;

	unpin_user_pages(region->pages, npages);
	kfree(region->pages);
	account_locked_vm(current->mm, npages, false);

	dev_dbg(dev, "%ld pages unpinned\n", npages);
}

#endif /* < KERNEL_VERSION(5, 3, 0) */

/**
 * afu_dma_check_continuous_pages - check if pages are continuous
 * @region: dma memory region
 *
 * Return true if pages of given dma memory region have continuous physical
 * address, otherwise return false.
 */
static bool afu_dma_check_continuous_pages(struct dfl_afu_dma_region *region)
{
	int npages = PFN_DOWN(region->length);
	int i;

	for (i = 0; i < npages - 1; i++)
		if (page_to_pfn(region->pages[i]) + 1 !=
				page_to_pfn(region->pages[i + 1]))
			return false;

	return true;
}

/**
 * dma_region_check_iova - check if memory area is fully contained in the region
 * @region: dma memory region
 * @iova: address of the dma memory area
 * @size: size of the dma memory area
 *
 * Compare the dma memory area defined by @iova and @size with given dma region.
 * Return true if memory area is fully contained in the region, otherwise false.
 */
static bool dma_region_check_iova(struct dfl_afu_dma_region *region,
				  u64 iova, u64 size)
{
	if (!size && region->iova != iova)
		return false;

	return (region->iova <= iova) &&
		(region->length + region->iova >= iova + size);
}

/**
 * afu_dma_region_add - add given dma region to rbtree
 * @fdata: feature dev data
 * @region: dma region to be added
 *
 * Return 0 for success, -EEXIST if dma region has already been added.
 *
 * Needs to be called with fdata->lock held.
 */
static int afu_dma_region_add(struct dfl_feature_dev_data *fdata,
			      struct dfl_afu_dma_region *region)
{
	struct dfl_afu *afu = dfl_fpga_fdata_get_private(fdata);
	struct rb_node **new, *parent = NULL;

	dev_dbg(&fdata->dev->dev, "add region (iova = %llx)\n", region->iova);

	new = &afu->dma_regions.rb_node;

	while (*new) {
		struct dfl_afu_dma_region *this;

		this = container_of(*new, struct dfl_afu_dma_region, node);

		parent = *new;

		if (dma_region_check_iova(this, region->iova, region->length))
			return -EEXIST;

		if (region->iova < this->iova)
			new = &((*new)->rb_left);
		else if (region->iova > this->iova)
			new = &((*new)->rb_right);
		else
			return -EEXIST;
	}

	rb_link_node(&region->node, parent, new);
	rb_insert_color(&region->node, &afu->dma_regions);

	return 0;
}

/**
 * afu_dma_region_remove - remove given dma region from rbtree
 * @fdata: feature dev data
 * @region: dma region to be removed
 *
 * Needs to be called with fdata->lock held.
 */
static void afu_dma_region_remove(struct dfl_feature_dev_data *fdata,
				  struct dfl_afu_dma_region *region)
{
	struct dfl_afu *afu;

	dev_dbg(&fdata->dev->dev, "del region (iova = %llx)\n", region->iova);

	afu = dfl_fpga_fdata_get_private(fdata);
	rb_erase(&region->node, &afu->dma_regions);
}

/**
 * afu_dma_region_destroy - destroy all regions in rbtree
 * @fdata: feature dev data
 *
 * Needs to be called with fdata->lock held.
 */
void afu_dma_region_destroy(struct dfl_feature_dev_data *fdata)
{
	struct dfl_afu *afu = dfl_fpga_fdata_get_private(fdata);
	struct rb_node *node = rb_first(&afu->dma_regions);
	struct dfl_afu_dma_region *region;

	while (node) {
		region = container_of(node, struct dfl_afu_dma_region, node);

		dev_dbg(&fdata->dev->dev, "del region (iova = %llx)\n",
			region->iova);

		rb_erase(node, &afu->dma_regions);

		if (region->iova)
			dma_unmap_page(dfl_fpga_fdata_to_parent(fdata),
				       region->iova, region->length,
				       DMA_BIDIRECTIONAL);

		if (region->pages)
			afu_dma_unpin_pages(fdata, region);

		node = rb_next(node);
		kfree(region);
	}
}

/**
 * afu_dma_region_find - find the dma region from rbtree based on iova and size
 * @fdata: feature dev data
 * @iova: address of the dma memory area
 * @size: size of the dma memory area
 *
 * It finds the dma region from the rbtree based on @iova and @size:
 * - if @size == 0, it finds the dma region which starts from @iova
 * - otherwise, it finds the dma region which fully contains
 *   [@iova, @iova+size)
 * If nothing is matched returns NULL.
 *
 * Needs to be called with fdata->lock held.
 */
struct dfl_afu_dma_region *
afu_dma_region_find(struct dfl_feature_dev_data *fdata, u64 iova, u64 size)
{
	struct dfl_afu *afu = dfl_fpga_fdata_get_private(fdata);
	struct rb_node *node = afu->dma_regions.rb_node;
	struct device *dev = &fdata->dev->dev;

	while (node) {
		struct dfl_afu_dma_region *region;

		region = container_of(node, struct dfl_afu_dma_region, node);

		if (dma_region_check_iova(region, iova, size)) {
			dev_dbg(dev, "find region (iova = %llx)\n",
				region->iova);
			return region;
		}

		if (iova < region->iova)
			node = node->rb_left;
		else if (iova > region->iova)
			node = node->rb_right;
		else
			/* the iova region is not fully covered. */
			break;
	}

	dev_dbg(dev, "region with iova %llx and size %llx is not found\n", iova,
		size);

	return NULL;
}

/**
 * afu_dma_region_find_iova - find the dma region from rbtree by iova
 * @fdata: feature dev data
 * @iova: address of the dma region
 *
 * Needs to be called with fdata->lock held.
 */
static struct dfl_afu_dma_region *
afu_dma_region_find_iova(struct dfl_feature_dev_data *fdata, u64 iova)
{
	return afu_dma_region_find(fdata, iova, 0);
}

static enum dma_data_direction dma_flag_to_dir(u32 flags)
{
	u32 mask = DFL_DMA_MAP_FLAG_READ | DFL_DMA_MAP_FLAG_WRITE;

	/*
	 * DMA is bidirectional if both read and write are specified or if
	 * neither read nor write is specified. The latter supports legacy
	 * code, which did not pass any flags.
	 */
	switch (flags & mask) {
	case DFL_DMA_MAP_FLAG_READ:
		return DMA_TO_DEVICE;
	case DFL_DMA_MAP_FLAG_WRITE:
		return DMA_FROM_DEVICE;
	}

	return DMA_BIDIRECTIONAL;
}

/**
 * afu_dma_map_region - map memory region for dma
 * @fdata: feature dev data
 * @user_addr: address of the memory region
 * @length: size of the memory region
 * @flags: dma mapping flags
 * @iova: pointer of iova address
 *
 * Map memory region defined by @user_addr and @length, and return dma address
 * of the memory region via @iova.
 * Return 0 for success, otherwise error code.
 */
int afu_dma_map_region(struct dfl_feature_dev_data *fdata,
		       u64 user_addr, u64 length, u32 flags, u64 *iova)
{
	struct device *dev = &fdata->dev->dev;
	struct dfl_afu_dma_region *region;
	int ret;

	/*
	 * Check Inputs, only accept page-aligned user memory region with
	 * valid length.
	 */
	if (!PAGE_ALIGNED(user_addr) || !PAGE_ALIGNED(length) || !length)
		return -EINVAL;

	/* Check overflow */
	if (user_addr + length < user_addr)
		return -EINVAL;

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	region->user_addr = user_addr;
	region->length = length;
	region->direction = dma_flag_to_dir(flags);

	/* Pin the user memory region */
	ret = afu_dma_pin_pages(fdata, region);
	if (ret) {
		dev_err(dev, "failed to pin memory region\n");
		goto free_region;
	}

	/* Only accept continuous pages, return error else */
	if (!afu_dma_check_continuous_pages(region)) {
		dev_err(dev, "pages are not continuous\n");
		ret = -EINVAL;
		goto unpin_pages;
	}

	/* As pages are continuous then start to do DMA mapping */
	region->iova = dma_map_page(dfl_fpga_fdata_to_parent(fdata),
				    region->pages[0], 0,
				    region->length,
				    region->direction);
	if (dma_mapping_error(dfl_fpga_fdata_to_parent(fdata), region->iova)) {
		dev_err(dev, "failed to map for dma\n");
		ret = -EFAULT;
		goto unpin_pages;
	}

	*iova = region->iova;

	mutex_lock(&fdata->lock);
	ret = afu_dma_region_add(fdata, region);
	mutex_unlock(&fdata->lock);
	if (ret) {
		dev_err(dev, "failed to add dma region\n");
		goto unmap_dma;
	}

	return 0;

unmap_dma:
	dma_unmap_page(dfl_fpga_fdata_to_parent(fdata),
		       region->iova, region->length, region->direction);
unpin_pages:
	afu_dma_unpin_pages(fdata, region);
free_region:
	kfree(region);
	return ret;
}

/**
 * afu_dma_unmap_region - unmap dma memory region
 * @fdata: feature dev data
 * @iova: dma address of the region
 *
 * Unmap dma memory region based on @iova.
 * Return 0 for success, otherwise error code.
 */
int afu_dma_unmap_region(struct dfl_feature_dev_data *fdata, u64 iova)
{
	struct dfl_afu_dma_region *region;

	mutex_lock(&fdata->lock);
	region = afu_dma_region_find_iova(fdata, iova);
	if (!region) {
		mutex_unlock(&fdata->lock);
		return -EINVAL;
	}

	if (region->in_use) {
		mutex_unlock(&fdata->lock);
		return -EBUSY;
	}

	afu_dma_region_remove(fdata, region);
	mutex_unlock(&fdata->lock);

	dma_unmap_page(dfl_fpga_fdata_to_parent(fdata),
		       region->iova, region->length, region->direction);
	afu_dma_unpin_pages(fdata, region);
	kfree(region);

	return 0;
}

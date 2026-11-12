// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/fb.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include <drm/drm_drv.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_prime.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_kms.h"

static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);

/*
 * fbdev funcs, to implement legacy fbdev interface on top of drm driver
 */

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(msm_fbdev,
				   drm_fb_helper_damage_range,
				   drm_fb_helper_damage_area)

static int msm_fbdev_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct drm_fb_helper *helper = (struct drm_fb_helper *)info->par;
	struct drm_gem_object *bo = msm_framebuffer_bo(helper->fb, 0);

	return drm_gem_prime_mmap(bo, vma);
}

static void msm_fbdev_fb_destroy(struct fb_info *info)
{
	struct drm_fb_helper *helper = (struct drm_fb_helper *)info->par;
	struct drm_framebuffer *fb = helper->fb;
	struct drm_gem_object *bo = msm_framebuffer_bo(fb, 0);

	DBG();

	drm_fb_helper_fini(helper);

	/* this will free the backing object */
	msm_gem_put_vaddr(bo);
	drm_framebuffer_remove(fb);

	drm_client_release(&helper->client);
}

static const struct fb_ops msm_fb_ops = {
	.owner = THIS_MODULE,
	__FB_DEFAULT_DEFERRED_OPS_RDWR(msm_fbdev),
	DRM_FB_HELPER_DEFAULT_OPS,
	__FB_DEFAULT_DEFERRED_OPS_DRAW(msm_fbdev),
	.fb_mmap = msm_fbdev_mmap,
	.fb_destroy = msm_fbdev_fb_destroy,
};

static int msm_fbdev_fb_dirty(struct drm_fb_helper *helper,
			      struct drm_clip_rect *clip)
{
	struct drm_device *dev = helper->dev;
	int ret;

	/* Call damage handlers only if necessary */
	if (!(clip->x1 < clip->x2 && clip->y1 < clip->y2))
		return 0;

	if (helper->fb->funcs->dirty) {
		ret = helper->fb->funcs->dirty(helper->fb, NULL, 0, 0, clip, 1);
		if (drm_WARN_ONCE(dev, ret, "Dirty helper failed: ret=%d\n", ret))
			return ret;
	}

	return 0;
}

static const struct drm_fb_helper_funcs msm_fbdev_helper_funcs = {
	.fb_dirty = msm_fbdev_fb_dirty,
};

/*
 * Start the console framebuffer with the picture the bootloader left on the panel (the /reserved-memory
 * splash_region it scans out, XRGB8888 rows of width * 4 bytes). The first modeset resets the panel, and a
 * zeroed buffer then put a black screen up between the boot logo and the boot splash; on an LCD at the
 * bootloader's brightness that black glows, and it read as a bright flash.
 */
static void msm_fbdev_copy_splash(struct drm_device *dev, struct drm_framebuffer *fb, void *vaddr)
{
	struct device_node *rmem, *np;
	struct resource res;
	unsigned int row, rows, len;
	void *splash;

	if (fb->format->format != DRM_FORMAT_XRGB8888 && fb->format->format != DRM_FORMAT_ARGB8888)
		return;

	rmem = of_find_node_by_path("/reserved-memory");
	if (!rmem)
		return;
	for_each_child_of_node(rmem, np)
		if (of_node_name_prefix(np, "splash_region"))
			break;
	of_node_put(rmem);
	if (!np)
		return;
	if (of_address_to_resource(np, 0, &res)) {
		of_node_put(np);
		return;
	}
	of_node_put(np);

	len = fb->width * 4;
	rows = min_t(u64, fb->height, resource_size(&res) / len);
	splash = memremap(res.start, (size_t)rows * len, MEMREMAP_WC);
	if (!splash)
		return;
	for (row = 0; row < rows; row++)
		memcpy(vaddr + row * fb->pitches[0], splash + row * len, len);
	memunmap(splash);
	DRM_DEV_INFO(dev->dev, "fbdev starts with the bootloader's splash (%u rows)\n", rows);
}

int msm_fbdev_driver_fbdev_probe(struct drm_fb_helper *helper,
				 struct drm_fb_helper_surface_size *sizes)
{
	struct drm_device *dev = helper->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct fb_info *fbi = helper->info;
	struct drm_framebuffer *fb = NULL;
	struct drm_gem_object *bo;
	uint64_t paddr;
	uint32_t format;
	int ret, pitch;

	format = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);

	DBG("create fbdev: %dx%d@%d (%dx%d)", sizes->surface_width,
			sizes->surface_height, sizes->surface_bpp,
			sizes->fb_width, sizes->fb_height);

	pitch = align_pitch(sizes->surface_width, sizes->surface_bpp);
	fb = msm_alloc_stolen_fb(dev, sizes->surface_width,
			sizes->surface_height, pitch, format);

	if (IS_ERR(fb)) {
		DRM_DEV_ERROR(dev->dev, "failed to allocate fb\n");
		return PTR_ERR(fb);
	}

	bo = msm_framebuffer_bo(fb, 0);

	/*
	 * NOTE: if we can be guaranteed to be able to map buffer
	 * in panic (ie. lock-safe, etc) we could avoid pinning the
	 * buffer now:
	 */
	ret = msm_gem_get_and_pin_iova(bo, priv->kms->vm, &paddr);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "failed to get buffer obj iova: %d\n", ret);
		goto fail;
	}

	DBG("fbi=%p, dev=%p", fbi, dev);

	helper->funcs = &msm_fbdev_helper_funcs;
	helper->fb = fb;

	fbi->fbops = &msm_fb_ops;

	drm_fb_helper_fill_info(fbi, helper, sizes);

	fbi->screen_buffer = msm_gem_get_vaddr(bo);
	if (IS_ERR(fbi->screen_buffer)) {
		ret = PTR_ERR(fbi->screen_buffer);
		goto fail;
	}
	msm_fbdev_copy_splash(dev, fb, fbi->screen_buffer);
	fbi->screen_size = bo->size;
	fbi->fix.smem_start = paddr;
	fbi->fix.smem_len = bo->size;

	DBG("par=%p, %dx%d", fbi->par, fbi->var.xres, fbi->var.yres);
	DBG("allocated %dx%d fb", fb->width, fb->height);

	return 0;

fail:
	drm_framebuffer_remove(fb);
	return ret;
}

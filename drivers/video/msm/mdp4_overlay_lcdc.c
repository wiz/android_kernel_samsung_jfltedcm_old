/* Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <mach/hardware.h>
#include <linux/io.h>

#include <asm/system.h>
#include <asm/mach-types.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>

#include <linux/fb.h>

#include "mdp.h"
#include "msm_fb.h"
#include "mdp4.h"

#ifdef CONFIG_FB_MSM_MDP40
#define LCDC_BASE	0xC0000
#else
#define LCDC_BASE	0xE0000
#endif

int first_pixel_start_x;
int first_pixel_start_y;

static int lcdc_enabled;

#define MAX_CONTROLLER	1

static struct vsycn_ctrl {
	struct device *dev;
	int inited;
	int update_ndx;
	int ov_koff;
	int ov_done;
	atomic_t suspend;
	atomic_t vsync_resume;
	int wait_vsync_cnt;
	int blt_change;
	int blt_free;
	int sysfs_created;
	struct mutex update_lock;
	struct completion ov_comp;
	struct completion dmap_comp;
	struct completion vsync_comp;
	spinlock_t spin_lock;
	struct msm_fb_data_type *mfd;
	struct mdp4_overlay_pipe *base_pipe;
	struct vsync_update vlist[2];
	int vsync_irq_enabled;
	ktime_t vsync_time;
} vsync_ctrl_db[MAX_CONTROLLER];


/*******************************************************
to do:
1) move vsync_irq_enable/vsync_irq_disable to mdp.c to be shared
*******************************************************/
static void vsync_irq_enable(int intr, int term)
{
	unsigned long flag;

	spin_lock_irqsave(&mdp_spin_lock, flag);
	outp32(MDP_INTR_CLEAR, intr);
	mdp_intr_mask |= intr;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	mdp_enable_irq(term);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);
	pr_debug("%s: IRQ-en done, term=%x\n", __func__, term);
}

static void vsync_irq_disable(int intr, int term)
{
	unsigned long flag;

	spin_lock_irqsave(&mdp_spin_lock, flag);
	outp32(MDP_INTR_CLEAR, intr);
	mdp_intr_mask &= ~intr;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	mdp_disable_irq_nosync(term);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);
	pr_debug("%s: IRQ-dis done, term=%x\n", __func__, term);
}

static void mdp4_overlay_lcdc_start(void)
{
	if (!lcdc_enabled) {
		/* enable DSI block */
		mdp4_iommu_attach();
		mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		MDP_OUTP(MDP_BASE + LCDC_BASE, 1);
		lcdc_enabled = 1;
	}
}

/*
 * mdp4_lcdc_pipe_queue:
 * called from thread context
 */
void mdp4_lcdc_pipe_queue(int cndx, struct mdp4_overlay_pipe *pipe)
{
	struct vsycn_ctrl *vctrl;
	struct vsync_update *vp;
	struct mdp4_overlay_pipe *pp;
	int undx;

	if (cndx >= MAX_CONTROLLER) {
		pr_err("%s: out or range: cndx=%d\n", __func__, cndx);
		return;
	}

	vctrl = &vsync_ctrl_db[cndx];

	if (atomic_read(&vctrl->suspend) > 0)
		return;

	mutex_lock(&vctrl->update_lock);
	undx =  vctrl->update_ndx;
	vp = &vctrl->vlist[undx];

	pp = &vp->plist[pipe->pipe_ndx - 1];	/* ndx start form 1 */

	pr_debug("%s: vndx=%d pipe_ndx=%d pid=%d\n", __func__,
			undx, pipe->pipe_ndx, current->pid);

	*pp = *pipe;	/* clone it */
	vp->update_cnt++;
	mutex_unlock(&vctrl->update_lock);
	mdp4_stat.overlay_play[pipe->mixer_num]++;
}

static void mdp4_lcdc_pipe_clean(struct vsync_update *vp)
{
	struct mdp4_overlay_pipe *pipe;
	int i;

	pipe = vp->plist;
	for (i = 0; i < OVERLAY_PIPE_MAX; i++, pipe++) {
		if (pipe->pipe_used) {
			mdp4_overlay_iommu_pipe_free(pipe->pipe_ndx, 0);
			pipe->pipe_used = 0; /* clear */
		}
	}
	vp->update_cnt = 0;     /* empty queue */
}

static void mdp4_lcdc_blt_ov_update(struct mdp4_overlay_pipe *pipe);
static void mdp4_lcdc_wait4dmap(int cndx);
static void mdp4_lcdc_wait4ov(int cndx);

int mdp4_lcdc_pipe_commit(int cndx, int wait)
{

	int  i, undx;
	int mixer = 0;
	struct vsycn_ctrl *vctrl;
	struct vsync_update *vp;
	struct mdp4_overlay_pipe *pipe;
	struct mdp4_overlay_pipe *real_pipe;
	unsigned long flags;
	int cnt = 0;

	vctrl = &vsync_ctrl_db[cndx];

	mutex_lock(&vctrl->update_lock);
	undx =  vctrl->update_ndx;
	vp = &vctrl->vlist[undx];
	pipe = vctrl->base_pipe;
	if (pipe == NULL) {
		pr_err("%s: NO base pipe\n", __func__);
		mutex_unlock(&vctrl->update_lock);
		return 0;
	}
	mixer = pipe->mixer_num;

        mdp_update_pm(vctrl->mfd, vctrl->vsync_time);

/*
* allow stage_commit without pipes queued
* (vp->update_cnt == 0) to unstage pipes after
* overlay_unset
*/

	vctrl->update_ndx++;
	vctrl->update_ndx &= 0x01;
	vp->update_cnt = 0;     /* reset */
	if (vctrl->blt_free) {
		vctrl->blt_free--;
		if (vctrl->blt_free == 0)
			mdp4_free_writeback_buf(vctrl->mfd, mixer);
	}
	mutex_unlock(&vctrl->update_lock);

	spin_lock_irqsave(&vctrl->spin_lock, flags);
	if (vctrl->ov_koff != vctrl->ov_done) {
		spin_unlock_irqrestore(&vctrl->spin_lock, flags);
		pr_err("%s: Error, frame dropped %d %d\n", __func__,
			vctrl->ov_koff, vctrl->ov_done);
		return 0;
	}
	spin_unlock_irqrestore(&vctrl->spin_lock, flags);

	mdp4_overlay_mdp_perf_upd(vctrl->mfd, 1);

	if (vctrl->blt_change) {
		pipe = vctrl->base_pipe;
		spin_lock_irqsave(&vctrl->spin_lock, flags);
		INIT_COMPLETION(vctrl->dmap_comp);
		INIT_COMPLETION(vctrl->ov_comp);
		vsync_irq_enable(INTR_DMA_P_DONE, MDP_DMAP_TERM);
		spin_unlock_irqrestore(&vctrl->spin_lock, flags);
		mdp4_lcdc_wait4dmap(0);
		if (pipe->ov_blt_addr)
			mdp4_lcdc_wait4ov(0);
	}

	pipe = vp->plist;
	for (i = 0; i < OVERLAY_PIPE_MAX; i++, pipe++) {
		if (pipe->pipe_used) {
			cnt++;
			real_pipe = mdp4_overlay_ndx2pipe(pipe->pipe_ndx);
			if (real_pipe && real_pipe->pipe_used) {
				/* pipe not unset */
				mdp4_overlay_vsync_commit(pipe);
			}
		}
	}

	mdp4_mixer_stage_commit(mixer);

	/* start timing generator & mmu if they are not started yet */
	mdp4_overlay_lcdc_start();
	/*
	 * there has possibility that pipe commit come very close to next vsync
	 * this may cause two consecutive pie_commits happen within same vsync
	 * period which casue iommu page fault when previous iommu buffer
	 * freed. Set ION_IOMMU_UNMAP_DELAYED flag at ion_map_iommu() to
	 * add delay unmap iommu buffer to fix this problem.
	 * Also ion_unmap_iommu() may take as long as 9 ms to free an ion buffer.
	 * therefore mdp4_overlay_iommu_unmap_freelist(mixer) should be called
	 * ater stage_commit() to ensure pipe_commit (up to stage_commit)
	 * is completed within vsync period.
	 */

	/* free previous committed iommu back to pool */
	mdp4_overlay_iommu_unmap_freelist(mixer);

	pipe = vp->plist;
	for (i = 0; i < OVERLAY_PIPE_MAX; i++, pipe++) {
		if (pipe->pipe_used) {
			/* free previous iommu to freelist
			* which will be freed at next
			* pipe_commit
			*/
			mdp4_overlay_iommu_pipe_free(pipe->pipe_ndx, 0);
			pipe->pipe_used = 0; /* clear */
		}
	}


	pipe = vctrl->base_pipe;
	spin_lock_irqsave(&vctrl->spin_lock, flags);
	if (pipe->ov_blt_addr) {
		mdp4_lcdc_blt_ov_update(pipe);
		pipe->ov_cnt++;
		INIT_COMPLETION(vctrl->ov_comp);
		vsync_irq_enable(INTR_OVERLAY0_DONE, MDP_OVERLAY0_TERM);
		mb();
		vctrl->ov_koff++;
		/* kickoff overlay engine */
		mdp4_stat.kickoff_ov0++;
		outpdw(MDP_BASE + 0x0004, 0);
	} else {
		/* schedule second phase update  at dmap */
		INIT_COMPLETION(vctrl->dmap_comp);
		vsync_irq_enable(INTR_DMA_P_DONE, MDP_DMAP_TERM);
	}
	spin_unlock_irqrestore(&vctrl->spin_lock, flags);

	mdp4_stat.overlay_commit[pipe->mixer_num]++;

	if (wait) {
		if (pipe->ov_blt_addr)
			mdp4_lcdc_wait4ov(0);
		else
			mdp4_lcdc_wait4dmap(0);
	}

	return cnt;
}

static void mdp4_lcdc_vsync_irq_ctrl(int cndx, int enable){
	struct vsycn_ctrl *vctrl;
	static int vsync_irq_cnt;

	vctrl = &vsync_ctrl_db[cndx];

	mutex_lock(&vctrl->update_lock);
	if (enable) {
		if (vsync_irq_cnt == 0)
			vsync_irq_enable(INTR_PRIMARY_VSYNC,
			MDP_PRIM_VSYNC_TERM);
		vsync_irq_cnt++;
	} else {
		if (vsync_irq_cnt) {
			vsync_irq_cnt--;
			if (vsync_irq_cnt == 0)
				vsync_irq_disable(INTR_PRIMARY_VSYNC,
				MDP_PRIM_VSYNC_TERM);
		}
	}
	pr_debug("%s: enable=%d cnt=%d\n", __func__, enable, vsync_irq_cnt);
	mutex_unlock(&vctrl->update_lock);
}

void mdp4_lcdc_vsync_ctrl(struct fb_info *info, int enable)
{
	struct vsycn_ctrl *vctrl;
	int cndx = 0;

	vctrl = &vsync_ctrl_db[cndx];

	if (vctrl->vsync_irq_enabled == enable)
		return;

	pr_debug("%s: vsync enable=%d\n", __func__, enable);

	vctrl->vsync_irq_enabled = enable;
	
	mdp4_lcdc_vsync_irq_ctrl(cndx, enable);


	if (vctrl->vsync_irq_enabled &&  atomic_read(&vctrl->suspend) == 0)
		atomic_set(&vctrl->vsync_resume, 1);
}

void mdp4_lcdc_wait4vsync(int cndx)
{
	struct vsycn_ctrl *vctrl;
	struct mdp4_overlay_pipe *pipe;
	unsigned long flags;

	if (cndx >= MAX_CONTROLLER) {
		pr_err("%s: out or range: cndx=%d\n", __func__, cndx);
		return;
	}

	vctrl = &vsync_ctrl_db[cndx];
	pipe = vctrl->base_pipe;

	if (atomic_read(&vctrl->suspend) > 0)
		return;

	mdp4_lcdc_vsync_irq_ctrl(cndx, 1);
	
	spin_lock_irqsave(&vctrl->spin_lock, flags);

	if (vctrl->wait_vsync_cnt == 0)
		INIT_COMPLETION(vctrl->vsync_comp);
	vctrl->wait_vsync_cnt++;
	spin_unlock_irqrestore(&vctrl->spin_lock, flags);

	wait_for_completion(&vctrl->vsync_comp);
	mdp4_lcdc_vsync_irq_ctrl(cndx, 0);
	mdp4_stat.wait4vsync0++;
}

static void mdp4_lcdc_wait4dmap(int cndx)
{
	struct vsycn_ctrl *vctrl;

	if (cndx >= MAX_CONTROLLER) {
		pr_err("%s: out or range: cndx=%d\n", __func__, cndx);
		return;
	}

	vctrl = &vsync_ctrl_db[cndx];

	if (atomic_read(&vctrl->suspend) > 0)
		return;

	wait_for_completion(&vctrl->dmap_comp);
}

static void mdp4_lcdc_wait4ov(int cndx)
{
	struct vsycn_ctrl *vctrl;

	if (cndx >= MAX_CONTROLLER) {
		pr_err("%s: out or range: cndx=%d\n", __func__, cndx);
		return;
	}

	vctrl = &vsync_ctrl_db[cndx];

	if (atomic_read(&vctrl->suspend) > 0)
		return;

	wait_for_completion(&vctrl->ov_comp);
}

ssize_t mdp4_lcdc_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int cndx;
	struct vsycn_ctrl *vctrl;
	ssize_t ret = 0;
	unsigned long flags;
	u64 vsync_tick;
	ktime_t ctime;
	u32 ctick, ptick;
	int diff;

	cndx = 0;
	vctrl = &vsync_ctrl_db[0];

	if (atomic_read(&vctrl->suspend) > 0 ||
		atomic_read(&vctrl->vsync_resume) == 0)
		return 0;
	/*
	 * show_event thread keep spinning on vctrl->vsync_comp
	 * race condition on x.done if multiple thread blocked
	 * at wait_for_completion(&vctrl->vsync_comp)
	 *
	 * if show_event thread waked up first then it will come back
	 * and call INIT_COMPLETION(vctrl->vsync_comp) which set x.done = 0
	 * then second thread wakeed up which set x.done = 0x7ffffffd
	 * after that wait_for_completion will never wait.
	 * To avoid this, force show_event thread to sleep 5 ms here
	 * since it has full vsycn period (16.6 ms) to wait
	 */
	ctime = ktime_get();
	ctick = (u32)ktime_to_us(ctime);
	ptick = (u32)ktime_to_us(vctrl->vsync_time);
	ptick += 5000;	/* 5ms */
	diff = ptick - ctick;
	if (diff > 0) {
		if (diff > 1000) /* 1 ms */
			diff = 1000;
		usleep(diff);
	}

	spin_lock_irqsave(&vctrl->spin_lock, flags);
	if (vctrl->wait_vsync_cnt == 0)
		INIT_COMPLETION(vctrl->vsync_comp);
	vctrl->wait_vsync_cnt++;
	spin_unlock_irqrestore(&vctrl->spin_lock, flags);
	ret = wait_for_completion_interruptible_timeout(&vctrl->vsync_comp,
		msecs_to_jiffies(VSYNC_PERIOD * 4));
	if (ret <= 0) {
		vctrl->wait_vsync_cnt = 0;
		vctrl->vsync_time = ktime_get();
	}

	spin_lock_irqsave(&vctrl->spin_lock, flags);
	vsync_tick = ktime_to_ns(vctrl->vsync_time);
	spin_unlock_irqrestore(&vctrl->spin_lock, flags);

	ret = snprintf(buf, PAGE_SIZE, "VSYNC=%llu", vsync_tick);
	buf[strlen(buf) + 1] = '\0';
	return ret;
}

void mdp4_lcdc_vsync_init(int cndx)
{
	struct vsycn_ctrl *vctrl;

	if (cndx >= MAX_CONTROLLER) {
		pr_err("%s: out or range: cndx=%d\n", __func__, cndx);
		return;
	}

	pr_info("%s: ndx=%d\n", __func__, cndx);

	vctrl = &vsync_ctrl_db[cndx];
	if (vctrl->inited)
		return;

	vctrl->inited = 1;
	vctrl->update_ndx = 0;
	mutex_init(&vctrl->update_lock);
	init_completion(&vctrl->vsync_comp);
	init_completion(&vctrl->dmap_comp);
	init_completion(&vctrl->ov_comp);
	atomic_set(&vctrl->suspend, 1);
	atomic_set(&vctrl->vsync_resume, 1);
	spin_lock_init(&vctrl->spin_lock);
}

void mdp4_lcdc_base_swap(int cndx, struct mdp4_overlay_pipe *pipe)
{
	struct vsycn_ctrl *vctrl;

	if (cndx >= MAX_CONTROLLER) {
		pr_err("%s: out or range: cndx=%d\n", __func__, cndx);
		return;
	}

	vctrl = &vsync_ctrl_db[cndx];
	vctrl->base_pipe = pipe;
}

int mdp4_lcdc_on(struct platform_device *pdev)
{
	int lcdc_width;
	int lcdc_height;
	int lcdc_bpp;
	int lcdc_border_clr;
	int lcdc_underflow_clr;
	int lcdc_hsync_skew;

	int hsync_period;
	int hsync_ctrl;
	int vsync_period;
	int display_hctl;
	int display_v_start;
	int display_v_end;
	int active_hctl;
	int active_h_start;
	int active_h_end;
	int active_v_start;
	int active_v_end;
	int ctrl_polarity;
	int h_back_porch;
	int h_front_porch;
	int v_back_porch;
	int v_front_porch;
	int hsync_pulse_width;
	int vsync_pulse_width;
	int hsync_polarity;
	int vsync_polarity;
	int data_en_polarity;
	int hsync_start_x;
	int hsync_end_x;
	uint8 *buf;
	unsigned int buf_offset;
	int bpp, ptype;
	struct fb_info *fbi;
	struct fb_var_screeninfo *var;
	struct msm_fb_data_type *mfd;
	struct mdp4_overlay_pipe *pipe;
	int ret = 0;
	int cndx = 0;
	struct vsycn_ctrl *vctrl;

	vctrl = &vsync_ctrl_db[cndx];
	mfd = (struct msm_fb_data_type *)platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	mutex_lock(&mfd->dma->ov_mutex);

	vctrl->mfd = mfd;
	vctrl->dev = mfd->fbi->dev;
	vctrl->vsync_irq_enabled = 0;

	/* mdp clock on */
	mdp_clk_ctrl(1);

	fbi = mfd->fbi;
	var = &fbi->var;

	bpp = fbi->var.bits_per_pixel / 8;
	buf = (uint8 *) fbi->fix.smem_start;
	buf_offset = calc_fb_offset(mfd, fbi, bpp);

	if (vctrl->base_pipe == NULL) {
		ptype = mdp4_overlay_format2type(mfd->fb_imgType);
		if (ptype < 0)
			printk(KERN_INFO "%s: format2type failed\n", __func__);
		pipe = mdp4_overlay_pipe_alloc(ptype, MDP4_MIXER0);
		if (pipe == NULL)
			printk(KERN_INFO "%s: pipe_alloc failed\n", __func__);
		pipe->pipe_used++;
		pipe->mixer_stage  = MDP4_MIXER_STAGE_BASE;
		pipe->mixer_num  = MDP4_MIXER0;
		pipe->src_format = mfd->fb_imgType;
		mdp4_overlay_panel_mode(pipe->mixer_num, MDP4_PANEL_LCDC);
		ret = mdp4_overlay_format2pipe(pipe);
		if (ret < 0)
			printk(KERN_INFO "%s: format2pipe failed\n", __func__);

		mdp4_init_writeback_buf(mfd, MDP4_MIXER0);
		pipe->ov_blt_addr = 0;
		pipe->dma_blt_addr = 0;

		vctrl->base_pipe = pipe; /* keep it */
	} else {
		pipe = vctrl->base_pipe;
	}


	pipe->src_height = fbi->var.yres;
	pipe->src_width = fbi->var.xres;
	pipe->src_h = fbi->var.yres;
	pipe->src_w = fbi->var.xres;
	pipe->src_y = 0;
	pipe->src_x = 0;
	pipe->dst_h = fbi->var.yres;
	pipe->dst_w = fbi->var.xres;

	if (mfd->display_iova)
		pipe->srcp0_addr = mfd->display_iova + buf_offset;
	else
		pipe->srcp0_addr = (uint32)(buf + buf_offset);

	pipe->srcp0_ystride = fbi->fix.line_length;
	pipe->bpp = bpp;

	mdp4_overlay_solidfill_init(pipe);

	mdp4_overlay_mdp_pipe_req(pipe, mfd);
	mdp4_calc_blt_mdp_bw(mfd, pipe);

	atomic_set(&vctrl->suspend, 0);

	mdp4_overlay_dmap_xy(pipe);
	mdp4_overlay_dmap_cfg(mfd, 1);
	mdp4_overlay_rgb_setup(pipe);
	mdp4_overlayproc_cfg(pipe);

	mdp4_overlay_reg_flush(pipe, 1);
	mdp4_mixer_stage_up(pipe, 0);
	mdp4_mixer_stage_commit(pipe->mixer_num);


	/*
	 * LCDC timing setting
	 */
	h_back_porch = var->left_margin;
	h_front_porch = var->right_margin;
	v_back_porch = var->upper_margin;
	v_front_porch = var->lower_margin;
	hsync_pulse_width = var->hsync_len;
	vsync_pulse_width = var->vsync_len;
	lcdc_border_clr = mfd->panel_info.lcdc.border_clr;
	lcdc_underflow_clr = mfd->panel_info.lcdc.underflow_clr;
	lcdc_hsync_skew = mfd->panel_info.lcdc.hsync_skew;

	lcdc_width = var->xres + mfd->panel_info.lcdc.xres_pad;
	lcdc_height = var->yres + mfd->panel_info.lcdc.yres_pad;
	lcdc_bpp = mfd->panel_info.bpp;

	hsync_period =
	    hsync_pulse_width + h_back_porch + h_front_porch;
	if ((mfd->panel_info.type == LVDS_PANEL) &&
		(mfd->panel_info.lvds.channel_mode == LVDS_DUAL_CHANNEL_MODE))
		hsync_period += lcdc_width / 2;
	else
		hsync_period += lcdc_width;
	hsync_ctrl = (hsync_period << 16) | hsync_pulse_width;
	hsync_start_x = hsync_pulse_width + h_back_porch;
	hsync_end_x = hsync_period - h_front_porch - 1;
	display_hctl = (hsync_end_x << 16) | hsync_start_x;

	vsync_period =
	    (vsync_pulse_width + v_back_porch + lcdc_height +
	     v_front_porch) * hsync_period;
	display_v_start =
	    (vsync_pulse_width + v_back_porch) * hsync_period + lcdc_hsync_skew;
	display_v_end =
	    vsync_period - (v_front_porch * hsync_period) + lcdc_hsync_skew - 1;

	if (lcdc_width != var->xres) {
		active_h_start = hsync_start_x + first_pixel_start_x;
		active_h_end = active_h_start + var->xres - 1;
		active_hctl =
		    ACTIVE_START_X_EN | (active_h_end << 16) | active_h_start;
	} else {
		active_hctl = 0;
	}

	if (lcdc_height != var->yres) {
		active_v_start =
		    display_v_start + first_pixel_start_y * hsync_period;
		active_v_end = active_v_start + (var->yres) * hsync_period - 1;
		active_v_start |= ACTIVE_START_Y_EN;
	} else {
		active_v_start = 0;
		active_v_end = 0;
	}


#ifdef CONFIG_FB_MSM_MDP40
	if (mfd->panel_info.lcdc.is_sync_active_high) {
		hsync_polarity = 0;
		vsync_polarity = 0;
	} else {
		hsync_polarity = 1;
		vsync_polarity = 1;
	}
	lcdc_underflow_clr |= 0x80000000;	/* enable recovery */
#else
	hsync_polarity = 0;
	vsync_polarity = 0;
#endif
	data_en_polarity = 0;

	ctrl_polarity =
	    (data_en_polarity << 2) | (vsync_polarity << 1) | (hsync_polarity);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x4, hsync_ctrl);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x8, vsync_period);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0xc, vsync_pulse_width * hsync_period);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x10, display_hctl);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x14, display_v_start);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x18, display_v_end);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x28, lcdc_border_clr);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x2c, lcdc_underflow_clr);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x30, lcdc_hsync_skew);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x38, ctrl_polarity);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x1c, active_hctl);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x20, active_v_start);
	MDP_OUTP(MDP_BASE + LCDC_BASE + 0x24, active_v_end);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mdp_histogram_ctrl_all(TRUE);
	mdp4_overlay_lcdc_start();
	mutex_unlock(&mfd->dma->ov_mutex);

	return ret;
}

/* timing generator off */
static void mdp4_lcdc_tg_off(struct vsycn_ctrl *vctrl)
{
	unsigned long flags;

	spin_lock_irqsave(&vctrl->spin_lock, flags);
	INIT_COMPLETION(vctrl->vsync_comp);
	vctrl->wait_vsync_cnt++;
	MDP_OUTP(MDP_BASE + LCDC_BASE, 0); /* turn off timing generator */
	spin_unlock_irqrestore(&vctrl->spin_lock, flags);

	mdp4_lcdc_wait4vsync(0);
}
int mdp4_lcdc_off(struct platform_device *pdev)
{
	int ret = 0;
	int cndx = 0;
	struct msm_fb_data_type *mfd;
	struct vsycn_ctrl *vctrl;
	struct mdp4_overlay_pipe *pipe;
	struct vsync_update *vp;
	unsigned long flags;
	int undx, need_wait = 0;

	mfd = (struct msm_fb_data_type *)platform_get_drvdata(pdev);

	mutex_lock(&mfd->dma->ov_mutex);
	vctrl = &vsync_ctrl_db[cndx];
	pipe = vctrl->base_pipe;

	mdp4_lcdc_wait4vsync(cndx);

	atomic_set(&vctrl->vsync_resume, 0);

	msleep(20);	/* >= 17 ms */

	complete_all(&vctrl->vsync_comp);
	vctrl->wait_vsync_cnt = 0;
	if (pipe == NULL)
		return -EINVAL;
	if (pipe->ov_blt_addr) {
		spin_lock_irqsave(&vctrl->spin_lock, flags);
		if (vctrl->ov_koff != vctrl->ov_done)
			need_wait = 1;
		spin_unlock_irqrestore(&vctrl->spin_lock, flags);
		if (need_wait)
			mdp4_lcdc_wait4ov(0);
	}

	mdp_histogram_ctrl_all(FALSE);

	lcdc_enabled = 0;

	undx =  vctrl->update_ndx;
	vp = &vctrl->vlist[undx];
	if (vp->update_cnt) {
		/*
		 * pipe's iommu will be freed at next overlay play
		 * and iommu_drop statistic will be increased by one
		 */
		pr_warn("%s: update_cnt=%d\n", __func__, vp->update_cnt);
		mdp4_lcdc_pipe_clean(vp);
	}

	if (pipe) {
		/* sanity check, free pipes besides base layer */
		mdp4_overlay_unset_mixer(pipe->mixer_num);
		if (mfd->ref_cnt == 0) {
			/* adb stop */
			if (pipe->pipe_type == OVERLAY_TYPE_BF)
				mdp4_overlay_borderfill_stage_down(pipe);

			/* base pipe may change after borderfill_stage_down */
			pipe = vctrl->base_pipe;
			mdp4_mixer_stage_down(pipe, 1);
			mdp4_overlay_pipe_free(pipe);
			vctrl->base_pipe = NULL;
		} else {
			/* system suspending */
			mdp4_mixer_stage_down(vctrl->base_pipe, 1);
			mdp4_overlay_iommu_pipe_free(
				vctrl->base_pipe->pipe_ndx, 1);
		}
	}

	mdp4_lcdc_tg_off(vctrl);

	atomic_set(&vctrl->suspend, 1);

	/* MDP clock disable */
	mdp_clk_ctrl(0);
	mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mutex_unlock(&mfd->dma->ov_mutex);

	return ret;
}

static void mdp4_lcdc_blt_ov_update(struct mdp4_overlay_pipe *pipe)
{
	uint32 off, addr;
	int bpp;
	char *overlay_base;

	if (pipe->ov_blt_addr == 0)
		return;

#ifdef BLT_RGB565
	bpp = 2; /* overlay ouput is RGB565 */
#else
	bpp = 3; /* overlay ouput is RGB888 */
#endif
	off = 0;
	if (pipe->ov_cnt & 0x01)
		off = pipe->src_height * pipe->src_width * bpp;
	addr = pipe->ov_blt_addr + off;

	/* overlay 0 */
	overlay_base = MDP_BASE + MDP4_OVERLAYPROC0_BASE;/* 0x10000 */
	outpdw(overlay_base + 0x000c, addr);
	outpdw(overlay_base + 0x001c, addr);
}

static void mdp4_lcdc_blt_dmap_update(struct mdp4_overlay_pipe *pipe)
{
	uint32 off, addr;
	int bpp;

	if (pipe->ov_blt_addr == 0)
		return;

#ifdef BLT_RGB565
	bpp = 2; /* overlay ouput is RGB565 */
#else
	bpp = 3; /* overlay ouput is RGB888 */
#endif
	off = 0;
	if (pipe->dmap_cnt & 0x01)
		off = pipe->src_height * pipe->src_width * bpp;
	addr = pipe->dma_blt_addr + off;

	/* dmap */
	MDP_OUTP(MDP_BASE + 0x90008, addr);
}

/*
 * mdp4_primary_vsync_lcdc: called from isr
 */
void mdp4_primary_vsync_lcdc(void)
{
	int cndx;
	struct vsycn_ctrl *vctrl;

	cndx = 0;
	vctrl = &vsync_ctrl_db[cndx];
	pr_debug("%s: cpu=%d\n", __func__, smp_processor_id());

	spin_lock(&vctrl->spin_lock);
	vctrl->vsync_time = ktime_get();

	complete_all(&vctrl->vsync_comp);
	vctrl->wait_vsync_cnt = 0;
	spin_unlock(&vctrl->spin_lock);
}

/*
 * mdp4_dma_p_done_lcdc: called from isr
 */
void mdp4_dmap_done_lcdc(int cndx)
{
	struct vsycn_ctrl *vctrl;
	struct mdp4_overlay_pipe *pipe;

	if (cndx >= MAX_CONTROLLER) {
		pr_err("%s: out or range: cndx=%d\n", __func__, cndx);
		return;
	}
	vctrl = &vsync_ctrl_db[cndx];
	pipe = vctrl->base_pipe;

	spin_lock(&vctrl->spin_lock);
	vsync_irq_disable(INTR_DMA_P_DONE, MDP_DMAP_TERM);
	if (vctrl->blt_change) {
		mdp4_overlayproc_cfg(pipe);
		mdp4_overlay_dmap_xy(pipe);
		if (pipe->ov_blt_addr) {
			mdp4_lcdc_blt_ov_update(pipe);
			pipe->ov_cnt++;
			/* Prefill one frame */
			vsync_irq_enable(INTR_OVERLAY0_DONE, MDP_OVERLAY0_TERM);
			/* kickoff overlay0 engine */
			mdp4_stat.kickoff_ov0++;
			vctrl->ov_koff++;       /* make up for prefill */
			outpdw(MDP_BASE + 0x0004, 0);
		}
		vctrl->blt_change = 0;
	}

	complete_all(&vctrl->dmap_comp);

	if (mdp_rev <= MDP_REV_41)
		mdp4_mixer_blend_cfg(MDP4_MIXER0);

	mdp4_overlay_dma_commit(cndx);
	spin_unlock(&vctrl->spin_lock);
}

/*
 * mdp4_overlay0_done_lcdc: called from isr
 */
void mdp4_overlay0_done_lcdc(int cndx)
{
	struct vsycn_ctrl *vctrl;
	struct mdp4_overlay_pipe *pipe;

	vctrl = &vsync_ctrl_db[cndx];
	pipe = vctrl->base_pipe;

	spin_lock(&vctrl->spin_lock);
	vsync_irq_disable(INTR_OVERLAY0_DONE, MDP_OVERLAY0_TERM);
	vctrl->ov_done++;
	complete_all(&vctrl->ov_comp);
	if (pipe->ov_blt_addr == 0) {
		spin_unlock(&vctrl->spin_lock);
		return;
	}

	mdp4_lcdc_blt_dmap_update(pipe);
	pipe->dmap_cnt++;
	spin_unlock(&vctrl->spin_lock);
}

static void mdp4_lcdc_do_blt(struct msm_fb_data_type *mfd, int enable)
{
	unsigned long flag;
	int cndx = 0;
	struct vsycn_ctrl *vctrl;
	struct mdp4_overlay_pipe *pipe;

	vctrl = &vsync_ctrl_db[cndx];
	pipe = vctrl->base_pipe;

	mdp4_allocate_writeback_buf(mfd, MDP4_MIXER0);

	if (mfd->ov0_wb_buf->write_addr == 0) {
		pr_info("%s: no blt_base assigned\n", __func__);
		return;
	}

	spin_lock_irqsave(&vctrl->spin_lock, flag);
	if (enable && pipe->ov_blt_addr == 0) {
		pipe->ov_blt_addr = mfd->ov0_wb_buf->write_addr;
		pipe->dma_blt_addr = mfd->ov0_wb_buf->read_addr;
		pipe->ov_cnt = 0;
		pipe->dmap_cnt = 0;
		vctrl->ov_koff = 0;
		vctrl->ov_done = 0;
		vctrl->blt_free = 0;
		mdp4_stat.blt_lcdc++;
		vctrl->blt_change++;
	} else if (enable == 0 && pipe->ov_blt_addr) {
		pipe->ov_blt_addr = 0;
		pipe->dma_blt_addr = 0;
		vctrl->blt_free = 4;    /* 4 commits to free wb buf */
		vctrl->blt_change++;
	}

	pr_info("%s: enable=%d change=%d blt_addr=%x\n", __func__,
		vctrl->blt_change, enable, (int)pipe->ov_blt_addr);

	if (!vctrl->blt_change) {
		spin_unlock_irqrestore(&vctrl->spin_lock, flag);
		return;
	}

	spin_unlock_irqrestore(&vctrl->spin_lock, flag);
}

void mdp4_lcdc_overlay_blt(struct msm_fb_data_type *mfd,
					struct msmfb_overlay_blt *req)
{
	mdp4_lcdc_do_blt(mfd, req->enable);
}

void mdp4_lcdc_overlay_blt_start(struct msm_fb_data_type *mfd)
{
	mdp4_lcdc_do_blt(mfd, 1);
}

void mdp4_lcdc_overlay_blt_stop(struct msm_fb_data_type *mfd)
{
	mdp4_lcdc_do_blt(mfd, 0);
}

void mdp4_lcdc_overlay(struct msm_fb_data_type *mfd)
{
	struct fb_info *fbi = mfd->fbi;
	uint8 *buf;
	unsigned int buf_offset;
	int bpp;
	int cnt, cndx = 0;
	struct vsycn_ctrl *vctrl;
	struct mdp4_overlay_pipe *pipe;

	mutex_lock(&mfd->dma->ov_mutex);

	vctrl = &vsync_ctrl_db[cndx];
	pipe = vctrl->base_pipe;

	if (!pipe || !mfd->panel_power_on) {
		mutex_unlock(&mfd->dma->ov_mutex);
		return;
	}

	pr_debug("%s: cpu=%d pid=%d\n", __func__,
			smp_processor_id(), current->pid);
	if (pipe->pipe_type == OVERLAY_TYPE_RGB) {
		bpp = fbi->var.bits_per_pixel / 8;
		buf = (uint8 *) fbi->fix.smem_start;
		buf_offset = calc_fb_offset(mfd, fbi, bpp);

		if (mfd->display_iova)
			pipe->srcp0_addr = mfd->display_iova + buf_offset;
		else
			pipe->srcp0_addr = (uint32)(buf + buf_offset);

		mdp4_lcdc_pipe_queue(0, pipe);
	}

	mdp4_overlay_mdp_perf_upd(mfd, 1);

	cnt = mdp4_lcdc_pipe_commit(cndx, 0);
	if (cnt >= 0){
		if (pipe->ov_blt_addr)
			mdp4_lcdc_wait4ov(cndx);
		else
			mdp4_lcdc_wait4dmap(cndx);
	}

	mdp4_overlay_mdp_perf_upd(mfd, 0);
	mutex_unlock(&mfd->dma->ov_mutex);
}

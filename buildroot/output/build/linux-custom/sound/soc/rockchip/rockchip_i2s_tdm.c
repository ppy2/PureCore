/* sound/soc/rockchip/rockchip_i2s_tdm.c
 *
 * ALSA SoC Audio Layer - Rockchip I2S/TDM Controller driver
 *
 * Copyright (c) 2018 Rockchip Electronics Co. Ltd.
 * Author: Sugar Zhang <sugar.zhang@rock-chips.com>
 */
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/gpio/consumer.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/rockchip.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/pinctrl/consumer.h>
#include <linux/random.h>
#include <linux/math64.h>
#include <linux/version.h>
#include <sound/pcm_params.h>
#include <sound/dmaengine_pcm.h>
#include <sound/soc.h>

/* Conditional NEON compilation */
#ifdef CONFIG_KERNEL_MODE_NEON
#include <linux/kernel.h>
#include <asm/neon.h>
#include <asm/simd.h>
/* Disable NEON intrinsics for soft-float ABI */
#if defined(__ARM_NEON__) && !defined(__SOFTFP__)
#include <arm_neon.h>
#define HAVE_NEON_SUPPORT
#endif
#endif

#include "rockchip_i2s_tdm.h"
#include "rockchip_dlp.h"

#define DRV_NAME "rockchip-i2s-tdm"

#if IS_ENABLED(CONFIG_CPU_PX30) || IS_ENABLED(CONFIG_CPU_RK1808) || IS_ENABLED(CONFIG_CPU_RK3308)
#define HAVE_SYNC_RESET
#endif

#define DEFAULT_MCLK_FS				256
#define DEFAULT_FS				48000
#define CH_GRP_MAX				4  /* The max channel 8 / 2 */
#define MULTIPLEX_CH_MAX			10
#define CLK_PPM_MIN				(-1000)
#define CLK_PPM_MAX				(1000)
#define MAXBURST_PER_FIFO			64  /* Match kernel 5.10 for proper DMA alignment */

/* Auto-mute timing defaults */
#define DEFAULT_POSTMUTE_DELAY_MS		450

#define QUIRK_ALWAYS_ON				BIT(0)
#define QUIRK_HDMI_PATH				BIT(1)
#define QUIRK_MCLK_ALWAYS_ON			BIT(2)


struct txrx_config {
    u32 addr;
    u32 reg;
    u32 txonly;
    u32 rxonly;
};

struct rk_i2s_soc_data {
    u32 softrst_offset;
    u32 grf_reg_offset;
    u32 grf_shift;
    int config_count;
    const struct txrx_config *configs;
    int (*init)(struct device *dev, u32 addr);
};

struct rk_i2s_tdm_dev {
    struct device *dev;
    struct clk *hclk;
    struct clk *mclk_tx;
    struct clk *mclk_rx;
    /* The mclk_tx_src is parent of mclk_tx */
    struct clk *mclk_tx_src;
    /* The mclk_rx_src is parent of mclk_rx */
    struct clk *mclk_rx_src;
    /*
     * The mclk_root0 and mclk_root1 are root parent and supplies for
     * the different FS.
     */
    struct clk *mclk_root0;
    struct clk *mclk_root1;
    struct clk *mclk_out;  /* MCLKOUT pin clock for external DAC */
    bool mclk_external;
    bool mclk_ext_mux;
    struct clk *mclk_ext;
    struct clk *clk_44;
    struct clk *clk_48;
    struct regmap *regmap;
    struct regmap *grf;
    struct snd_dmaengine_dai_dma_data capture_dma_data;
    struct snd_dmaengine_dai_dma_data playback_dma_data;
    struct snd_pcm_substream *substreams[SNDRV_PCM_STREAM_LAST + 1];
    struct reset_control *tx_reset;
    struct reset_control *rx_reset;
    const struct rk_i2s_soc_data *soc_data;
#ifdef HAVE_SYNC_RESET
    void __iomem *cru_base;
    int tx_reset_id;
    int rx_reset_id;
#endif
    bool is_master_mode;
    bool io_multiplex;
    bool mclk_calibrate;
    bool tdm_mode;
    bool tdm_fsync_half_frame;
    unsigned int mclk_rx_freq;
    unsigned int mclk_tx_freq;
    unsigned int mclk_root0_freq;
    unsigned int mclk_root1_freq;
    unsigned int mclk_root0_initial_freq;
    unsigned int mclk_root1_initial_freq;
    unsigned int bclk_fs;
    unsigned int clk_trcm;
    unsigned int i2s_sdis[CH_GRP_MAX];
    unsigned int i2s_sdos[CH_GRP_MAX];
    unsigned int quirks;
    int clk_ppm;
    atomic_t refcount;
    spinlock_t lock; /* xfer lock */
    int volume;
    bool mute;
    struct gpio_desc *mute_gpio;
    struct gpio_desc *mute_inv_gpio;  /* Inverted mute signal (GPIO2_A5, pin 69) */
    struct gpio_desc *freq_domain_gpio;  /* Frequency domain indicator GPIO (GPIO1_D1) 44.1/48 kHz */
    bool freq_domain_invert;        // Invert frequency domain GPIO polarity
    
    /* MCLK multiplier for switching 512/1024 */
    int mclk_multiplier;            // MCLK multiplier: 512 or 1024
    
    /* Automatic mute during switching */
    bool auto_mute_active;          // Active state of automatic mute
    bool user_mute_priority;        // User priority over automation
    bool format_change_mute;        // Mute during PCM/DSD format change (higher priority)
    struct delayed_work mute_post_work;   // Timer to disable mute after delay
    struct mutex mute_lock;         // Mutex for protecting mute operations
    
    
    /* Add pause state */
    bool playback_paused;
    bool capture_paused;
    
    /* Debounce for auto-mute */
    unsigned long last_auto_mute_time;
    
    /* Configurable auto-mute times via sysfs */
    unsigned int postmute_delay_ms;     // Mute hold time after start (default 450ms)
    
    /* GPIO for DSD-on signal */
    struct gpio_desc *dsd_on_gpio;
    bool dsd_mode_active;
    
    /* DSD sample swap to eliminate purple noise */
    bool dsd_sample_swap;
    
    /* Channel swap controls */
    bool pcm_channel_swap;     /* PCM: LRCK inversion */
    bool dsd_physical_swap;    /* DSD: swap pins A6/A3 */
    
    /* ALSA control for sysfs and alsamixer synchronization */
    struct snd_kcontrol *mute_kcontrol;
    struct snd_soc_dai *dai; /* For ALSA card access */
    
    /* Saved format for forced changes application */
    unsigned int format;
};

/* Forward declarations for auto-mute functions */
static void rockchip_i2s_tdm_apply_mute(struct rk_i2s_tdm_dev *i2s_tdm, bool enable);
static void rockchip_i2s_tdm_tx_path_config(struct rk_i2s_tdm_dev *i2s_tdm, int num);
static void rockchip_i2s_tdm_handle_dsd_switch(struct rk_i2s_tdm_dev *i2s_tdm, bool enable_dsd);



/* DSD physical channel swap function (I2S routing) */
static void rockchip_i2s_tdm_apply_dsd_physical_swap(struct rk_i2s_tdm_dev *i2s_tdm)
{
    /* Change I2S TX routing for DSD physical swap
     * Standard configuration: i2s_sdos[0]=2, i2s_sdos[1]=3, i2s_sdos[2]=0, i2s_sdos[3]=1
     * Swap configuration: exchange i2s_sdos[1] and i2s_sdos[3] (channels A6/A3)
     * IMPORTANT: swap applies only in DSD mode!
     */
    
    
    if (i2s_tdm->dsd_physical_swap && i2s_tdm->dsd_mode_active) {
        /* Check if swap needs to be applied - only if routing is standard [2,3,0,1] */
        if (i2s_tdm->i2s_sdos[2] == 0 && i2s_tdm->i2s_sdos[3] == 1) {
            /* Swap: exchange channels 2 and 3 (A6/A3) */
            unsigned int temp = i2s_tdm->i2s_sdos[2];
            i2s_tdm->i2s_sdos[2] = i2s_tdm->i2s_sdos[3];
            i2s_tdm->i2s_sdos[3] = temp;
            
            
            /* Apply new routing */
            rockchip_i2s_tdm_tx_path_config(i2s_tdm, 4);
        }
    } else {
        /* Check if standard routing needs to be restored - only if current is swap [2,3,1,0] */
        if (i2s_tdm->i2s_sdos[2] == 1 && i2s_tdm->i2s_sdos[3] == 0) {
            /* Restore standard configuration: 2,3,0,1 */
            i2s_tdm->i2s_sdos[0] = 2;
            i2s_tdm->i2s_sdos[1] = 3;
            i2s_tdm->i2s_sdos[2] = 0;
            i2s_tdm->i2s_sdos[3] = 1;
            
            
            /* Apply standard routing */
            rockchip_i2s_tdm_tx_path_config(i2s_tdm, 4);
        }
    }
}

/* DSD format detection */
static inline int is_dsd(snd_pcm_format_t format)
{
    switch (format) {
        case SNDRV_PCM_FORMAT_DSD_U8:
        case SNDRV_PCM_FORMAT_DSD_U16_LE:
        case SNDRV_PCM_FORMAT_DSD_U16_BE:
        case SNDRV_PCM_FORMAT_DSD_U32_LE:
        case SNDRV_PCM_FORMAT_DSD_U32_BE:
            return 1;
        default:
            return 0;
    }
}

/* Common DSD switch handling function */
static void rockchip_i2s_tdm_handle_dsd_switch(struct rk_i2s_tdm_dev *i2s_tdm, bool enable_dsd)
{
    if (!i2s_tdm->dsd_on_gpio)
        return;

    if (enable_dsd == i2s_tdm->dsd_mode_active)
        return;  /* Already in desired state */

    /* Enable mute before format switch to eliminate clicks */
    if (i2s_tdm->mute_gpio) {
        /* Cancel any pending post-mute work from trigger */
        cancel_delayed_work_sync(&i2s_tdm->mute_post_work);

        i2s_tdm->format_change_mute = true;
        gpiod_set_value(i2s_tdm->mute_gpio, 1);
        if (i2s_tdm->mute_inv_gpio)
            gpiod_set_value(i2s_tdm->mute_inv_gpio, 0);
        msleep(50);
    }

    if (enable_dsd) {
        i2s_tdm->dsd_mode_active = true;
        gpiod_set_value(i2s_tdm->dsd_on_gpio, 1);
        dev_info(i2s_tdm->dev, "ROCKCHIP_I2S_TDM: DSD-on GPIO activated (DSD mode ON)\n");
    } else {
        i2s_tdm->dsd_mode_active = false;
        gpiod_set_value(i2s_tdm->dsd_on_gpio, 0);
        dev_info(i2s_tdm->dev, "ROCKCHIP_I2S_TDM: DSD-on GPIO deactivated (PCM mode)\n");
    }

    /* Apply routing for the new mode - only if DSD is being enabled */
    if (enable_dsd) {
        rockchip_i2s_tdm_apply_dsd_physical_swap(i2s_tdm);
    }

    /* Wait for DAC to settle, then let normal trigger unmute handle it */
    if (i2s_tdm->mute_gpio) {
        msleep(500);
        /* Clear flag and restore auto_mute for next trigger */
        i2s_tdm->format_change_mute = false;
        i2s_tdm->auto_mute_active = true;
        /* DO NOT unmute here - let trigger's mute_post_work handle it */
    }
}

/* Calculate proper BCLK frequency for DSD formats */
static unsigned int calculate_dsd_bclk(snd_pcm_format_t format, unsigned int sample_rate)
{
    /* CORRECT BCLK frequencies for DSD (determine by sample_rate):
     * DSD64: BCLK = 2.8224 MHz 
     * DSD128: BCLK = 5.6448 MHz  
     * DSD256: BCLK = 11.2896 MHz
     * DSD512: BCLK = 22.5792 MHz
     */
    
    /* Determine DSD type by sample_rate */
    if (sample_rate <= 88200) {
        return 2822400;  /* DSD64: 2.8224 MHz - CORRECT! */
    } else if (sample_rate <= 176400) {
        return 5644800;  /* DSD128: 5.6448 MHz */
    } else if (sample_rate <= 352800) {
        return 11289600; /* DSD256: 11.2896 MHz */
    } else {
        return 22579200; /* DSD512: 22.5792 MHz */
    }
}

static void rockchip_i2s_tdm_mute_post_work(struct work_struct *work);

static struct i2s_of_quirks {
    char *quirk;
    int id;
} of_quirks[] = {
    {
    .quirk = "rockchip,always-on",
    .id = QUIRK_ALWAYS_ON,
    },
    {
    .quirk = "rockchip,hdmi-path",
    .id = QUIRK_HDMI_PATH,
    },
    {
    .quirk = "rockchip,mclk-always-on",
    .id = QUIRK_MCLK_ALWAYS_ON,
    },
};


static int to_ch_num(unsigned int val)
{
    int chs;

    switch (val) {
    case I2S_CHN_4:
    chs = 4;
    break;
    case I2S_CHN_6:
    chs = 6;
    break;
    case I2S_CHN_8:
    chs = 8;
    break;
    default:
    chs = 2;
    break;
    }

    return chs;
}

static int i2s_tdm_runtime_suspend(struct device *dev)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);

    regcache_cache_only(i2s_tdm->regmap, true);
    
    /* Do not turn off MCLK if continuous MCLK quirk is enabled */
    if (!(i2s_tdm->quirks & QUIRK_MCLK_ALWAYS_ON)) {
        clk_disable_unprepare(i2s_tdm->mclk_tx);
        clk_disable_unprepare(i2s_tdm->mclk_rx);
    } else {
        dev_dbg(i2s_tdm->dev, "MCLK kept running during suspend (quirk enabled)\n");
    }

    return 0;
}

static int i2s_tdm_runtime_resume(struct device *dev)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int ret;

    /* Enable MCLK only if it was turned off (quirk not active) */
    if (!(i2s_tdm->quirks & QUIRK_MCLK_ALWAYS_ON)) {
        dev_info(i2s_tdm->dev, "Runtime resume: enabling mclk_tx and mclk_rx\n");
        ret = clk_prepare_enable(i2s_tdm->mclk_tx);
        if (ret) {
            dev_err(i2s_tdm->dev, "Failed to enable mclk_tx: %d\n", ret);
            goto err_mclk_tx;
        }

        ret = clk_prepare_enable(i2s_tdm->mclk_rx);
        if (ret) {
            dev_err(i2s_tdm->dev, "Failed to enable mclk_rx: %d\n", ret);
            goto err_mclk_rx;
        }
        dev_info(i2s_tdm->dev, "Runtime resume: mclk_tx and mclk_rx enabled successfully\n");
    } else {
        dev_info(i2s_tdm->dev, "MCLK already running (quirk enabled)\n");
    }

    regcache_cache_only(i2s_tdm->regmap, false);
    regcache_mark_dirty(i2s_tdm->regmap);

    ret = regcache_sync(i2s_tdm->regmap);
    if (ret)
        goto err_regmap;

    return 0;

err_regmap:
    if (!(i2s_tdm->quirks & QUIRK_MCLK_ALWAYS_ON))
        clk_disable_unprepare(i2s_tdm->mclk_rx);
err_mclk_rx:
    if (!(i2s_tdm->quirks & QUIRK_MCLK_ALWAYS_ON))
        clk_disable_unprepare(i2s_tdm->mclk_tx);
err_mclk_tx:
    return ret;
}

static inline struct rk_i2s_tdm_dev *to_info(struct snd_soc_dai *dai)
{
    return snd_soc_dai_get_drvdata(dai);
}

static inline bool is_stream_active(struct rk_i2s_tdm_dev *i2s_tdm, int stream)
{
    unsigned int val;

    regmap_read(i2s_tdm->regmap, I2S_XFER, &val);

    if (stream == SNDRV_PCM_STREAM_PLAYBACK)
    return (val & I2S_XFER_TXS_START);
    else
    return (val & I2S_XFER_RXS_START);
}

#ifdef HAVE_SYNC_RESET
#if defined(CONFIG_ARM) && !defined(writeq)
static inline void __raw_writeq(u64 val, volatile void __iomem *addr)
{
    asm volatile("strd %0, %H0, [%1]" : : "r" (val), "r" (addr));
}
#define writeq(v,c) ({ __iowmb(); __raw_writeq((__force u64) cpu_to_le64(v), c); })
#endif

static void rockchip_i2s_tdm_reset_assert(struct rk_i2s_tdm_dev *i2s_tdm)
{
    int tx_bank, rx_bank, tx_offset, rx_offset, tx_id, rx_id;
    void __iomem *cru_reset, *addr;
    unsigned long flags;
    u64 val;

    if (!i2s_tdm->cru_base || !i2s_tdm->soc_data || !i2s_tdm->is_master_mode)
    return;

    tx_id = i2s_tdm->tx_reset_id;
    rx_id = i2s_tdm->rx_reset_id;
    if (tx_id < 0 || rx_id < 0)
    return;

    tx_bank = tx_id / 16;
    tx_offset = tx_id % 16;
    rx_bank = rx_id / 16;
    rx_offset = rx_id % 16;

    dev_dbg(i2s_tdm->dev,
    "tx_bank: %d, rx_bank: %d,tx_offset: %d, rx_offset: %d\n",
    tx_bank, rx_bank, tx_offset, rx_offset);

    cru_reset = i2s_tdm->cru_base + i2s_tdm->soc_data->softrst_offset;
    switch (abs(tx_bank - rx_bank)) {
    case 0:
    writel(BIT(tx_offset) | BIT(rx_offset) |
           (BIT(tx_offset) << 16) | (BIT(rx_offset) << 16),
           cru_reset + (tx_bank * 4));
    break;
    case 1:
    if (tx_bank < rx_bank) {
        val = BIT(rx_offset) | (BIT(rx_offset) << 16);
        val <<= 32;
        val |= BIT(tx_offset) | (BIT(tx_offset) << 16);
        addr = cru_reset + (tx_bank * 4);
    } else {
        val = BIT(tx_offset) | (BIT(tx_offset) << 16);
        val <<= 32;
        val |= BIT(rx_offset) | (BIT(rx_offset) << 16);
        addr = cru_reset + (rx_bank * 4);
    }
    if (IS_ALIGNED((uintptr_t)addr, 8)) {
        writeq(val, addr);
        break;
    }
    fallthrough;
    default:
    local_irq_save(flags);
    writel(BIT(tx_offset) | (BIT(tx_offset) << 16),
           cru_reset + (tx_bank * 4));
    writel(BIT(rx_offset) | (BIT(rx_offset) << 16),
           cru_reset + (rx_bank * 4));
    local_irq_restore(flags);
    break;
    }

    /* delay for reset assert done */
    udelay(10);
}

static void rockchip_i2s_tdm_reset_deassert(struct rk_i2s_tdm_dev *i2s_tdm)
{
    int tx_bank, rx_bank, tx_offset, rx_offset, tx_id, rx_id;
    void __iomem *cru_reset, *addr;
    unsigned long flags;
    u64 val;

    if (!i2s_tdm->cru_base || !i2s_tdm->soc_data || !i2s_tdm->is_master_mode)
    return;

    tx_id = i2s_tdm->tx_reset_id;
    rx_id = i2s_tdm->rx_reset_id;
    if (tx_id < 0 || rx_id < 0)
    return;

    tx_bank = tx_id / 16;
    tx_offset = tx_id % 16;
    rx_bank = rx_id / 16;
    rx_offset = rx_id % 16;

    dev_dbg(i2s_tdm->dev,
    "tx_bank: %d, rx_bank: %d,tx_offset: %d, rx_offset: %d\n",
    tx_bank, rx_bank, tx_offset, rx_offset);

    cru_reset = i2s_tdm->cru_base + i2s_tdm->soc_data->softrst_offset;
    switch (abs(tx_bank - rx_bank)) {
    case 0:
    writel((BIT(tx_offset) << 16) | (BIT(rx_offset) << 16),
           cru_reset + (tx_bank * 4));
    break;
    case 1:
    if (tx_bank < rx_bank) {
        val = (BIT(rx_offset) << 16);
        val <<= 32;
        val |= (BIT(tx_offset) << 16);
        addr = cru_reset + (tx_bank * 4);
    } else {
        val = (BIT(tx_offset) << 16);
        val <<= 32;
        val |= (BIT(rx_offset) << 16);
        addr = cru_reset + (rx_bank * 4);
    }
    if (IS_ALIGNED((uintptr_t)addr, 8)) {
        writeq(val, addr);
        break;
    }
    fallthrough;
    default:
    local_irq_save(flags);
    writel((BIT(tx_offset) << 16),
           cru_reset + (tx_bank * 4));
    writel((BIT(rx_offset) << 16),
           cru_reset + (rx_bank * 4));
    local_irq_restore(flags);
    break;
    }

    /* delay for reset deassert done */
    udelay(10);
}

/*
 * make sure both tx and rx are reset at the same time for sync lrck
 * when clk_trcm > 0
 */
static void rockchip_i2s_tdm_sync_reset(struct rk_i2s_tdm_dev *i2s_tdm)
{
    rockchip_i2s_tdm_reset_assert(i2s_tdm);
    rockchip_i2s_tdm_reset_deassert(i2s_tdm);
}
#else
static inline void rockchip_i2s_tdm_reset_assert(struct rk_i2s_tdm_dev *i2s_tdm)
{
}

static inline void rockchip_i2s_tdm_reset_deassert(struct rk_i2s_tdm_dev *i2s_tdm)
{
}

static inline void rockchip_i2s_tdm_sync_reset(struct rk_i2s_tdm_dev *i2s_tdm)
{
}
#endif

static void rockchip_i2s_tdm_reset(struct reset_control *rc)
{
    if (IS_ERR_OR_NULL(rc))
    return;

    reset_control_assert(rc);
    /* delay for reset assert done */
    udelay(10);

    reset_control_deassert(rc);
    /* delay for reset deassert done */
    udelay(10);
}

static int rockchip_i2s_tdm_clear(struct rk_i2s_tdm_dev *i2s_tdm,
      unsigned int clr)
{
    struct reset_control *rst = NULL;
    unsigned int val = 0;
    int ret = 0;

    if (!i2s_tdm->is_master_mode)
    goto reset;

    switch (clr) {
    case I2S_CLR_TXC:
    rst = i2s_tdm->tx_reset;
    break;
    case I2S_CLR_RXC:
    rst = i2s_tdm->rx_reset;
    break;
    case I2S_CLR_TXC | I2S_CLR_RXC:
    break;
    default:
    return -EINVAL;
    }

    regmap_update_bits(i2s_tdm->regmap, I2S_CLR, clr, clr);

    ret = regmap_read_poll_timeout_atomic(i2s_tdm->regmap, I2S_CLR, val,
              !(val & clr), 10, 100);
    if (ret < 0) {
    dev_warn(i2s_tdm->dev, "failed to clear %u\n", clr);
    goto reset;
    }

    return 0;

reset:
    if (i2s_tdm->clk_trcm)
    rockchip_i2s_tdm_sync_reset(i2s_tdm);
    else
    rockchip_i2s_tdm_reset(rst);

    return 0;
}

/*
 * HDMI controller ignores the first FRAME_SYNC cycle, Lost one frame is no big deal
 * for LPCM, but it does matter for Bitstream (NLPCM/HBR), So, padding one frame
 * before xfer the real data to fix it.
 */
static void rockchip_i2s_tdm_tx_fifo_padding(struct rk_i2s_tdm_dev *i2s_tdm, bool en)
{
    unsigned int val, w, c, i;

    if (!en)
    return;

    regmap_read(i2s_tdm->regmap, I2S_TXCR, &val);
    w = ((val & I2S_TXCR_VDW_MASK) >> I2S_TXCR_VDW_SHIFT) + 1;
    c = to_ch_num(val & I2S_TXCR_CSR_MASK) * w / 32;

    for (i = 0; i < c; i++)
    regmap_write(i2s_tdm->regmap, I2S_TXDR, 0x0);
}

static void rockchip_i2s_tdm_fifo_xrun_detect(struct rk_i2s_tdm_dev *i2s_tdm,
              int stream, bool en)
{
    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
    /* clear irq status which was asserted before TXUIE enabled */
    regmap_update_bits(i2s_tdm->regmap, I2S_INTCR,
       I2S_INTCR_TXUIC, I2S_INTCR_TXUIC);
    regmap_update_bits(i2s_tdm->regmap, I2S_INTCR,
       I2S_INTCR_TXUIE_MASK,
       I2S_INTCR_TXUIE(en));
    } else {
    /* clear irq status which was asserted before RXOIE enabled */
    regmap_update_bits(i2s_tdm->regmap, I2S_INTCR,
       I2S_INTCR_RXOIC, I2S_INTCR_RXOIC);
    /* Disable RX overrun interrupts for external clock mode to reduce CPU load
     * RX is used only for clock sync, overruns are expected at high sample rates */
    regmap_update_bits(i2s_tdm->regmap, I2S_INTCR,
       I2S_INTCR_RXOIE_MASK,
       I2S_INTCR_RXOIE(0)); /* Force disable RX overrun interrupts */
    }
}

static void rockchip_i2s_tdm_dma_ctrl(struct rk_i2s_tdm_dev *i2s_tdm,
          int stream, bool en)
{
    if (!en)
    rockchip_i2s_tdm_fifo_xrun_detect(i2s_tdm, stream, 0);

    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
    if (i2s_tdm->quirks & QUIRK_HDMI_PATH)
        rockchip_i2s_tdm_tx_fifo_padding(i2s_tdm, en);

    regmap_update_bits(i2s_tdm->regmap, I2S_DMACR,
       I2S_DMACR_TDE_MASK,
       I2S_DMACR_TDE(en));
    /*
     * Explicitly delay 1 usec for dma to fill FIFO,
     * though there was a implied HW delay that around
     * half LRCK cycle (e.g. 2.6us@192k) from XFER-start
     * to FIFO-pop.
     *
     * 1 usec is enough to fill at lease 4 entry each FIFO
     * @192k 8ch 32bit situation.
     */
    udelay(1);
    } else {
    regmap_update_bits(i2s_tdm->regmap, I2S_DMACR,
       I2S_DMACR_RDE_MASK,
       I2S_DMACR_RDE(en));
    }

    if (en)
    rockchip_i2s_tdm_fifo_xrun_detect(i2s_tdm, stream, 1);
}

static void rockchip_i2s_tdm_xfer_start(struct rk_i2s_tdm_dev *i2s_tdm,
        int stream)
{
    if (i2s_tdm->clk_trcm) {
    rockchip_i2s_tdm_reset_assert(i2s_tdm);
    regmap_update_bits(i2s_tdm->regmap, I2S_XFER,
       I2S_XFER_TXS_MASK |
       I2S_XFER_RXS_MASK,
       I2S_XFER_TXS_START |
       I2S_XFER_RXS_START);
    rockchip_i2s_tdm_reset_deassert(i2s_tdm);
    } else if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
    regmap_update_bits(i2s_tdm->regmap, I2S_XFER,
       I2S_XFER_TXS_MASK,
       I2S_XFER_TXS_START);
    } else {
    regmap_update_bits(i2s_tdm->regmap, I2S_XFER,
       I2S_XFER_RXS_MASK,
       I2S_XFER_RXS_START);
    }
}

static void rockchip_i2s_tdm_xfer_stop(struct rk_i2s_tdm_dev *i2s_tdm,
           int stream, bool force)
{
    unsigned int msk, val, clr;

    if (i2s_tdm->quirks & QUIRK_ALWAYS_ON && !force)
    return;

    if (i2s_tdm->clk_trcm) {
    msk = I2S_XFER_TXS_MASK | I2S_XFER_RXS_MASK;
    val = I2S_XFER_TXS_STOP | I2S_XFER_RXS_STOP;
    clr = I2S_CLR_TXC | I2S_CLR_RXC;
    } else if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
    msk = I2S_XFER_TXS_MASK;
    val = I2S_XFER_TXS_STOP;
    clr = I2S_CLR_TXC;
    } else {
    msk = I2S_XFER_RXS_MASK;
    val = I2S_XFER_RXS_STOP;
    clr = I2S_CLR_RXC;
    }

    regmap_update_bits(i2s_tdm->regmap, I2S_XFER, msk, val);

    /* delay for LRCK signal integrity */
    udelay(150);

    rockchip_i2s_tdm_clear(i2s_tdm, clr);
}

static void rockchip_i2s_tdm_xfer_trcm_start(struct rk_i2s_tdm_dev *i2s_tdm)
{
    unsigned long flags;

    spin_lock_irqsave(&i2s_tdm->lock, flags);
    if (atomic_inc_return(&i2s_tdm->refcount) == 1)
    rockchip_i2s_tdm_xfer_start(i2s_tdm, 0);
    spin_unlock_irqrestore(&i2s_tdm->lock, flags);
}

static void rockchip_i2s_tdm_xfer_trcm_stop(struct rk_i2s_tdm_dev *i2s_tdm)
{
    unsigned long flags;

    spin_lock_irqsave(&i2s_tdm->lock, flags);
    if (atomic_dec_and_test(&i2s_tdm->refcount))
    rockchip_i2s_tdm_xfer_stop(i2s_tdm, 0, false);
    spin_unlock_irqrestore(&i2s_tdm->lock, flags);
}

static void rockchip_i2s_tdm_trcm_pause(struct snd_pcm_substream *substream,
        struct rk_i2s_tdm_dev *i2s_tdm)
{
    int stream = substream->stream;
    int bstream = SNDRV_PCM_STREAM_LAST - stream;

    /* disable dma for both tx and rx */
    rockchip_i2s_tdm_dma_ctrl(i2s_tdm, stream, 0);
    rockchip_i2s_tdm_dma_ctrl(i2s_tdm, bstream, 0);
    rockchip_i2s_tdm_xfer_stop(i2s_tdm, bstream, true);
}

static void rockchip_i2s_tdm_trcm_resume(struct snd_pcm_substream *substream,
         struct rk_i2s_tdm_dev *i2s_tdm)
{
    int bstream = SNDRV_PCM_STREAM_LAST - substream->stream;

    /*
     * just resume bstream, because current stream will be
     * startup in the trigger-cmd-START
     */
    rockchip_i2s_tdm_dma_ctrl(i2s_tdm, bstream, 1);
    rockchip_i2s_tdm_xfer_start(i2s_tdm, bstream);
}

/* Additional function to check pause state */
static bool rockchip_i2s_tdm_is_paused(struct rk_i2s_tdm_dev *i2s_tdm, int stream)
{
    if (stream == SNDRV_PCM_STREAM_PLAYBACK)
    return i2s_tdm->playback_paused;
    else
    return i2s_tdm->capture_paused;
}

static void rockchip_i2s_tdm_start(struct rk_i2s_tdm_dev *i2s_tdm, int stream)
{
    /* Check if stream is in pause state */
    if (rockchip_i2s_tdm_is_paused(i2s_tdm, stream)) {
    dev_dbg(i2s_tdm->dev, "Stream is paused, not starting\n");
    return;
    }

    /* Note: Mute is now handled in trigger with proper delayed start timing */

    /* Always start DMA and transmission */
    rockchip_i2s_tdm_dma_ctrl(i2s_tdm, stream, 1);
    
    if (i2s_tdm->clk_trcm)
    rockchip_i2s_tdm_xfer_trcm_start(i2s_tdm);
    else
    rockchip_i2s_tdm_xfer_start(i2s_tdm, stream);
    
    /* Check mute */
    if (stream == SNDRV_PCM_STREAM_PLAYBACK && i2s_tdm->mute) {
    dev_dbg(i2s_tdm->dev, "ROCKCHIP_I2S_TDM: Playback started with mute\n");
    /* DO NOT disable DMA when muted - let GPIO mute do its job */
    /* rockchip_i2s_tdm_dma_ctrl(i2s_tdm, stream, 0); */
    }
}

static void rockchip_i2s_tdm_stop(struct rk_i2s_tdm_dev *i2s_tdm, int stream)
{
    /* Mute is handled in trigger callback */
    
    /* First stop transmission (BCLK/DATA), then DMA */
    if (i2s_tdm->clk_trcm)
        rockchip_i2s_tdm_xfer_trcm_stop(i2s_tdm);
    else
        rockchip_i2s_tdm_xfer_stop(i2s_tdm, stream, false);
    
    /* Then stop DMA */
    rockchip_i2s_tdm_dma_ctrl(i2s_tdm, stream, 0);
    
    /* Only logging, no mute state change */
    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
    dev_dbg(i2s_tdm->dev, "ROCKCHIP_I2S_TDM: Playback stopped, mute state: %s\n", 
        i2s_tdm->mute ? "enabled" : "disabled");
    }
}

/* New functions for pause/resume handling */
static void rockchip_i2s_tdm_pause(struct rk_i2s_tdm_dev *i2s_tdm, int stream)
{
    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
    if (i2s_tdm->playback_paused)
        return;
    i2s_tdm->playback_paused = true;
    } else {
    if (i2s_tdm->capture_paused)
        return;
    i2s_tdm->capture_paused = true;
    }

    /* Disable DMA but preserve device state */
    rockchip_i2s_tdm_dma_ctrl(i2s_tdm, stream, 0);
    
    /* Use special function for TRCM mode */
    if (i2s_tdm->clk_trcm) {
    struct snd_pcm_substream *substream = i2s_tdm->substreams[stream];
    if (substream)
        rockchip_i2s_tdm_trcm_pause(substream, i2s_tdm);
    } else {
    /* For normal mode, pause transmission */
    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
        regmap_update_bits(i2s_tdm->regmap, I2S_XFER,
           I2S_XFER_TXS_MASK,
           I2S_XFER_TXS_STOP);
    } else {
        regmap_update_bits(i2s_tdm->regmap, I2S_XFER,
           I2S_XFER_RXS_MASK,
           I2S_XFER_RXS_STOP);
    }
    }

    dev_dbg(i2s_tdm->dev, "I2S/TDM %s stream paused\n",
    stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture");
}

static void rockchip_i2s_tdm_resume(struct rk_i2s_tdm_dev *i2s_tdm, int stream)
{
    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
    if (!i2s_tdm->playback_paused)
        return;
    i2s_tdm->playback_paused = false;
    } else {
    if (!i2s_tdm->capture_paused)
        return;
    i2s_tdm->capture_paused = false;
    }

    /* Use special function for TRCM mode */
    if (i2s_tdm->clk_trcm) {
    struct snd_pcm_substream *substream = i2s_tdm->substreams[stream];
    if (substream)
        rockchip_i2s_tdm_trcm_resume(substream, i2s_tdm);
    } else {
    /* Restore data transmission */
    if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
        regmap_update_bits(i2s_tdm->regmap, I2S_XFER,
           I2S_XFER_TXS_MASK,
           I2S_XFER_TXS_START);
    } else {
        regmap_update_bits(i2s_tdm->regmap, I2S_XFER,
           I2S_XFER_RXS_MASK,
           I2S_XFER_RXS_START);
    }
    }

    /* Enable DMA */
    rockchip_i2s_tdm_dma_ctrl(i2s_tdm, stream, 1);

    dev_dbg(i2s_tdm->dev, "I2S/TDM %s stream resumed\n",
    stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture");
}

static int rockchip_i2s_tdm_set_fmt(struct snd_soc_dai *cpu_dai,
        unsigned int fmt)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(cpu_dai);
    unsigned int mask = 0, val = 0, tdm_val = 0;
    int ret = 0;
    bool is_tdm = i2s_tdm->tdm_mode;

    pm_runtime_get_sync(cpu_dai->dev);
    
    /* Save format for forced changes application */
    i2s_tdm->format = fmt;

    mask = I2S_CKR_MSS_MASK;
    dev_info(cpu_dai->dev, "set_fmt called: fmt=0x%x, master_mask=0x%x\n", 
             fmt, fmt & SND_SOC_DAIFMT_MASTER_MASK);
    
    switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
    case SND_SOC_DAIFMT_CBS_CFS:
    val = I2S_CKR_MSS_MASTER;
    i2s_tdm->is_master_mode = true;
    dev_info(cpu_dai->dev, "Setting MASTER mode (CBS_CFS)\n");
    break;
    case SND_SOC_DAIFMT_CBM_CFM:
    /* Force master mode if mclk_calibrate or mclk_external is enabled (kernel 6.1 fix) */
    if (i2s_tdm->mclk_calibrate || i2s_tdm->mclk_external) {
        val = I2S_CKR_MSS_MASTER;
        i2s_tdm->is_master_mode = true;
        if (i2s_tdm->mclk_calibrate)
            dev_info(cpu_dai->dev, "Forcing MASTER mode for mclk_calibrate (was CBM_CFM)\n");
        else
            dev_info(cpu_dai->dev, "Forcing MASTER mode for mclk_external (was CBM_CFM)\n");
    } else {
        val = I2S_CKR_MSS_SLAVE;
        i2s_tdm->is_master_mode = false;
        dev_info(cpu_dai->dev, "Setting SLAVE mode (CBM_CFM)\n");
    }
    break;
    default:
    dev_err(cpu_dai->dev, "Unknown master mode: 0x%x\n", fmt & SND_SOC_DAIFMT_MASTER_MASK);
    ret = -EINVAL;
    goto err_pm_put;
    }

    regmap_update_bits(i2s_tdm->regmap, I2S_CKR, mask, val);
    dev_info(cpu_dai->dev, "Applied MSS to CKR: val=0x%x\n", val);

    mask = I2S_CKR_CKP_MASK | I2S_CKR_TLP_MASK | I2S_CKR_RLP_MASK;
    switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
    case SND_SOC_DAIFMT_NB_NF:
    val = I2S_CKR_CKP_NORMAL |
          I2S_CKR_TLP_NORMAL |
          I2S_CKR_RLP_NORMAL;
    break;
    case SND_SOC_DAIFMT_NB_IF:
    val = I2S_CKR_CKP_NORMAL |
          I2S_CKR_TLP_INVERTED |
          I2S_CKR_RLP_INVERTED;
    break;
    case SND_SOC_DAIFMT_IB_NF:
    val = I2S_CKR_CKP_INVERTED |
          I2S_CKR_TLP_NORMAL |
          I2S_CKR_RLP_NORMAL;
    break;
    case SND_SOC_DAIFMT_IB_IF:
    val = I2S_CKR_CKP_INVERTED |
          I2S_CKR_TLP_INVERTED |
          I2S_CKR_RLP_INVERTED;
    break;
    default:
    ret = -EINVAL;
    goto err_pm_put;
    }

    /* Apply PCM channel swap if enabled and not in DSD mode */
    if (i2s_tdm->pcm_channel_swap && !i2s_tdm->dsd_mode_active) {
        /* Invert LRCK polarity for channel switching */
        bool was_inverted = (val & I2S_CKR_TLP_INVERTED) != 0;
        val &= ~(I2S_CKR_TLP_MASK | I2S_CKR_RLP_MASK);
        if (was_inverted) {
            val |= I2S_CKR_TLP_NORMAL | I2S_CKR_RLP_NORMAL;
        } else {
            val |= I2S_CKR_TLP_INVERTED | I2S_CKR_RLP_INVERTED;
        }
    }

    regmap_update_bits(i2s_tdm->regmap, I2S_CKR, mask, val);


    mask = I2S_TXCR_IBM_MASK | I2S_TXCR_TFS_MASK | I2S_TXCR_PBM_MASK;
    switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
    case SND_SOC_DAIFMT_RIGHT_J:
    val = I2S_TXCR_IBM_RSJM;
    break;
    case SND_SOC_DAIFMT_LEFT_J:
    val = I2S_TXCR_IBM_LSJM;
    break;
    case SND_SOC_DAIFMT_I2S:
    val = I2S_TXCR_IBM_NORMAL;
    break;
    case SND_SOC_DAIFMT_DSP_A: /* PCM delay 1 mode */
    val = I2S_TXCR_TFS_PCM | I2S_TXCR_PBM_MODE(1);
    break;
    case SND_SOC_DAIFMT_DSP_B: /* PCM no delay mode */
    val = I2S_TXCR_TFS_PCM;
    break;
    default:
    ret = -EINVAL;
    goto err_pm_put;
    }

    regmap_update_bits(i2s_tdm->regmap, I2S_TXCR, mask, val);

    mask = I2S_RXCR_IBM_MASK | I2S_RXCR_TFS_MASK | I2S_RXCR_PBM_MASK;
    switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
    case SND_SOC_DAIFMT_RIGHT_J:
    val = I2S_RXCR_IBM_RSJM;
    break;
    case SND_SOC_DAIFMT_LEFT_J:
    val = I2S_RXCR_IBM_LSJM;
    break;
    case SND_SOC_DAIFMT_I2S:
    val = I2S_RXCR_IBM_NORMAL;
    break;
    case SND_SOC_DAIFMT_DSP_A: /* PCM delay 1 mode */
    val = I2S_RXCR_TFS_PCM | I2S_RXCR_PBM_MODE(1);
    break;
    case SND_SOC_DAIFMT_DSP_B: /* PCM no delay mode */
    val = I2S_RXCR_TFS_PCM;
    break;
    default:
    ret = -EINVAL;
    goto err_pm_put;
    }

    regmap_update_bits(i2s_tdm->regmap, I2S_RXCR, mask, val);

    if (is_tdm) {
    switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
    case SND_SOC_DAIFMT_RIGHT_J:
        val = I2S_TXCR_TFS_TDM_I2S;
        tdm_val = TDM_SHIFT_CTRL(2);
        break;
    case SND_SOC_DAIFMT_LEFT_J:
        val = I2S_TXCR_TFS_TDM_I2S;
        tdm_val = TDM_SHIFT_CTRL(1);
        break;
    case SND_SOC_DAIFMT_I2S:
        val = I2S_TXCR_TFS_TDM_I2S;
        tdm_val = TDM_SHIFT_CTRL(0);
        break;
    case SND_SOC_DAIFMT_DSP_A:
        val = I2S_TXCR_TFS_TDM_PCM;
        tdm_val = TDM_SHIFT_CTRL(2);
        break;
    case SND_SOC_DAIFMT_DSP_B:
        val = I2S_TXCR_TFS_TDM_PCM;
        tdm_val = TDM_SHIFT_CTRL(4);
        break;
    default:
        ret = -EINVAL;
        goto err_pm_put;
    }

    tdm_val |= TDM_FSYNC_WIDTH_SEL1(1);
    if (i2s_tdm->tdm_fsync_half_frame)
        tdm_val |= TDM_FSYNC_WIDTH_HALF_FRAME;
    else
        tdm_val |= TDM_FSYNC_WIDTH_ONE_FRAME;

    mask = I2S_TXCR_TFS_MASK;
    regmap_update_bits(i2s_tdm->regmap, I2S_TXCR, mask, val);
    regmap_update_bits(i2s_tdm->regmap, I2S_RXCR, mask, val);

    mask = TDM_FSYNC_WIDTH_SEL1_MSK | TDM_FSYNC_WIDTH_SEL0_MSK |
           TDM_SHIFT_CTRL_MSK;
    regmap_update_bits(i2s_tdm->regmap, I2S_TDM_TXCR,
       mask, tdm_val);
    regmap_update_bits(i2s_tdm->regmap, I2S_TDM_RXCR,
       mask, tdm_val);

    if (val == I2S_TXCR_TFS_TDM_I2S && !i2s_tdm->tdm_fsync_half_frame) {
        /* refine frame width for TDM_I2S_ONE_FRAME */
        mask = TDM_FRAME_WIDTH_MSK;
        tdm_val = TDM_FRAME_WIDTH(i2s_tdm->bclk_fs >> 1);
        regmap_update_bits(i2s_tdm->regmap, I2S_TDM_TXCR,
           mask, tdm_val);
        regmap_update_bits(i2s_tdm->regmap, I2S_TDM_RXCR,
           mask, tdm_val);
    }
    }

err_pm_put:
    pm_runtime_put(cpu_dai->dev);

    return ret;
}

static int rockchip_i2s_tdm_clk_set_rate(struct rk_i2s_tdm_dev *i2s_tdm,
         struct clk *clk, unsigned long rate,
         int ppm)
{
    unsigned long rate_target;
    int delta, ret;

    if (ppm == i2s_tdm->clk_ppm)
    return 0;

    ret = rockchip_pll_clk_compensation(clk, ppm);
    if (ret != -ENOSYS)
    goto out;

    delta = (ppm < 0) ? -1 : 1;
    delta *= (int)div64_u64((uint64_t)rate * (uint64_t)abs(ppm) + 500000, 1000000);

    rate_target = rate + delta;
    if (!rate_target)
    return -EINVAL;

    ret = clk_set_rate(clk, rate_target);
    if (ret)
    return ret;

out:
    if (!ret)
    i2s_tdm->clk_ppm = ppm;

    return ret;
}

static int rockchip_i2s_tdm_calibrate_mclk(struct rk_i2s_tdm_dev *i2s_tdm,
           struct snd_pcm_substream *substream,
           unsigned int lrck_freq)
{
    struct clk *mclk_root;
    struct clk *mclk_parent;
    unsigned int target_mclk, pll_freq, src_freq, ideal_pll;
    unsigned int div;
    int ret;

    if (i2s_tdm->mclk_external) {
        dev_info(i2s_tdm->dev, "MCLK calibrate: skipped (external clock mode)\n");
        return 0;
    }

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
    mclk_parent = i2s_tdm->mclk_tx_src;
    else
    mclk_parent = i2s_tdm->mclk_rx_src;

    mclk_root = clk_get_parent(mclk_parent);
    if (!mclk_root) {
        dev_err(i2s_tdm->dev, "Failed to get parent clock\n");
        ret = -EINVAL;
        goto out;
    }
    
    target_mclk = i2s_tdm->mclk_multiplier * lrck_freq;
    
    mclk_root = clk_get_parent(mclk_parent);
    pll_freq = clk_get_rate(mclk_root);
    
    if (lrck_freq % 44100 == 0) {
        ideal_pll = 993484800;
        src_freq = 45158400;
    } else if (lrck_freq % 48000 == 0) {
        ideal_pll = 983040000;
        src_freq = 49152000;
    } else {
        ideal_pll = 983040000;
        src_freq = 49152000;
    }
    
    dev_info(i2s_tdm->dev, "Current PLL: %u Hz, target: %u Hz (for %u Hz family, target SRC=%u Hz)\n",
             pll_freq, ideal_pll, lrck_freq, src_freq);
    
    if (pll_freq != ideal_pll) {
        ret = clk_set_rate(mclk_root, ideal_pll);
        if (ret == 0) {
            pll_freq = clk_get_rate(mclk_root);
            dev_info(i2s_tdm->dev, "PLL changed to: %u Hz\n", pll_freq);
        }
    }
    
    ret = clk_set_rate(mclk_parent, src_freq);
    if (ret) {
        dev_err(i2s_tdm->dev, "Failed to set SRC to %u Hz: %d\n", src_freq, ret);
        goto out;
    }
    
    src_freq = clk_get_rate(mclk_parent);
    div = pll_freq / src_freq;

    dev_info(i2s_tdm->dev, "Clock config: PLL=%u Hz ÷%u → SRC=%u Hz (%s family, %ux multiplier)\n",
         pll_freq, div, src_freq, (lrck_freq % 44100 == 0) ? "44.1k" : "48k", i2s_tdm->mclk_multiplier);

out:
    return ret;
}

static int rockchip_i2s_tdm_set_mclk(struct rk_i2s_tdm_dev *i2s_tdm,
         struct snd_pcm_substream *substream,
         struct clk **mclk)
{
    unsigned int mclk_freq;
    int ret;

    if (i2s_tdm->clk_trcm) {
    if (i2s_tdm->mclk_tx_freq != i2s_tdm->mclk_rx_freq) {
        dev_err(i2s_tdm->dev,
    "clk_trcm, tx: %d and rx: %d should be same\n",
    i2s_tdm->mclk_tx_freq,
    i2s_tdm->mclk_rx_freq);
        ret = -EINVAL;
        goto err;
    }

    /* Skip clk_set_rate when mclk_calibrate or mclk_external is enabled */
    if (!i2s_tdm->mclk_calibrate && !i2s_tdm->mclk_external) {
        ret = clk_set_rate(i2s_tdm->mclk_tx, i2s_tdm->mclk_tx_freq);
        if (ret)
            goto err;

        ret = clk_set_rate(i2s_tdm->mclk_rx, i2s_tdm->mclk_rx_freq);
        if (ret)
            goto err;
    } else {
        if (i2s_tdm->mclk_calibrate)
            dev_info(i2s_tdm->dev, "Skipping clk_set_rate (mclk_calibrate active, TX_SRC=%lu Hz)\n",
                     clk_get_rate(i2s_tdm->mclk_tx_src));
        else
            dev_info(i2s_tdm->dev, "Skipping clk_set_rate (mclk_external active, MCLK=%lu Hz)\n",
                     clk_get_rate(i2s_tdm->mclk_tx));
    }

    /* mclk_rx is also ok. */
    *mclk = i2s_tdm->mclk_tx;
    } else {
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        *mclk = i2s_tdm->mclk_tx;
        mclk_freq = i2s_tdm->mclk_tx_freq;
    } else {
        *mclk = i2s_tdm->mclk_rx;
        mclk_freq = i2s_tdm->mclk_rx_freq;
    }

    ret = clk_set_rate(*mclk, mclk_freq);
    if (ret)
        goto err;
    }

    return 0;

err:
    return ret;
}

static int rockchip_i2s_io_multiplex(struct snd_pcm_substream *substream,
         struct snd_soc_dai *dai)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(dai);
    int usable_chs = MULTIPLEX_CH_MAX;
    unsigned int val = 0;

    if (!i2s_tdm->io_multiplex)
    return 0;

    if (IS_ERR(i2s_tdm->grf))
    return 0;

    if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
    struct snd_pcm_str *playback_str =
        &substream->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK];

    if (playback_str->substream_opened) {
        regmap_read(i2s_tdm->regmap, I2S_TXCR, &val);
        val &= I2S_TXCR_CSR_MASK;
        usable_chs = MULTIPLEX_CH_MAX - to_ch_num(val);
    }

    regmap_read(i2s_tdm->regmap, I2S_RXCR, &val);
    val &= I2S_RXCR_CSR_MASK;

    if (to_ch_num(val) > usable_chs) {
        dev_err(i2s_tdm->dev,
    "Capture chs(%d) > usable chs(%d)\n",
    to_ch_num(val), usable_chs);
        return -EINVAL;
    }

    switch (val) {
    case I2S_CHN_4:
        val = I2S_IO_6CH_OUT_4CH_IN;
        break;
    case I2S_CHN_6:
        val = I2S_IO_4CH_OUT_6CH_IN;
        break;
    case I2S_CHN_8:
        val = I2S_IO_2CH_OUT_8CH_IN;
        break;
    default:
        val = I2S_IO_8CH_OUT_2CH_IN;
        break;
    }
    } else {
    struct snd_pcm_str *capture_str =
        &substream->pcm->streams[SNDRV_PCM_STREAM_CAPTURE];

    if (capture_str->substream_opened) {
        regmap_read(i2s_tdm->regmap, I2S_RXCR, &val);
        val &= I2S_RXCR_CSR_MASK;
        usable_chs = MULTIPLEX_CH_MAX - to_ch_num(val);
    }

    regmap_read(i2s_tdm->regmap, I2S_TXCR, &val);
    val &= I2S_TXCR_CSR_MASK;

    if (to_ch_num(val) > usable_chs) {
        dev_err(i2s_tdm->dev,
    "Playback chs(%d) > usable chs(%d)\n",
    to_ch_num(val), usable_chs);
        return -EINVAL;
    }

    switch (val) {
    case I2S_CHN_4:
        val = I2S_IO_4CH_OUT_6CH_IN;
        break;
    case I2S_CHN_6:
        val = I2S_IO_6CH_OUT_4CH_IN;
        break;
    case I2S_CHN_8:
        val = I2S_IO_8CH_OUT_2CH_IN;
        break;
    default:
        val = I2S_IO_2CH_OUT_8CH_IN;
        break;
    }
    }

    val <<= i2s_tdm->soc_data->grf_shift;
    val |= (I2S_IO_DIRECTION_MASK << i2s_tdm->soc_data->grf_shift) << 16;
    regmap_write(i2s_tdm->grf, i2s_tdm->soc_data->grf_reg_offset, val);

    return 0;
}

static bool is_params_dirty(struct snd_pcm_substream *substream,
            struct snd_soc_dai *dai,
            unsigned int div_bclk,
            unsigned int div_lrck,
            unsigned int fmt)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(dai);
    unsigned int last_div_bclk, last_div_lrck, last_fmt, val;

    regmap_read(i2s_tdm->regmap, I2S_CLKDIV, &val);
    last_div_bclk = ((val & I2S_CLKDIV_TXM_MASK) >> I2S_CLKDIV_TXM_SHIFT) + 1;
    if (last_div_bclk != div_bclk)
    return true;

    regmap_read(i2s_tdm->regmap, I2S_CKR, &val);
    last_div_lrck = ((val & I2S_CKR_TSD_MASK) >> I2S_CKR_TSD_SHIFT) + 1;
    if (last_div_lrck != div_lrck)
    return true;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
    regmap_read(i2s_tdm->regmap, I2S_TXCR, &val);
    last_fmt = val & (I2S_TXCR_VDW_MASK | I2S_TXCR_CSR_MASK);
    } else {
    regmap_read(i2s_tdm->regmap, I2S_RXCR, &val);
    last_fmt = val & (I2S_RXCR_VDW_MASK | I2S_RXCR_CSR_MASK);
    }

    if (last_fmt != fmt)
    return true;

    return false;
}

static int rockchip_i2s_tdm_params_trcm(struct snd_pcm_substream *substream,
        struct snd_soc_dai *dai,
        unsigned int div_bclk,
        unsigned int div_lrck,
        unsigned int fmt)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(dai);
    unsigned long flags;

    spin_lock_irqsave(&i2s_tdm->lock, flags);
    if (atomic_read(&i2s_tdm->refcount))
    rockchip_i2s_tdm_trcm_pause(substream, i2s_tdm);

    regmap_update_bits(i2s_tdm->regmap, I2S_CLKDIV,
           I2S_CLKDIV_TXM_MASK | I2S_CLKDIV_RXM_MASK,
           I2S_CLKDIV_TXM(div_bclk) | I2S_CLKDIV_RXM(div_bclk));
    regmap_update_bits(i2s_tdm->regmap, I2S_CKR,
           I2S_CKR_TSD_MASK | I2S_CKR_RSD_MASK,
           I2S_CKR_TSD(div_lrck) | I2S_CKR_RSD(div_lrck));

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
    regmap_update_bits(i2s_tdm->regmap, I2S_TXCR,
       I2S_TXCR_VDW_MASK | I2S_TXCR_CSR_MASK,
       fmt);
    else
    regmap_update_bits(i2s_tdm->regmap, I2S_RXCR,
       I2S_RXCR_VDW_MASK | I2S_RXCR_CSR_MASK,
       fmt);

    if (atomic_read(&i2s_tdm->refcount))
    rockchip_i2s_tdm_trcm_resume(substream, i2s_tdm);
    spin_unlock_irqrestore(&i2s_tdm->lock, flags);

    return 0;
}

static int rockchip_i2s_tdm_params(struct snd_pcm_substream *substream,
       struct snd_soc_dai *dai,
       unsigned int div_bclk,
       unsigned int div_lrck,
       unsigned int fmt)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(dai);
    int stream = substream->stream;

    if (is_stream_active(i2s_tdm, stream))
    rockchip_i2s_tdm_xfer_stop(i2s_tdm, stream, true);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
    regmap_update_bits(i2s_tdm->regmap, I2S_CLKDIV,
       I2S_CLKDIV_TXM_MASK,
       I2S_CLKDIV_TXM(div_bclk));
    regmap_update_bits(i2s_tdm->regmap, I2S_CKR,
       I2S_CKR_TSD_MASK,
       I2S_CKR_TSD(div_lrck));
    regmap_update_bits(i2s_tdm->regmap, I2S_TXCR,
       I2S_TXCR_VDW_MASK | I2S_TXCR_CSR_MASK,
       fmt);
    } else {
    regmap_update_bits(i2s_tdm->regmap, I2S_CLKDIV,
       I2S_CLKDIV_RXM_MASK,
       I2S_CLKDIV_RXM(div_bclk));
    regmap_update_bits(i2s_tdm->regmap, I2S_CKR,
       I2S_CKR_RSD_MASK,
       I2S_CKR_RSD(div_lrck));
    regmap_update_bits(i2s_tdm->regmap, I2S_RXCR,
       I2S_RXCR_VDW_MASK | I2S_RXCR_CSR_MASK,
       fmt);
    }

    /*
     * Bring back CLK ASAP after cfg changed to make SINK devices active
     * on HDMI-PATH-ALWAYS-ON situation, this workaround for some TVs no
     * sound issue. at the moment, it's 8K@60Hz display situation.
     */
    if ((i2s_tdm->quirks & QUIRK_HDMI_PATH) &&
        (i2s_tdm->quirks & QUIRK_ALWAYS_ON) &&
        (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)) {
    rockchip_i2s_tdm_xfer_start(i2s_tdm, SNDRV_PCM_STREAM_PLAYBACK);
    }

    return 0;
}

static int rockchip_i2s_tdm_params_channels(struct snd_pcm_substream *substream,
            struct snd_pcm_hw_params *params,
            struct snd_soc_dai *dai)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(dai);
    unsigned int reg_fmt, fmt;
    int ret = 0;
    snd_pcm_format_t format = params_format(params);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
    reg_fmt = I2S_TXCR;
    else
    reg_fmt = I2S_RXCR;
    /* Special handling for DSD formats - force 4-channel configuration for stereo DSD */
    if (is_dsd(format)) {
        dev_info(i2s_tdm->dev, "DSD channel config: FORCING 4-channel setup for stereo DSD\n");
        /* Force 4-channel mode for DSD stereo */
        regmap_update_bits(i2s_tdm->regmap, reg_fmt, I2S_TXCR_CSR_MASK, I2S_CHN_4);
        return I2S_CHN_4; /* DSD requires 4 channels for stereo */
    }
    regmap_read(i2s_tdm->regmap, reg_fmt, &fmt);
    fmt &= I2S_TXCR_TFS_MASK;

    if (fmt == I2S_TXCR_TFS_TDM_I2S && !i2s_tdm->tdm_fsync_half_frame) {
    switch (params_channels(params)) {
    case 16:
        ret = I2S_CHN_8;
        break;
    case 12:
        ret = I2S_CHN_6;
        break;
    case 8:
        ret = I2S_CHN_4;
        break;
    case 4:
        ret = I2S_CHN_2;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    } else {
    switch (params_channels(params)) {
    case 8:
        ret = I2S_CHN_8;
        break;
    case 6:
        ret = I2S_CHN_6;
        break;
    case 4:
        ret = I2S_CHN_4;
        break;
    case 2:
        ret = I2S_CHN_2;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    }

    return ret;
}

static int rockchip_i2s_tdm_hw_params(struct snd_pcm_substream *substream,
          struct snd_pcm_hw_params *params,
          struct snd_soc_dai *dai)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(dai);
    struct snd_dmaengine_dai_dma_data *dma_data;
    struct clk *mclk;
    int ret = 0;
    unsigned int val = 0;
    unsigned int mclk_rate, bclk_rate, div_bclk = 4, div_lrck = 64;

    dma_data = snd_soc_dai_get_dma_data(dai, substream);
    dma_data->maxburst = MAXBURST_PER_FIFO * params_channels(params) / 2;


    /* Note: Mute is now handled in trigger for proper timing */
    

    if (i2s_tdm->is_master_mode) {
        if (i2s_tdm->mclk_calibrate)
            rockchip_i2s_tdm_calibrate_mclk(i2s_tdm, substream,
                            params_rate(params));
if( i2s_tdm->mclk_external ){
            mclk = i2s_tdm->mclk_tx;
            if( i2s_tdm->mclk_ext_mux ) {
                /* Consider MCLK multiplier for external PLL - match kernel 5.10 behavior */
                if( params_rate(params) % 44100 ) {
                    clk_set_parent( i2s_tdm->mclk_ext, i2s_tdm->clk_48);
                    /* 48kHz family: 24.576MHz (512x) or 49.152MHz (1024x) */
                    if (i2s_tdm->mclk_multiplier == 1024) {
                        clk_set_rate(i2s_tdm->mclk_tx, 49152000);
                    } else {
                        clk_set_rate(i2s_tdm->mclk_tx, 24576000);
                    }
                }
                else {
                    clk_set_parent( i2s_tdm->mclk_ext, i2s_tdm->clk_44);
                    /* 44.1kHz family: 22.579MHz (512x) or 45.158MHz (1024x) */
                    if (i2s_tdm->mclk_multiplier == 1024) {
                        clk_set_rate(i2s_tdm->mclk_tx, 45158400);
                    } else {
                        clk_set_rate(i2s_tdm->mclk_tx, 22579200);
                    }
                }
                dev_info(i2s_tdm->dev, "External PLL: MCLK set to %lu Hz (multiplier %dx)\n",
                         clk_get_rate(i2s_tdm->mclk_tx), i2s_tdm->mclk_multiplier);
            }
        }
        else {
            ret = rockchip_i2s_tdm_set_mclk(i2s_tdm, substream, &mclk);
            if (ret)
                goto err;
        }

        mclk_rate = clk_get_rate(mclk);
        
        /* Special handling for DSD formats */
        if (is_dsd(params_format(params))) {
            bclk_rate = calculate_dsd_bclk(params_format(params), params_rate(params));
            /* DSD always uses 22.579 MHz MCLK - force it if different */
            if (mclk_rate != 22579200) {
                dev_info(i2s_tdm->dev, "DSD: MCLK rate %u Hz, expected 22579200 Hz\n", mclk_rate);
            }
            dev_info(i2s_tdm->dev, "DSD mode: BCLK=%u Hz, MCLK=%u Hz\n", bclk_rate, mclk_rate);
        } else {
            bclk_rate = i2s_tdm->bclk_fs * params_rate(params);
        }
        
        if (!bclk_rate) {
            ret = -EINVAL;
            goto err;
        }

        div_bclk = DIV_ROUND_CLOSEST(mclk_rate, bclk_rate);
        div_lrck = bclk_rate / params_rate(params);
        dev_info(i2s_tdm->dev, "Clock dividers: mclk_rate=%u, bclk_rate=%u, div_bclk=%u, div_lrck=%u\n",
                 mclk_rate, bclk_rate, div_bclk, div_lrck);
    }

    /* Static 1MB buffers are set in rockchip_i2s_tdm_pcm_hardware structure */

    switch (params_format(params)) {
    case SNDRV_PCM_FORMAT_S8:
        val |= I2S_TXCR_VDW(8);
        /* Disable DSD-on signal for PCM formats */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, false);
        break;
    case SNDRV_PCM_FORMAT_S16_LE:
        val |= I2S_TXCR_VDW(16);
        /* Disable DSD-on signal for PCM formats */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, false);
        break;
    case SNDRV_PCM_FORMAT_S20_3LE:
        val |= I2S_TXCR_VDW(20);
        /* Disable DSD-on signal for PCM formats */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, false);
        break;
    case SNDRV_PCM_FORMAT_S24_LE:
        val |= I2S_TXCR_VDW(24);
        /* Disable DSD-on signal for PCM formats */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, false);
        break;
    case SNDRV_PCM_FORMAT_S32_LE:
    case SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE:
        val |= I2S_TXCR_VDW(32);
        /* Disable DSD-on signal for PCM formats */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, false);
        break;
    case SNDRV_PCM_FORMAT_DSD_U8:
        val |= I2S_TXCR_VDW(8); /* DSD_U8: return standard 8-bit container */

        /* FORCE disable mmap for DSD_U8 - force use of copy_user */
        substream->runtime->hw.info &= ~SNDRV_PCM_INFO_MMAP;
        substream->runtime->hw.info &= ~SNDRV_PCM_INFO_MMAP_VALID;
        dev_info(i2s_tdm->dev, "DSD U8: mmap DISABLED, copy_user FORCED\n");

        /* Activate DSD-on signal */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, true);
        break;
    case SNDRV_PCM_FORMAT_DSD_U16_LE:
        val |= I2S_TXCR_VDW(16);

        /* FORCE disable mmap for DSD - force use of copy_user */
        substream->runtime->hw.info &= ~SNDRV_PCM_INFO_MMAP;
        substream->runtime->hw.info &= ~SNDRV_PCM_INFO_MMAP_VALID;
        dev_info(i2s_tdm->dev, "DSD U16: mmap DISABLED, copy_user FORCED\n");

        /* Activate DSD-on signal */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, true);
        break;
    case SNDRV_PCM_FORMAT_DSD_U32_LE:
    case SNDRV_PCM_FORMAT_DSD_U32_BE:
        val |= I2S_TXCR_VDW(16); /* DSD: only 16 bits of data in 32-bit container */

        /* FORCE disable mmap for DSD - force use of copy_user */
        substream->runtime->hw.info &= ~SNDRV_PCM_INFO_MMAP;
        substream->runtime->hw.info &= ~SNDRV_PCM_INFO_MMAP_VALID;
        dev_info(i2s_tdm->dev, "DSD: mmap DISABLED, copy_user FORCED\n");

        /* Activate DSD-on signal */
        rockchip_i2s_tdm_handle_dsd_switch(i2s_tdm, true);
        break;
    default:
        ret = -EINVAL;
        goto err;
    }

    ret = rockchip_i2s_tdm_params_channels(substream, params, dai);
    if (ret < 0)
        goto err;

    val |= ret;

    /* Apply PCM channel swap if enabled and not in DSD mode */
    if (!i2s_tdm->dsd_mode_active) {
        unsigned int mask, ckr_val;

        mask = I2S_CKR_TLP_MASK | I2S_CKR_RLP_MASK;
        regmap_read(i2s_tdm->regmap, I2S_CKR, &ckr_val);

        ckr_val &= ~mask;
        if (i2s_tdm->pcm_channel_swap) {
            ckr_val |= I2S_CKR_TLP_INVERTED | I2S_CKR_RLP_INVERTED;
        } else {
            ckr_val |= I2S_CKR_TLP_NORMAL | I2S_CKR_RLP_NORMAL;
        }
        regmap_update_bits(i2s_tdm->regmap, I2S_CKR, mask, ckr_val);
    }

    if (!is_params_dirty(substream, dai, div_bclk, div_lrck, val))
        return 0;

    if (i2s_tdm->clk_trcm)
        rockchip_i2s_tdm_params_trcm(substream, dai, div_bclk, div_lrck, val);
    else
        rockchip_i2s_tdm_params(substream, dai, div_bclk, div_lrck, val);

    ret = rockchip_i2s_io_multiplex(substream, dai);

err:
    return ret;
}

/* Updated trigger function */
static int rockchip_i2s_tdm_trigger(struct snd_pcm_substream *substream,
        int cmd, struct snd_soc_dai *dai)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(dai);
    int ret = 0;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    /* Reset pause state on start */
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        i2s_tdm->playback_paused = false;
        
        /* Ensure auto_mute is active for this playback session */
        if (!i2s_tdm->user_mute_priority) {
            i2s_tdm->auto_mute_active = true;
        }
        
        /* Start stream immediately - mute is already ON by default */
        rockchip_i2s_tdm_start(i2s_tdm, substream->stream);
        
        /* Schedule unmute after postmute delay */
        if (!i2s_tdm->user_mute_priority && i2s_tdm->postmute_delay_ms > 0) {
            schedule_delayed_work(&i2s_tdm->mute_post_work, 
                                msecs_to_jiffies(i2s_tdm->postmute_delay_ms));
            dev_info(i2s_tdm->dev, "TRIGGER START: Stream started, unmute in %dms\n", 
                     i2s_tdm->postmute_delay_ms);
        }
    } else {
        i2s_tdm->capture_paused = false;
        rockchip_i2s_tdm_start(i2s_tdm, substream->stream);
    }
    break;
    case SNDRV_PCM_TRIGGER_RESUME:
    /* Reset pause state on system resume */
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
        i2s_tdm->playback_paused = false;
    else
        i2s_tdm->capture_paused = false;
    rockchip_i2s_tdm_start(i2s_tdm, substream->stream);
    break;
    case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
    /* Resume after pause */
    rockchip_i2s_tdm_resume(i2s_tdm, substream->stream);
    break;
    case SNDRV_PCM_TRIGGER_SUSPEND:
    case SNDRV_PCM_TRIGGER_STOP:
    /* Reset pause state on stop */
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        i2s_tdm->playback_paused = false;
        
        /* Cancel any pending unmute work */
        cancel_delayed_work_sync(&i2s_tdm->mute_post_work);
        
        /* Enable mute when playback stops (no useful signal) */
        mutex_lock(&i2s_tdm->mute_lock);
        if (!i2s_tdm->user_mute_priority && !i2s_tdm->format_change_mute) {
            if (i2s_tdm->mute_gpio) {
                gpiod_set_value(i2s_tdm->mute_gpio, 1);
                if (i2s_tdm->mute_inv_gpio)
                    gpiod_set_value(i2s_tdm->mute_inv_gpio, 0);
            }
            i2s_tdm->auto_mute_active = true;
            rockchip_i2s_tdm_apply_mute(i2s_tdm, true);
            dev_info(i2s_tdm->dev, "TRIGGER STOP: Mute enabled (no signal)\n");
        }
        mutex_unlock(&i2s_tdm->mute_lock);
    } else {
        i2s_tdm->capture_paused = false;
    }
    rockchip_i2s_tdm_stop(i2s_tdm, substream->stream);
    break;
    case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
    /* Stream suspension */
    rockchip_i2s_tdm_pause(i2s_tdm, substream->stream);
    break;
    default:
    ret = -EINVAL;
    break;
    }

    return ret;
}

static int rockchip_i2s_tdm_set_sysclk(struct snd_soc_dai *cpu_dai, int stream,
           unsigned int freq, int dir)
{
    struct rk_i2s_tdm_dev *i2s_tdm = to_info(cpu_dai);
    unsigned int fixed_freq;

    /* Fix MCLK to standard frequencies for each domain with multiplier support */
    if (freq % 44100 == 0) {
        /* 44.1 kHz family - use 22579200 Hz (512x) or 45158400 Hz (1024x) */
        fixed_freq = (i2s_tdm->mclk_multiplier == 1024) ? 45158400 : 22579200;
    } else {
        /* 48 kHz family - use 24576000 Hz (512x) or 49152000 Hz (1024x) */
        fixed_freq = (i2s_tdm->mclk_multiplier == 1024) ? 49152000 : 24576000;
    }

    /* Put set mclk rate into rockchip_i2s_tdm_set_mclk() */
    if (i2s_tdm->clk_trcm) {
    i2s_tdm->mclk_tx_freq = fixed_freq;
    i2s_tdm->mclk_rx_freq = fixed_freq;
    } else {
    if (stream == SNDRV_PCM_STREAM_PLAYBACK)
        i2s_tdm->mclk_tx_freq = fixed_freq;
    else
        i2s_tdm->mclk_rx_freq = fixed_freq;
    }

    dev_dbg(i2s_tdm->dev, "The target mclk_%s freq is: %d (fixed from %d)\n",
    stream ? "rx" : "tx", fixed_freq, freq);

    return 0;
}

static int rockchip_i2s_tdm_clk_compensation_info(struct snd_kcontrol *kcontrol,
          struct snd_ctl_elem_info *uinfo)
{
    uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count = 1;
    uinfo->value.integer.min = CLK_PPM_MIN;
    uinfo->value.integer.max = CLK_PPM_MAX;
    uinfo->value.integer.step = 1;

    return 0;
}

static int rockchip_i2s_tdm_clk_compensation_get(struct snd_kcontrol *kcontrol,
         struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_dai *dai = snd_kcontrol_chip(kcontrol);
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);

    ucontrol->value.integer.value[0] = i2s_tdm->clk_ppm;

    return 0;
}

static int rockchip_i2s_tdm_clk_compensation_put(struct snd_kcontrol *kcontrol,
         struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_dai *dai = snd_kcontrol_chip(kcontrol);
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);
    int ret = 0, ppm = 0;

    if ((ucontrol->value.integer.value[0] < CLK_PPM_MIN) ||
        (ucontrol->value.integer.value[0] > CLK_PPM_MAX))
    return -EINVAL;

    ppm = ucontrol->value.integer.value[0];

    ret = rockchip_i2s_tdm_clk_set_rate(i2s_tdm, i2s_tdm->mclk_root0,
            i2s_tdm->mclk_root0_freq, ppm);
    if (ret)
    return ret;

    if (clk_is_match(i2s_tdm->mclk_root0, i2s_tdm->mclk_root1))
    return 0;

    ret = rockchip_i2s_tdm_clk_set_rate(i2s_tdm, i2s_tdm->mclk_root1,
            i2s_tdm->mclk_root1_freq, ppm);

    return ret;
}

static struct snd_kcontrol_new rockchip_i2s_tdm_compensation_control = {
    .iface = SNDRV_CTL_ELEM_IFACE_PCM,
    .name = "PCM Clk Compensation In PPM",
    .info = rockchip_i2s_tdm_clk_compensation_info,
    .get = rockchip_i2s_tdm_clk_compensation_get,
    .put = rockchip_i2s_tdm_clk_compensation_put,
};


static const struct snd_kcontrol_new rockchip_i2s_tdm_snd_controls[] = {
};

/* Control structures defined after functions */

static int rockchip_i2s_tdm_volume_info(struct snd_kcontrol *kcontrol,
                                        struct snd_ctl_elem_info *uinfo)
{
    uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count = 1;
    uinfo->value.integer.min = 0;
    uinfo->value.integer.max = 100;
    uinfo->value.integer.step = 1;
    return 0;
}

static int rockchip_i2s_tdm_volume_get(struct snd_kcontrol *kcontrol,
                                       struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_dai *dai = snd_kcontrol_chip(kcontrol);
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);

    ucontrol->value.integer.value[0] = i2s_tdm->volume;
    return 0;
}

/* Basic volume setting function */
static int rockchip_i2s_tdm_volume_put(struct snd_kcontrol *kcontrol,
                                       struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_dai *dai = snd_kcontrol_chip(kcontrol);
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);
    int volume = ucontrol->value.integer.value[0];
    int old_volume = i2s_tdm->volume;

    if (volume < 0 || volume > 100)
        return -EINVAL;

    if (volume == old_volume)
        return 0;

    i2s_tdm->volume = volume;
    
    dev_info(i2s_tdm->dev, "Volume changed: %d%% -> %d%%\n",
             old_volume, volume);
    
    return 1;
}

static int rockchip_i2s_tdm_mute_info(struct snd_kcontrol *kcontrol,
                                     struct snd_ctl_elem_info *uinfo)
{
    uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
    uinfo->count = 1;
    uinfo->value.integer.min = 0;
    uinfo->value.integer.max = 1;
    return 0;
}

static int rockchip_i2s_tdm_mute_get(struct snd_kcontrol *kcontrol,
                                     struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_dai *dai = snd_kcontrol_chip(kcontrol);
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);

    /* Invert value for player: mute=false -> return 1 (unmute) */
    ucontrol->value.integer.value[0] = !i2s_tdm->mute;
    return 0;
}

static int rockchip_i2s_tdm_mute_put(struct snd_kcontrol *kcontrol,
                                     struct snd_ctl_elem_value *ucontrol)
{
    struct snd_soc_dai *dai = snd_kcontrol_chip(kcontrol);
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);
    bool mute_request = ucontrol->value.integer.value[0];
    
    /* Invert logic: player passes 1=unmute, 0=mute */
    bool mute = !mute_request;

    if (i2s_tdm->mute == mute)
        return 0;

    mutex_lock(&i2s_tdm->mute_lock);
    
    i2s_tdm->mute = mute;

    if (mute) {
        /* User enabled mute - set priority */
        i2s_tdm->user_mute_priority = true;
        
        /* Cancel any automatic timers */
        cancel_delayed_work(&i2s_tdm->mute_post_work);
        
        /* Enable mute instantly */
        rockchip_i2s_tdm_apply_mute(i2s_tdm, true);
        dev_info(i2s_tdm->dev, "User mute ON: mute_gpio=%p inv_gpio=%p\n", 
                 i2s_tdm->mute_gpio, i2s_tdm->mute_inv_gpio);
        
    } else {
        /* User disabled mute - reset priority but keep auto_mute if no signal */
        i2s_tdm->user_mute_priority = false;
        
        /* Check if stream is running */
        bool stream_running = false;
        if (i2s_tdm->substreams[SNDRV_PCM_STREAM_PLAYBACK]) {
            struct snd_pcm_substream *substream = i2s_tdm->substreams[SNDRV_PCM_STREAM_PLAYBACK];
            struct snd_pcm_runtime *runtime = substream->runtime;
            if (runtime && runtime->status->state == SNDRV_PCM_STATE_RUNNING) {
                stream_running = true;
            }
        }
        
        /* Only unmute if stream is running */
        if (stream_running) {
            i2s_tdm->auto_mute_active = false;
            cancel_delayed_work(&i2s_tdm->mute_post_work);
            rockchip_i2s_tdm_apply_mute(i2s_tdm, false);
            dev_info(i2s_tdm->dev, "User unmute: stream running, mute OFF\n");
        } else {
            /* Keep auto mute active - no signal yet */
            dev_info(i2s_tdm->dev, "User unmute: no stream, keeping mute ON\n");
        }
    }

    mutex_unlock(&i2s_tdm->mute_lock);
    
    /* Notify ALSA about mute state change for synchronization with alsamixer */
    if (i2s_tdm->mute_kcontrol && i2s_tdm->dai && i2s_tdm->dai->component) {
        snd_ctl_notify(i2s_tdm->dai->component->card->snd_card, SNDRV_CTL_EVENT_MASK_VALUE, &i2s_tdm->mute_kcontrol->id);
    }
    
    return 1; /* Return 1 to notify ALSA of change */
}

/* Automatic mute during switching */
static void rockchip_i2s_tdm_apply_mute(struct rk_i2s_tdm_dev *i2s_tdm, bool enable)
{
    if (enable) {
        /* Enable mute INSTANTLY */
        if (i2s_tdm->mute_gpio)
            gpiod_set_value(i2s_tdm->mute_gpio, 1);
        if (i2s_tdm->mute_inv_gpio) {
            gpiod_set_value(i2s_tdm->mute_inv_gpio, 0);
            dev_dbg(i2s_tdm->dev, "Set inverted GPIO to 0 (mute ON)\n");
        }
        
        /* Software mute through volume = 0% for DACs without GPIO mute */
        /* Clear active DMA buffers immediately for instant mute effect */
        if (i2s_tdm->substreams[SNDRV_PCM_STREAM_PLAYBACK]) {
            struct snd_pcm_substream *substream = i2s_tdm->substreams[SNDRV_PCM_STREAM_PLAYBACK];
            struct snd_pcm_runtime *runtime = substream->runtime;
            
            if (runtime && runtime->status->state == SNDRV_PCM_STATE_RUNNING && runtime->dma_area) {
                /* Clear current DMA buffers for immediate silence */
                memset(runtime->dma_area, 0, runtime->dma_bytes);
                dev_dbg(i2s_tdm->dev, "DMA buffers cleared for immediate mute\n");
            }
        }
        
        
        /* DO NOT disable DMA - this leads to pause instead of mute */
        /* DMA continues to work, but sound is muted through GPIO + software */
        
    } else {
        /* Disable mute - called only from scheduled work after trigger start */
        if (i2s_tdm->mute_gpio) {
            gpiod_set_value(i2s_tdm->mute_gpio, 0);
            if (i2s_tdm->mute_inv_gpio) {
                gpiod_set_value(i2s_tdm->mute_inv_gpio, 1);
                dev_dbg(i2s_tdm->dev, "Set inverted GPIO to 1 (mute OFF)\n");
            }
        }
    }
}

/* Function for post-mute work thread (disable mute after delay) */
static void rockchip_i2s_tdm_mute_post_work(struct work_struct *work)
{
    struct rk_i2s_tdm_dev *i2s_tdm = container_of(work, struct rk_i2s_tdm_dev, mute_post_work.work);
    
    /* Check that device is still active */
    if (!i2s_tdm->dev || !device_is_registered(i2s_tdm->dev)) {
        dev_warn(i2s_tdm->dev, "Device unregistered during post-mute work\n");
        return;
    }
    
    mutex_lock(&i2s_tdm->mute_lock);
    
    /* Do NOT unmute if format change mute is active */
    if (i2s_tdm->format_change_mute) {
        dev_info(i2s_tdm->dev, "POST-MUTE: Skipping unmute - format change in progress\n");
        mutex_unlock(&i2s_tdm->mute_lock);
        return;
    }
    
    /* Unmute if auto_mute is active and no user mute priority */
    /* This work is only scheduled from TRIGGER_START, so stream is running */
    dev_info(i2s_tdm->dev, "POST-MUTE: auto_mute=%d user_priority=%d\n", 
             i2s_tdm->auto_mute_active, i2s_tdm->user_mute_priority);
    
    if (i2s_tdm->auto_mute_active && !i2s_tdm->user_mute_priority) {
        i2s_tdm->auto_mute_active = false;
        i2s_tdm->mute = false;
        if (i2s_tdm->mute_gpio) {
            gpiod_set_value(i2s_tdm->mute_gpio, 0);
            if (i2s_tdm->mute_inv_gpio)
                gpiod_set_value(i2s_tdm->mute_inv_gpio, 1);
        }
        dev_info(i2s_tdm->dev, "POST-MUTE: Unmuted after %dms\n", i2s_tdm->postmute_delay_ms);
        
        /* Notify ALSA control about mute state change */
        if (i2s_tdm->mute_kcontrol && i2s_tdm->dai && i2s_tdm->dai->component) {
            snd_ctl_notify(i2s_tdm->dai->component->card->snd_card, 
                          SNDRV_CTL_EVENT_MASK_VALUE, 
                          &i2s_tdm->mute_kcontrol->id);
        }
    } else {
        dev_info(i2s_tdm->dev, "POST-MUTE: Unmute BLOCKED\n");
    }
    
    mutex_unlock(&i2s_tdm->mute_lock);
}

/* MCLK multiplier sysfs interface */
static ssize_t mclk_multiplier_show(struct device *dev,
                                   struct device_attribute *attr,
                                   char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", i2s_tdm->mclk_multiplier);
}

static ssize_t mclk_multiplier_store(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int multiplier;
    
    if (sscanf(buf, "%d", &multiplier) != 1)
        return -EINVAL;
    
    if (multiplier != 512 && multiplier != 1024) {
        dev_err(dev, "Invalid MCLK multiplier: %d. Must be 512 or 1024.\n", multiplier);
        return -EINVAL;
    }
    
    i2s_tdm->mclk_multiplier = multiplier;
    dev_info(dev, "MCLK multiplier set to %dx\n", multiplier);
    
    return count;
}

static DEVICE_ATTR(mclk_multiplier, 0644, mclk_multiplier_show, mclk_multiplier_store);

/* DSD channel swap sysfs interface */

static ssize_t dsd_sample_swap_show(struct device *dev,
                                   struct device_attribute *attr,
                                   char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", i2s_tdm->dsd_sample_swap ? 1 : 0);
}

static ssize_t dsd_sample_swap_store(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int enable;
    
    if (sscanf(buf, "%d", &enable) != 1)
        return -EINVAL;
    
    i2s_tdm->dsd_sample_swap = enable ? true : false;
    dev_info(dev, "DSD Sample Swap to eliminate purple noise %s\n", 
             enable ? "ENABLED" : "DISABLED");
    
    return count;
}

static DEVICE_ATTR(dsd_sample_swap, 0644, dsd_sample_swap_show, dsd_sample_swap_store);

static ssize_t pcm_channel_swap_show(struct device *dev,
                                    struct device_attribute *attr,
                                    char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", i2s_tdm->pcm_channel_swap ? 1 : 0);
}

static ssize_t pcm_channel_swap_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int enable;
    
    if (sscanf(buf, "%d", &enable) != 1)
        return -EINVAL;
    
    /* Accept only 0 or 1 */
    if (enable != 0 && enable != 1)
        return -EINVAL;
    
    i2s_tdm->pcm_channel_swap = (enable == 1);
    
    dev_info(dev, "PCM Channel Swap (LRCK inversion) %s\n", 
             enable ? "ENABLED" : "DISABLED");
    
    /* Changes will apply on next playback */
    
    return count;
}

static DEVICE_ATTR(pcm_channel_swap, 0644, pcm_channel_swap_show, pcm_channel_swap_store);

static ssize_t dsd_physical_swap_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", i2s_tdm->dsd_physical_swap ? 1 : 0);
}

static ssize_t dsd_physical_swap_store(struct device *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int enable;
    
    if (sscanf(buf, "%d", &enable) != 1)
        return -EINVAL;
    
    i2s_tdm->dsd_physical_swap = enable ? true : false;
    dev_info(dev, "DSD Physical Channel Swap %s\n", 
             enable ? "enabled" : "disabled");
    
    /* FIX: Apply routing changes ONLY for current DSD mode */
    if (i2s_tdm->dsd_mode_active) {
        /* If DSD mode is active - apply swap immediately */
        rockchip_i2s_tdm_apply_dsd_physical_swap(i2s_tdm);
        dev_info(dev, "DSD Physical Channel Swap applied immediately (DSD mode active)\n");
    } else {
        /* If PCM mode - only save setting, will be applied when switching to DSD */
        dev_info(dev, "DSD Physical Channel Swap setting saved (will apply in DSD mode)\n");
    }
    
    return count;
}

static DEVICE_ATTR(dsd_physical_swap, 0644, dsd_physical_swap_show, dsd_physical_swap_store);

/* Frequency domain GPIO polarity control sysfs interface */
static ssize_t freq_domain_invert_show(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", i2s_tdm->freq_domain_invert ? 1 : 0);
}

static ssize_t freq_domain_invert_store(struct device *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int value;
    
    if (sscanf(buf, "%d", &value) != 1)
        return -EINVAL;
    
    if (value != 0 && value != 1)
        return -EINVAL;
    
    i2s_tdm->freq_domain_invert = (value == 1);
    dev_dbg(dev, "Frequency domain GPIO polarity inversion %s\n", 
             i2s_tdm->freq_domain_invert ? "ENABLED" : "DISABLED");
    
    return count;
}

static DEVICE_ATTR(freq_domain_invert, 0644, freq_domain_invert_show, freq_domain_invert_store);

/* Manual DSD mode control sysfs interface */
static ssize_t dsd_mode_manual_show(struct device *dev,
                                    struct device_attribute *attr,
                                    char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", i2s_tdm->dsd_mode_active ? 1 : 0);
}

static ssize_t dsd_mode_manual_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int mode;
    
    if (sscanf(buf, "%d", &mode) != 1)
        return -EINVAL;
    
    if (mode != 0 && mode != 1)
        return -EINVAL;
    
    if (!i2s_tdm->dsd_on_gpio)
        return -ENODEV;
    
    if (mode == 0 && i2s_tdm->dsd_mode_active) {
        dev_info(dev, "Manual switch: DSD -> PCM\n");
        /* Enable mute before switch */
        if (i2s_tdm->mute_gpio) {
            cancel_delayed_work_sync(&i2s_tdm->mute_post_work);
            i2s_tdm->format_change_mute = true;
            gpiod_set_value(i2s_tdm->mute_gpio, 1);
            if (i2s_tdm->mute_inv_gpio)
                gpiod_set_value(i2s_tdm->mute_inv_gpio, 0);
            msleep(50);
        }
        i2s_tdm->dsd_mode_active = false;
        gpiod_set_value(i2s_tdm->dsd_on_gpio, 0);
        dev_info(dev, "DSD-on GPIO deactivated (PCM mode)\n");
        rockchip_i2s_tdm_apply_dsd_physical_swap(i2s_tdm);
        if (i2s_tdm->mute_gpio) {
            msleep(500);
            i2s_tdm->format_change_mute = false;
        }
    } else if (mode == 1 && !i2s_tdm->dsd_mode_active) {
        dev_info(dev, "Manual switch: PCM -> DSD\n");
        /* Enable mute before switch */
        if (i2s_tdm->mute_gpio) {
            cancel_delayed_work_sync(&i2s_tdm->mute_post_work);
            i2s_tdm->format_change_mute = true;
            gpiod_set_value(i2s_tdm->mute_gpio, 1);
            if (i2s_tdm->mute_inv_gpio)
                gpiod_set_value(i2s_tdm->mute_inv_gpio, 0);
            msleep(50);
        }
        i2s_tdm->dsd_mode_active = true;
        gpiod_set_value(i2s_tdm->dsd_on_gpio, 1);
        dev_info(dev, "DSD-on GPIO activated (DSD mode)\n");
        rockchip_i2s_tdm_apply_dsd_physical_swap(i2s_tdm);
        if (i2s_tdm->mute_gpio) {
            msleep(500);
            i2s_tdm->format_change_mute = false;
        }
    }
    
    return count;
}

static DEVICE_ATTR(dsd_mode_manual, 0644, dsd_mode_manual_show, dsd_mode_manual_store);

/* Postmute delay sysfs interface */
static ssize_t postmute_delay_ms_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%u\n", i2s_tdm->postmute_delay_ms);
}

static ssize_t postmute_delay_ms_store(struct device *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    unsigned int delay;

    if (kstrtouint(buf, 10, &delay) || delay > 2000)
        return -EINVAL;

    i2s_tdm->postmute_delay_ms = delay;
    dev_info(i2s_tdm->dev, "Postmute delay set to %u ms", delay);

    return count;
}

static DEVICE_ATTR(postmute_delay_ms, 0644, postmute_delay_ms_show, postmute_delay_ms_store);

/* Mute control sysfs interface */
static ssize_t mute_show(struct device *dev,
                        struct device_attribute *attr,
                        char *buf)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", i2s_tdm->mute ? 1 : 0);
}

static ssize_t mute_store(struct device *dev,
                         struct device_attribute *attr,
                         const char *buf, size_t count)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int enable;
    
    if (sscanf(buf, "%d", &enable) != 1)
        return -EINVAL;
    
    /* Accept only 0 or 1 */
    if (enable != 0 && enable != 1)
        return -EINVAL;
    
    mutex_lock(&i2s_tdm->mute_lock);
    
    if (enable && !i2s_tdm->mute) {
        /* Enable mute */
        i2s_tdm->mute = true;
        i2s_tdm->user_mute_priority = true;
        
        /* Cancel any automatic timers */
        cancel_delayed_work(&i2s_tdm->mute_post_work);
        
        /* Enable mute instantly */
        rockchip_i2s_tdm_apply_mute(i2s_tdm, true);
        
    } else if (!enable && i2s_tdm->mute) {
        /* Disable mute */
        i2s_tdm->mute = false;
        i2s_tdm->user_mute_priority = false;
        i2s_tdm->auto_mute_active = false;
        
        /* Cancel any automatic timers */
        cancel_delayed_work(&i2s_tdm->mute_post_work);
        
        /* Disable mute */
        rockchip_i2s_tdm_apply_mute(i2s_tdm, false);
    }
    
    mutex_unlock(&i2s_tdm->mute_lock);
    
    /* Notify ALSA about mute state change for synchronization with alsamixer */
    if (i2s_tdm->mute_kcontrol && i2s_tdm->dai && i2s_tdm->dai->component) {
        snd_ctl_notify(i2s_tdm->dai->component->card->snd_card, SNDRV_CTL_EVENT_MASK_VALUE, &i2s_tdm->mute_kcontrol->id);
    }
    
    return count;
}

static DEVICE_ATTR(mute, 0644, mute_show, mute_store);


/* Main ALSA controls */
static struct snd_kcontrol_new rockchip_i2s_tdm_volume_control = {
    .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
    .name = "PCM Playback Volume",
    .info = rockchip_i2s_tdm_volume_info,
    .get = rockchip_i2s_tdm_volume_get,
    .put = rockchip_i2s_tdm_volume_put,
};

static struct snd_kcontrol_new rockchip_i2s_tdm_mute_control = {
    .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
    .name = "PCM Playback Switch",
    .info = rockchip_i2s_tdm_mute_info,
    .get = rockchip_i2s_tdm_mute_get,
    .put = rockchip_i2s_tdm_mute_put,
};


/* PCM copy callback for audio data processing */
static int rockchip_i2s_tdm_pcm_copy_user(struct snd_soc_component *component,
                                          struct snd_pcm_substream *substream,
                                          int channel, unsigned long pos,
                                          void __user *buf, unsigned long bytes)
{
    struct rk_i2s_tdm_dev *i2s_tdm;
    void *dma_area;
    static int copy_call_count = 0;
    
    /* Get our driver through component */
    i2s_tdm = snd_soc_component_get_drvdata(component);
    if (!i2s_tdm) {
        /* Debug message only for first calls */
        if (copy_call_count < 3) {
            dev_warn(component->dev, "Failed to get I2S TDM device from component (call %d)\n", copy_call_count++);
        }
        /* Fallback: standard copying without processing */
        dma_area = substream->runtime->dma_area + pos;
        if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
            return copy_from_user(dma_area, buf, bytes) ? -EFAULT : 0;
        } else {
            return copy_to_user(buf, dma_area, bytes) ? -EFAULT : 0;
        }
    }
    
    /* Get pointer to DMA buffer */
    dma_area = substream->runtime->dma_area + pos;
    if (!dma_area) {
        dev_err(component->dev, "Invalid DMA area\n");
        return -EINVAL;
    }
    
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        /* PLAYBACK: copy from user and process */
        if (copy_from_user(dma_area, buf, bytes))
            return -EFAULT;
        
        /* DSD MUTE: Replace data with silence signal (50% duty cycle) */
        if ((i2s_tdm->mute || i2s_tdm->auto_mute_active) && i2s_tdm->dsd_mode_active &&
            (substream->runtime->format == SNDRV_PCM_FORMAT_DSD_U32_LE || 
             substream->runtime->format == SNDRV_PCM_FORMAT_DSD_U32_BE ||
             substream->runtime->format == SNDRV_PCM_FORMAT_DSD_U16_LE) && 
            bytes >= 4) {
            
            /* For DSD silence = 50% duty cycle meander within each byte */
            uint8_t *data = (uint8_t *)dma_area;
            uint32_t i;
            
            for (i = 0; i < bytes; i++) {
                data[i] = 0x55;  /* 01010101 - perfect 50% duty cycle meander for DSD silence */
            }
            
            dev_dbg(i2s_tdm->dev, "DSD Mute: replaced %lu bytes with silence signal\n", bytes);
            
            /* During mute do not apply sample swap - send clean silence signal */
            goto skip_dsd_processing;
        }

        /* PCM MUTE: Replace data with silence (0 volume) */
        if ((i2s_tdm->mute || i2s_tdm->auto_mute_active) && !i2s_tdm->dsd_mode_active) {
            memset(dma_area, 0, bytes);
            dev_dbg(i2s_tdm->dev, "PCM Mute: replaced %lu bytes with silence\n", bytes);
            goto skip_volume_processing;
        }

        /* Simple volume control with linear scaling */
        if (i2s_tdm->volume < 100 && !i2s_tdm->dsd_mode_active) {
            int volume_percent = i2s_tdm->volume;
            int32_t volume_linear = (volume_percent * 65536) / 100; /* Simple linear scaling */
            int format = substream->runtime->format;
            int i;

            /* Handle different bit depths with pseudo-logarithmic scaling */
            if (format == SNDRV_PCM_FORMAT_S16_LE) {
                /* 16-bit samples */
                s16 *samples = (s16 *)dma_area;
                int num_samples = bytes / 2;
                
                for (i = 0; i < num_samples; i++) {
                    /* Apply pseudo-logarithmic volume using Q15.16 format */
                    s64 scaled = (s64)samples[i] * volume_linear;
                    samples[i] = (s16)(scaled >> 16);
                }
            } else if (format == SNDRV_PCM_FORMAT_S24_LE || format == SNDRV_PCM_FORMAT_S32_LE) {
                /* 24-bit or 32-bit samples - use 64-bit math with pseudo-logarithmic scaling */
                s32 *samples = (s32 *)dma_area;
                int num_samples = bytes / 4;
                
                for (i = 0; i < num_samples; i++) {
                    /* Apply pseudo-logarithmic volume using Q15.16 format */
                    s64 scaled = (s64)samples[i] * volume_linear;
                    samples[i] = (s32)(scaled >> 16);
                }
            } else if (format == SNDRV_PCM_FORMAT_S24_3LE) {
                /* 24-bit packed samples (3 bytes per sample) */
                u8 *data = (u8 *)dma_area;
                int num_samples = bytes / 3;
                
                for (i = 0; i < num_samples; i++) {
                    int sample_offset = i * 3;
                    /* Convert 3-byte little-endian to s32 */
                    s32 sample = (data[sample_offset] | 
                                 (data[sample_offset + 1] << 8) | 
                                 (data[sample_offset + 2] << 16));
                    
                    /* Sign extend from 24-bit */
                    if (sample & 0x800000)
                        sample |= 0xFF000000;
                    
                    /* Apply pseudo-logarithmic volume using Q15.16 format */
                    s64 scaled = (s64)sample * volume_linear;
                    sample = (s32)(scaled >> 16);
                    
                    /* Convert back to 3-byte little-endian */
                    data[sample_offset] = sample & 0xFF;
                    data[sample_offset + 1] = (sample >> 8) & 0xFF;
                    data[sample_offset + 2] = (sample >> 16) & 0xFF;
                }
            }
        }

        /* CRITICAL FIX FOR DSD: Swap upper and lower 16 bits */
        if (i2s_tdm->dsd_sample_swap && i2s_tdm->dsd_mode_active &&
            (substream->runtime->format == SNDRV_PCM_FORMAT_DSD_U32_LE || 
             substream->runtime->format == SNDRV_PCM_FORMAT_DSD_U32_BE) && 
            bytes >= 4 && (bytes % 4) == 0) {
            
            uint32_t *samples = (uint32_t *)dma_area;
            uint32_t total_samples = bytes / 4;
            uint32_t i;
            
            for (i = 0; i < total_samples; i++) {
                /* Swap upper and lower 16 bits: ABCD -> CDAB */
                uint32_t sample = samples[i];
                samples[i] = ((sample & 0xFFFF0000) >> 16) | ((sample & 0x0000FFFF) << 16);
            }
        }
        
    skip_dsd_processing:
    skip_volume_processing:
        /* Debug message only for first calls */
        if (copy_call_count < 3) {
            dev_info(i2s_tdm->dev, "PCM copy: %lu bytes, simple volume control (call %d)\n", 
                     bytes, copy_call_count++);
        } else if (copy_call_count == 3) {
            dev_info(i2s_tdm->dev, "PCM copy working, suppressing further debug messages\n");
            copy_call_count++;
        }
        
        dev_dbg(i2s_tdm->dev, "Processed %lu bytes for playback\n", bytes);
    } else {
        /* CAPTURE: simply copy to user */
        if (copy_to_user(buf, dma_area, bytes))
            return -EFAULT;
    }
    
    return 0;
}

/* DSD rates for RoonReady compatibility */
static const unsigned int dsd_rates[] = {
    2822400,   /* DSD64 */
    5644800,   /* DSD128 */
    11289600,  /* DSD256 */
    22579200,  /* DSD512 */
};

/* Add pause/resume support to PCM hardware */
static const struct snd_pcm_hardware rockchip_i2s_tdm_pcm_hardware = {
    .info = SNDRV_PCM_INFO_MMAP |
        SNDRV_PCM_INFO_MMAP_VALID |
        SNDRV_PCM_INFO_INTERLEAVED |
        SNDRV_PCM_INFO_PAUSE |        /* Pause support */
        SNDRV_PCM_INFO_RESUME |       /* Resume support */
        SNDRV_PCM_INFO_BLOCK_TRANSFER,
    .formats = SNDRV_PCM_FMTBIT_S8 |
       SNDRV_PCM_FMTBIT_S16_LE |
       SNDRV_PCM_FMTBIT_S20_3LE |
       SNDRV_PCM_FMTBIT_S24_LE |
       SNDRV_PCM_FMTBIT_S32_LE |
       SNDRV_PCM_FMTBIT_DSD_U16_LE |
       SNDRV_PCM_FMTBIT_DSD_U32_LE,
    .rates = SNDRV_PCM_RATE_8000_384000 | SNDRV_PCM_RATE_KNOT,
    .rate_min = 8000,
    .rate_max = 22579200,  /* DSD512 support (22.5792 MHz) */
    .channels_min = 2,
    .channels_max = 16,
    .buffer_bytes_max = 1024 * 1024,  /* 1MB maximum for ultimate stability */
    .period_bytes_min = 8192,         
    .period_bytes_max = 64 * 1024,    /* 64KB maximum for deep buffering */
    .periods_min = 16,                
    .periods_max = 512,
    .fifo_size = 512,  /* Increased from 256 to 512 for maximum buffering on single-core ARM */
};

static const struct snd_dmaengine_pcm_config rockchip_i2s_tdm_dmaengine_pcm_config = {
    .pcm_hardware = &rockchip_i2s_tdm_pcm_hardware,
    .prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
    .prealloc_buffer_size = 1024 * 1024,  /* 1MB preallocation for ultimate stability */
};

/* Component probe function to set driver data */
static int rockchip_i2s_tdm_component_probe(struct snd_soc_component *component)
{
    struct device *dev = component->dev;
    struct rk_i2s_tdm_dev *i2s_tdm;
    
    /* Get our driver from platform device */
    i2s_tdm = dev_get_drvdata(dev);
    if (!i2s_tdm) {
        dev_err(dev, "Failed to get I2S TDM device data in component probe\n");
        return -ENODEV;
    }
    
    /* Set driver data for component */
    snd_soc_component_set_drvdata(component, i2s_tdm);
    
    dev_info(dev, "Audiophile component probe: driver data set successfully\n");
    
    return 0;
}

/* Alternative way through ioctl for older ALSA versions */
static int rockchip_i2s_tdm_pcm_ioctl(struct snd_soc_component *component,
                                      struct snd_pcm_substream *substream,
                                      unsigned int cmd, void *arg)
{
    /* Standard ioctl without additional processing */
    return snd_pcm_lib_ioctl(substream, cmd, arg);
}


/* Component with copy callbacks support */
static const struct snd_soc_component_driver rockchip_i2s_tdm_component_with_copy = {
    .name = DRV_NAME,
    .probe = rockchip_i2s_tdm_component_probe,
    .controls = rockchip_i2s_tdm_snd_controls,
    .num_controls = ARRAY_SIZE(rockchip_i2s_tdm_snd_controls),
    .copy_user = rockchip_i2s_tdm_pcm_copy_user, /* DSD processing + simple volume */
    .ioctl = rockchip_i2s_tdm_pcm_ioctl,
};

static int rockchip_i2s_tdm_dai_probe(struct snd_soc_dai *dai)
{
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);
    int ret;

    dai->capture_dma_data = &i2s_tdm->capture_dma_data;
    dai->playback_dma_data = &i2s_tdm->playback_dma_data;

    dev_info(i2s_tdm->dev, "Audiophile processing DISABLED - using standard ALSA\n");

    if (i2s_tdm->mclk_calibrate) {
        ret = snd_soc_add_dai_controls(dai, &rockchip_i2s_tdm_compensation_control, 1);
        if (ret)
            dev_err(i2s_tdm->dev, "Failed to add compensation control: %d\n", ret);
    }

    ret = snd_soc_add_dai_controls(dai, &rockchip_i2s_tdm_volume_control, 1);
    if (ret)
        dev_err(i2s_tdm->dev, "Failed to add volume control: %d\n", ret);
    else
        dev_info(i2s_tdm->dev, "Basic volume control added (no processing)\n");

    ret = snd_soc_add_dai_controls(dai, &rockchip_i2s_tdm_mute_control, 1);
    if (ret)
        dev_err(i2s_tdm->dev, "Failed to add mute control: %d\n", ret);
    else {
        dev_info(i2s_tdm->dev, "Basic mute control added (no processing)\n");
        /* Save pointers for automute system */
        i2s_tdm->mute_kcontrol = snd_soc_card_get_kcontrol(dai->component->card, rockchip_i2s_tdm_mute_control.name);
        i2s_tdm->dai = dai;
    }

    return 0;
}

static int rockchip_dai_tdm_slot(struct snd_soc_dai *dai,
     unsigned int tx_mask, unsigned int rx_mask,
     int slots, int slot_width)
{
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);
    unsigned int mask, val;

    i2s_tdm->tdm_mode = true;
    i2s_tdm->bclk_fs = slots * slot_width;
    mask = TDM_SLOT_BIT_WIDTH_MSK | TDM_FRAME_WIDTH_MSK;
    val = TDM_SLOT_BIT_WIDTH(slot_width) |
          TDM_FRAME_WIDTH(slots * slot_width);

    pm_runtime_get_sync(dai->dev);
    regmap_update_bits(i2s_tdm->regmap, I2S_TDM_TXCR,
           mask, val);
    regmap_update_bits(i2s_tdm->regmap, I2S_TDM_RXCR,
           mask, val);
    pm_runtime_put(dai->dev);

    return 0;
}

static int rockchip_i2s_tdm_startup(struct snd_pcm_substream *substream,
        struct snd_soc_dai *dai)
{
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);

    if (i2s_tdm->substreams[substream->stream])
    return -EBUSY;

    i2s_tdm->substreams[substream->stream] = substream;

    /* Export DSD rates for userspace applications like RoonReady */
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        dev_info(i2s_tdm->dev, "DSD support available: 2.8M, 5.6M, 11.2M, 22.5M Hz\n");
    }

    return 0;
}

static void rockchip_i2s_tdm_shutdown(struct snd_pcm_substream *substream,
          struct snd_soc_dai *dai)
{
    struct rk_i2s_tdm_dev *i2s_tdm = snd_soc_dai_get_drvdata(dai);

    i2s_tdm->substreams[substream->stream] = NULL;
}

static const struct snd_soc_dai_ops rockchip_i2s_tdm_dai_ops = {
    .startup = rockchip_i2s_tdm_startup,
    .shutdown = rockchip_i2s_tdm_shutdown,
    .hw_params = rockchip_i2s_tdm_hw_params,
    .set_sysclk = rockchip_i2s_tdm_set_sysclk,
    .set_fmt = rockchip_i2s_tdm_set_fmt,
    .set_tdm_slot = rockchip_dai_tdm_slot,
    .trigger = rockchip_i2s_tdm_trigger,
};

static const struct snd_soc_component_driver rockchip_i2s_tdm_component = {
    .name = DRV_NAME,
    .controls = rockchip_i2s_tdm_snd_controls,
    .num_controls = ARRAY_SIZE(rockchip_i2s_tdm_snd_controls),
};

static bool rockchip_i2s_tdm_wr_reg(struct device *dev, unsigned int reg)
{
    switch (reg) {
    case I2S_TXCR:
    case I2S_RXCR:
    case I2S_CKR:
    case I2S_DMACR:
    case I2S_INTCR:
    case I2S_XFER:
    case I2S_CLR:
    case I2S_TXDR:
    case I2S_TDM_TXCR:
    case I2S_TDM_RXCR:
    case I2S_CLKDIV:
    return true;
    default:
    return false;
    }
}

static bool rockchip_i2s_tdm_rd_reg(struct device *dev, unsigned int reg)
{
    switch (reg) {
    case I2S_TXCR:
    case I2S_RXCR:
    case I2S_CKR:
    case I2S_DMACR:
    case I2S_INTCR:
    case I2S_XFER:
    case I2S_CLR:
    case I2S_TXDR:
    case I2S_RXDR:
    case I2S_TXFIFOLR:
    case I2S_INTSR:
    case I2S_RXFIFOLR:
    case I2S_TDM_TXCR:
    case I2S_TDM_RXCR:
    case I2S_CLKDIV:
    return true;
    default:
    return false;
    }
}

static bool rockchip_i2s_tdm_volatile_reg(struct device *dev, unsigned int reg)
{
    switch (reg) {
    case I2S_TXFIFOLR:
    case I2S_INTCR:
    case I2S_INTSR:
    case I2S_CLR:
    case I2S_TXDR:
    case I2S_RXDR:
    case I2S_RXFIFOLR:
    return true;
    default:
    return false;
    }
}

static bool rockchip_i2s_tdm_precious_reg(struct device *dev, unsigned int reg)
{
    switch (reg) {
    case I2S_RXDR:
    return true;
    default:
    return false;
    }
}

static const struct reg_default rockchip_i2s_tdm_reg_defaults[] = {
    {0x00, 0x7200000f},
    {0x04, 0x01c8000f},
    {0x08, 0x00001f1f},
    {0x10, 0x001f0000},
    {0x14, 0x01f00000},
    {0x30, 0x00003eff},
    {0x34, 0x00003eff},
    {0x38, 0x00000707},
};

static const struct regmap_config rockchip_i2s_tdm_regmap_config = {
    .reg_bits = 32,
    .reg_stride = 4,
    .val_bits = 32,
    .max_register = I2S_CLKDIV,
    .reg_defaults = rockchip_i2s_tdm_reg_defaults,
    .num_reg_defaults = ARRAY_SIZE(rockchip_i2s_tdm_reg_defaults),
    .writeable_reg = rockchip_i2s_tdm_wr_reg,
    .readable_reg = rockchip_i2s_tdm_rd_reg,
    .volatile_reg = rockchip_i2s_tdm_volatile_reg,
    .precious_reg = rockchip_i2s_tdm_precious_reg,
    .cache_type = REGCACHE_FLAT,
};

static int common_soc_init(struct device *dev, u32 addr)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    const struct txrx_config *configs = i2s_tdm->soc_data->configs;
    u32 reg = 0, val = 0, trcm = i2s_tdm->clk_trcm;
    int i;

    dev_info(dev, "common_soc_init called: addr=0x%08x, trcm=%u\n", addr, trcm);

    if (IS_ERR(i2s_tdm->grf)) {
        dev_err(dev, "GRF is not available (error)\n");
        return 0;
    }

    switch (trcm) {
    case I2S_CKR_TRCM_TXONLY:
        dev_info(dev, "TRCM mode: TXONLY\n");
        break;
    case I2S_CKR_TRCM_RXONLY:
        dev_info(dev, "TRCM mode: RXONLY\n");
        break;
    default:
        dev_info(dev, "TRCM mode not TXONLY/RXONLY (%u), skipping GRF config\n", trcm);
        return 0;
    }

    dev_info(dev, "Searching for matching config (count=%u)...\n", i2s_tdm->soc_data->config_count);
    for (i = 0; i < i2s_tdm->soc_data->config_count; i++) {
        dev_info(dev, "  Config[%d]: addr=0x%08x, reg=0x%x\n", i, configs[i].addr, configs[i].reg);
        if (addr != configs[i].addr)
            continue;
        reg = configs[i].reg;
        if (trcm == I2S_CKR_TRCM_TXONLY)
            val = configs[i].txonly;
        else
            val = configs[i].rxonly;

        if (reg) {
            dev_info(dev, "Writing GRF: reg=0x%x, val=0x%x (MCLKOUT source config)\n", reg, val);
            regmap_write(i2s_tdm->grf, reg, val);
        } else {
            dev_warn(dev, "Config matched but reg=0!\n");
        }
    }

    return 0;
}

static const struct txrx_config px30_txrx_config[] = {
    { 0xff060000, 0x184, PX30_I2S0_CLK_TXONLY, PX30_I2S0_CLK_RXONLY },
};

static const struct txrx_config rk1808_txrx_config[] = {
    { 0xff7e0000, 0x190, RK1808_I2S0_CLK_TXONLY, RK1808_I2S0_CLK_RXONLY },
};

static const struct txrx_config rk3308_txrx_config[] = {
    { 0xff300000, 0x308, RK3308_I2S0_CLK_TXONLY, RK3308_I2S0_CLK_RXONLY },
    { 0xff310000, 0x308, RK3308_I2S1_CLK_TXONLY, RK3308_I2S1_CLK_RXONLY },
};

static const struct txrx_config rk3568_txrx_config[] = {
    { 0xfe410000, 0x504, RK3568_I2S1_CLK_TXONLY, RK3568_I2S1_CLK_RXONLY },
    { 0xfe430000, 0x504, RK3568_I2S3_CLK_TXONLY, RK3568_I2S3_CLK_RXONLY },
    { 0xfe430000, 0x508, RK3568_I2S3_MCLK_TXONLY, RK3568_I2S3_MCLK_RXONLY },
};

static const struct txrx_config rv1126_txrx_config[] = {
    { 0xff800000, 0x10260, RV1126_I2S0_CLK_TXONLY, RV1126_I2S0_CLK_RXONLY },
};

static const struct txrx_config rv1106_txrx_config[] = {
    { 0xffae0000, 0x10260, RV1126_I2S0_CLK_TXONLY, RV1126_I2S0_CLK_RXONLY },
};

static const struct rk_i2s_soc_data px30_i2s_soc_data = {
    .softrst_offset = 0x0300,
    .configs = px30_txrx_config,
    .config_count = ARRAY_SIZE(px30_txrx_config),
    .init = common_soc_init,
};

static const struct rk_i2s_soc_data rk1808_i2s_soc_data = {
    .softrst_offset = 0x0300,
    .configs = rk1808_txrx_config,
    .config_count = ARRAY_SIZE(rk1808_txrx_config),
    .init = common_soc_init,
};

static const struct rk_i2s_soc_data rk3308_i2s_soc_data = {
    .softrst_offset = 0x0400,
    .grf_reg_offset = 0x0308,
    .grf_shift = 5,
    .configs = rk3308_txrx_config,
    .config_count = ARRAY_SIZE(rk3308_txrx_config),
    .init = common_soc_init,
};

static const struct rk_i2s_soc_data rk3568_i2s_soc_data = {
    .softrst_offset = 0x0400,
    .configs = rk3568_txrx_config,
    .config_count = ARRAY_SIZE(rk3568_txrx_config),
    .init = common_soc_init,
};

static const struct rk_i2s_soc_data rv1126_i2s_soc_data = {
    .softrst_offset = 0x0300,
    .configs = rv1126_txrx_config,
    .config_count = ARRAY_SIZE(rv1126_txrx_config),
    .init = common_soc_init,
};

static const struct rk_i2s_soc_data rv1106_i2s_soc_data = {
    .softrst_offset = 0x0300,
    .configs = rv1106_txrx_config,
    .config_count = ARRAY_SIZE(rv1106_txrx_config),
    .init = common_soc_init,
};

static const struct of_device_id rockchip_i2s_tdm_match[] = {
#ifdef CONFIG_CPU_PX30
    { .compatible = "rockchip,px30-i2s-tdm", .data = &px30_i2s_soc_data },
#endif
#ifdef CONFIG_CPU_RK1808
    { .compatible = "rockchip,rk1808-i2s-tdm", .data = &rk1808_i2s_soc_data },
#endif
#ifdef CONFIG_CPU_RK3308
    { .compatible = "rockchip,rk3308-i2s-tdm", .data = &rk3308_i2s_soc_data },
#endif
#ifdef CONFIG_CPU_RK3568
    { .compatible = "rockchip,rk3568-i2s-tdm", .data = &rk3568_i2s_soc_data },
#endif
#ifdef CONFIG_CPU_RK3588
    { .compatible = "rockchip,rk3588-i2s-tdm", },
#endif
#ifdef CONFIG_CPU_RV1106
    { .compatible = "rockchip,rv1106-i2s-tdm", .data = &rv1106_i2s_soc_data },
#endif
#ifdef CONFIG_CPU_RV1126
    { .compatible = "rockchip,rv1126-i2s-tdm", .data = &rv1126_i2s_soc_data },
#endif
    {},
};

#ifdef HAVE_SYNC_RESET
static int of_i2s_resetid_get(struct device_node *node,
              const char *id)
{
    struct of_phandle_args args;
    int index = 0;
    int ret;

    if (id)
    index = of_property_match_string(node,
         "reset-names", id);
    ret = of_parse_phandle_with_args(node, "resets", "#reset-cells",
         index, &args);
    if (ret)
    return ret;

    return args.args[0];
}
#endif

static int rockchip_i2s_tdm_dai_prepare(struct platform_device *pdev,
        struct snd_soc_dai_driver **soc_dai)
{
    struct snd_soc_dai_driver rockchip_i2s_tdm_dai = {
    .name = DRV_NAME,
    .probe = rockchip_i2s_tdm_dai_probe,
    .playback = {
        .stream_name = "Playback",
        .channels_min = 2,
        .channels_max = 16,
        .rates = SNDRV_PCM_RATE_8000_384000 | SNDRV_PCM_RATE_KNOT,
        .rate_min = 8000,
        .rate_max = 22579200,  /* DSD512 support */
        .formats = (SNDRV_PCM_FMTBIT_S8 |
        SNDRV_PCM_FMTBIT_S16_LE |
        SNDRV_PCM_FMTBIT_S20_3LE |
        SNDRV_PCM_FMTBIT_S24_LE |
        SNDRV_PCM_FMTBIT_S32_LE |
        SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE |
        SNDRV_PCM_FMTBIT_DSD_U16_LE |
        SNDRV_PCM_FMTBIT_DSD_U32_LE),
    },
    .capture = {
        .stream_name = "Capture",
        .channels_min = 2,
        .channels_max = 16,
        .rates = SNDRV_PCM_RATE_8000_384000 | SNDRV_PCM_RATE_KNOT,
        .rate_min = 8000,
        .rate_max = 22579200,  /* DSD512 support */
        .formats = (SNDRV_PCM_FMTBIT_S8 |
        SNDRV_PCM_FMTBIT_S16_LE |
        SNDRV_PCM_FMTBIT_S20_3LE |
        SNDRV_PCM_FMTBIT_S24_LE |
        SNDRV_PCM_FMTBIT_S32_LE |
        SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE |
        SNDRV_PCM_FMTBIT_DSD_U16_LE |
        SNDRV_PCM_FMTBIT_DSD_U32_LE),
    },
    .ops = &rockchip_i2s_tdm_dai_ops,
    };

    *soc_dai = devm_kmemdup(&pdev->dev, &rockchip_i2s_tdm_dai,
    sizeof(rockchip_i2s_tdm_dai), GFP_KERNEL);
    if (!(*soc_dai))
    return -ENOMEM;

    return 0;
}

static int rockchip_i2s_tdm_path_check(struct rk_i2s_tdm_dev *i2s_tdm,
           int num,
           bool is_rx_path)
{
    unsigned int *i2s_data;
    int i, j, ret = 0;

    if (is_rx_path)
    i2s_data = i2s_tdm->i2s_sdis;
    else
    i2s_data = i2s_tdm->i2s_sdos;

    for (i = 0; i < num; i++) {
    if (i2s_data[i] > CH_GRP_MAX - 1) {
        dev_err(i2s_tdm->dev,
    "%s path i2s_data[%d]: %d is overflow, max is: %d\n",
    is_rx_path ? "RX" : "TX",
    i, i2s_data[i], CH_GRP_MAX);
        ret = -EINVAL;
        goto err;
    }

    for (j = 0; j < num; j++) {
        if (i == j)
    continue;

        if (i2s_data[i] == i2s_data[j]) {
    dev_err(i2s_tdm->dev,
        "%s path invalid routed i2s_data: [%d]%d == [%d]%d\n",
        is_rx_path ? "RX" : "TX",
        i, i2s_data[i],
        j, i2s_data[j]);
    ret = -EINVAL;
    goto err;
        }
    }
    }

err:
    return ret;
}

static void rockchip_i2s_tdm_tx_path_config(struct rk_i2s_tdm_dev *i2s_tdm,
            int num)
{
    int idx;


    for (idx = 0; idx < num; idx++) {
    regmap_update_bits(i2s_tdm->regmap, I2S_TXCR,
       I2S_TXCR_PATH_MASK(idx),
       I2S_TXCR_PATH(idx, i2s_tdm->i2s_sdos[idx]));
    }
}

static void rockchip_i2s_tdm_rx_path_config(struct rk_i2s_tdm_dev *i2s_tdm,
            int num)
{
    int idx;

    for (idx = 0; idx < num; idx++) {
    regmap_update_bits(i2s_tdm->regmap, I2S_RXCR,
       I2S_RXCR_PATH_MASK(idx),
       I2S_RXCR_PATH(idx, i2s_tdm->i2s_sdis[idx]));
    }
}

static void rockchip_i2s_tdm_path_config(struct rk_i2s_tdm_dev *i2s_tdm,
         int num, bool is_rx_path)
{
    if (is_rx_path)
    rockchip_i2s_tdm_rx_path_config(i2s_tdm, num);
    else
    rockchip_i2s_tdm_tx_path_config(i2s_tdm, num);
}

static int rockchip_i2s_tdm_path_prepare(struct rk_i2s_tdm_dev *i2s_tdm,
         struct device_node *np,
         bool is_rx_path)
{
    char *i2s_tx_path_prop = "rockchip,i2s-tx-route";
    char *i2s_rx_path_prop = "rockchip,i2s-rx-route";
    char *i2s_path_prop;
    unsigned int *i2s_data;
    int num, ret = 0;

    if (is_rx_path) {
    i2s_path_prop = i2s_rx_path_prop;
    i2s_data = i2s_tdm->i2s_sdis;
    } else {
    i2s_path_prop = i2s_tx_path_prop;
    i2s_data = i2s_tdm->i2s_sdos;
    }

    num = of_count_phandle_with_args(np, i2s_path_prop, NULL);
    if (num < 0) {
    if (num != -ENOENT) {
        dev_err(i2s_tdm->dev,
    "Failed to read '%s' num: %d\n",
    i2s_path_prop, num);
        ret = num;
    }
    goto out;
    } else if (num != CH_GRP_MAX) {
    dev_err(i2s_tdm->dev,
        "The num: %d should be: %d\n", num, CH_GRP_MAX);
    ret = -EINVAL;
    goto out;
    }

    ret = of_property_read_u32_array(np, i2s_path_prop,
         i2s_data, num);
    if (ret < 0) {
    dev_err(i2s_tdm->dev,
        "Failed to read '%s': %d\n",
        i2s_path_prop, ret);
    goto out;
    }

    ret = rockchip_i2s_tdm_path_check(i2s_tdm, num, is_rx_path);
    if (ret < 0) {
    dev_err(i2s_tdm->dev,
        "Failed to check i2s data bus: %d\n", ret);
    goto out;
    }

    rockchip_i2s_tdm_path_config(i2s_tdm, num, is_rx_path);

out:
    return ret;
}

static int rockchip_i2s_tdm_tx_path_prepare(struct rk_i2s_tdm_dev *i2s_tdm,
            struct device_node *np)
{
    return rockchip_i2s_tdm_path_prepare(i2s_tdm, np, 0);
}

static int rockchip_i2s_tdm_rx_path_prepare(struct rk_i2s_tdm_dev *i2s_tdm,
            struct device_node *np)
{
    return rockchip_i2s_tdm_path_prepare(i2s_tdm, np, 1);
}

static int rockchip_i2s_tdm_get_fifo_count(struct device *dev, struct snd_pcm_substream *substream)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int val = 0;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
    regmap_read(i2s_tdm->regmap, I2S_TXFIFOLR, &val);
    else
    regmap_read(i2s_tdm->regmap, I2S_RXFIFOLR, &val);

    val = ((val & I2S_FIFOLR_TFL3_MASK) >> I2S_FIFOLR_TFL3_SHIFT) +
          ((val & I2S_FIFOLR_TFL2_MASK) >> I2S_FIFOLR_TFL2_SHIFT) +
          ((val & I2S_FIFOLR_TFL1_MASK) >> I2S_FIFOLR_TFL1_SHIFT) +
          ((val & I2S_FIFOLR_TFL0_MASK) >> I2S_FIFOLR_TFL0_SHIFT);

    return val;
}

static const struct snd_dlp_config dconfig = {
    .get_fifo_count = rockchip_i2s_tdm_get_fifo_count,
};

static irqreturn_t rockchip_i2s_tdm_isr(int irq, void *devid)
{
    struct rk_i2s_tdm_dev *i2s_tdm = (struct rk_i2s_tdm_dev *)devid;
    struct snd_pcm_substream *substream;
    u32 val;

    regmap_read(i2s_tdm->regmap, I2S_INTSR, &val);

    if (val & I2S_INTSR_TXUI_ACT) {
    dev_warn_ratelimited(i2s_tdm->dev, "TX FIFO Underrun\n");
    regmap_update_bits(i2s_tdm->regmap, I2S_INTCR,
       I2S_INTCR_TXUIC, I2S_INTCR_TXUIC);
    substream = i2s_tdm->substreams[SNDRV_PCM_STREAM_PLAYBACK];
    if (substream)
        snd_pcm_stop_xrun(substream);
    }

    if (val & I2S_INTSR_RXOI_ACT) {
        /* Silently clear RX FIFO Overrun for external clock mode
         * RX is used only for clock sync at high sample rates (192kHz)
         * Suppress logging to reduce CPU overhead */
        regmap_update_bits(i2s_tdm->regmap, I2S_INTCR,
           I2S_INTCR_RXOIC, I2S_INTCR_RXOIC);
        /* Don't stop capture stream for external clock sync mode */
    }

    return IRQ_HANDLED;
}

static int rockchip_i2s_tdm_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;
    const struct of_device_id *of_id;
    struct rk_i2s_tdm_dev *i2s_tdm;
    struct snd_soc_dai_driver *soc_dai;
    struct resource *res;
    void __iomem *regs;
#ifdef HAVE_SYNC_RESET
    bool sync;
#endif
    int ret, val, i, irq;

    ret = rockchip_i2s_tdm_dai_prepare(pdev, &soc_dai);
    if (ret)
    return ret;

    i2s_tdm = devm_kzalloc(&pdev->dev, sizeof(*i2s_tdm), GFP_KERNEL);
    if (!i2s_tdm)
    return -ENOMEM;

    i2s_tdm->dev = &pdev->dev;
    i2s_tdm->volume = 100;
    /* Initial mute state = true (muted on boot) */
    i2s_tdm->mute = true; 
    
    /* Initialize ALSA control pointers */
    i2s_tdm->mute_kcontrol = NULL;
    i2s_tdm->dai = NULL;
    
    /* Initialize MCLK multiplier - 512 by default */
    i2s_tdm->mclk_multiplier = 512;
    
    /* Initialize automatic mute - default ON (no signal yet) */
    i2s_tdm->auto_mute_active = true;
    i2s_tdm->user_mute_priority = false;
    mutex_init(&i2s_tdm->mute_lock);
    INIT_DELAYED_WORK(&i2s_tdm->mute_post_work, rockchip_i2s_tdm_mute_post_work);
    
    /* Initialize pause state */
    i2s_tdm->playback_paused = false;
    i2s_tdm->capture_paused = false;
    
    /* Initialize configurable postmute delay */
    i2s_tdm->postmute_delay_ms = DEFAULT_POSTMUTE_DELAY_MS;    // default for mute hold
    
    dev_info(&pdev->dev, "ROCKCHIP_I2S_TDM: Initial volume = %d, mute = %d (sound %s)\n", 
     i2s_tdm->volume, i2s_tdm->mute, i2s_tdm->mute ? "OFF" : "ON");

    i2s_tdm->mute_gpio = devm_gpiod_get_optional(&pdev->dev, "mute", GPIOD_OUT_HIGH);
    if (IS_ERR(i2s_tdm->mute_gpio)) {
    ret = PTR_ERR(i2s_tdm->mute_gpio);
    dev_err(&pdev->dev, "Failed to get mute GPIO: %d\n", ret);
    i2s_tdm->mute_gpio = NULL;
    } else if (i2s_tdm->mute_gpio) {
    /* Set GPIO: mute=true -> GPIO=1 (muted) */
    gpiod_set_value(i2s_tdm->mute_gpio, i2s_tdm->mute ? 1 : 0);
    dev_info(&pdev->dev, "ROCKCHIP_I2S_TDM: GPIO mute initialized to %d (sound %s)\n", 
         i2s_tdm->mute ? 1 : 0, i2s_tdm->mute ? "OFF" : "ON");
    }

    /* Initialize inverted mute GPIO (GPIO2_A5, pin 69) - LOW when muted */
    i2s_tdm->mute_inv_gpio = devm_gpiod_get_optional(&pdev->dev, "mute-inv", GPIOD_OUT_LOW);
    if (IS_ERR(i2s_tdm->mute_inv_gpio)) {
        ret = PTR_ERR(i2s_tdm->mute_inv_gpio);
        dev_err(&pdev->dev, "Failed to get inverted mute GPIO: %d\n", ret);
        i2s_tdm->mute_inv_gpio = NULL;
    } else if (i2s_tdm->mute_inv_gpio) {
        gpiod_set_value(i2s_tdm->mute_inv_gpio, i2s_tdm->mute ? 0 : 1);
        dev_info(&pdev->dev, "ROCKCHIP_I2S_TDM: Inverted mute GPIO initialized to %d\n", 
             i2s_tdm->mute ? 0 : 1);
    }

    /* Initialize DSD-on GPIO */
    i2s_tdm->dsd_on_gpio = devm_gpiod_get_optional(&pdev->dev, "dsd-enable", GPIOD_OUT_LOW);
    if (IS_ERR(i2s_tdm->dsd_on_gpio)) {
    ret = PTR_ERR(i2s_tdm->dsd_on_gpio);
    dev_err(&pdev->dev, "Failed to get DSD-on GPIO: %d\n", ret);
    i2s_tdm->dsd_on_gpio = NULL;
    } else if (i2s_tdm->dsd_on_gpio) {
    /* Initial state: DSD mode disabled */
    i2s_tdm->dsd_mode_active = false;
    gpiod_set_value(i2s_tdm->dsd_on_gpio, 0);
    dev_info(&pdev->dev, "ROCKCHIP_I2S_TDM: DSD-on GPIO initialized to 0 (DSD mode OFF)\n");
    }
    
    /* Initialize DSD sample swap to eliminate purple noise */
    i2s_tdm->dsd_sample_swap = true;  /* Enabled by default */
    
    /* Initialize Channel swap controls */
    i2s_tdm->pcm_channel_swap = false;   /* PCM channel swap disabled by default */
    i2s_tdm->dsd_physical_swap = false;  /* DSD physical swap disabled by default */

    /* Initialize frequency domain GPIO (GPIO1_D1) polarity control */
    i2s_tdm->freq_domain_invert = false;  /* Default: no inversion */
    i2s_tdm->freq_domain_gpio = devm_gpiod_get_optional(&pdev->dev, "freq-domain", GPIOD_ASIS);
    if (IS_ERR(i2s_tdm->freq_domain_gpio)) {
        /* GPIO might be controlled by gpio-mux-clock, this is normal */
        i2s_tdm->freq_domain_gpio = NULL;
        dev_info(&pdev->dev, "ROCKCHIP_I2S_TDM: Frequency domain GPIO controlled by gpio-mux-clock\n");
    } else if (i2s_tdm->freq_domain_gpio) {
        dev_info(&pdev->dev, "ROCKCHIP_I2S_TDM: Frequency domain GPIO available for polarity control\n");
    }

    of_id = of_match_device(rockchip_i2s_tdm_match, &pdev->dev);
    if (!of_id)
    return -EINVAL;

    spin_lock_init(&i2s_tdm->lock);
    i2s_tdm->soc_data = (const struct rk_i2s_soc_data *)of_id->data;

    for (i = 0; i < ARRAY_SIZE(of_quirks); i++)
    if (of_property_read_bool(node, of_quirks[i].quirk))
        i2s_tdm->quirks |= of_quirks[i].id;

    i2s_tdm->bclk_fs = 64;
    if (!of_property_read_u32(node, "rockchip,bclk-fs", &val)) {
    if ((val >= 32) && (val % 2 == 0))
        i2s_tdm->bclk_fs = val;
    }

    i2s_tdm->clk_trcm = I2S_CKR_TRCM_TXRX;
    if (!of_property_read_u32(node, "rockchip,clk-trcm", &val)) {
    if (val >= 0 && val <= 2) {
        i2s_tdm->clk_trcm = val << I2S_CKR_TRCM_SHIFT;
        if (i2s_tdm->clk_trcm)
    soc_dai->symmetric_rate = 1;
    }
    }

    i2s_tdm->tdm_fsync_half_frame =
    of_property_read_bool(node, "rockchip,tdm-fsync-half-frame");

    if (of_property_read_bool(node, "rockchip,playback-only"))
    soc_dai->capture.channels_min = 0;
    else if (of_property_read_bool(node, "rockchip,capture-only"))
    soc_dai->playback.channels_min = 0;

    i2s_tdm->grf = syscon_regmap_lookup_by_phandle(node, "rockchip,grf");

#ifdef HAVE_SYNC_RESET
    sync = of_device_is_compatible(node, "rockchip,px30-i2s-tdm") ||
           of_device_is_compatible(node, "rockchip,rk1808-i2s-tdm") ||
           of_device_is_compatible(node, "rockchip,rk3308-i2s-tdm");

    if (i2s_tdm->clk_trcm && sync) {
    struct device_node *cru_node;

    cru_node = of_parse_phandle(node, "rockchip,cru", 0);
    i2s_tdm->cru_base = of_iomap(cru_node, 0);
    if (!i2s_tdm->cru_base)
        return -ENOENT;

    i2s_tdm->tx_reset_id = of_i2s_resetid_get(node, "tx-m");
    i2s_tdm->rx_reset_id = of_i2s_resetid_get(node, "rx-m");
    }
#endif

    i2s_tdm->tx_reset = devm_reset_control_get(&pdev->dev, "tx-m");
    if (IS_ERR(i2s_tdm->tx_reset)) {
    ret = PTR_ERR(i2s_tdm->tx_reset);
    if (ret != -ENOENT)
        return ret;
    }

    i2s_tdm->rx_reset = devm_reset_control_get(&pdev->dev, "rx-m");
    if (IS_ERR(i2s_tdm->rx_reset)) {
    ret = PTR_ERR(i2s_tdm->rx_reset);
    if (ret != -ENOENT)
        return ret;
    }

    i2s_tdm->hclk = devm_clk_get(&pdev->dev, "hclk");
    if (IS_ERR(i2s_tdm->hclk))
    return PTR_ERR(i2s_tdm->hclk);

    ret = clk_prepare_enable(i2s_tdm->hclk);
    if (ret)
    return ret;

    i2s_tdm->mclk_tx = devm_clk_get(&pdev->dev, "mclk_tx");
    if (IS_ERR(i2s_tdm->mclk_tx))
    return PTR_ERR(i2s_tdm->mclk_tx);

    i2s_tdm->mclk_rx = devm_clk_get(&pdev->dev, "mclk_rx");
    if (IS_ERR(i2s_tdm->mclk_rx))
    return PTR_ERR(i2s_tdm->mclk_rx);

  i2s_tdm->mclk_external = 0;
    i2s_tdm->mclk_external =
        of_property_read_bool(node, "my,mclk_external");
    if (i2s_tdm->mclk_external) {
        dev_dbg(&pdev->dev, "External MCLK mode detected\n");
        i2s_tdm->mclk_ext = devm_clk_get(&pdev->dev, "mclk_ext");
        if (IS_ERR(i2s_tdm->mclk_ext)) {
            return dev_err_probe(i2s_tdm->dev, PTR_ERR(i2s_tdm->mclk_ext),
                 "Failed to get clock mclk_ext\n");
        }
        dev_info(&pdev->dev, "mclk_ext clock loaded successfully\n");

        i2s_tdm->mclk_ext_mux = 0;
        i2s_tdm->clk_44 = devm_clk_get(&pdev->dev, "clk_44");
        if (!IS_ERR(i2s_tdm->clk_44)) {
            dev_info(&pdev->dev, "clk_44 loaded successfully\n");
            i2s_tdm->clk_48 = devm_clk_get(&pdev->dev, "clk_48");
            if (!IS_ERR(i2s_tdm->clk_48)) {
                i2s_tdm->mclk_ext_mux = 1;
                dev_info(&pdev->dev, "clk_48 loaded successfully - external clock switching enabled\n");
            } else {
                dev_warn(&pdev->dev, "Failed to get clk_48: %ld\n", PTR_ERR(i2s_tdm->clk_48));
            }
        } else {
            dev_warn(&pdev->dev, "Failed to get clk_44: %ld\n", PTR_ERR(i2s_tdm->clk_44));
        }
    }

    i2s_tdm->io_multiplex =
        of_property_read_bool(node, "rockchip,io-multiplex");

    i2s_tdm->mclk_calibrate =
    of_property_read_bool(node, "rockchip,mclk-calibrate");

    if (i2s_tdm->mclk_calibrate) {
    i2s_tdm->mclk_tx_src = devm_clk_get(&pdev->dev, "mclk_tx_src");
    if (IS_ERR(i2s_tdm->mclk_tx_src))
        return PTR_ERR(i2s_tdm->mclk_tx_src);

    i2s_tdm->mclk_rx_src = devm_clk_get(&pdev->dev, "mclk_rx_src");
    if (IS_ERR(i2s_tdm->mclk_rx_src))
        return PTR_ERR(i2s_tdm->mclk_rx_src);

    i2s_tdm->mclk_root0 = devm_clk_get(&pdev->dev, "mclk_root0");
    if (IS_ERR(i2s_tdm->mclk_root0))
        return PTR_ERR(i2s_tdm->mclk_root0);

    i2s_tdm->mclk_root1 = devm_clk_get(&pdev->dev, "mclk_root1");
    if (IS_ERR(i2s_tdm->mclk_root1))
        return PTR_ERR(i2s_tdm->mclk_root1);

    i2s_tdm->mclk_root0_initial_freq = clk_get_rate(i2s_tdm->mclk_root0);
    i2s_tdm->mclk_root1_initial_freq = clk_get_rate(i2s_tdm->mclk_root1);
    i2s_tdm->mclk_root0_freq = i2s_tdm->mclk_root0_initial_freq;
    i2s_tdm->mclk_root1_freq = i2s_tdm->mclk_root1_initial_freq;
    }

    regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
    if (IS_ERR(regs))
    return PTR_ERR(regs);

    i2s_tdm->regmap = devm_regmap_init_mmio(&pdev->dev, regs,
        &rockchip_i2s_tdm_regmap_config);
    if (IS_ERR(i2s_tdm->regmap))
    return PTR_ERR(i2s_tdm->regmap);

    irq = platform_get_irq_optional(pdev, 0);
    if (irq > 0) {
    ret = devm_request_irq(&pdev->dev, irq, rockchip_i2s_tdm_isr,
           IRQF_SHARED, node->name, i2s_tdm);
    if (ret) {
        dev_err(&pdev->dev, "failed to request irq %u\n", irq);
        return ret;
    }
    }

    i2s_tdm->playback_dma_data.addr = res->start + I2S_TXDR;
    i2s_tdm->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
    i2s_tdm->playback_dma_data.maxburst = MAXBURST_PER_FIFO;

    i2s_tdm->capture_dma_data.addr = res->start + I2S_RXDR;
    i2s_tdm->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
    i2s_tdm->capture_dma_data.maxburst = MAXBURST_PER_FIFO;

    ret = rockchip_i2s_tdm_tx_path_prepare(i2s_tdm, node);
    if (ret < 0) {
    dev_err(&pdev->dev, "I2S TX path prepare failed: %d\n", ret);
    return ret;
    }
    
    /* After TX routing initialization apply DSD physical swap settings if needed */
    rockchip_i2s_tdm_apply_dsd_physical_swap(i2s_tdm);

    ret = rockchip_i2s_tdm_rx_path_prepare(i2s_tdm, node);
    if (ret < 0) {
    dev_err(&pdev->dev, "I2S RX path prepare failed: %d\n", ret);
    return ret;
    }

    atomic_set(&i2s_tdm->refcount, 0);
    dev_set_drvdata(&pdev->dev, i2s_tdm);
    pm_runtime_enable(&pdev->dev);
    
    dev_info(&pdev->dev, "ROCKCHIP_I2S_TDM: Pause/Resume support enabled\n");

    if (!pm_runtime_enabled(&pdev->dev)) {
    ret = i2s_tdm_runtime_resume(&pdev->dev);
    if (ret)
        goto err_pm_disable;
    }

    if (i2s_tdm->quirks & QUIRK_ALWAYS_ON) {
    unsigned int rate = DEFAULT_FS * DEFAULT_MCLK_FS;
    unsigned int div_bclk = DEFAULT_FS * DEFAULT_MCLK_FS;
    unsigned int div_lrck = i2s_tdm->bclk_fs;

    div_bclk = DIV_ROUND_CLOSEST(rate, div_lrck * DEFAULT_FS);

    /* assign generic freq */
    clk_set_rate(i2s_tdm->mclk_rx, rate);
    clk_set_rate(i2s_tdm->mclk_tx, rate);

    regmap_update_bits(i2s_tdm->regmap, I2S_CLKDIV,
       I2S_CLKDIV_RXM_MASK | I2S_CLKDIV_TXM_MASK,
       I2S_CLKDIV_RXM(div_bclk) | I2S_CLKDIV_TXM(div_bclk));
    regmap_update_bits(i2s_tdm->regmap, I2S_CKR,
       I2S_CKR_RSD_MASK | I2S_CKR_TSD_MASK,
       I2S_CKR_RSD(div_lrck) | I2S_CKR_TSD(div_lrck));

    if (i2s_tdm->clk_trcm)
        rockchip_i2s_tdm_xfer_trcm_start(i2s_tdm);
    else
        rockchip_i2s_tdm_xfer_start(i2s_tdm, SNDRV_PCM_STREAM_PLAYBACK);

    pm_runtime_forbid(&pdev->dev);
    }
    
    /* Enable continuous MCLK if corresponding quirk is set */
    if (i2s_tdm->quirks & QUIRK_MCLK_ALWAYS_ON) {
        dev_info(&pdev->dev, "MCLK always-on mode enabled\n");
        /* Make sure MCLK is enabled and will remain enabled */
        ret = clk_prepare_enable(i2s_tdm->mclk_tx);
        if (ret) {
            dev_err(&pdev->dev, "Failed to enable mclk_tx for always-on: %d\n", ret);
            goto err_pm_disable;
        }
        ret = clk_prepare_enable(i2s_tdm->mclk_rx);
        if (ret) {
            dev_err(&pdev->dev, "Failed to enable mclk_rx for always-on: %d\n", ret);
            clk_disable_unprepare(i2s_tdm->mclk_tx);
            goto err_pm_disable;
        }
    }

    regmap_update_bits(i2s_tdm->regmap, I2S_DMACR, I2S_DMACR_TDL_MASK,
           I2S_DMACR_TDL(16));
    regmap_update_bits(i2s_tdm->regmap, I2S_DMACR, I2S_DMACR_RDL_MASK,
           I2S_DMACR_RDL(16));
    regmap_update_bits(i2s_tdm->regmap, I2S_CKR,
           I2S_CKR_TRCM_MASK, i2s_tdm->clk_trcm);

    /* Initialize MSS bit to MASTER mode (generate clocks) by default */
    regmap_update_bits(i2s_tdm->regmap, I2S_CKR,
           I2S_CKR_MSS_MASK, I2S_CKR_MSS_MASTER);
    i2s_tdm->is_master_mode = true;
    dev_info(&pdev->dev, "I2S initialized in MASTER mode (will generate BCLK/LRCK)\n");

    /* Apply default pinctrl state to enable I2S pins */
    ret = pinctrl_pm_select_default_state(&pdev->dev);
    if (ret) {
        dev_warn(&pdev->dev, "Failed to set default pinctrl state: %d\n", ret);
    } else {
        dev_info(&pdev->dev, "Applied default pinctrl state (I2S pins enabled)\n");
    }

    if (i2s_tdm->soc_data && i2s_tdm->soc_data->init)
    i2s_tdm->soc_data->init(&pdev->dev, res->start);

    ret = devm_snd_soc_register_component(&pdev->dev,
              &rockchip_i2s_tdm_component_with_copy,
              soc_dai, 1);

    if (ret) {
    dev_warn(&pdev->dev, "Failed to register component with copy support: %d\n", ret);
    dev_info(&pdev->dev, "Falling back to standard component (no volume processing)\n");
    
    /* Fallback to standard component */
    ret = devm_snd_soc_register_component(&pdev->dev,
                  &rockchip_i2s_tdm_component,
                  soc_dai, 1);
    if (ret) {
        dev_err(&pdev->dev, "Could not register DAI\n");
        goto err_suspend;
    }
    } else {
    dev_info(&pdev->dev, "Audiophile component registered successfully with copy callbacks\n");
    }

    if (of_property_read_bool(node, "rockchip,no-dmaengine"))
    return ret;

    if (of_property_read_bool(node, "rockchip,digital-loopback"))
    ret = devm_snd_dmaengine_dlp_register(&pdev->dev, &dconfig);
    else
    /* Use custom configuration with pause/resume support */
    ret = devm_snd_dmaengine_pcm_register(&pdev->dev, 
             &rockchip_i2s_tdm_dmaengine_pcm_config, 
             0);
    if (ret) {
    dev_err(&pdev->dev, "Could not register PCM\n");
    return ret;
    }

    /* Create sysfs attribute for MCLK multiplier switching */
    ret = device_create_file(&pdev->dev, &dev_attr_mclk_multiplier);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create mclk_multiplier sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }

    
    /* Create sysfs attribute for DSD sample swap */
    ret = device_create_file(&pdev->dev, &dev_attr_dsd_sample_swap);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create dsd_sample_swap sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }
    
    /* Create sysfs attribute for PCM channel swap */
    ret = device_create_file(&pdev->dev, &dev_attr_pcm_channel_swap);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create pcm_channel_swap sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }
    
    /* Create sysfs attribute for DSD physical swap */
    ret = device_create_file(&pdev->dev, &dev_attr_dsd_physical_swap);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create dsd_physical_swap sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }
    
    /* Create sysfs attribute for frequency domain GPIO polarity control */
    ret = device_create_file(&pdev->dev, &dev_attr_freq_domain_invert);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create freq_domain_invert sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }
    
    /* Create sysfs attribute for manual DSD mode control */
    ret = device_create_file(&pdev->dev, &dev_attr_dsd_mode_manual);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create dsd_mode_manual sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }
    
    /* Create sysfs attribute for postmute delay */
    ret = device_create_file(&pdev->dev, &dev_attr_postmute_delay_ms);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create postmute_delay_ms sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }
    
    /* Create sysfs attribute for mute control */
    ret = device_create_file(&pdev->dev, &dev_attr_mute);
    if (ret) {
        dev_err(&pdev->dev, "Failed to create mute sysfs attribute: %d\n", ret);
        /* Not critical, continue */
    }

    return 0;

err_suspend:
    if (!pm_runtime_status_suspended(&pdev->dev))
    i2s_tdm_runtime_suspend(&pdev->dev);
err_pm_disable:
    pm_runtime_disable(&pdev->dev);

    return ret;
}

static int rockchip_i2s_tdm_remove(struct platform_device *pdev)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(&pdev->dev);

    /* Cleanup auto-mute timer */
    cancel_delayed_work_sync(&i2s_tdm->mute_post_work);

    /* Remove sysfs attributes */
    device_remove_file(&pdev->dev, &dev_attr_mclk_multiplier);
    device_remove_file(&pdev->dev, &dev_attr_dsd_sample_swap);
    device_remove_file(&pdev->dev, &dev_attr_pcm_channel_swap);
    device_remove_file(&pdev->dev, &dev_attr_dsd_physical_swap);
    device_remove_file(&pdev->dev, &dev_attr_freq_domain_invert);

    pm_runtime_disable(&pdev->dev);
    if (!pm_runtime_status_suspended(&pdev->dev))
    i2s_tdm_runtime_suspend(&pdev->dev);

    /* Turn off MCLK regardless of quirk when removing driver */
    clk_disable_unprepare(i2s_tdm->mclk_tx);
    clk_disable_unprepare(i2s_tdm->mclk_rx);
    clk_disable_unprepare(i2s_tdm->hclk);

    return 0;
}

static void rockchip_i2s_tdm_platform_shutdown(struct platform_device *pdev)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(&pdev->dev);

    pm_runtime_get_sync(i2s_tdm->dev);
    rockchip_i2s_tdm_stop(i2s_tdm, SNDRV_PCM_STREAM_PLAYBACK);
    rockchip_i2s_tdm_stop(i2s_tdm, SNDRV_PCM_STREAM_CAPTURE);
    pm_runtime_put(i2s_tdm->dev);
}

#ifdef CONFIG_PM_SLEEP
static int rockchip_i2s_tdm_suspend(struct device *dev)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);

    regcache_mark_dirty(i2s_tdm->regmap);

    return 0;
}

static int rockchip_i2s_tdm_resume(struct device *dev)
{
    struct rk_i2s_tdm_dev *i2s_tdm = dev_get_drvdata(dev);
    int ret;

    ret = pm_runtime_get_sync(dev);
    if (ret < 0)
    return ret;

    ret = regcache_sync(i2s_tdm->regmap);

    pm_runtime_put(dev);

    return ret;
}
#endif

static const struct dev_pm_ops rockchip_i2s_tdm_pm_ops = {
    SET_RUNTIME_PM_OPS(i2s_tdm_runtime_suspend, i2s_tdm_runtime_resume,
           NULL)
    SET_SYSTEM_SLEEP_PM_OPS(rockchip_i2s_tdm_suspend,
    rockchip_i2s_tdm_resume)
};

static struct platform_driver rockchip_i2s_tdm_driver = {
    .probe = rockchip_i2s_tdm_probe,
    .remove = rockchip_i2s_tdm_remove,
    .shutdown = rockchip_i2s_tdm_platform_shutdown,
    .driver = {
    .name = DRV_NAME,
    .of_match_table = of_match_ptr(rockchip_i2s_tdm_match),
    .pm = &rockchip_i2s_tdm_pm_ops,
    },
};

module_platform_driver(rockchip_i2s_tdm_driver);

MODULE_DESCRIPTION("ROCKCHIP I2S/TDM ASoC Interface");
MODULE_AUTHOR("Sugar Zhang <sugar.zhang@rock-chips.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DEVICE_TABLE(of, rockchip_i2s_tdm_match);
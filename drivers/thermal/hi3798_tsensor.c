// SPDX-License-Identifier: GPL-2.0
/*
 * HiSilicon Hi3798CV200/MV200/MV300 on-chip T-Sensor (PMC)
 *
 * Formula from vendor SDK hi3798cvx-thermal.c
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define PERI_PMC10		0x28
#define PERI_PMC12		0x30

#define TSENSOR_CTRL_ENABLE	0x6005
#define TSENSOR_CTRL_DISABLE	0x0
#define TSENSOR_SETTLE_MS	30
#define TSENSOR_NUM_WORDS	4
#define TSENSOR_SAMPLES		8

/* EMA: out = (alpha * new + (256-alpha) * old) / 256, alpha=32 ≈ 1/8 */
#define TSENSOR_EMA_ALPHA	32
/* 单次最大变化 (m°C)，抑制异常跳变；0 表示不限制 */
#define TSENSOR_MAX_DELTA_MC	8000

struct hi3798_tsensor {
	void __iomem *base;
	struct mutex lock;
	struct thermal_zone_device *tz;
	int last_temp;		/* m°C，EMA 输出 */
	bool have_last;
};

static struct hi3798_tsensor *hi3798_ts_singleton;

static int hi3798_tsensor_sample_once(struct hi3798_tsensor *ts, u32 *avg_out)
{
	u32 sum = 0;
	u32 reg;
	int j, i;

	writel_relaxed(TSENSOR_CTRL_ENABLE, ts->base + PERI_PMC10);
	msleep(TSENSOR_SETTLE_MS);

	for (j = 0; j < TSENSOR_NUM_WORDS; j++) {
		reg = readl_relaxed(ts->base + PERI_PMC12 + 4 * j);
		for (i = 0; i < 2; i++)
			sum += (reg >> (16 * i)) & 0x3ff;
	}

	writel_relaxed(TSENSOR_CTRL_DISABLE, ts->base + PERI_PMC10);

	*avg_out = sum / TSENSOR_SAMPLES;
	return 0;
}

/* 厂商公式 → 毫摄氏度 */
static int hi3798_avg_to_mC(u32 avg)
{
	s32 t_c = ((s32)avg - 125) * 165 / 806 - 40;

	return t_c * 1000;
}

static int hi3798_tsensor_read_mC(struct hi3798_tsensor *ts, int *temp)
{
	u32 avg_discard, avg;
	int raw, out;

	mutex_lock(&ts->lock);

	/* 第一次采样丢弃（上电/使能后未稳定） */
	hi3798_tsensor_sample_once(ts, &avg_discard);
	/* 间隔后再采有效值 */
	msleep(5);
	hi3798_tsensor_sample_once(ts, &avg);

	raw = hi3798_avg_to_mC(avg);

	if (!ts->have_last) {
		ts->last_temp = raw;
		ts->have_last = true;
		out = raw;
	} else {
#if TSENSOR_MAX_DELTA_MC > 0
		if (raw > ts->last_temp + TSENSOR_MAX_DELTA_MC)
			raw = ts->last_temp + TSENSOR_MAX_DELTA_MC;
		else if (raw < ts->last_temp - TSENSOR_MAX_DELTA_MC)
			raw = ts->last_temp - TSENSOR_MAX_DELTA_MC;
#endif
		/* EMA */
		out = (TSENSOR_EMA_ALPHA * raw +
		       (256 - TSENSOR_EMA_ALPHA) * ts->last_temp) / 256;
		ts->last_temp = out;
	}

	mutex_unlock(&ts->lock);

	*temp = out;
	return 0;
}

static int hi3798_tsensor_get_temp(struct thermal_zone_device *tz, int *temp)
{
	if (!hi3798_ts_singleton)
		return -ENODEV;

	return hi3798_tsensor_read_mC(hi3798_ts_singleton, temp);
}

static const struct thermal_zone_device_ops hi3798_tsensor_ops = {
	.get_temp = hi3798_tsensor_get_temp,
};

static int hi3798_tsensor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi3798_tsensor *ts;
	struct resource *res;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	mutex_init(&ts->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ts->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(ts->base))
		return PTR_ERR(ts->base);

	platform_set_drvdata(pdev, ts);
	hi3798_ts_singleton = ts;

	ts->tz = thermal_zone_device_register_with_trips("cpu-thermal",
							 NULL, 0, 0,
							 &hi3798_tsensor_ops,
							 NULL, 0, 1000);
	if (IS_ERR(ts->tz)) {
		hi3798_ts_singleton = NULL;
		return dev_err_probe(dev, PTR_ERR(ts->tz),
				     "thermal_zone register failed\n");
	}

	dev_info(dev, "Hi3798 PMC T-Sensor registered\n");
	return 0;
}

static void hi3798_tsensor_remove(struct platform_device *pdev)
{
	struct hi3798_tsensor *ts = platform_get_drvdata(pdev);

	if (ts && !IS_ERR_OR_NULL(ts->tz))
		thermal_zone_device_unregister(ts->tz);
	hi3798_ts_singleton = NULL;
}

static const struct of_device_id hi3798_tsensor_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv300-tsensor" },
	{ .compatible = "hisilicon,hi3798mv200-tsensor" },
	{ .compatible = "hisilicon,hi3798cv200-tsensor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hi3798_tsensor_of_match);

static struct platform_driver hi3798_tsensor_driver = {
	.probe = hi3798_tsensor_probe,
	.remove = hi3798_tsensor_remove,
	.driver = {
		.name = "hi3798-tsensor",
		.of_match_table = hi3798_tsensor_of_match,
	},
};
module_platform_driver(hi3798_tsensor_driver);

MODULE_DESCRIPTION("HiSilicon Hi3798 PMC T-Sensor driver");
MODULE_AUTHOR("Hi3798 mainline port");
MODULE_LICENSE("GPL");

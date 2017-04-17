/*
 * Microchip RN2483/RN2903
 *
 * Copyright (c) 2017 Andreas Färber
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/serdev.h>

#include "lora.h"

#define RN2483_CMD_TIMEOUT HZ

struct rn2483_device {
	struct serdev_device *serdev;
	struct gpio_desc *reset_gpio;
	unsigned model;
	lora_eui hweui;
	unsigned band;
	bool saw_cr;
	void *buf;
	size_t buflen;
	struct completion line_recv_comp;
	struct completion line_read_comp;
	struct mutex cmd_lock;
};

static int rn2483_readline_timeout(struct rn2483_device *rndev, char **line, unsigned long timeout);

static int rn2483_send_command_timeout(struct rn2483_device *rndev,
	const char *cmd, char **resp, unsigned long timeout)
{
	int ret;

	ret = serdev_device_write_buf(rndev->serdev, cmd, strlen(cmd));
	if (ret < 0)
		return ret;

	ret = serdev_device_write_buf(rndev->serdev, "\r\n", 2);
	if (ret < 0)
		return ret;

	return rn2483_readline_timeout(rndev, resp, timeout);
}

static int rn2483_sys_get_hweui(struct rn2483_device *rndev, lora_eui *val)
{
	int ret;
	char *line;

	mutex_lock(&rndev->cmd_lock);
	ret = rn2483_send_command_timeout(rndev, "sys get hweui", &line, RN2483_CMD_TIMEOUT);
	mutex_unlock(&rndev->cmd_lock);
	if (ret)
		return ret;

	ret = lora_strtoeui(line, val);
	devm_kfree(&rndev->serdev->dev, line);
	return ret;
}

static int rn2483_mac_get_band(struct rn2483_device *rndev, uint *val)
{
	int ret;
	char *line;

	mutex_lock(&rndev->cmd_lock);
	ret = rn2483_send_command_timeout(rndev, "mac get band", &line, RN2483_CMD_TIMEOUT);
	mutex_unlock(&rndev->cmd_lock);
	if (ret)
		return ret;

	ret = kstrtouint(line, 10, val);
	devm_kfree(&rndev->serdev->dev, line);

	return ret;
}

static int rn2483_mac_get_status(struct rn2483_device *rndev, u32 *val)
{
	int ret;
	char *line;

	mutex_lock(&rndev->cmd_lock);
	ret = rn2483_send_command_timeout(rndev, "mac get status", &line, RN2483_CMD_TIMEOUT);
	mutex_unlock(&rndev->cmd_lock);
	if (ret)
		return ret;

	ret = kstrtou32(line, 16, val);
	devm_kfree(&rndev->serdev->dev, line);
	return ret;
}

static int rn2483_mac_reset_band(struct rn2483_device *rndev, unsigned band)
{
	int ret;
	char *line, *cmd;

	cmd = devm_kasprintf(&rndev->serdev->dev, GFP_KERNEL, "mac reset %u", band);
	mutex_lock(&rndev->cmd_lock);
	ret = rn2483_send_command_timeout(rndev, cmd, &line, RN2483_CMD_TIMEOUT);
	mutex_unlock(&rndev->cmd_lock);
	devm_kfree(&rndev->serdev->dev, cmd);
	if (ret)
		return ret;

	if (strcmp(line, "ok") == 0)
		ret = 0;
	else if (strcmp(line, "invalid_param") == 0)
		ret = -EINVAL;
	else
		ret = -EPROTO;

	devm_kfree(&rndev->serdev->dev, line);
	return ret;
}

static int rn2483_mac_pause(struct rn2483_device *rndev, u32 *max_pause)
{
	int ret;
	char *line;

	mutex_lock(&rndev->cmd_lock);
	ret = rn2483_send_command_timeout(rndev, "mac pause", &line, RN2483_CMD_TIMEOUT);
	mutex_unlock(&rndev->cmd_lock);
	if (ret)
		return ret;

	ret = kstrtou32(line, 10, max_pause);
	devm_kfree(&rndev->serdev->dev, line);
	return ret;
}

static int rn2483_mac_resume(struct rn2483_device *rndev)
{
	int ret;
	char *line;

	mutex_lock(&rndev->cmd_lock);
	ret = rn2483_send_command_timeout(rndev, "mac resume", &line, RN2483_CMD_TIMEOUT);
	mutex_unlock(&rndev->cmd_lock);
	if (ret)
		return ret;

	ret = (strcmp(line, "ok") == 0) ? 0 : -EPROTO;
	devm_kfree(&rndev->serdev->dev, line);
	return ret;
}

static int rn2483_readline_timeout(struct rn2483_device *rndev, char **line, unsigned long timeout)
{
	timeout = wait_for_completion_timeout(&rndev->line_recv_comp, timeout);
	if (!timeout)
		return -ETIMEDOUT;

	*line = devm_kstrdup(&rndev->serdev->dev, rndev->buf, GFP_KERNEL);
	complete(&rndev->line_read_comp);
	if (!*line)
		return -ENOMEM;

	return 0;
}

static void rn2483_receive_line(struct rn2483_device *rndev, const char *sz, size_t len)
{
	dev_dbg(&rndev->serdev->dev, "Received line '%s' (%d)", sz, (int)len);

	reinit_completion(&rndev->line_read_comp);
	complete(&rndev->line_recv_comp);
	wait_for_completion(&rndev->line_read_comp);
	reinit_completion(&rndev->line_recv_comp);
}

static int rn2483_receive_buf(struct serdev_device *serdev, const u8 *data, size_t count)
{
	struct rn2483_device *rndev = serdev_device_get_drvdata(serdev);
	size_t i;

	dev_dbg(&serdev->dev, "Receive (%d)", (int)count);
	if (!rndev->buf) {
		rndev->buf = devm_kmalloc(&serdev->dev, count, GFP_KERNEL);
		if (!rndev->buf)
			return 0;
		rndev->buflen = 0;
	} else {
		void *tmp = devm_kmalloc(&serdev->dev, rndev->buflen + count, GFP_KERNEL);
		if (!tmp)
			return 0;
		memcpy(tmp, rndev->buf, rndev->buflen);
		devm_kfree(&serdev->dev, rndev->buf);
		rndev->buf = tmp;
	}

	for (i = 0; i < count; i++) {
		if (data[i] == '\r') {
			rndev->saw_cr = true;
		} else if (data[i] == '\n' && rndev->saw_cr) {
			if (i > 1)
				memcpy(rndev->buf + rndev->buflen, data, i - 1);
			((char *)rndev->buf)[rndev->buflen + i - 1] = 0;
			rn2483_receive_line(rndev, rndev->buf, rndev->buflen + i - 1);
			rndev->saw_cr = false;
			devm_kfree(&serdev->dev, rndev->buf);
			rndev->buf = NULL;
			rndev->buflen = 0;
			return i + 1;
		} else
			rndev->saw_cr = false;
	}

	memcpy(rndev->buf + rndev->buflen, data, count);
	rndev->buflen += count;
	return count;
}

static const struct serdev_device_ops rn2483_serdev_client_ops = {
	.receive_buf = rn2483_receive_buf,
};

static int rn2483_probe(struct serdev_device *sdev)
{
	struct rn2483_device *rndev;
	char *line, *cmd;
	char sz[5];
	u32 status;
	int ret;

	dev_info(&sdev->dev, "Probing");

	rndev = devm_kzalloc(&sdev->dev, sizeof(struct rn2483_device), GFP_KERNEL);
	if (!rndev)
		return -ENOMEM;

	rndev->serdev = sdev;
	init_completion(&rndev->line_recv_comp);
	init_completion(&rndev->line_read_comp);
	mutex_init(&rndev->cmd_lock);
	serdev_device_set_drvdata(sdev, rndev);

	rndev->reset_gpio = devm_gpiod_get_optional(&sdev->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(rndev->reset_gpio))
		return PTR_ERR(rndev->reset_gpio);

	ret = serdev_device_open(sdev);
	if (ret) {
		dev_err(&sdev->dev, "Failed to open (%d)", ret);
		return ret;
	}

	serdev_device_set_baudrate(sdev, 57600);
	serdev_device_set_flow_control(sdev, false);

	gpiod_set_value_cansleep(rndev->reset_gpio, 0);
	msleep(5);
	serdev_device_set_client_ops(sdev, &rn2483_serdev_client_ops);
	gpiod_set_value_cansleep(rndev->reset_gpio, 1);
	msleep(100);

	ret = rn2483_readline_timeout(rndev, &line, HZ);
	if (ret) {
		if (ret != -ENOMEM)
			dev_err(&sdev->dev, "Timeout waiting for firmware identification");
		goto err_timeout;
	}

	if (strlen(line) < strlen("RNxxxx X.Y.Z MMM DD YYYY HH:MM:SS") || line[6] != ' ' ||
			strncmp(line, "RN", 2) != 0) {
		dev_err(&sdev->dev, "Unexpected response '%s'", line);
		devm_kfree(&sdev->dev, line);
		ret = -EINVAL;
		goto err_version;
	}
	dev_info(&sdev->dev, "Firmware '%s'", line);
	strncpy(sz, line + 2, 4);
	sz[4] = 0;
	devm_kfree(&sdev->dev, line);
	ret = kstrtouint(sz, 10, &rndev->model);
	if (ret)
		goto err_model;
	if (!(rndev->model == 2483 || rndev->model == 2903)) {
		dev_err(&sdev->dev, "Unknown model %u", rndev->model);
		ret = -ENOTSUPP;
		goto err_model;
	}
	dev_info(&sdev->dev, "Detected RN%u", rndev->model);

	ret = rn2483_sys_get_hweui(rndev, &rndev->hweui);
	if (ret) {
		if (ret != -ENOMEM)
			dev_err(&sdev->dev, "Failed to read HWEUI (%d)", ret);
		goto err_hweui;
	}
	dev_info(&sdev->dev, "HWEUI " PRIxLORAEUI, LORA_EUI(rndev->hweui));

	switch (rndev->model) {
	case 2483:
		ret = rn2483_mac_get_band(rndev, &rndev->band);
		if (ret) {
			dev_err(&sdev->dev, "Failed to read band (%d)", ret);
			goto err_band;
		}
		dev_info(&sdev->dev, "Frequency band %u MHz", rndev->band);

		ret = rn2483_mac_reset_band(rndev, 433);
		if (ret) {
			dev_err(&sdev->dev, "Failed to reset band (%d)", ret);
			goto err_band;
		}
		rndev->band = 433;

		ret = rn2483_mac_get_band(rndev, &rndev->band);
		if (!ret)
			dev_info(&sdev->dev, "New frequency band: %u MHz", rndev->band);
		break;
	case 2903:
		/* No "mac get band" command available */
		rndev->band = 915;
		break;
	}

	ret = rn2483_mac_get_status(rndev, &status);
	if (!ret)
		dev_info(&sdev->dev, "MAC status %08x", status);

	if (true) {
		u32 pause;
		ret = rn2483_mac_pause(rndev, &pause);
		if (!ret)
			dev_info(&sdev->dev, "MAC pausing (0x%08x)", pause);
		ret = rn2483_mac_resume(rndev);
		if (!ret)
			dev_info(&sdev->dev, "MAC resuming");
	}

	cmd = "mac get sync";
	mutex_lock(&rndev->cmd_lock);
	ret = rn2483_send_command_timeout(rndev, cmd, &line, HZ);
	mutex_unlock(&rndev->cmd_lock);
	if (!ret) {
		dev_info(&sdev->dev, "%s => '%s'", cmd, line);
		devm_kfree(&sdev->dev, line);
	}

	return 0;

err_band:
err_hweui:
err_model:
err_version:
err_timeout:
	gpiod_set_value_cansleep(rndev->reset_gpio, 0);
	return ret;
}

static void rn2483_remove(struct serdev_device *sdev)
{
	struct rn2483_device *rndev = serdev_device_get_drvdata(sdev);

	gpiod_set_value_cansleep(rndev->reset_gpio, 0);

	complete(&rndev->line_read_comp);

	serdev_device_close(sdev);

	dev_info(&sdev->dev, "Removed");
}

static const struct of_device_id rn2483_of_match[] = {
	{ .compatible = "microchip,rn2483" },
	{ .compatible = "microchip,rn2903" },
	{}
};
MODULE_DEVICE_TABLE(of, rn2483_of_match);

static struct serdev_device_driver rn2483_serdev_driver = {
	.probe = rn2483_probe,
	.remove = rn2483_remove,
	.driver = {
		.name = "rn2483",
		.of_match_table = rn2483_of_match,
	},
};

static int __init rn2483_init(void)
{
	int ret;

	ret = serdev_device_driver_register(&rn2483_serdev_driver);
	if (ret)
		return ret;

	return 0;
}

static void __exit rn2483_exit(void)
{
	serdev_device_driver_unregister(&rn2483_serdev_driver);
}

module_init(rn2483_init);
module_exit(rn2483_exit);

MODULE_DESCRIPTION("RN2483 serdev driver");
MODULE_AUTHOR("Andreas Färber <afaerber@suse.de>");
MODULE_LICENSE("GPL");

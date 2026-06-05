/*
 * Copyright (c) 2025, Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys_clock.h>
#include <zephyr/logging/log.h>

#include <xc.h>

#include "i2c_mchp_dspic33_g1.h"

#define DT_DRV_COMPAT microchip_dspic33_i2c

LOG_MODULE_REGISTER(i2c_mchp_dspic33, CONFIG_I2C_LOG_LEVEL);

#define I2C_TIMEOUT_US    10000
#define I2C_POLL_DELAY_US 2
#define I2C_RECOVERY_CLK_COUNT 9

struct i2c_dspic_config {
	uint32_t base;
	const struct pinctrl_dev_config *pcfg;
	unsigned int gen_irq_num;
	unsigned int err_irq_num;
	unsigned int rx_irq_num;
	unsigned int tx_irq_num;
	struct gpio_dt_spec scl_gpio;
	struct gpio_dt_spec sda_gpio;
#ifdef CONFIG_I2C_TARGET
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct i2c_dspic_data {
	struct k_spinlock lock;
	uint32_t dev_config;
#ifdef CONFIG_I2C_TARGET
	struct i2c_target_config *target_cfg;
	bool target_mode;
#endif
};

static inline uint32_t i2c_dspic_get_fcy(void)
{
	return sys_clock_hw_cycles_per_sec() / 2U;
}

static inline uint32_t i2c_dspic_calculate_brg(uint32_t bitrate)
{
	uint32_t fcy = i2c_dspic_get_fcy();
	uint32_t brg;

	if (bitrate == 0U) {
		return 4U;
	}
	brg = (fcy / (2U * bitrate)) - 1U;
	if (brg < 4U) {
		brg = 4U;
	}
	return brg;
}

static void i2c_dspic_module_enable(const struct i2c_dspic_config *cfg, bool enable)
{
	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;


	if (enable) {
		*con1 |= I2C_CON1_ON;
	} else {
		*con1 &= ~I2C_CON1_ON;
	}
}

static void i2c_dspic_wait_bus_idle(const struct i2c_dspic_config *cfg)
{
	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;


	WAIT_FOR(!(*con1 & (I2C_CON1_SEN | I2C_CON1_RSEN |
			    I2C_CON1_PEN | I2C_CON1_RCEN |
			    I2C_CON1_ACKEN)),
		 I2C_TIMEOUT_US, k_busy_wait(I2C_POLL_DELAY_US));
}

static int i2c_dspic_send_start(const struct i2c_dspic_config *cfg)
{
	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;


	*con1 |= I2C_CON1_SEN;

	if (WAIT_FOR(!(*con1 & I2C_CON1_SEN),
		     I2C_TIMEOUT_US, k_busy_wait(I2C_POLL_DELAY_US)) == false) {
		LOG_ERR("Start condition timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

static int i2c_dspic_send_restart(const struct i2c_dspic_config *cfg)
{
	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;


	*con1 |= I2C_CON1_RSEN;

	if (WAIT_FOR(!(*con1 & I2C_CON1_RSEN),
		     I2C_TIMEOUT_US, k_busy_wait(I2C_POLL_DELAY_US)) == false) {
		LOG_ERR("Restart condition timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

static int i2c_dspic_send_stop(const struct i2c_dspic_config *cfg)
{
	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;


	*con1 |= I2C_CON1_PEN;

	if (WAIT_FOR(!(*con1 & I2C_CON1_PEN),
		     I2C_TIMEOUT_US, k_busy_wait(I2C_POLL_DELAY_US)) == false) {
		LOG_ERR("Stop condition timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

static int i2c_dspic_send_byte(const struct i2c_dspic_config *cfg, uint8_t byte)
{
	volatile uint32_t *trn = (volatile uint32_t *)(cfg->base + I2C_OFFSET_TRN);
	volatile uint32_t *stat1 = (volatile uint32_t *)(cfg->base + I2C_OFFSET_STAT1);


	*trn = byte;

	if (WAIT_FOR(!(*stat1 & I2C_STAT1_TRSTAT),
		     I2C_TIMEOUT_US, k_busy_wait(I2C_POLL_DELAY_US)) == false) {
		LOG_ERR("Transmit timeout");
		return -ETIMEDOUT;
	}

	if (*stat1 & I2C_STAT1_ACKSTAT) {
		return -EIO;
	}

	return 0;
}

static int i2c_dspic_recv_byte(const struct i2c_dspic_config *cfg, uint8_t *byte,
			       bool send_ack)
{
	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;
	volatile uint32_t *rcv = (volatile uint32_t *)(cfg->base + I2C_OFFSET_RCV);


	*con1 |= I2C_CON1_RCEN;

	if (WAIT_FOR(!(*con1 & I2C_CON1_RCEN),
		     I2C_TIMEOUT_US, k_busy_wait(I2C_POLL_DELAY_US)) == false) {
		LOG_ERR("Receive timeout");
		return -ETIMEDOUT;
	}

	*byte = (uint8_t)(*rcv & I2C_RCV_DATA_MASK);

	/* Send ACK or NACK */
	if (send_ack) {
		*con1 &= ~I2C_CON1_ACKDT;
	} else {
		*con1 |= I2C_CON1_ACKDT;
	}

	*con1 |= I2C_CON1_ACKEN;

	if (WAIT_FOR(!(*con1 & I2C_CON1_ACKEN),
		     I2C_TIMEOUT_US, k_busy_wait(I2C_POLL_DELAY_US)) == false) {
		LOG_ERR("ACK/NACK timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

static int i2c_dspic_check_bus_error(const struct i2c_dspic_config *cfg)
{
	volatile uint32_t *stat1 = (volatile uint32_t *)(cfg->base + I2C_OFFSET_STAT1);


	if (*stat1 & I2C_STAT1_BCL) {
		*stat1 &= ~I2C_STAT1_BCL;
		LOG_ERR("Bus collision detected");
		return -EAGAIN;
	}

	if (*stat1 & I2C_STAT1_IWCOL) {
		*stat1 &= ~I2C_STAT1_IWCOL;
		LOG_ERR("Write collision detected");
		return -EIO;
	}

	if (*stat1 & I2C_STAT1_I2COV) {
		*stat1 &= ~I2C_STAT1_I2COV;
		LOG_ERR("Receive overflow detected");
		return -EIO;
	}

	return 0;
}

static int i2c_dspic_write_msg(const struct device *dev, struct i2c_msg *msg,
			       uint16_t addr, bool is_first, bool is_last)
{
	const struct i2c_dspic_config *cfg = dev->config;
	bool is_10bit = (msg->flags & I2C_MSG_ADDR_10_BITS) != 0;
	int ret;

	if (is_first || (msg->flags & I2C_MSG_RESTART)) {
		if (is_first) {
			ret = i2c_dspic_send_start(cfg);
		} else {
			ret = i2c_dspic_send_restart(cfg);
		}
		if (ret != 0) {
			return ret;
		}

		if (is_10bit) {
			ret = i2c_dspic_send_byte(cfg,
				(uint8_t)(0xF0U | ((addr >> 7U) & 0x06U)));
			if (ret != 0) {
				i2c_dspic_send_stop(cfg);
				return ret;
			}
			ret = i2c_dspic_send_byte(cfg,
				(uint8_t)(addr & 0xFFU));
		} else {
			ret = i2c_dspic_send_byte(cfg,
				(uint8_t)(addr << 1U));
		}
		if (ret != 0) {
			i2c_dspic_send_stop(cfg);
			return ret;
		}
	}

	for (uint32_t i = 0U; i < msg->len; i++) {
		ret = i2c_dspic_check_bus_error(cfg);
		if (ret != 0) {
			i2c_dspic_send_stop(cfg);
			return ret;
		}

		ret = i2c_dspic_send_byte(cfg, msg->buf[i]);
		if (ret != 0) {
			i2c_dspic_send_stop(cfg);
			return ret;
		}
	}

	if (is_last || (msg->flags & I2C_MSG_STOP)) {
		ret = i2c_dspic_send_stop(cfg);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int i2c_dspic_read_msg(const struct device *dev, struct i2c_msg *msg,
			      uint16_t addr, bool is_first, bool is_last)
{
	const struct i2c_dspic_config *cfg = dev->config;
	bool is_10bit = (msg->flags & I2C_MSG_ADDR_10_BITS) != 0;
	int ret;

	if (is_first || (msg->flags & I2C_MSG_RESTART)) {
		if (is_first) {
			ret = i2c_dspic_send_start(cfg);
		} else {
			ret = i2c_dspic_send_restart(cfg);
		}
		if (ret != 0) {
			return ret;
		}

		if (is_10bit) {
			/* 10-bit read: send address in write mode first */
			ret = i2c_dspic_send_byte(cfg,
				(uint8_t)(0xF0U | ((addr >> 7U) & 0x06U)));
			if (ret != 0) {
				i2c_dspic_send_stop(cfg);
				return ret;
			}
			ret = i2c_dspic_send_byte(cfg,
				(uint8_t)(addr & 0xFFU));
			if (ret != 0) {
				i2c_dspic_send_stop(cfg);
				return ret;
			}
			/* Restart with read bit set (high byte only) */
			ret = i2c_dspic_send_restart(cfg);
			if (ret != 0) {
				i2c_dspic_send_stop(cfg);
				return ret;
			}
			ret = i2c_dspic_send_byte(cfg,
				(uint8_t)(0xF1U | ((addr >> 7U) & 0x06U)));
		} else {
			ret = i2c_dspic_send_byte(cfg,
				(uint8_t)((addr << 1U) | 0x01U));
		}
		if (ret != 0) {
			i2c_dspic_send_stop(cfg);
			return ret;
		}
	}

	for (uint32_t i = 0U; i < msg->len; i++) {
		ret = i2c_dspic_check_bus_error(cfg);
		if (ret != 0) {
			i2c_dspic_send_stop(cfg);
			return ret;
		}

		bool send_ack = (i < (msg->len - 1U));

		ret = i2c_dspic_recv_byte(cfg, &msg->buf[i], send_ack);
		if (ret != 0) {
			i2c_dspic_send_stop(cfg);
			return ret;
		}
	}

	if (is_last || (msg->flags & I2C_MSG_STOP)) {
		ret = i2c_dspic_send_stop(cfg);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int i2c_dspic_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_dspic_config *cfg = dev->config;
	struct i2c_dspic_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t bitrate;
	uint32_t brg;


	if (!(dev_config & I2C_MODE_CONTROLLER)) {
		LOG_ERR("Only controller mode is supported via configure API");
		return -EINVAL;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		bitrate = KHZ(100);
		break;
	case I2C_SPEED_FAST:
		bitrate = KHZ(400);
		break;
	case I2C_SPEED_FAST_PLUS:
		bitrate = MHZ(1);
		break;
	default:
		LOG_ERR("Unsupported I2C speed: %u", I2C_SPEED_GET(dev_config));
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);

	i2c_dspic_module_enable(cfg, false);

	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;
	volatile uint32_t *hbrg = (volatile uint32_t *)(cfg->base + I2C_OFFSET_HBRG);
	volatile uint32_t *lbrg = (volatile uint32_t *)(cfg->base + I2C_OFFSET_LBRG);

	*con1 = 0U;
	if (bitrate == KHZ(400)) {
		*con1 |= I2C_CON1_DISSLW;
	}

	brg = i2c_dspic_calculate_brg(bitrate);
	*hbrg = brg;
	*lbrg = brg;

	*con1 |= I2C_CON1_SCLREL;
	i2c_dspic_module_enable(cfg, true);

	data->dev_config = dev_config;

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int i2c_dspic_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct i2c_dspic_data *data = dev->data;

	if (data->dev_config == 0U) {
		return -EINVAL;
	}

	*dev_config = data->dev_config;

	return 0;
}

static int i2c_dspic_transfer(const struct device *dev, struct i2c_msg *msgs,
			      uint8_t num_msgs, uint16_t addr)
{
	struct i2c_dspic_data *data = dev->data;
	const struct i2c_dspic_config *cfg = dev->config;
	k_spinlock_key_t key;
	int ret = 0;

	if (num_msgs == 0U) {
		return -EINVAL;
	}

#ifdef CONFIG_I2C_TARGET
	if (data->target_mode) {
		LOG_ERR("Device in target mode");
		return -EBUSY;
	}
#endif

	key = k_spin_lock(&data->lock);

	i2c_dspic_wait_bus_idle(cfg);

	for (uint8_t i = 0U; i < num_msgs; i++) {
		bool is_first = (i == 0U);
		bool is_last = (i == (num_msgs - 1U));
		bool is_read = ((msgs[i].flags & I2C_MSG_RW_MASK) == I2C_MSG_READ);

		if (is_read) {
			ret = i2c_dspic_read_msg(dev, &msgs[i], addr,
						 is_first, is_last);
		} else {
			ret = i2c_dspic_write_msg(dev, &msgs[i], addr,
						  is_first, is_last);
		}

		if (ret != 0) {
			LOG_DBG("I2C transfer failed at msg %u: %d", i, ret);
			break;
		}
	}

	k_spin_unlock(&data->lock, key);

	return ret;
}

static int i2c_dspic_recover_bus(const struct device *dev)
{
	const struct i2c_dspic_config *cfg = dev->config;
	struct i2c_dspic_data *data = dev->data;
	k_spinlock_key_t key;

	if (!gpio_is_ready_dt(&cfg->scl_gpio) ||
	    !gpio_is_ready_dt(&cfg->sda_gpio)) {
		LOG_WRN("GPIO not available for bus recovery, resetting module");
		i2c_dspic_module_enable(cfg, false);
		k_busy_wait(100);
		i2c_dspic_module_enable(cfg, true);
		return i2c_dspic_configure(dev, data->dev_config);
	}

	key = k_spin_lock(&data->lock);

	i2c_dspic_module_enable(cfg, false);

	gpio_pin_configure_dt(&cfg->sda_gpio, GPIO_INPUT);
	gpio_pin_configure_dt(&cfg->scl_gpio, GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN);

	for (int i = 0; i < I2C_RECOVERY_CLK_COUNT; i++) {
		k_busy_wait(5);
		gpio_pin_set_dt(&cfg->scl_gpio, 0);
		k_busy_wait(5);
		gpio_pin_set_dt(&cfg->scl_gpio, 1);

		if (gpio_pin_get_dt(&cfg->sda_gpio) != 0) {
			LOG_DBG("SDA released after %d clocks", i + 1);
			break;
		}
	}

	/* Generate STOP: SDA LOW -> HIGH while SCL is HIGH */
	gpio_pin_configure_dt(&cfg->sda_gpio, GPIO_OUTPUT_LOW | GPIO_OPEN_DRAIN);
	k_busy_wait(5);
	gpio_pin_set_dt(&cfg->scl_gpio, 1);
	k_busy_wait(5);
	gpio_pin_set_dt(&cfg->sda_gpio, 1);
	k_busy_wait(5);

	/* Release pins back to I2C module */
	gpio_pin_configure_dt(&cfg->scl_gpio, GPIO_INPUT);
	gpio_pin_configure_dt(&cfg->sda_gpio, GPIO_INPUT);

	k_spin_unlock(&data->lock, key);

	/* Re-apply pinctrl and reconfigure */
	pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	return i2c_dspic_configure(dev, data->dev_config);
}

#ifdef CONFIG_I2C_TARGET

static void i2c_dspic_target_isr(const struct device *dev)
{
	const struct i2c_dspic_config *cfg = dev->config;
	struct i2c_dspic_data *data = dev->data;
	volatile uint32_t *stat1 = (volatile uint32_t *)(cfg->base + I2C_OFFSET_STAT1);
	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;
	volatile uint32_t *rcv = (volatile uint32_t *)(cfg->base + I2C_OFFSET_RCV);
	volatile uint32_t *trn = (volatile uint32_t *)(cfg->base + I2C_OFFSET_TRN);

	uint32_t status = *stat1;
	const struct i2c_target_callbacks *cb;

	if (data->target_cfg == NULL || data->target_cfg->callbacks == NULL) {
		return;
	}

	cb = data->target_cfg->callbacks;

	/* Check for error interrupt */
	if (arch_dspic_irq_isset(cfg->err_irq_num)) {
		*stat1 &= ~(I2C_STAT1_BCL | I2C_STAT1_IWCOL | I2C_STAT1_I2COV);
		if (cb->stop != NULL) {
			cb->stop(data->target_cfg);
		}
		return;
	}

	/* General interrupt - handle address match, data, stop */
	if (arch_dspic_irq_isset(cfg->gen_irq_num)) {
		/* Stop condition detected */
		if (status & I2C_STAT1_P) {
			if (cb->stop != NULL) {
				cb->stop(data->target_cfg);
			}
			return;
		}

		/* Address match (D/A = 0) */
		if (!(status & I2C_STAT1_DA)) {
			if (status & I2C_STAT1_RW) {
				/* Host wants to read from us */
				uint8_t val = 0U;

				if (cb->read_requested != NULL) {
					cb->read_requested(data->target_cfg, &val);
				}
				*trn = val;
			} else {
				/* Host wants to write to us */
				if (cb->write_requested != NULL) {
					cb->write_requested(data->target_cfg);
				}
			}
			/* Release clock */
			*con1 |= I2C_CON1_SCLREL;
			return;
		}

		/* Data phase */
		if (status & I2C_STAT1_DA) {
			if (status & I2C_STAT1_RW) {
				/* Host reading - provide next byte */
				uint8_t val = 0U;

				if (!(status & I2C_STAT1_ACKSTAT)) {
					if (cb->read_processed != NULL) {
						cb->read_processed(data->target_cfg,
								   &val);
					}
					*trn = val;
				}
			} else {
				/* Host writing - receive byte */
				uint8_t val = (uint8_t)(*rcv & I2C_RCV_DATA_MASK);

				if (cb->write_received != NULL) {
					cb->write_received(data->target_cfg, val);
				}
			}
			/* Release clock */
			*con1 |= I2C_CON1_SCLREL;
		}
	}
}

static int i2c_dspic_target_register(const struct device *dev,
				     struct i2c_target_config *target_cfg)
{
	const struct i2c_dspic_config *cfg = dev->config;
	struct i2c_dspic_data *data = dev->data;
	k_spinlock_key_t key;


	if (target_cfg == NULL || target_cfg->callbacks == NULL) {
		return -EINVAL;
	}

	if (data->target_mode) {
		return -EBUSY;
	}

	key = k_spin_lock(&data->lock);

	i2c_dspic_module_enable(cfg, false);

	data->target_cfg = target_cfg;
	data->target_mode = true;

	volatile uint32_t *con1 = (volatile uint32_t *)cfg->base;
	volatile uint32_t *add = (volatile uint32_t *)(cfg->base + I2C_OFFSET_ADD);

	/* Set client address */
	*add = target_cfg->address;

	/* Enable clock stretching, stop/start interrupts */
	*con1 |= I2C_CON1_STREN | I2C_CON1_PCIE | I2C_CON1_SCIE |
		 I2C_CON1_AHEN | I2C_CON1_DHEN | I2C_CON1_SCLREL;

	i2c_dspic_module_enable(cfg, true);

	/* Enable interrupts */
	irq_enable(cfg->gen_irq_num);
	irq_enable(cfg->err_irq_num);

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int i2c_dspic_target_unregister(const struct device *dev,
				       struct i2c_target_config *target_cfg)
{
	const struct i2c_dspic_config *cfg = dev->config;
	struct i2c_dspic_data *data = dev->data;
	k_spinlock_key_t key;

	if (!data->target_mode) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	/* Disable interrupts */
	irq_disable(cfg->gen_irq_num);
	irq_disable(cfg->err_irq_num);

	i2c_dspic_module_enable(cfg, false);

	volatile uint32_t *add = (volatile uint32_t *)(cfg->base + I2C_OFFSET_ADD);

	*add = 0U;

	data->target_cfg = NULL;
	data->target_mode = false;

	k_spin_unlock(&data->lock, key);

	return 0;
}

#endif /* CONFIG_I2C_TARGET */

static int i2c_dspic_init(const struct device *dev)
{
	const struct i2c_dspic_config *cfg = dev->config;
	uint32_t dev_config;
	int ret;

	LOG_DBG("I2C init: base=0x%x", cfg->base);

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		LOG_ERR("Failed to configure I2C pins: %d", ret);
		return ret;
	}

	/* Bus recovery: unstick slaves holding SDA low from power-up */
	if (gpio_is_ready_dt(&cfg->sda_gpio) &&
	    gpio_is_ready_dt(&cfg->scl_gpio)) {
		gpio_pin_configure_dt(&cfg->sda_gpio, GPIO_INPUT);
		if (gpio_pin_get_dt(&cfg->sda_gpio) == 0) {
			LOG_WRN("SDA stuck low at init, recovering bus");
			gpio_pin_configure_dt(&cfg->scl_gpio,
					      GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN);
			for (int i = 0; i < I2C_RECOVERY_CLK_COUNT; i++) {
				k_busy_wait(5);
				gpio_pin_set_dt(&cfg->scl_gpio, 0);
				k_busy_wait(5);
				gpio_pin_set_dt(&cfg->scl_gpio, 1);
				if (gpio_pin_get_dt(&cfg->sda_gpio) != 0) {
					LOG_DBG("SDA released after %d clocks", i + 1);
					break;
				}
			}
			/* Generate STOP: SDA LOW -> HIGH while SCL HIGH */
			gpio_pin_configure_dt(&cfg->sda_gpio,
					      GPIO_OUTPUT_LOW | GPIO_OPEN_DRAIN);
			k_busy_wait(5);
			gpio_pin_set_dt(&cfg->scl_gpio, 1);
			k_busy_wait(5);
			gpio_pin_set_dt(&cfg->sda_gpio, 1);
			k_busy_wait(5);
			/* Release pins and re-apply pinctrl */
			gpio_pin_configure_dt(&cfg->scl_gpio, GPIO_INPUT);
			gpio_pin_configure_dt(&cfg->sda_gpio, GPIO_INPUT);
			pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		}
	}

	dev_config = I2C_SPEED_SET(I2C_SPEED_STANDARD) | I2C_MODE_CONTROLLER;

	ret = i2c_dspic_configure(dev, dev_config);
	if (ret != 0) {
		LOG_ERR("Failed to configure I2C: %d", ret);
		return ret;
	}

#ifdef CONFIG_I2C_TARGET
	{
		struct i2c_dspic_data *data = dev->data;

		data->target_mode = false;
		data->target_cfg = NULL;

		if (cfg->irq_config_func != NULL) {
			cfg->irq_config_func(dev);
		}
	}
#endif

	return 0;
}

static DEVICE_API(i2c, i2c_dspic_api) = {
	.configure = i2c_dspic_configure,
	.get_config = i2c_dspic_get_config,
	.transfer = i2c_dspic_transfer,
	.recover_bus = i2c_dspic_recover_bus,
#ifdef CONFIG_I2C_TARGET
	.target_register = i2c_dspic_target_register,
	.target_unregister = i2c_dspic_target_unregister,
#endif
};

#ifdef CONFIG_I2C_TARGET
#define I2C_DSPIC_IRQ_HANDLER_DECLARE(inst) \
	static void i2c_dspic_irq_config_##inst(const struct device *dev);

#define I2C_DSPIC_IRQ_HANDLER_DEFINE(inst)                                              \
	static void i2c_dspic_irq_config_##inst(const struct device *dev)               \
	{                                                                               \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(inst, 0, irq),                          \
			    DT_INST_IRQ_BY_IDX(inst, 0, priority),                      \
			    i2c_dspic_target_isr,                                       \
			    DEVICE_DT_INST_GET(inst), 0);                               \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(inst, 1, irq),                          \
			    DT_INST_IRQ_BY_IDX(inst, 1, priority),                      \
			    i2c_dspic_target_isr,                                       \
			    DEVICE_DT_INST_GET(inst), 0);                               \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(inst, 2, irq),                          \
			    DT_INST_IRQ_BY_IDX(inst, 2, priority),                      \
			    i2c_dspic_target_isr,                                       \
			    DEVICE_DT_INST_GET(inst), 0);                               \
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(inst, 3, irq),                          \
			    DT_INST_IRQ_BY_IDX(inst, 3, priority),                      \
			    i2c_dspic_target_isr,                                       \
			    DEVICE_DT_INST_GET(inst), 0);                               \
	}

#define I2C_DSPIC_IRQ_CONFIG_FUNC(inst) \
	.irq_config_func = i2c_dspic_irq_config_##inst,

#define I2C_DSPIC_IRQ_NUMS(inst)                                                        \
	.err_irq_num = DT_INST_IRQ_BY_IDX(inst, 0, irq),                               \
	.gen_irq_num = DT_INST_IRQ_BY_IDX(inst, 1, irq),                               \
	.rx_irq_num = DT_INST_IRQ_BY_IDX(inst, 2, irq),                                \
	.tx_irq_num = DT_INST_IRQ_BY_IDX(inst, 3, irq),

#else
#define I2C_DSPIC_IRQ_HANDLER_DECLARE(inst)
#define I2C_DSPIC_IRQ_HANDLER_DEFINE(inst)
#define I2C_DSPIC_IRQ_CONFIG_FUNC(inst)
#define I2C_DSPIC_IRQ_NUMS(inst)                                                        \
	.err_irq_num = DT_INST_IRQ_BY_IDX(inst, 0, irq),                               \
	.gen_irq_num = DT_INST_IRQ_BY_IDX(inst, 1, irq),                               \
	.rx_irq_num = DT_INST_IRQ_BY_IDX(inst, 2, irq),                                \
	.tx_irq_num = DT_INST_IRQ_BY_IDX(inst, 3, irq),
#endif

#define I2C_DSPIC_INIT(inst)                                                            \
	I2C_DSPIC_IRQ_HANDLER_DECLARE(inst)                                             \
	PINCTRL_DT_INST_DEFINE(inst);                                                   \
	static const struct i2c_dspic_config i2c_dspic_config_##inst = {                \
		.base = DT_INST_REG_ADDR(inst),                                         \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                           \
		.scl_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, scl_gpios, {0}),            \
		.sda_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, sda_gpios, {0}),            \
		I2C_DSPIC_IRQ_NUMS(inst)                                                \
		I2C_DSPIC_IRQ_CONFIG_FUNC(inst)                                         \
	};                                                                              \
	static struct i2c_dspic_data i2c_dspic_data_##inst;                             \
	I2C_DEVICE_DT_INST_DEFINE(inst, i2c_dspic_init, NULL,                           \
				  &i2c_dspic_data_##inst,                               \
				  &i2c_dspic_config_##inst,                             \
				  POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,                \
				  &i2c_dspic_api);                                      \
	I2C_DSPIC_IRQ_HANDLER_DEFINE(inst)

DT_INST_FOREACH_STATUS_OKAY(I2C_DSPIC_INIT)

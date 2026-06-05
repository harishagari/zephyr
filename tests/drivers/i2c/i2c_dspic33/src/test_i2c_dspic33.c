/*
 * Copyright (c) 2025, Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(i2c_0))
#define I2C_DEV_NODE DT_ALIAS(i2c_0)
#elif DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(i2c_1))
#define I2C_DEV_NODE DT_ALIAS(i2c_1)
#elif DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(i2c_2))
#define I2C_DEV_NODE DT_ALIAS(i2c_2)
#else
#error "Please set the correct I2C device"
#endif

static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_DEV_NODE);

ZTEST(i2c_dspic33, test_i2c_device_ready)
{
	zassert_true(device_is_ready(i2c_dev), "I2C device is not ready");
}

ZTEST(i2c_dspic33, test_i2c_configure_standard)
{
	uint32_t cfg = I2C_SPEED_SET(I2C_SPEED_STANDARD) | I2C_MODE_CONTROLLER;
	int ret;

	ret = i2c_configure(i2c_dev, cfg);
	zassert_equal(ret, 0, "I2C configure standard speed failed: %d", ret);
}

ZTEST(i2c_dspic33, test_i2c_configure_fast)
{
	uint32_t cfg = I2C_SPEED_SET(I2C_SPEED_FAST) | I2C_MODE_CONTROLLER;
	int ret;

	ret = i2c_configure(i2c_dev, cfg);
	zassert_equal(ret, 0, "I2C configure fast speed failed: %d", ret);
}

ZTEST(i2c_dspic33, test_i2c_configure_fast_plus)
{
	uint32_t cfg = I2C_SPEED_SET(I2C_SPEED_FAST_PLUS) | I2C_MODE_CONTROLLER;
	int ret;

	ret = i2c_configure(i2c_dev, cfg);
	zassert_equal(ret, 0, "I2C configure fast-plus speed failed: %d", ret);
}

ZTEST(i2c_dspic33, test_i2c_get_config)
{
	uint32_t cfg = I2C_SPEED_SET(I2C_SPEED_STANDARD) | I2C_MODE_CONTROLLER;
	uint32_t cfg_read;
	int ret;

	ret = i2c_configure(i2c_dev, cfg);
	zassert_equal(ret, 0, "I2C configure failed: %d", ret);

	ret = i2c_get_config(i2c_dev, &cfg_read);
	zassert_equal(ret, 0, "I2C get_config failed: %d", ret);
	zassert_equal(cfg, cfg_read, "Config mismatch: expected 0x%x, got 0x%x",
		      cfg, cfg_read);
}

ZTEST(i2c_dspic33, test_i2c_configure_invalid_speed)
{
	uint32_t cfg = I2C_SPEED_SET(I2C_SPEED_ULTRA) | I2C_MODE_CONTROLLER;
	int ret;

	ret = i2c_configure(i2c_dev, cfg);
	zassert_not_equal(ret, 0, "I2C configure should fail for ultra speed");
}

ZTEST(i2c_dspic33, test_i2c_transfer_no_msgs)
{
	int ret;

	ret = i2c_transfer(i2c_dev, NULL, 0, 0x50);
	zassert_equal(ret, 0, "I2C transfer with 0 msgs should be a no-op: %d", ret);
}

ZTEST(i2c_dspic33, test_i2c_recover_bus)
{
	int ret;

	ret = i2c_recover_bus(i2c_dev);
	zassert_equal(ret, 0, "I2C bus recovery failed: %d", ret);
}

ZTEST(i2c_dspic33, test_i2c_write_no_device)
{
	uint8_t data[] = {0x00, 0x01};
	int ret;

	ret = i2c_write(i2c_dev, data, sizeof(data), 0x7F);
	zassert_not_equal(ret, 0, "I2C write to non-existent device should fail");
}

ZTEST(i2c_dspic33, test_i2c_bus_scan)
{
	uint8_t dummy;
	int ret;
	int found = 0;

	TC_PRINT("Scanning I2C bus (0x08 - 0x77)...\n");

	for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
		ret = i2c_read(i2c_dev, &dummy, 1, addr);
		if (ret == 0) {
			TC_PRINT("  Device found at 0x%02X\n", addr);
			found++;
		}
	}

	TC_PRINT("Scan complete: %d device(s) found\n", found);
}

ZTEST_SUITE(i2c_dspic33, NULL, NULL, NULL, NULL, NULL);

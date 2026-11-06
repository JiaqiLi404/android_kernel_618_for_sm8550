#!/usr/bin/env python3
# k618_tof_fix.py <dir>: the 13 Ultra's ToF driver (ST stmvl53l5 3.3.0.0, Xiaomi ishtar-t-oss, 5.15)
# on 6.18 -- three interface changes, nothing of the driver's own behaviour:
#   - an I2C driver's probe has one argument since 6.3 (the device id it never used is gone)
#   - I2C (6.1) and SPI (5.18) remove callbacks return void
import os
import sys

EDITS = [
    # an _IOW() number is size_t-typed on 6.x (sizeof in the macro): "%u" is a -Werror there
    ("stmvl53l5_module.c",
     "\tSTMVL53L5_LOG_DEBUG(\"power: %u\", STMVL53L5_IOCTL_POWER);\n",
     "\tSTMVL53L5_LOG_DEBUG(\"power: %u\", (unsigned int)STMVL53L5_IOCTL_POWER);\t/* k618 */\n"),
    ("stmvl53l5_module.c",
     "static int stmvl53l5_i2c_probe(struct i2c_client *client,\n"
     "\t\t\t\tconst struct i2c_device_id *id)\n",
     "static int stmvl53l5_i2c_probe(struct i2c_client *client)\t/* k618: 6.3+, one argument */\n"),
    ("stmvl53l5_module.c",
     "static int stmvl53l5_i2c_remove(struct i2c_client *client)\n"
     "{\n"
     "#ifdef STM_VL53L5_GPIO_ENABLE\n"
     "\t(void)stmvl53l5_platform_terminate(&stm_module->stdev);\n"
     "#endif\n"
     "\tSTMVL53L5_LOG_INFO(\"Remove i2c\");\n"
     "\n"
     "\tstm_module->gpio = NULL;\n"
     "\tkfree(stm_module);\n"
     "\n"
     "\treturn STMVL53L5_ERROR_NONE;\n"
     "}\n",
     "static void stmvl53l5_i2c_remove(struct i2c_client *client)\t/* k618: 6.1+, void */\n"
     "{\n"
     "#ifdef STM_VL53L5_GPIO_ENABLE\n"
     "\t(void)stmvl53l5_platform_terminate(&stm_module->stdev);\n"
     "#endif\n"
     "\tSTMVL53L5_LOG_INFO(\"Remove i2c\");\n"
     "\n"
     "\tstm_module->gpio = NULL;\n"
     "\tkfree(stm_module);\n"
     "}\n"),
    ("stmvl53l5_module.c",
     "static int stmvl53l5_spi_remove(struct spi_device *device)\n"
     "{\n"
     "#ifdef STM_VL53L5_GPIO_ENABLE\n"
     "\t(void)stmvl53l5_platform_terminate(&stm_module->stdev);\n"
     "#endif\n"
     "\tSTMVL53L5_LOG_INFO(\"Remove SPI\");\n"
     "\n"
     "\tstm_module->gpio = NULL;\n"
     "\tkfree(stm_module);\n"
     "\tdisable_power();\n"
     "\treturn STMVL53L5_ERROR_NONE;\n"
     "}\n",
     "static void stmvl53l5_spi_remove(struct spi_device *device)\t/* k618: 5.18+, void */\n"
     "{\n"
     "#ifdef STM_VL53L5_GPIO_ENABLE\n"
     "\t(void)stmvl53l5_platform_terminate(&stm_module->stdev);\n"
     "#endif\n"
     "\tSTMVL53L5_LOG_INFO(\"Remove SPI\");\n"
     "\n"
     "\tstm_module->gpio = NULL;\n"
     "\tkfree(stm_module);\n"
     "\tdisable_power();\n"
     "}\n"),
]


def main(root):
    n = 0
    for rel, old, new in EDITS:
        p = os.path.join(root, rel)
        s = open(p, encoding="utf-8", errors="surrogateescape").read()
        if new in s:
            continue
        if s.count(old) != 1:
            sys.exit(f"k618_tof_fix: {rel}: anchor found {s.count(old)} times: {old[:50]!r}")
        open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s.replace(old, new))
        n += 1
    print(f"k618_tof_fix: {n} of {len(EDITS)} edits applied")


if __name__ == "__main__":
    main(sys.argv[1])

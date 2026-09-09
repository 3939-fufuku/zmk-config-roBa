/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

LOG_MODULE_REGISTER(sticky_layer_notify, CONFIG_STICKY_LAYER_NOTIFY_LOG_LEVEL);

#define STICKY_LAYER_UUID(value)                                                                  \
    BT_UUID_128_ENCODE(value, 0x7d8b, 0x4f2c, 0x9a61, 0x6e7e3c5b1a00)

#define STICKY_LAYER_SERVICE_UUID STICKY_LAYER_UUID(0x3a7d9f10)
#define STICKY_LAYER_CHARACTERISTIC_UUID STICKY_LAYER_UUID(0x3a7d9f11)
#define STICKY_BATTERY_CHARACTERISTIC_UUID STICKY_LAYER_UUID(0x3a7d9f12)
#define STICKY_KEYBOARD_ID_CHARACTERISTIC_UUID STICKY_LAYER_UUID(0x3a7d9f13)

struct sticky_battery_levels {
    uint8_t right;
    uint8_t left;
};

static uint8_t active_layer;
static struct sticky_battery_levels battery_levels = {.right = UINT8_MAX, .left = UINT8_MAX};
static const char keyboard_id[] = CONFIG_STICKY_KEYBOARD_ID;

static ssize_t read_active_layer(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                 uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &active_layer, sizeof(active_layer));
}

static ssize_t read_battery_levels(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                   uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &battery_levels,
                             sizeof(battery_levels));
}

static ssize_t read_keyboard_id(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, keyboard_id,
                             sizeof(keyboard_id) - 1);
}

static void layer_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    LOG_DBG("Layer notifications %s", value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

static void battery_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    LOG_DBG("Battery notifications %s", value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(
    sticky_layer_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(STICKY_LAYER_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(STICKY_LAYER_CHARACTERISTIC_UUID),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                           read_active_layer, NULL, &active_layer),
    BT_GATT_CCC(layer_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(STICKY_BATTERY_CHARACTERISTIC_UUID),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                           read_battery_levels, NULL, &battery_levels),
    BT_GATT_CCC(battery_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(STICKY_KEYBOARD_ID_CHARACTERISTIC_UUID),
                           BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
                           read_keyboard_id, NULL, keyboard_id), );

static void notify_layer_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    int err =
        bt_gatt_notify(NULL, &sticky_layer_svc.attrs[1], &active_layer, sizeof(active_layer));
    if (err && err != -ENOTCONN) {
        LOG_WRN("Layer notification failed: %d", err);
    } else {
        LOG_DBG("Active layer: %u", active_layer);
    }
}

K_WORK_DEFINE(notify_layer_work, notify_layer_work_handler);

static void notify_battery_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    int err = bt_gatt_notify(NULL, &sticky_layer_svc.attrs[4], &battery_levels,
                             sizeof(battery_levels));
    if (err && err != -ENOTCONN) {
        LOG_WRN("Battery notification failed: %d", err);
    } else {
        LOG_DBG("Battery levels: left=%u right=%u", battery_levels.left,
                battery_levels.right);
    }
}

K_WORK_DEFINE(notify_battery_work, notify_battery_work_handler);

static int sticky_layer_listener(const zmk_event_t *event) {
    if (as_zmk_layer_state_changed(event) == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint8_t next_layer = zmk_keymap_highest_layer_active();
    if (next_layer != active_layer) {
        active_layer = next_layer;
        k_work_submit(&notify_layer_work);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sticky_layer_notify, sticky_layer_listener);
ZMK_SUBSCRIPTION(sticky_layer_notify, zmk_layer_state_changed);

static int sticky_battery_listener(const zmk_event_t *event) {
    const struct zmk_battery_state_changed *central = as_zmk_battery_state_changed(event);
    if (central != NULL) {
#if CONFIG_STICKY_CENTRAL_IS_LEFT
        if (battery_levels.left != central->state_of_charge) {
            battery_levels.left = central->state_of_charge;
#else
        if (battery_levels.right != central->state_of_charge) {
            battery_levels.right = central->state_of_charge;
#endif
            k_work_submit(&notify_battery_work);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_peripheral_battery_state_changed *peripheral =
        as_zmk_peripheral_battery_state_changed(event);
    if (peripheral != NULL && peripheral->source == 0) {
#if CONFIG_STICKY_CENTRAL_IS_LEFT
        if (battery_levels.right != peripheral->state_of_charge) {
            battery_levels.right = peripheral->state_of_charge;
#else
        if (battery_levels.left != peripheral->state_of_charge) {
        battery_levels.left = peripheral->state_of_charge;
#endif
        k_work_submit(&notify_battery_work);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sticky_battery_notify, sticky_battery_listener);
ZMK_SUBSCRIPTION(sticky_battery_notify, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(sticky_battery_notify, zmk_peripheral_battery_state_changed);

static int sticky_layer_notify_init(void) {
    active_layer = zmk_keymap_highest_layer_active();
#if CONFIG_STICKY_CENTRAL_IS_LEFT
    battery_levels.left = zmk_battery_state_of_charge();
#else
    battery_levels.right = zmk_battery_state_of_charge();
#endif

    uint8_t peripheral_level;
    if (zmk_split_central_get_peripheral_battery_level(0, &peripheral_level) == 0) {
#if CONFIG_STICKY_CENTRAL_IS_LEFT
        battery_levels.right = peripheral_level;
#else
        battery_levels.left = peripheral_level;
#endif
    }

    LOG_INF("Sticky service ready; id=%s layer=%u left=%u right=%u", keyboard_id,
            active_layer, battery_levels.left, battery_levels.right);
    return 0;
}

SYS_INIT(sticky_layer_notify_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

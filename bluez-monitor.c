/* Simplified bluez monitoring interface
 *
 *  Originally based on bluez-5.46 client code, 
 *
 *  Copyright (C) 2012  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <string.h>
#include <sys/ioctl.h>

#include <glib.h>

#include "gdbus/gdbus.h"

#include "bluetooth.h"
#include "hci.h"

#include "linux_bt_rfkill.h"

/* Target interface */
const char *bt_interface;
const char *bt_interface_address;

static GMainLoop *main_loop;
static DBusConnection *dbus_connection;

static GDBusProxy *agent_manager;

struct gdbus_bluez_adapter {
    GDBusProxy *proxy;
    GList *devices;
};


static void connect_handler(DBusConnection *connection, void *user_data) {
    fprintf(stderr, "DEBUG - connect_handler\n");
}

static void handle_device(GDBusProxy *proxy, const char *description) {
    DBusMessageIter iter;
    const char *address, *alias;

    if (!g_dbus_proxy_get_property(proxy, "Address", &iter)) {
        return;
    }

    dbus_message_iter_get_basic(&iter, &address);

    if (g_dbus_proxy_get_property(proxy, "Alias", &iter)) {
        dbus_message_iter_get_basic(&iter, &alias);
    } else {
        alias = NULL;
    }

    fprintf(stderr, "DEBUG - device address %s alias %s\n", address, alias);
}

static void dbus_adapter_scan_reply(DBusMessage *message, void *user_data) {
    DBusError error;
    dbus_bool_t enable = GPOINTER_TO_UINT(user_data);

    dbus_error_init(&error);

    if (dbus_set_error_from_message(&error, message) == TRUE) {
        fprintf(stderr, "FATAL - Failed to initiate discovery: %s\n", error.name);
        dbus_error_free(&error);
        exit(1);
    }

    fprintf(stderr, "DEBUG - Discovery initiated: %s\n", enable == TRUE ? "true" : "false");
}

static void dbus_initiate_adapter_scan(GDBusProxy *proxy) {
    const char *method = "StartDiscovery";
    dbus_bool_t enable;

    fprintf(stderr, "debug - starting scan mode\n");

    if (g_dbus_proxy_method_call(proxy, method, NULL,
                dbus_adapter_scan_reply, GUINT_TO_POINTER(enable), NULL) == FALSE) {
        fprintf(stderr, "FATAL - Failed to initiate discovery\n");
        exit(1);
    }
}

static void dbus_proxy_added(GDBusProxy *proxy, void *user_data) {
    const char *interface;
    DBusMessageIter iter;
    const char *address;
    dbus_bool_t scan_enabled;

    interface = g_dbus_proxy_get_interface(proxy);

    if (!strcmp(interface, "org.bluez.Device1")) {
        fprintf(stderr, "debug - proxy - device added\n");
    } else if (!strcmp(interface, "org.bluez.Adapter1")) {
        /* We've been notified there's a new adapter; we need to compare it to our
         * desired adapter, see if it has scan enabled, and enable scan if it
         * doesn't
         */

        fprintf(stderr, "debug - adapter added\n");

        if (g_dbus_proxy_get_property(proxy, "Address", &iter)) {
            dbus_message_iter_get_basic(&iter, &address);

            fprintf(stderr, "   adapter %s\n", address);

            if (strcmp(address, bt_interface_address)) {
                fprintf(stderr, "DEBUG - Got adapter %s but we want %s, skipping\n",
                        address, bt_interface_address);
                return;
            }

            if (!g_dbus_proxy_get_property(proxy, "Discovering", &iter)) {
                fprintf(stderr, "FATAL - Adapter doesn't have 'Discovering' attribute\n");
                exit(1);
            }

            dbus_message_iter_get_basic(&iter, &scan_enabled);

            if (scan_enabled) {
                fprintf(stderr, "DEBUG - Scan already enabled\n");
            } else {
                dbus_initiate_adapter_scan(proxy);
            }
        }
    }
}

static void dbus_proxy_removed(GDBusProxy *proxy, void *user_data) {
    const char *interface;

    interface = g_dbus_proxy_get_interface(proxy);

    if (!strcmp(interface, "org.bluez.Device1")) {
        // device_removed(proxy);
        fprintf(stderr, "debug - proxy - device removed\n");
    } else if (!strcmp(interface, "org.bluez.Adapter1")) {
        // adapter_removed(proxy);
        fprintf(stderr, "debug - proxy - adapter removed\n");
    }
}

static void dbus_property_changed(GDBusProxy *proxy, const char *name,
        DBusMessageIter *iter, void *user_data) {

    const char *interface;

    interface = g_dbus_proxy_get_interface(proxy);

    if (!strcmp(interface, "org.bluez.Device1")) {
        DBusMessageIter addr_iter;
        char *str;

        if (g_dbus_proxy_get_property(proxy, "Address", &addr_iter) == TRUE) {
            const char *address;
            dbus_message_iter_get_basic(&addr_iter, &address);

            fprintf(stderr, "debug - property changed device addr %s\n", address);
        } 
    } else if (!strcmp(interface, "org.bluez.Adapter1")) {
        DBusMessageIter addr_iter;
        char *str;

        if (g_dbus_proxy_get_property(proxy, "Address", &addr_iter) == TRUE) {
            const char *address;

            dbus_message_iter_get_basic(&addr_iter, &address);

            fprintf(stderr, "debug - controller changed %s\n", address);
        }
    }
}

static void dbus_client_ready(GDBusClient *client, void *user_data) {
    const char *method = "StartDiscovery";

    fprintf(stderr, "debug - client ready\n");

}

int main(int argc, char *argv[]) {
    GError *error = NULL;
    GDBusClient *client;
    int hci_sock;
    int devid;
    static struct hci_dev_info di;
    char bdaddr[18];

    if (argc < 2) {
        fprintf(stderr, "FATAL - expected %s [interface]\n", argv[0]);
        exit(1);
    }

    bt_interface = strdup(argv[1]);

    fprintf(stderr, "DEBUG - Targetting interface %s\n", bt_interface);

    if ((hci_sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)) < 0) {
        fprintf(stderr, "FATAL - %s couldn't create HCI socket: %s\n", 
                bt_interface, strerror(errno));
        exit(1);
    }

    if (sscanf(bt_interface, "hci%u", &devid) != 1) {
        fprintf(stderr, "FATAL - %s couldn't parse device id\n", bt_interface);
        exit(1);
    }

    if (ioctl(hci_sock, HCIGETDEVINFO, (void *) &di)) {
        fprintf(stderr, "FATAL - %s couldn't get device info\n", bt_interface);
        exit(1);
    }

    snprintf(bdaddr, 18, "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X",
            di.bdaddr.b[5], di.bdaddr.b[4], di.bdaddr.b[3],
            di.bdaddr.b[2], di.bdaddr.b[1], di.bdaddr.b[0]);

    fprintf(stderr, "DEBUG - %s %s\n", bt_interface, bdaddr);

    bt_interface_address = strdup(bdaddr);

    close(hci_sock);

    if (linux_sys_get_bt_rfkill(bt_interface, LINUX_BT_RFKILL_TYPE_HARD)) {
        fprintf(stderr, "FATAL - %s rfkill hardkill blocked\n", bt_interface);
        exit(1);
    } else {
        fprintf(stderr, "DEBUG - %s rfkill hardkill unblocked\n", bt_interface);
    }

    if (linux_sys_get_bt_rfkill(bt_interface, LINUX_BT_RFKILL_TYPE_SOFT)) {
        fprintf(stderr, "DEBUG - %s rfkill softkill blocked\n", bt_interface);

        if (linux_sys_clear_bt_rfkill(bt_interface) < 0) {
            fprintf(stderr, "DEBUG - %s rfkill softkill, could not unblock", bt_interface);
            exit(1);
        }
    } else {
        fprintf(stderr, "DEBUG - %s rfkill softkill unblocked\n", bt_interface);
    }


    main_loop = g_main_loop_new(NULL, FALSE);
    dbus_connection = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);
    g_dbus_attach_object_manager(dbus_connection);

    client = g_dbus_client_new(dbus_connection, "org.bluez", "/org/bluez");

    g_dbus_client_set_proxy_handlers(client, dbus_proxy_added, 
            dbus_proxy_removed, dbus_property_changed, NULL);

    g_dbus_client_set_ready_watch(client, dbus_client_ready, NULL);

    g_main_loop_run(main_loop);

    g_dbus_client_unref(client);
    dbus_connection_unref(dbus_connection);
    g_main_loop_unref(main_loop);

    return 0;
}


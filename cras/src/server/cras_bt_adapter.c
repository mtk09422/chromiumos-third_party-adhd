/* Copyright (c) 2013 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dbus/dbus.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "cras_bt_adapter.h"
#include "cras_bt_constants.h"
#include "utlist.h"

struct cras_bt_adapter {
	char *object_path;
	char *address;
	char *name;
	uint32_t bluetooth_class;
	int powered;

	struct cras_bt_adapter *prev, *next;
};

static struct cras_bt_adapter *adapters;

struct cras_bt_adapter *cras_bt_adapter_create(const char *object_path)
{
	struct cras_bt_adapter *adapter;

	adapter = calloc(1, sizeof(*adapter));
	if (adapter == NULL)
		return NULL;

	adapter->object_path = strdup(object_path);
	if (adapter->object_path == NULL) {
		free(adapter);
		return NULL;
	}

	DL_APPEND(adapters, adapter);

	return adapter;
}

void cras_bt_adapter_destroy(struct cras_bt_adapter *adapter)
{
	DL_DELETE(adapters, adapter);

	free(adapter->object_path);
	free(adapter->address);
	free(adapter->name);
	free(adapter);
}

void cras_bt_adapter_reset()
{
	while (adapters) {
		syslog(LOG_INFO, "Bluetooth Adapter: %s removed",
		       adapters->address);
		cras_bt_adapter_destroy(adapters);
	}
}


struct cras_bt_adapter *cras_bt_adapter_get(const char *object_path)
{
	struct cras_bt_adapter *adapter;

	DL_FOREACH(adapters, adapter) {
		if (strcmp(adapter->object_path, object_path) == 0)
			return adapter;
	}

	return NULL;
}

size_t cras_bt_adapter_get_list(struct cras_bt_adapter ***adapter_list_out)
{
	struct cras_bt_adapter *adapter;
	struct cras_bt_adapter **adapter_list = NULL;
	size_t num_adapters = 0;

	DL_FOREACH(adapters, adapter) {
		struct cras_bt_adapter **tmp;

		tmp = realloc(adapter_list,
			      sizeof(adapter_list[0]) * (num_adapters + 1));
		if (!tmp) {
			free(adapter_list);
			return -ENOMEM;
		}

		adapter_list = tmp;
		adapter_list[num_adapters++] = adapter;
	}

	*adapter_list_out = adapter_list;
	return num_adapters;
}

const char *cras_bt_adapter_object_path(const struct cras_bt_adapter *adapter)
{
	return adapter->object_path;
}

const char *cras_bt_adapter_address(const struct cras_bt_adapter *adapter)
{
	return adapter->address;
}

const char *cras_bt_adapter_name(const struct cras_bt_adapter *adapter)
{
	return adapter->name;
}

int cras_bt_adapter_powered(const struct cras_bt_adapter *adapter)
{
	return adapter->powered;
}


void cras_bt_adapter_update_properties(struct cras_bt_adapter *adapter,
				       DBusMessageIter *properties_array_iter,
				       DBusMessageIter *invalidated_array_iter)
{
	while (dbus_message_iter_get_arg_type(properties_array_iter) !=
	       DBUS_TYPE_INVALID) {
		DBusMessageIter properties_dict_iter, variant_iter;
		const char *key;
		int type;

		dbus_message_iter_recurse(properties_array_iter,
					  &properties_dict_iter);

		dbus_message_iter_get_basic(&properties_dict_iter, &key);
		dbus_message_iter_next(&properties_dict_iter);

		dbus_message_iter_recurse(&properties_dict_iter, &variant_iter);
		type = dbus_message_iter_get_arg_type(&variant_iter);

		if (type == DBUS_TYPE_STRING) {
			const char *value;

			dbus_message_iter_get_basic(&variant_iter, &value);

			if (strcmp(key, "Address") == 0) {
				free(adapter->address);
				adapter->address = strdup(value);

			} else if (strcmp(key, "Alias") == 0) {
				free(adapter->name);
				adapter->name = strdup(value);

			}

		} else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&variant_iter, &value);

			if (strcmp(key, "Class") == 0)
				adapter->bluetooth_class = value;

		} else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&variant_iter, &value);

			if (strcmp(key, "Powered") == 0)
				adapter->powered = value;

		}

		dbus_message_iter_next(properties_array_iter);
	}

	while (invalidated_array_iter &&
	       dbus_message_iter_get_arg_type(invalidated_array_iter) !=
	       DBUS_TYPE_INVALID) {
		const char *key;

		dbus_message_iter_get_basic(invalidated_array_iter, &key);

		if (strcmp(key, "Address") == 0) {
			free(adapter->address);
			adapter->address = NULL;
		} else if (strcmp(key, "Alias") == 0) {
			free(adapter->name);
			adapter->name = NULL;
		} else if (strcmp(key, "Class") == 0) {
			adapter->bluetooth_class = 0;
		} else if (strcmp(key, "Powered") == 0) {
			adapter->powered = 0;
		}

		dbus_message_iter_next(invalidated_array_iter);
	}
}

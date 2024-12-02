// SPDX-License-Identifier: GPL-2.0

#include <linux/memblock.h>
#include <linux/of.h>

#include "of_private.h"

static struct property * __init dup_prop(const struct property *prop)
{
	struct property *new;
	size_t sz = sizeof(*new);
	size_t namelen = strlen(prop->name) + 1;

	sz += namelen;
	sz += prop->length;

	new = memblock_alloc(sz, __alignof__(struct property));
	if (!new)
		return NULL;

	new->length = prop->length;
	new->value = new + 1;
	memcpy(new->value, prop->value, prop->length);
	new->name = new->value + prop->length;
	strcpy(new->name, prop->name);

	return new;
}

static void __init dup_parent_cells(struct device_node *np, const char *cellname)
{
	struct device_node __free(device_node) *parent = of_get_parent(np);
	struct property *prop;

	if (of_property_present(np, cellname))
		return;

	prop = of_find_property(parent, cellname, NULL);
	if (prop)
		prop = dup_prop(prop);
	if (!prop)
		return;
	of_add_property(np, prop);
}

/*
 * Some Google Chromebooks have MMIO addresses in firmware nodes and fail to
 * populate /firmware node with #address-cells and #size-cells relying on the
 * parent (root) node.
 */
static void __init fixup_firmware_cells(void)
{
	struct device_node __free(device_node) *np = of_find_node_by_path("/firmware");

	if (!np || !of_property_present(np, "ranges"))
		return;

	dup_parent_cells(np, "#size-cells");
	dup_parent_cells(np, "#address-cells");
}

void __init of_apply_fixups(void)
{
	fixup_firmware_cells();
}
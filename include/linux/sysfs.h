/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BACKPORT_SYSFS_H
#define BACKPORT_SYSFS_H

#include <linux/version.h>
#include_next <linux/sysfs.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 104) || (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0))) && RHEL_RELEASE_CODE < 0x805
#include <linux/mm.h>
/**
 *	sysfs_emit - scnprintf equivalent, aware of PAGE_SIZE buffer
 *	@buf:	start of PAGE_SIZE buffer.
 *	@fmt:	format
 *	@...:	optional arguments to @format
 *
 *
 * Returns number of characters written to @buf.
 */
static inline int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || offset_in_page(buf),
		 "invalid sysfs_emit: buf:%p\n", buf))
		return 0;

	va_start(args, fmt);
	len = vscnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);

	return len;
}
#endif /* < KERNEL_VERSION(5, 10, 0) */

#endif

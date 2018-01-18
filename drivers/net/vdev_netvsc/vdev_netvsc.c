/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 6WIND S.A.
 * Copyright 2017 Mellanox Technologies, Ltd.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/ip.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rte_alarm.h>
#include <rte_bus.h>
#include <rte_bus_vdev.h>
#include <rte_common.h>
#include <rte_config.h>
#include <rte_dev.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_kvargs.h>
#include <rte_log.h>

#define VDEV_NETVSC_DRIVER net_vdev_netvsc
#define VDEV_NETVSC_ARG_IFACE "iface"
#define VDEV_NETVSC_ARG_MAC "mac"
#define VDEV_NETVSC_ARG_FORCE "force"
#define VDEV_NETVSC_PROBE_MS 1000

#define NETVSC_CLASS_ID "{f8615163-df3e-46c5-913f-f2d2f965ed0e}"
#define NETVSC_MAX_ROUTE_LINE_SIZE 300

#define DRV_LOG(level, ...) \
	rte_log(RTE_LOG_ ## level, \
		vdev_netvsc_logtype, \
		RTE_FMT(RTE_STR(VDEV_NETVSC_DRIVER) ": " \
			RTE_FMT_HEAD(__VA_ARGS__,) "\n", \
		RTE_FMT_TAIL(__VA_ARGS__,)))

/** Driver-specific log messages type. */
static int vdev_netvsc_logtype;

/** Context structure for a vdev_netvsc instance. */
struct vdev_netvsc_ctx {
	LIST_ENTRY(vdev_netvsc_ctx) entry; /**< Next entry in list. */
	unsigned int id;		   /**< Unique ID. */
	char name[64];			   /**< Unique name. */
	char devname[64];		   /**< Fail-safe instance name. */
	char devargs[256];		   /**< Fail-safe device arguments. */
	char if_name[IF_NAMESIZE];	   /**< NetVSC netdevice name. */
	unsigned int if_index;		   /**< NetVSC netdevice index. */
	struct ether_addr if_addr;	   /**< NetVSC MAC address. */
	int pipe[2];			   /**< Fail-safe communication pipe. */
	char yield[256];		   /**< PCI sub-device arguments. */
};

/** Context list is common to all driver instances. */
static LIST_HEAD(, vdev_netvsc_ctx) vdev_netvsc_ctx_list =
	LIST_HEAD_INITIALIZER(vdev_netvsc_ctx_list);

/** Number of entries in context list. */
static unsigned int vdev_netvsc_ctx_count;

/** Number of driver instances relying on context list. */
static unsigned int vdev_netvsc_ctx_inst;

/**
 * Destroy a vdev_netvsc context instance.
 *
 * @param ctx
 *   Context to destroy.
 */
static void
vdev_netvsc_ctx_destroy(struct vdev_netvsc_ctx *ctx)
{
	if (ctx->pipe[0] != -1)
		close(ctx->pipe[0]);
	if (ctx->pipe[1] != -1)
		close(ctx->pipe[1]);
	free(ctx);
}

/**
 * Iterate over system network interfaces.
 *
 * This function runs a given callback function for each netdevice found on
 * the system.
 *
 * @param func
 *   Callback function pointer. List traversal is aborted when this function
 *   returns a nonzero value.
 * @param ...
 *   Variable parameter list passed as @p va_list to @p func.
 *
 * @return
 *   0 when the entire list is traversed successfully, a negative error code
 *   in case or failure, or the nonzero value returned by @p func when list
 *   traversal is aborted.
 */
static int
vdev_netvsc_foreach_iface(int (*func)(const struct if_nameindex *iface,
				      const struct ether_addr *eth_addr,
				      va_list ap), ...)
{
	struct if_nameindex *iface = if_nameindex();
	int s = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	unsigned int i;
	int ret = 0;

	if (!iface) {
		ret = -ENOBUFS;
		DRV_LOG(ERR, "cannot retrieve system network interfaces");
		goto error;
	}
	if (s == -1) {
		ret = -errno;
		DRV_LOG(ERR, "cannot open socket: %s", rte_strerror(errno));
		goto error;
	}
	for (i = 0; iface[i].if_name; ++i) {
		struct ifreq req;
		struct ether_addr eth_addr;
		va_list ap;

		strncpy(req.ifr_name, iface[i].if_name, sizeof(req.ifr_name));
		if (ioctl(s, SIOCGIFHWADDR, &req) == -1) {
			DRV_LOG(WARNING, "cannot retrieve information about"
					 " interface \"%s\": %s",
					 req.ifr_name, rte_strerror(errno));
			continue;
		}
		if (req.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
			DRV_LOG(DEBUG, "interface %s is non-ethernet device",
				req.ifr_name);
			continue;
		}
		memcpy(eth_addr.addr_bytes, req.ifr_hwaddr.sa_data,
		       RTE_DIM(eth_addr.addr_bytes));
		va_start(ap, func);
		ret = func(&iface[i], &eth_addr, ap);
		va_end(ap);
		if (ret)
			break;
	}
error:
	if (s != -1)
		close(s);
	if (iface)
		if_freenameindex(iface);
	return ret;
}

/**
 * Determine if a network interface is NetVSC.
 *
 * @param[in] iface
 *   Pointer to netdevice description structure (name and index).
 *
 * @return
 *   A nonzero value when interface is detected as NetVSC. In case of error,
 *   rte_errno is updated and 0 returned.
 */
static int
vdev_netvsc_iface_is_netvsc(const struct if_nameindex *iface)
{
	static const char temp[] = "/sys/class/net/%s/device/class_id";
	char path[sizeof(temp) + IF_NAMESIZE];
	FILE *f;
	int ret;
	int len = 0;

	ret = snprintf(path, sizeof(path), temp, iface->if_name);
	if (ret == -1 || (size_t)ret >= sizeof(path)) {
		rte_errno = ENOBUFS;
		return 0;
	}
	f = fopen(path, "r");
	if (!f) {
		rte_errno = errno;
		return 0;
	}
	ret = fscanf(f, NETVSC_CLASS_ID "%n", &len);
	if (ret == EOF)
		rte_errno = errno;
	ret = len == (int)strlen(NETVSC_CLASS_ID);
	fclose(f);
	return ret;
}

/**
 * Determine if a network interface has a route.
 *
 * @param[in] name
 *   Network device name.
 *
 * @return
 *   A nonzero value when interface has an route. In case of error,
 *   rte_errno is updated and 0 returned.
 */
static int
vdev_netvsc_has_route(const char *name)
{
	FILE *fp;
	int ret = 0;
	char route[NETVSC_MAX_ROUTE_LINE_SIZE];
	char *netdev;

	fp = fopen("/proc/net/route", "r");
	if (!fp) {
		rte_errno = errno;
		return 0;
	}
	while (fgets(route, NETVSC_MAX_ROUTE_LINE_SIZE, fp) != NULL) {
		netdev = strtok(route, "\t");
		if (strcmp(netdev, name) == 0) {
			ret = 1;
			break;
		}
		/* Move file pointer to the next line. */
		while (strchr(route, '\n') == NULL &&
		       fgets(route, NETVSC_MAX_ROUTE_LINE_SIZE, fp) != NULL)
			;
	}
	fclose(fp);
	return ret;
}

/**
 * Retrieve network interface data from sysfs symbolic link.
 *
 * @param[out] buf
 *   Output data buffer.
 * @param size
 *   Output buffer size.
 * @param[in] if_name
 *   Netdevice name.
 * @param[in] relpath
 *   Symbolic link path relative to netdevice sysfs entry.
 *
 * @return
 *   0 on success, a negative error code otherwise.
 */
static int
vdev_netvsc_sysfs_readlink(char *buf, size_t size, const char *if_name,
			   const char *relpath)
{
	int ret;

	ret = snprintf(buf, size, "/sys/class/net/%s/%s", if_name, relpath);
	if (ret == -1 || (size_t)ret >= size)
		return -ENOBUFS;
	ret = readlink(buf, buf, size);
	if (ret == -1)
		return -errno;
	if ((size_t)ret >= size - 1)
		return -ENOBUFS;
	buf[ret] = '\0';
	return 0;
}

/**
 * Probe a network interface to associate with vdev_netvsc context.
 *
 * This function determines if the network device matches the properties of
 * the NetVSC interface associated with the vdev_netvsc context and
 * communicates its bus address to the fail-safe PMD instance if so.
 *
 * It is normally used with vdev_netvsc_foreach_iface().
 *
 * @param[in] iface
 *   Pointer to netdevice description structure (name and index).
 * @param[in] eth_addr
 *   MAC address associated with @p iface.
 * @param ap
 *   Variable arguments list comprising:
 *
 *   - struct vdev_netvsc_ctx *ctx:
 *     Context to associate network interface with.
 *
 * @return
 *   A nonzero value when interface matches, 0 otherwise or in case of
 *   error.
 */
static int
vdev_netvsc_device_probe(const struct if_nameindex *iface,
		    const struct ether_addr *eth_addr,
		    va_list ap)
{
	struct vdev_netvsc_ctx *ctx = va_arg(ap, struct vdev_netvsc_ctx *);
	char buf[RTE_MAX(sizeof(ctx->yield), 256u)];
	const char *addr;
	size_t len;
	int ret;

	/* Skip non-matching or unwanted NetVSC interfaces. */
	if (ctx->if_index == iface->if_index) {
		if (!strcmp(ctx->if_name, iface->if_name))
			return 0;
		DRV_LOG(DEBUG,
			"NetVSC interface \"%s\" (index %u) renamed \"%s\"",
			ctx->if_name, ctx->if_index, iface->if_name);
		strncpy(ctx->if_name, iface->if_name, sizeof(ctx->if_name));
		return 0;
	}
	if (vdev_netvsc_iface_is_netvsc(iface))
		return 0;
	if (!is_same_ether_addr(eth_addr, &ctx->if_addr))
		return 0;
	/* Look for associated PCI device. */
	ret = vdev_netvsc_sysfs_readlink(buf, sizeof(buf), iface->if_name,
					 "device/subsystem");
	if (ret)
		return 0;
	addr = strrchr(buf, '/');
	addr = addr ? addr + 1 : buf;
	if (strcmp(addr, "pci"))
		return 0;
	ret = vdev_netvsc_sysfs_readlink(buf, sizeof(buf), iface->if_name,
					 "device");
	if (ret)
		return 0;
	addr = strrchr(buf, '/');
	addr = addr ? addr + 1 : buf;
	len = strlen(addr);
	if (!len)
		return 0;
	/* Send PCI device argument to fail-safe PMD instance. */
	if (strcmp(addr, ctx->yield))
		DRV_LOG(DEBUG, "associating PCI device \"%s\" with NetVSC"
			" interface \"%s\" (index %u)", addr, ctx->if_name,
			ctx->if_index);
	memmove(buf, addr, len + 1);
	addr = buf;
	buf[len] = '\n';
	ret = write(ctx->pipe[1], addr, len + 1);
	buf[len] = '\0';
	if (ret == -1) {
		if (errno == EINTR || errno == EAGAIN)
			return 1;
		DRV_LOG(WARNING, "cannot associate PCI device name \"%s\" with"
			" interface \"%s\": %s", addr, ctx->if_name,
			rte_strerror(errno));
		return 1;
	}
	if ((size_t)ret != len + 1) {
		/*
		 * Attempt to override previous partial write, no need to
		 * recover if that fails.
		 */
		ret = write(ctx->pipe[1], "\n", 1);
		(void)ret;
		return 1;
	}
	fsync(ctx->pipe[1]);
	memcpy(ctx->yield, addr, len + 1);
	return 1;
}

/**
 * Alarm callback that regularly probes system network interfaces.
 *
 * This callback runs at a frequency determined by VDEV_NETVSC_PROBE_MS as
 * long as an vdev_netvsc context instance exists.
 *
 * @param arg
 *   Ignored.
 */
static void
vdev_netvsc_alarm(__rte_unused void *arg)
{
	struct vdev_netvsc_ctx *ctx;
	int ret;

	LIST_FOREACH(ctx, &vdev_netvsc_ctx_list, entry) {
		ret = vdev_netvsc_foreach_iface(vdev_netvsc_device_probe, ctx);
		if (ret)
			break;
	}
	if (!vdev_netvsc_ctx_count)
		return;
	ret = rte_eal_alarm_set(VDEV_NETVSC_PROBE_MS * 1000,
				vdev_netvsc_alarm, NULL);
	if (ret < 0) {
		DRV_LOG(ERR, "unable to reschedule alarm callback: %s",
			rte_strerror(-ret));
	}
}

/**
 * Probe a NetVSC interface to generate a vdev_netvsc context from.
 *
 * This function instantiates vdev_netvsc contexts either for all NetVSC
 * devices found on the system or only a subset provided as device
 * arguments.
 *
 * It is normally used with vdev_netvsc_foreach_iface().
 *
 * @param[in] iface
 *   Pointer to netdevice description structure (name and index).
 * @param[in] eth_addr
 *   MAC address associated with @p iface.
 * @param ap
 *   Variable arguments list comprising:
 *
 *   - const char *name:
 *     Name associated with current driver instance.
 *
 *   - struct rte_kvargs *kvargs:
 *     Device arguments provided to current driver instance.
 *
 *   - int force:
 *     Accept specified interface even if not detected as NetVSC.
 *
 *   - unsigned int specified:
 *     Number of specific netdevices provided as device arguments.
 *
 *   - unsigned int *matched:
 *     The number of specified netdevices matched by this function.
 *
 * @return
 *   A nonzero value when interface matches, 0 otherwise or in case of
 *   error.
 */
static int
vdev_netvsc_netvsc_probe(const struct if_nameindex *iface,
			 const struct ether_addr *eth_addr,
			 va_list ap)
{
	const char *name = va_arg(ap, const char *);
	struct rte_kvargs *kvargs = va_arg(ap, struct rte_kvargs *);
	int force = va_arg(ap, int);
	unsigned int specified = va_arg(ap, unsigned int);
	unsigned int *matched = va_arg(ap, unsigned int *);
	unsigned int i;
	struct vdev_netvsc_ctx *ctx;
	int ret;

	/* Probe all interfaces when none are specified. */
	if (specified) {
		for (i = 0; i != kvargs->count; ++i) {
			const struct rte_kvargs_pair *pair = &kvargs->pairs[i];

			if (!strcmp(pair->key, VDEV_NETVSC_ARG_IFACE)) {
				if (!strcmp(pair->value, iface->if_name))
					break;
			} else if (!strcmp(pair->key, VDEV_NETVSC_ARG_MAC)) {
				struct ether_addr tmp;

				if (sscanf(pair->value,
					   "%" SCNx8 ":%" SCNx8 ":%" SCNx8 ":"
					   "%" SCNx8 ":%" SCNx8 ":%" SCNx8,
					   &tmp.addr_bytes[0],
					   &tmp.addr_bytes[1],
					   &tmp.addr_bytes[2],
					   &tmp.addr_bytes[3],
					   &tmp.addr_bytes[4],
					   &tmp.addr_bytes[5]) != 6) {
					DRV_LOG(ERR,
						"invalid MAC address format"
						" \"%s\"",
						pair->value);
					return -EINVAL;
				}
				if (is_same_ether_addr(eth_addr, &tmp))
					break;
			}
		}
		if (i == kvargs->count)
			return 0;
		++(*matched);
	}
	/* Weed out interfaces already handled. */
	LIST_FOREACH(ctx, &vdev_netvsc_ctx_list, entry)
		if (ctx->if_index == iface->if_index)
			break;
	if (ctx) {
		if (!specified)
			return 0;
		DRV_LOG(WARNING,
			"interface \"%s\" (index %u) is already handled,"
			" skipping",
			iface->if_name, iface->if_index);
		return 0;
	}
	if (!vdev_netvsc_iface_is_netvsc(iface)) {
		if (!specified || !force)
			return 0;
		DRV_LOG(WARNING,
			"using non-NetVSC interface \"%s\" (index %u)",
			iface->if_name, iface->if_index);
	}
	/* Routed NetVSC should not be probed. */
	if (vdev_netvsc_has_route(iface->if_name)) {
		if (!specified || !force)
			return 0;
		DRV_LOG(WARNING, "using routed NetVSC interface \"%s\""
			" (index %u)", iface->if_name, iface->if_index);
	}
	/* Create interface context. */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		ret = -errno;
		DRV_LOG(ERR, "cannot allocate context for interface \"%s\": %s",
			iface->if_name, rte_strerror(errno));
		goto error;
	}
	ctx->id = vdev_netvsc_ctx_count;
	strncpy(ctx->if_name, iface->if_name, sizeof(ctx->if_name));
	ctx->if_index = iface->if_index;
	ctx->if_addr = *eth_addr;
	ctx->pipe[0] = -1;
	ctx->pipe[1] = -1;
	ctx->yield[0] = '\0';
	if (pipe(ctx->pipe) == -1) {
		ret = -errno;
		DRV_LOG(ERR,
			"cannot allocate control pipe for interface \"%s\": %s",
			ctx->if_name, rte_strerror(errno));
		goto error;
	}
	for (i = 0; i != RTE_DIM(ctx->pipe); ++i) {
		int flf = fcntl(ctx->pipe[i], F_GETFL);

		if (flf != -1 &&
		    fcntl(ctx->pipe[i], F_SETFL, flf | O_NONBLOCK) != -1)
			continue;
		ret = -errno;
		DRV_LOG(ERR, "cannot toggle non-blocking flag on control file"
			" descriptor #%u (%d): %s", i, ctx->pipe[i],
			rte_strerror(errno));
		goto error;
	}
	/* Generate virtual device name and arguments. */
	i = 0;
	ret = snprintf(ctx->name, sizeof(ctx->name), "%s_id%u",
		       name, ctx->id);
	if (ret == -1 || (size_t)ret >= sizeof(ctx->name))
		++i;
	ret = snprintf(ctx->devname, sizeof(ctx->devname), "net_failsafe_%s",
		       ctx->name);
	if (ret == -1 || (size_t)ret >= sizeof(ctx->devname))
		++i;
	ret = snprintf(ctx->devargs, sizeof(ctx->devargs),
		       "fd(%d),dev(net_tap_%s,remote=%s)",
		       ctx->pipe[0], ctx->name, ctx->if_name);
	if (ret == -1 || (size_t)ret >= sizeof(ctx->devargs))
		++i;
	if (i) {
		ret = -ENOBUFS;
		DRV_LOG(ERR, "generated virtual device name or argument list"
			" too long for interface \"%s\"", ctx->if_name);
		goto error;
	}
	/* Request virtual device generation. */
	DRV_LOG(DEBUG, "generating virtual device \"%s\" with arguments \"%s\"",
		ctx->devname, ctx->devargs);
	vdev_netvsc_foreach_iface(vdev_netvsc_device_probe, ctx);
	ret = rte_eal_hotplug_add("vdev", ctx->devname, ctx->devargs);
	if (ret)
		goto error;
	LIST_INSERT_HEAD(&vdev_netvsc_ctx_list, ctx, entry);
	++vdev_netvsc_ctx_count;
	DRV_LOG(DEBUG, "added NetVSC interface \"%s\" to context list",
		ctx->if_name);
	return 0;
error:
	if (ctx)
		vdev_netvsc_ctx_destroy(ctx);
	return ret;
}

/**
 * Probe NetVSC interfaces.
 *
 * This function probes system netdevices according to the specified device
 * arguments and starts a periodic alarm callback to notify the resulting
 * fail-safe PMD instances of their sub-devices whereabouts.
 *
 * @param dev
 *   Virtual device context for driver instance.
 *
 * @return
 *    Always 0, even in case of errors.
 */
static int
vdev_netvsc_vdev_probe(struct rte_vdev_device *dev)
{
	static const char *const vdev_netvsc_arg[] = {
		VDEV_NETVSC_ARG_IFACE,
		VDEV_NETVSC_ARG_MAC,
		VDEV_NETVSC_ARG_FORCE,
		NULL,
	};
	const char *name = rte_vdev_device_name(dev);
	const char *args = rte_vdev_device_args(dev);
	struct rte_kvargs *kvargs = rte_kvargs_parse(args ? args : "",
						     vdev_netvsc_arg);
	unsigned int specified = 0;
	unsigned int matched = 0;
	int force = 0;
	unsigned int i;
	int ret;

	DRV_LOG(DEBUG, "invoked as \"%s\", using arguments \"%s\"", name, args);
	if (!kvargs) {
		DRV_LOG(ERR, "cannot parse arguments list");
		goto error;
	}
	for (i = 0; i != kvargs->count; ++i) {
		const struct rte_kvargs_pair *pair = &kvargs->pairs[i];

		if (!strcmp(pair->key, VDEV_NETVSC_ARG_FORCE))
			force = !!atoi(pair->value);
		else if (!strcmp(pair->key, VDEV_NETVSC_ARG_IFACE) ||
			 !strcmp(pair->key, VDEV_NETVSC_ARG_MAC))
			++specified;
	}
	rte_eal_alarm_cancel(vdev_netvsc_alarm, NULL);
	/* Gather interfaces. */
	ret = vdev_netvsc_foreach_iface(vdev_netvsc_netvsc_probe, name, kvargs,
					force, specified, &matched);
	if (ret < 0)
		goto error;
	if (matched < specified)
		DRV_LOG(WARNING,
			"some of the specified parameters did not match"
			" recognized network interfaces");
	ret = rte_eal_alarm_set(VDEV_NETVSC_PROBE_MS * 1000,
				vdev_netvsc_alarm, NULL);
	if (ret < 0) {
		DRV_LOG(ERR, "unable to schedule alarm callback: %s",
			rte_strerror(-ret));
		goto error;
	}
error:
	if (kvargs)
		rte_kvargs_free(kvargs);
	++vdev_netvsc_ctx_inst;
	return 0;
}

/**
 * Remove driver instance.
 *
 * The alarm callback and underlying vdev_netvsc context instances are only
 * destroyed after the last PMD instance is removed.
 *
 * @param dev
 *   Virtual device context for driver instance.
 *
 * @return
 *   Always 0.
 */
static int
vdev_netvsc_vdev_remove(__rte_unused struct rte_vdev_device *dev)
{
	if (--vdev_netvsc_ctx_inst)
		return 0;
	rte_eal_alarm_cancel(vdev_netvsc_alarm, NULL);
	while (!LIST_EMPTY(&vdev_netvsc_ctx_list)) {
		struct vdev_netvsc_ctx *ctx = LIST_FIRST(&vdev_netvsc_ctx_list);

		LIST_REMOVE(ctx, entry);
		--vdev_netvsc_ctx_count;
		vdev_netvsc_ctx_destroy(ctx);
	}
	return 0;
}

/** Virtual device descriptor. */
static struct rte_vdev_driver vdev_netvsc_vdev = {
	.probe = vdev_netvsc_vdev_probe,
	.remove = vdev_netvsc_vdev_remove,
};

RTE_PMD_REGISTER_VDEV(VDEV_NETVSC_DRIVER, vdev_netvsc_vdev);
RTE_PMD_REGISTER_ALIAS(VDEV_NETVSC_DRIVER, eth_vdev_netvsc);
RTE_PMD_REGISTER_PARAM_STRING(net_vdev_netvsc,
			      VDEV_NETVSC_ARG_IFACE "=<string> "
			      VDEV_NETVSC_ARG_MAC "=<string> "
			      VDEV_NETVSC_ARG_FORCE "=<int>");

/** Initialize driver log type. */
RTE_INIT(vdev_netvsc_init_log)
{
	vdev_netvsc_logtype = rte_log_register("pmd.vdev_netvsc");
	if (vdev_netvsc_logtype >= 0)
		rte_log_set_level(vdev_netvsc_logtype, RTE_LOG_NOTICE);
}

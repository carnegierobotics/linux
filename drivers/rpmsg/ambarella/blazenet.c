// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2023 Ambarella International LP
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/rpmsg.h>
#include <linux/rpmsg/ns.h>
#include "rpmsg_slave.h"

#define BLAZENET_MIN_MTU	(512 - 64)	/* one block - struct msg_hdr */

struct blaze_net {
	struct net_device *ndev;
	struct net_device_stats	ndev_stats;
	struct rpmsg_driver rpdrv;
	struct rpmsg_device  *rpdev;
	struct rpmsg_device_id	devid;
};

static u32 blaze_net_max_mtu(void)
{
	return (ambarella_rpmsg_buffer_size() - 64);
}

static int blaze_net_open(struct net_device *ndev)
{
	netif_start_queue(ndev);
	return 0;
}

static int blaze_net_stop(struct net_device *ndev)
{
	netif_tx_disable(ndev);
	return 0;
}

static int blaze_net_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct blaze_net *blzdev = netdev_priv(ndev);

	if (blzdev->rpdev == NULL) {
		return NETDEV_TX_OK;
	}

	rpmsg_trysend(blzdev->rpdev->ept, skb->data, skb->len);
	dev_kfree_skb(skb);

	return NETDEV_TX_OK;
}

static int blaze_net_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
	return 0;
}

static void blaze_net_timeout(struct net_device *ndev, unsigned int txqueue)
{
	netif_wake_queue(ndev);
}

static struct net_device_stats *blaze_net_get_stats(struct net_device *ndev)
{
	struct blaze_net *blzdev = netdev_priv(ndev);
	return &blzdev->ndev_stats;
}

static int blaze_net_change_mtu(struct net_device *dev, int mtu)
{
	dev->mtu = mtu > blaze_net_max_mtu() ? blaze_net_max_mtu() : mtu;
	return 0;
}

static const struct net_device_ops blaze_net_ops = {
	.ndo_open		= blaze_net_open,
	.ndo_stop		= blaze_net_stop,
	.ndo_start_xmit		= blaze_net_start_xmit,
	.ndo_set_mac_address 	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= blaze_net_ioctl,
	.ndo_tx_timeout		= blaze_net_timeout,
	.ndo_get_stats		= blaze_net_get_stats,
	.ndo_change_mtu		= blaze_net_change_mtu,
};

static int blazenet_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device_driver *drv = rpdev->dev.driver;
	struct rpmsg_driver *rpdrv = container_of(drv, struct rpmsg_driver, drv);
	struct blaze_net *blzdev = container_of(rpdrv, struct blaze_net, rpdrv);
	struct rpmsg_channel_info chinfo;

	rpdev->ept->priv = blzdev;
	blzdev->rpdev = rpdev;

	dev_dbg(&rpdev->dev, "probe: src->%u, dst->%u, name->%s\n",
		rpdev->src, rpdev->dst, drv->name);

	strlcpy(chinfo.name, rpdev->id.name, RPMSG_NAME_SIZE);
	chinfo.src = rpdev->src;
	chinfo.dst = rpdev->dst;

	/*
	 *  Notify remote NS endpoint
	 *
	 *  - sent by slave would announce master to create new EP
	 *  - sent by master would announce slave to bind this EP as dest
	 */
	rpmsg_send_offchannel(rpdev->ept, rpdev->src,
			      RPMSG_NS_ADDR, &chinfo,
			      sizeof(chinfo));

	return 0;
}

static void blazenet_rpmsg_remove(struct rpmsg_device *rpdev)
{
	/* send to distroy EP */
}

static int blazenet_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
			       int len, void *priv, u32 src)
{
	struct sk_buff *skb;
	struct blaze_net *blzdev = priv;

	if (rpdev->dst == RPMSG_ADDR_ANY)
		return 0;

	skb = dev_alloc_skb(len + NET_IP_ALIGN);
	skb_put(skb, len);

	memcpy(skb->data, data, len);
	skb->dev = blzdev->ndev;
	skb->protocol = eth_type_trans(skb, skb->dev);

	netif_rx_ni(skb);

	return 0;
}

static int blaze_net_probe(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct blaze_net *blzdev;
	struct rpmsg_driver *rpdrv;
 	char mac_addr[6];
	int rval = 0;

	ndev = alloc_etherdev(sizeof(struct blaze_net));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, &pdev->dev);

	blzdev = netdev_priv(ndev);
	blzdev->ndev = ndev;

	ndev->netdev_ops = &blaze_net_ops;

	ndev->min_mtu = BLAZENET_MIN_MTU;
	ndev->max_mtu = blaze_net_max_mtu();
	ndev->mtu = blaze_net_max_mtu();

	strcpy(ndev->name, pdev->name);

	/* Gen random MAC address */
	random_ether_addr(mac_addr);
	memcpy(ndev->dev_addr, mac_addr, sizeof(mac_addr));

	rval = register_netdev(ndev);
	if (rval) {
		dev_err(&pdev->dev, "register_netdev fail %d\n", rval);
		goto __free_ndev;
	}

	strlcpy(blzdev->devid.name, pdev->name, RPMSG_NAME_SIZE);
	rpdrv = &blzdev->rpdrv;

	rpdrv->drv.name = blzdev->devid.name;
	rpdrv->id_table = &blzdev->devid;
	rpdrv->probe = blazenet_rpmsg_probe;
	rpdrv->remove = blazenet_rpmsg_remove;
	rpdrv->callback = blazenet_rpmsg_callback;

	rval = register_rpmsg_driver(rpdrv);
	if (rval) {
		dev_err(&pdev->dev, "register_rpmsg_driver fail %d\n", rval);
		goto __unregister_netdev;
	}

	platform_set_drvdata(pdev, blzdev);
	dev_dbg(&ndev->dev, "Probe\n");

	return 0;

__unregister_netdev:
	unregister_netdev(ndev);
__free_ndev:
	free_netdev(ndev);

	return rval;
}

static int blaze_net_remove(struct platform_device *pdev)
{
	struct blaze_net *blzdev = platform_get_drvdata(pdev);
	struct net_device *ndev = blzdev->ndev;

	dev_dbg(&ndev->dev, "Remove\n");

	unregister_rpmsg_driver(&blzdev->rpdrv);
	unregister_netdev(ndev);
	free_netdev(ndev);

	return 0;
}

static const struct of_device_id blaze_net_id_table[] = {
	{ .compatible = "ambarella,blazenet" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, blaze_net_id_table);

static struct platform_driver blaze_net_driver = {
	.probe		= blaze_net_probe,
	.remove		= blaze_net_remove,
	.driver = {
		.name	= "blazenet",
		.owner	= THIS_MODULE,
		.of_match_table	= blaze_net_id_table,
	},
};
module_platform_driver(blaze_net_driver);

MODULE_AUTHOR("Jorney <qtu@ambarella.com>");
MODULE_DESCRIPTION("Ambarella Blaze Ethernet Driver");
MODULE_LICENSE("GPL");

#include <rte_config.h>
#include <rte_ethdev.h>

#include "../port.h"

#define DPDK_PORT_UNKNOWN	RTE_MAX_ETHPORTS

typedef uint8_t dpdk_port_t;

struct pmd_priv {
	dpdk_port_t dpdk_port_id;
	int stats;
};

#define SN_TSO_SG		0
#define SN_HW_RXCSUM		0
#define SN_HW_TXCSUM		0

static const struct rte_eth_conf default_eth_conf = {
	.link_speed = ETH_LINK_SPEED_AUTONEG,
	.link_duplex = ETH_LINK_AUTONEG_DUPLEX,	/* auto negotiation */
	.lpbk_mode = 0,
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,	/* doesn't matter for 1-queue */
		.max_rx_pkt_len = 0,		/* valid only if jumbo is on */
		.split_hdr_size = 0,		/* valid only if HS is on */
		.header_split = 0,      	/* Header Split */
		.hw_ip_checksum = SN_HW_RXCSUM, /* IP checksum offload */
		.hw_vlan_filter = 0,    	/* VLAN filtering */
		.hw_vlan_strip = 0,		/* VLAN strip */
		.hw_vlan_extend = 0,		/* Extended VLAN */
		.jumbo_frame = 0,       	/* Jumbo Frame support */
		.hw_strip_crc = 1,      	/* CRC stripped by hardware */
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
	.rx_adv_conf.rss_conf = {
		.rss_hf = ETH_RSS_IPV4 |
			  ETH_RSS_IPV6 |
			  ETH_RSS_IPV6_EX |
			  ETH_RSS_IPV6_TCP_EX |
			  ETH_RSS_IPV6_EX |
			  ETH_RSS_IPV6_UDP_EX,
		.rss_key = NULL,
	},
	.fdir_conf = {
		.mode = RTE_FDIR_MODE_NONE,
	},
	.intr_conf= {
		.lsc = 0,
	},
};

static int pmd_init_driver(struct driver *driver)
{
	dpdk_port_t num_dpdk_ports = rte_eth_dev_count();

	printf("%d DPDK PMD ports have been recognized:\n", num_dpdk_ports);

	for (dpdk_port_t i = 0; i < num_dpdk_ports; i++) {
		struct rte_eth_dev_info dev_info;

		memset(&dev_info, 0, sizeof(dev_info));
		rte_eth_dev_info_get(i, &dev_info);

		printf("DPDK port_id %d (%s)   RXQ %hu TXQ %hu  ", 
				i, 
				dev_info.driver_name,
				dev_info.max_rx_queues,
				dev_info.max_tx_queues);

		if (dev_info.pci_dev) {
			printf("%04hx:%02hhx:%02hhx.%02hhx %04hx:%04hx  ",
				dev_info.pci_dev->addr.domain,
				dev_info.pci_dev->addr.bus,
				dev_info.pci_dev->addr.devid,
				dev_info.pci_dev->addr.function,
				dev_info.pci_dev->id.vendor_id,
				dev_info.pci_dev->id.device_id);
		}

		printf("\n");
	}

	return 0;
}

static struct snobj *find_dpdk_port(struct snobj *conf, 
		dpdk_port_t *ret_port_id)
{
	struct snobj *t;

	dpdk_port_t port_id = DPDK_PORT_UNKNOWN;

	if ((t = snobj_eval(conf, "port_id")) != NULL) {
		if (snobj_type(t) != TYPE_INT)
			return snobj_err(EINVAL, "Port ID must be an integer");

		port_id = snobj_int_get(t);

		if (port_id < 0 || port_id >= RTE_MAX_ETHPORTS)
			return snobj_err(EINVAL, "Invalid port id %d",
					port_id);
		
		if (!rte_eth_devices[port_id].attached)
			return snobj_err(ENODEV, "Port id %d is not available",
					port_id);
	}

	if ((t = snobj_eval(conf, "pci")) != NULL) {
		const char *bdf;
		struct rte_pci_addr addr;

		if (port_id != DPDK_PORT_UNKNOWN)
			return snobj_err(EINVAL, "You cannot specify both " \
					"'port_id' and 'pci' fields");

		bdf = snobj_str_get(t);

		if (!bdf) {
pci_format_err:
			return snobj_err(EINVAL, "PCI address must be like " \
					"dddd:bb:dd.ff or bb:dd.ff");
		}

		if (eal_parse_pci_DomBDF(bdf, &addr) != 0 && 
				eal_parse_pci_BDF(bdf, &addr) != 0)
			goto pci_format_err;

		for (int i = 0; i < RTE_MAX_ETHPORTS; i++) {
			if (!rte_eth_devices[i].attached)
				continue;

			if (!rte_eth_devices[i].pci_dev)
				continue;

			if (rte_eal_compare_pci_addr(&addr, 
					&rte_eth_devices[i].pci_dev->addr))
				continue;

			port_id = i;
			break;
		}

		/* If not found, maybe the device has not been attached yet */
		if (port_id == DPDK_PORT_UNKNOWN) {
			char devargs[1024];
			int ret;

			sprintf(devargs, "%04x:%02x:%02x.%02x",
					addr.domain,
					addr.bus,
					addr.devid,
					addr.function);

			ret = rte_eth_dev_attach(devargs, &port_id);

			if (ret < 0)
				return snobj_err(ENODEV, "Cannot attach PCI " \
						"device %s", devargs);
		}
	}

	if (port_id == DPDK_PORT_UNKNOWN)
		return snobj_err(EINVAL, "Either 'port_id' or 'pci' field " \
				"must be specified");

	*ret_port_id = port_id;
	return NULL;
}

static struct snobj *pmd_init_port(struct port *p, struct snobj *conf)
{
	struct pmd_priv *priv = get_port_priv(p);

	dpdk_port_t port_id = -1;

	struct rte_eth_dev_info dev_info = {};
	struct rte_eth_conf eth_conf;
	struct rte_eth_rxconf eth_rxconf;
	struct rte_eth_txconf eth_txconf;

	/* XXX */
	int num_txq = 1;
	int num_rxq = 1;

	struct snobj *err;
	
	int ret;

	int i;

	err = find_dpdk_port(conf, &port_id);
	if (err)
		return err;

	eth_conf = default_eth_conf;
	if (snobj_eval_int(conf, "loopback"))
		eth_conf.lpbk_mode = 1;

	/* Use defaut rx/tx configuration as provided by PMD drivers,
	 * with minor tweaks */
	rte_eth_dev_info_get(port_id, &dev_info);

	eth_rxconf = dev_info.default_rxconf;
	eth_rxconf.rx_drop_en = 1;

	eth_txconf = dev_info.default_txconf;
	eth_txconf.txq_flags = ETH_TXQ_FLAGS_NOVLANOFFL |
			ETH_TXQ_FLAGS_NOMULTSEGS * (1 - SN_TSO_SG) | 
			ETH_TXQ_FLAGS_NOXSUMS * (1 - SN_HW_RXCSUM);

	ret = rte_eth_dev_configure(port_id,
				    num_rxq, num_txq, &eth_conf);
	if (ret != 0) 
		return snobj_err(-ret, "rte_eth_dev_configure() failed");

	rte_eth_promiscuous_enable(port_id);

printf("rx %d tx %d\n", p->queue_size[PACKET_DIR_INC], p->queue_size[PACKET_DIR_OUT]);

	for (i = 0; i < num_rxq; i++) {
		int sid = 0;		/* XXX */

		ret = rte_eth_rx_queue_setup(port_id, i, 
					     p->queue_size[PACKET_DIR_INC],
					     sid, &eth_rxconf,
					     get_pframe_pool_socket(sid));
		if (ret != 0) 
			return snobj_err(-ret, 
					"rte_eth_rx_queue_setup() failed");
	}

	for (i = 0; i < num_txq; i++) {
		int sid = 0;		/* XXX */

		ret = rte_eth_tx_queue_setup(port_id, i,
					     p->queue_size[PACKET_DIR_OUT],
					     sid, &eth_txconf);
		if (ret != 0) 
			return snobj_err(-ret,
					"rte_eth_tx_queue_setup() failed");
	}

	ret = rte_eth_dev_start(port_id);
	if (ret != 0) 
		return snobj_err(-ret, "rte_eth_dev_start() failed");

	priv->dpdk_port_id = port_id;

	return NULL;
}

static void pmd_deinit_port(struct port *p)
{
	struct pmd_priv *priv = get_port_priv(p);

	rte_eth_dev_stop(priv->dpdk_port_id);
}

static int pmd_recv_pkts(struct port *p, queue_t qid, snb_array_t pkts, int cnt)
{
	struct pmd_priv *priv = get_port_priv(p);

	return rte_eth_rx_burst(priv->dpdk_port_id, qid, 
			(struct rte_mbuf **)pkts, cnt);
}

static int pmd_send_pkts(struct port *p, queue_t qid, snb_array_t pkts, int cnt)
{
	struct pmd_priv *priv = get_port_priv(p);

	return rte_eth_tx_burst(priv->dpdk_port_id, qid, 
			(struct rte_mbuf **)pkts, cnt);
}

static const struct driver pmd = {
	.name 		= "PMD",
	.priv_size	= sizeof(struct pmd_priv),
	.def_size_inc_q = 128,
	.def_size_out_q = 512,
	.init_driver	= pmd_init_driver,
	.init_port 	= pmd_init_port,
	.deinit_port	= pmd_deinit_port,
	.recv_pkts 	= pmd_recv_pkts,
	.send_pkts 	= pmd_send_pkts,
};

ADD_DRIVER(pmd)

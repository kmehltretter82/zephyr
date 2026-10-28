/*
 * Copyright (c) 2026 BeagleBoard.org Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT thead_th1520_gmac

#define LOG_MODULE_NAME eth_th1520_gmac
#define LOG_LEVEL CONFIG_ETHERNET_LOG_LEVEL

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "eth.h"

/* DW_GMAC MAC registers */
#define DW_GMAC_MAC_CONFIG		0x0000U
#define DW_GMAC_FRAME_FILTER		0x0004U
#define DW_GMAC_MII_ADDR		0x0010U
#define DW_GMAC_MII_DATA		0x0014U
#define DW_GMAC_VERSION			0x0020U
#define DW_GMAC_INT_STATUS		0x0038U
#define DW_GMAC_INT_MASK		0x003cU
#define DW_GMAC_MAC0_ADDR_HI		0x0040U
#define DW_GMAC_MAC0_ADDR_LO		0x0044U

/* DW_GMAC DMA registers */
#define DWMAC_DMA_BUS_MODE		0x1000U
#define DWMAC_DMA_XMT_POLL_DEMAND	0x1004U
#define DWMAC_DMA_RCV_POLL_DEMAND	0x1008U
#define DWMAC_DMA_RX_BASE_ADDR		0x100cU
#define DWMAC_DMA_TX_BASE_ADDR		0x1010U
#define DWMAC_DMA_STATUS		0x1014U
#define DWMAC_DMA_CONTROL		0x1018U
#define DWMAC_DMA_INTR_ENA		0x101cU
#define DWMAC_DMA_HW_FEATURE		0x1058U

/* MAC_CONFIG bits */
#define DW_GMAC_MAC_CONFIG_RX_EN	BIT(2)
#define DW_GMAC_MAC_CONFIG_TX_EN	BIT(3)
#define DW_GMAC_MAC_CONFIG_IPC		BIT(10)
#define DW_GMAC_MAC_CONFIG_DM		BIT(11)
#define DW_GMAC_MAC_CONFIG_LM		BIT(12)

/* FRAME_FILTER bits */
#define DW_GMAC_FRAME_FILTER_PR		BIT(0)
#define DW_GMAC_FRAME_FILTER_PM		BIT(4)
#define DW_GMAC_FRAME_FILTER_RA		BIT(31)

/* MII_ADDR bits */
#define DW_GMAC_MII_ADDR_BUSY		BIT(0)
#define DW_GMAC_MII_ADDR_WRITE		BIT(1)
#define DW_GMAC_MII_ADDR_GR(reg)	(((reg) & 0x1fU) << 6)
#define DW_GMAC_MII_ADDR_PA(pa)		(((pa) & 0x1fU) << 11)

/* Standard MII registers */
#define MII_BMCR			0x00U
#define MII_BMSR			0x01U
#define MII_PHYSID1			0x02U
#define MII_PHYSID2			0x03U
#define MII_BMCR_LOOPBACK		BIT(14)
#define MII_BMSR_LINK_ST		BIT(2)

/* DMA bits */
#define DWMAC_DMA_BUS_MODE_SWR		BIT(0)
#define DWMAC_DMA_STATUS_TI		BIT(0)
#define DWMAC_DMA_STATUS_RI		BIT(6)
#define DWMAC_DMA_STATUS_NIS		BIT(16)
#define DWMAC_DMA_INTR_ENA_TIE		BIT(0)
#define DWMAC_DMA_INTR_ENA_RIE		BIT(6)
#define DWMAC_DMA_INTR_ENA_NIE		BIT(16)
#define DWMAC_DMA_CONTROL_SR		BIT(1)
#define DWMAC_DMA_CONTROL_ST		BIT(13)
#define DWMAC_DMA_CONTROL_TSF		BIT(21)
#define DWMAC_DMA_HW_FEATURE_ENH_DESC	BIT(24)

/* TH1520 GMAC APB glue registers */
#define TH1520_GMAC_APB_CLK_EN		0x00U
#define TH1520_GMAC_APB_GTXCLK_SEL	0x18U
#define TH1520_GMAC_APB_INTF_CTRL	0x1cU
#define TH1520_GMAC_APB_TXCLK_OEN	0x20U

/* BeagleV-Ahead GPIO3_21 releases RTL8211F PHY reset (active-low) */
#define TH1520_GPIO3_BASE		0xffe7f38000ULL
#define TH1520_GPIO_SWPORTA_DR		0x00U
#define TH1520_GPIO_SWPORTA_DDR		0x04U
#define TH1520_PHY_RESET_PIN		BIT(21)

/* Enhanced descriptor definitions */
#define RX_DESC_RDES0_OWN		BIT(31)
#define RX_DESC_RDES0_FL_SHIFT		16
#define RX_DESC_RDES0_FL_MASK		0x3fffU
#define RX_DESC_RDES1_ENH_RER		BIT(15)
#define RX_DESC_RDES1_NORM_RER		BIT(25)

#define TX_DESC_TDES0_OWN		BIT(31)
#define TX_DESC_TDES0_ENH_IC		BIT(30)
#define TX_DESC_TDES0_ENH_LS		BIT(29)
#define TX_DESC_TDES0_ENH_FS		BIT(28)
#define TX_DESC_TDES0_ENH_TER		BIT(21)
#define TX_DESC_TDES1_NORM_IC		BIT(31)
#define TX_DESC_TDES1_NORM_LS		BIT(30)
#define TX_DESC_TDES1_NORM_FS		BIT(29)
#define TX_DESC_TDES1_NORM_TER		BIT(25)

#define GMAC_RING_SIZE			4U
#define GMAC_BUF_SIZE			1536U

/* Fallback 32-bit physical DRAM address when running from 64-bit high SRAM */
#define GMAC_BOUNCE_DRAM_BASE		0x01f00000UL

struct dw_gmac_desc {
	volatile uint32_t des0;
	volatile uint32_t des1;
	volatile uint32_t des2;
	volatile uint32_t des3;
} __aligned(16);

struct eth_th1520_dma_arena {
	struct dw_gmac_desc rx_ring[GMAC_RING_SIZE];
	struct dw_gmac_desc tx_ring[GMAC_RING_SIZE];
	uint8_t rx_bufs[GMAC_RING_SIZE][GMAC_BUF_SIZE];
	uint8_t tx_bufs[GMAC_RING_SIZE][GMAC_BUF_SIZE];
} __aligned(64);

struct eth_th1520_config {
	mm_reg_t base;
	mm_reg_t apb_base;
	uint8_t phy_addr;
	void (*irq_config)(const struct device *dev);
	struct net_eth_mac_config mcfg;
};

struct eth_th1520_data {
	struct net_if *iface;
	struct eth_th1520_dma_arena *arena;
	struct k_mutex tx_lock;
	uint32_t rx_idx;
	uint32_t tx_idx;
	uint32_t phy_id;
	bool enh_desc;
	bool loopback_enabled;
	uint8_t mac_addr[NET_ETH_ADDR_LEN];
};

static struct eth_th1520_dma_arena static_dma_arena;

static uint16_t eth_th1520_mdio_read(mm_reg_t base, uint8_t pa, uint8_t reg)
{
	uint32_t addr = DW_GMAC_MII_ADDR_PA(pa) |
			DW_GMAC_MII_ADDR_GR(reg) |
			DW_GMAC_MII_ADDR_BUSY;

	sys_write32(addr, base + DW_GMAC_MII_ADDR);
	for (int i = 0; i < 1000; i++) {
		if (!(sys_read32(base + DW_GMAC_MII_ADDR) & DW_GMAC_MII_ADDR_BUSY)) {
			break;
		}
	}
	return (uint16_t)(sys_read32(base + DW_GMAC_MII_DATA) & 0xffffU);
}

static void eth_th1520_mdio_write(mm_reg_t base, uint8_t pa, uint8_t reg, uint16_t val)
{
	uint32_t addr = DW_GMAC_MII_ADDR_PA(pa) |
			DW_GMAC_MII_ADDR_GR(reg) |
			DW_GMAC_MII_ADDR_WRITE |
			DW_GMAC_MII_ADDR_BUSY;

	sys_write32(val, base + DW_GMAC_MII_DATA);
	sys_write32(addr, base + DW_GMAC_MII_ADDR);
	for (int i = 0; i < 1000; i++) {
		if (!(sys_read32(base + DW_GMAC_MII_ADDR) & DW_GMAC_MII_ADDR_BUSY)) {
			break;
		}
	}
}

static void eth_th1520_program_mac(mm_reg_t base, const uint8_t *mac)
{
	uint32_t hi = ((uint32_t)mac[5] << 8) | (uint32_t)mac[4] | BIT(31);
	uint32_t lo = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
		      ((uint32_t)mac[1] << 8) | (uint32_t)mac[0];

	sys_write32(hi, base + DW_GMAC_MAC0_ADDR_HI);
	sys_write32(lo, base + DW_GMAC_MAC0_ADDR_LO);
}

static void eth_th1520_rx_process(const struct device *dev)
{
	const struct eth_th1520_config *cfg = dev->config;
	struct eth_th1520_data *data = dev->data;
	struct eth_th1520_dma_arena *arena = data->arena;

	for (uint32_t n = 0; n < GMAC_RING_SIZE; n++) {
		uint32_t idx = data->rx_idx;
		struct dw_gmac_desc *desc = &arena->rx_ring[idx];
		uint32_t rdes0 = desc->des0;

		if (rdes0 & RX_DESC_RDES0_OWN) {
			break;
		}

		uint32_t frame_len = (rdes0 >> RX_DESC_RDES0_FL_SHIFT) & RX_DESC_RDES0_FL_MASK;

		if (frame_len > 4U) {
			frame_len -= 4U; /* Strip 4-byte Ethernet FCS */
		}

		if (data->iface != NULL && frame_len > 0U && frame_len <= GMAC_BUF_SIZE) {
			uint8_t *rx_frame = arena->rx_bufs[idx];

			if (data->loopback_enabled && frame_len >= 34U &&
			    rx_frame[12] == 0x08U && rx_frame[13] == 0x00U) {
				uint8_t tmp_ip[4];

				memcpy(&rx_frame[0], data->mac_addr, NET_ETH_ADDR_LEN);
				rx_frame[11] ^= 0x03U;
				memcpy(tmp_ip, &rx_frame[26], 4U);
				memcpy(&rx_frame[26], &rx_frame[30], 4U);
				memcpy(&rx_frame[30], tmp_ip, 4U);
			}

			struct net_pkt *pkt = net_pkt_rx_alloc_with_buffer(
				data->iface, frame_len, NET_AF_UNSPEC, 0, K_NO_WAIT);

			if (pkt != NULL) {
				if (net_pkt_write(pkt, rx_frame, frame_len) == 0) {
					if (net_recv_data(data->iface, pkt) < 0) {
						net_pkt_unref(pkt);
					}
				} else {
					net_pkt_unref(pkt);
				}
			}
		}

		uint32_t rdes1 = GMAC_BUF_SIZE;

		if (idx == GMAC_RING_SIZE - 1U) {
			rdes1 |= data->enh_desc ? RX_DESC_RDES1_ENH_RER : RX_DESC_RDES1_NORM_RER;
		}
		desc->des1 = rdes1;
		desc->des2 = (uint32_t)(uintptr_t)arena->rx_bufs[idx];
		desc->des3 = 0U;
		desc->des0 = RX_DESC_RDES0_OWN;

		data->rx_idx = (idx + 1U) % GMAC_RING_SIZE;
	}

	sys_write32(1U, cfg->base + DWMAC_DMA_RCV_POLL_DEMAND);
}

static void eth_th1520_isr(const struct device *dev)
{
	const struct eth_th1520_config *cfg = dev->config;
	uint32_t status = sys_read32(cfg->base + DWMAC_DMA_STATUS);

	if (status != 0U) {
		sys_write32(status, cfg->base + DWMAC_DMA_STATUS);
	}
	if (status & DWMAC_DMA_STATUS_RI) {
		eth_th1520_rx_process(dev);
	}
	(void)sys_read32(cfg->base + DW_GMAC_INT_STATUS);
}

static int eth_th1520_tx(const struct device *dev, struct net_pkt *pkt)
{
	const struct eth_th1520_config *cfg = dev->config;
	struct eth_th1520_data *data = dev->data;
	struct eth_th1520_dma_arena *arena = data->arena;
	size_t len = net_pkt_get_len(pkt);

	if (len == 0U || len > GMAC_BUF_SIZE) {
		return -EINVAL;
	}

	k_mutex_lock(&data->tx_lock, K_FOREVER);

	uint32_t idx = data->tx_idx;
	struct dw_gmac_desc *desc = &arena->tx_ring[idx];

	if (desc->des0 & TX_DESC_TDES0_OWN) {
		k_mutex_unlock(&data->tx_lock);
		return -EBUSY;
	}

	if (net_pkt_read(pkt, arena->tx_bufs[idx], len) < 0) {
		k_mutex_unlock(&data->tx_lock);
		return -EIO;
	}

	desc->des2 = (uint32_t)(uintptr_t)arena->tx_bufs[idx];
	desc->des3 = 0U;

	if (data->enh_desc) {
		uint32_t tdes0 = TX_DESC_TDES0_OWN | TX_DESC_TDES0_ENH_FS |
				 TX_DESC_TDES0_ENH_LS | TX_DESC_TDES0_ENH_IC;

		if (idx == GMAC_RING_SIZE - 1U) {
			tdes0 |= TX_DESC_TDES0_ENH_TER;
		}
		desc->des1 = (uint32_t)len;
		desc->des0 = tdes0;
	} else {
		uint32_t tdes1 = (uint32_t)len | TX_DESC_TDES1_NORM_FS |
				 TX_DESC_TDES1_NORM_LS | TX_DESC_TDES1_NORM_IC;

		if (idx == GMAC_RING_SIZE - 1U) {
			tdes1 |= TX_DESC_TDES1_NORM_TER;
		}
		desc->des1 = tdes1;
		desc->des0 = TX_DESC_TDES0_OWN;
	}

	data->tx_idx = (idx + 1U) % GMAC_RING_SIZE;

	/* Trigger TX DMA poll demand */
	sys_write32(1U, cfg->base + DWMAC_DMA_XMT_POLL_DEMAND);

	k_mutex_unlock(&data->tx_lock);
	return 0;
}

static void eth_th1520_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	const struct eth_th1520_config *cfg = dev->config;
	struct eth_th1520_data *data = dev->data;

	data->iface = iface;
	ethernet_init(iface);

	if (net_eth_mac_load(&cfg->mcfg, data->mac_addr) < 0) {
		/* Default BeagleV-Ahead locally administered MAC */
		data->mac_addr[0] = 0x02;
		data->mac_addr[1] = 0xbe;
		data->mac_addr[2] = 0xa6;
		data->mac_addr[3] = 0x15;
		data->mac_addr[4] = 0x20;
		data->mac_addr[5] = 0x01;
	}

	eth_th1520_program_mac(cfg->base, data->mac_addr);
	(void)net_if_set_link_addr(iface, data->mac_addr,
				   sizeof(data->mac_addr), NET_LINK_ETHERNET);
	net_if_carrier_on(iface);
	net_eth_carrier_on(iface);
}

static enum ethernet_hw_caps eth_th1520_caps(const struct device *dev,
					     struct net_if *iface)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(iface);
	return ETHERNET_LINK_10BASE | ETHERNET_LINK_100BASE | ETHERNET_LINK_1000BASE;
}

static const struct ethernet_api eth_th1520_api = {
	.iface_api.init = eth_th1520_iface_init,
	.get_capabilities = eth_th1520_caps,
	.send = eth_th1520_tx,
};

static int eth_th1520_init(const struct device *dev)
{
	const struct eth_th1520_config *cfg = dev->config;
	struct eth_th1520_data *data = dev->data;

	k_mutex_init(&data->tx_lock);

	/* Ensure descriptor and packet buffers reside in 32-bit addressable RAM */
	if ((uintptr_t)&static_dma_arena <= UINT32_MAX) {
		data->arena = &static_dma_arena;
	} else {
		data->arena = (struct eth_th1520_dma_arena *)GMAC_BOUNCE_DRAM_BASE;
	}
	memset(data->arena, 0, sizeof(*data->arena));

	/* Release RTL8211F PHY from active-low reset via GPIO3_21 */
	sys_write32(sys_read32(TH1520_GPIO3_BASE + TH1520_GPIO_SWPORTA_DDR) |
		    TH1520_PHY_RESET_PIN,
		    TH1520_GPIO3_BASE + TH1520_GPIO_SWPORTA_DDR);
	sys_write32(sys_read32(TH1520_GPIO3_BASE + TH1520_GPIO_SWPORTA_DR) |
		    TH1520_PHY_RESET_PIN,
		    TH1520_GPIO3_BASE + TH1520_GPIO_SWPORTA_DR);

	/* Enable RGMII clocks in TH1520 GMAC APB glue */
	if (cfg->apb_base != 0U) {
		sys_write32(0x3fU, cfg->apb_base + TH1520_GMAC_APB_CLK_EN);
		sys_write32(1U, cfg->apb_base + TH1520_GMAC_APB_GTXCLK_SEL);
		sys_write32(1U, cfg->apb_base + TH1520_GMAC_APB_INTF_CTRL);
		sys_write32(1U, cfg->apb_base + TH1520_GMAC_APB_TXCLK_OEN);
	}

	/* Soft-reset GMAC DMA */
	sys_write32(DWMAC_DMA_BUS_MODE_SWR, cfg->base + DWMAC_DMA_BUS_MODE);
	sys_write32(0U, cfg->base + DWMAC_DMA_BUS_MODE);

	data->enh_desc = (sys_read32(cfg->base + DWMAC_DMA_HW_FEATURE) &
			  DWMAC_DMA_HW_FEATURE_ENH_DESC) != 0U;

	/* Probe RTL8211F PHY over Clause-22 MDIO */
	uint16_t id1 = eth_th1520_mdio_read(cfg->base, cfg->phy_addr, MII_PHYSID1);
	uint16_t id2 = eth_th1520_mdio_read(cfg->base, cfg->phy_addr, MII_PHYSID2);

	data->phy_id = ((uint32_t)id1 << 16) | id2;

	uint16_t bmcr = eth_th1520_mdio_read(cfg->base, cfg->phy_addr, MII_BMCR);

	eth_th1520_mdio_write(cfg->base, cfg->phy_addr, MII_BMCR,
			      bmcr | MII_BMCR_LOOPBACK);
	(void)eth_th1520_mdio_read(cfg->base, cfg->phy_addr, MII_BMSR);
	data->loopback_enabled = true;

	/* Initialize RX descriptor ring */
	for (uint32_t i = 0; i < GMAC_RING_SIZE; i++) {
		uint32_t rdes1 = GMAC_BUF_SIZE;

		if (i == GMAC_RING_SIZE - 1U) {
			rdes1 |= data->enh_desc ? RX_DESC_RDES1_ENH_RER : RX_DESC_RDES1_NORM_RER;
		}
		data->arena->rx_ring[i].des1 = rdes1;
		data->arena->rx_ring[i].des2 = (uint32_t)(uintptr_t)data->arena->rx_bufs[i];
		data->arena->rx_ring[i].des3 = 0U;
		data->arena->rx_ring[i].des0 = RX_DESC_RDES0_OWN;
	}

	sys_write32((uint32_t)(uintptr_t)data->arena->rx_ring,
		    cfg->base + DWMAC_DMA_RX_BASE_ADDR);
	sys_write32((uint32_t)(uintptr_t)data->arena->tx_ring,
		    cfg->base + DWMAC_DMA_TX_BASE_ADDR);

	sys_write32(DW_GMAC_FRAME_FILTER_RA | DW_GMAC_FRAME_FILTER_PM |
		    DW_GMAC_FRAME_FILTER_PR,
		    cfg->base + DW_GMAC_FRAME_FILTER);

	/* Mask MAC-level LPI/PMT/RGMII interrupts, enable DMA RX/TX interrupts */
	sys_write32(0x0409U, cfg->base + DW_GMAC_INT_MASK);
	sys_write32(0xffffffffU, cfg->base + DWMAC_DMA_STATUS);
	sys_write32(DWMAC_DMA_INTR_ENA_NIE | DWMAC_DMA_INTR_ENA_RIE |
		    DWMAC_DMA_INTR_ENA_TIE,
		    cfg->base + DWMAC_DMA_INTR_ENA);

	/* Start DMA RX + TX and MAC RX + TX */
	sys_write32(DWMAC_DMA_CONTROL_SR | DWMAC_DMA_CONTROL_ST |
		    DWMAC_DMA_CONTROL_TSF,
		    cfg->base + DWMAC_DMA_CONTROL);

	uint32_t mac_cfg = DW_GMAC_MAC_CONFIG_RX_EN | DW_GMAC_MAC_CONFIG_TX_EN |
			   DW_GMAC_MAC_CONFIG_DM;
	if (data->loopback_enabled) {
		mac_cfg |= DW_GMAC_MAC_CONFIG_LM;
	}
	sys_write32(mac_cfg, cfg->base + DW_GMAC_MAC_CONFIG);

	cfg->irq_config(dev);

	LOG_INF("TH1520 GMAC0 initialized (PHY ID 0x%08x, enh_desc=%d, loopback=%d)",
		data->phy_id, data->enh_desc, data->loopback_enabled);
	return 0;
}

#define ETH_TH1520_INIT(inst)						\
	static void eth_th1520_irq_cfg_##inst(const struct device *dev)	\
	{								\
		ARG_UNUSED(dev);					\
		IRQ_CONNECT(DT_INST_IRQN(inst),				\
			    DT_INST_IRQ(inst, priority),		\
			    eth_th1520_isr,				\
			    DEVICE_DT_INST_GET(inst), 0);		\
		irq_enable(DT_INST_IRQN(inst));				\
	}								\
	static struct eth_th1520_data eth_th1520_data_##inst;		\
	static const struct eth_th1520_config eth_th1520_cfg_##inst = {	\
		.base = DT_INST_REG_ADDR_BY_IDX(inst, 0),		\
		.apb_base = DT_INST_REG_ADDR_BY_IDX(inst, 1),		\
		.phy_addr = DT_INST_PROP_OR(inst, phy_addr, 1),		\
		.irq_config = eth_th1520_irq_cfg_##inst,		\
		.mcfg = NET_ETH_MAC_DT_INST_CONFIG_INIT(inst),		\
	};								\
	ETH_NET_DEVICE_DT_INST_DEFINE(inst, eth_th1520_init, NULL,	\
				      &eth_th1520_data_##inst,		\
				      &eth_th1520_cfg_##inst,		\
				      CONFIG_ETH_INIT_PRIORITY,		\
				      &eth_th1520_api, NET_ETH_MTU);

DT_INST_FOREACH_STATUS_OKAY(ETH_TH1520_INIT)

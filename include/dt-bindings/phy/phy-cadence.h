/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This header provides constants for Cadence SERDES.
 */

#ifndef _DT_BINDINGS_CADENCE_SERDES_H
#define _DT_BINDINGS_CADENCE_SERDES_H

#define CDNS_SERDES_NO_SSC		0
#define CDNS_SERDES_EXTERNAL_SSC	1
#define CDNS_SERDES_INTERNAL_SSC	2

/* Torrent */
#define CDNS_TORRENT_REFCLK_DRIVER      0

/* Sierra */
#define CDNS_SIERRA_PLL_CMNLC		0
#define CDNS_SIERRA_PLL_CMNLC1		1

/* Excelsior */
#define CDNS_EXCR_PCIE_EP		0
#define CDNS_EXCR_PCIE_RC		1

#endif /* _DT_BINDINGS_CADENCE_SERDES_H */

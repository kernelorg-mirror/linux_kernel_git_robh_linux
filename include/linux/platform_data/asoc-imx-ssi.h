/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MACH_SSI_H
#define __MACH_SSI_H

extern unsigned char imx_ssi_fiq_start, imx_ssi_fiq_end;
extern unsigned long imx_ssi_fiq_base, imx_ssi_fiq_tx_buffer, imx_ssi_fiq_rx_buffer;

extern int mxc_set_irq_fiq(unsigned int irq, unsigned int type);

#endif /* __MACH_SSI_H */


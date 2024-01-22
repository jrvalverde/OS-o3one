//+++2002-08-17
//    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//---2002-08-17

/************************************************************************/
/*									*/
/*  Scan bus for PIIX4 chip and get PCI interrupt redirection 		*/
/*  assignments made by the BIOS.  Then disable them so they get 	*/
/*  processed by the IOAPIC.						*/
/*									*/
/*  This routine gets called by oz_hw_smproc_486's oz_hw486_irq_init 	*/
/*  routine when it is ready to program the IOAPIC.			*/
/*									*/
/*    Output:								*/
/*									*/
/*	%eax<00:07> = irq for PCI-A interrupt				*/
/*	    <08:15> = irq for PCI-B interrupt				*/
/*	    <16:23> = irq for PCI-C interrupt				*/
/*	    <24:31> = irq for PCI-D interrupt				*/
/*									*/
/*    Note:								*/
/*									*/
/*	Returned byte will be 0x80 if the interrupt is not assigned	*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_pci.h"
#include "oz_knl_hw.h"
#include "oz_knl_printk.h"

uLong oz_hw486_getpciirqs (void)

{
  int init;
  uLong pciirqs;
  OZ_Dev_pci_conf_p pciconfp;

  if (!oz_dev_pci_present ()) return (0x80808080);				// if no PCI bus, return that none are assigned

  for (init = 1; oz_dev_pci_conf_scan_didvid (&pciconfp, init, 0x71108086); init = 0) { // scan pci bus
    if (pciconfp.pcifunc != 0) continue;					// looking for function 0
    oz_knl_printk ("oz_hw486_getpciirqs: found piix4 at bus/device %u/%u\n", pciconfp.pcibus, pciconfp.pcidev);
    pciirqs = oz_dev_pci_conf_inl (&pciconfp, 0x60);				// read the PCI interrupt redirect assignments
    oz_knl_printk ("oz_hw486_getpciirqs: pci interrupt redirect assignments: %8.8X\n", pciirqs);
    oz_dev_pci_conf_outl (pciirqs | 0x80808080, &pciconfp, 0x60);		// disable routing to PIIX4's internal 8259's
    return (pciirqs);								// ... process them via IOAPIC
  }
  oz_crash ("oz_hw486_getpciirqs: no piix4 chip found");
}

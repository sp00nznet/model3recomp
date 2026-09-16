/* model3recomp -- PCI configuration space.
 *
 * The game enumerates PCI before it configures the graphics hardware. See
 * src/pci.c for the accessor it uses and why the values are byte-reversed.
 */
#ifndef MODEL3RECOMP_PCI_H
#define MODEL3RECOMP_PCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M3_PCI_CONFIG_ADDR 0xF0800CF8u
#define M3_PCI_CONFIG_DATA 0xF0C00CFCu

void     pci_init(void);
void     pci_config_address_write(uint32_t v);
uint32_t pci_config_data_read(void);
void     pci_config_data_write(uint32_t v);

#ifdef __cplusplus
}
#endif
#endif

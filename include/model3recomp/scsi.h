/* model3recomp -- NCR 53C810, used as a memory-to-memory DMA engine.
 *
 * Step 1.x games move bulk data with its SCRIPTS processor. No SCSI bus and
 * no target device are involved. The chip is little-endian; see src/scsi.c.
 */
#ifndef MODEL3RECOMP_SCSI_H
#define MODEL3RECOMP_SCSI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     scsi_init(void);
uint32_t scsi_read(uint32_t addr, unsigned size);
void     scsi_write(uint32_t addr, uint32_t v, unsigned size);

#ifdef __cplusplus
}
#endif
#endif

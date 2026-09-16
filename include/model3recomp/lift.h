/* model3recomp -- the surface lifted code is compiled against.
 *
 * Generated C includes this and nothing else from the runtime. Keeping the
 * spelling short matters: a lifted Model 3 game is on the order of a hundred
 * thousand statements, and every one of them goes through these macros.
 */
#ifndef MODEL3RECOMP_LIFT_H
#define MODEL3RECOMP_LIFT_H

#include <math.h>

#include "model3recomp/ppc.h"
#include "model3recomp/bus.h"
#include "model3recomp/func_table.h"

/* Memory */
#define MEM_R8(a)     bus_read8 ((uint32_t)(a))
#define MEM_R16(a)    bus_read16((uint32_t)(a))
#define MEM_R32(a)    bus_read32((uint32_t)(a))
#define MEM_R64(a)    bus_read64((uint32_t)(a))
#define MEM_W8(a,v)   bus_write8 ((uint32_t)(a),(uint8_t)(v))
#define MEM_W16(a,v)  bus_write16((uint32_t)(a),(uint16_t)(v))
#define MEM_W32(a,v)  bus_write32((uint32_t)(a),(uint32_t)(v))
#define MEM_W64(a,v)  bus_write64((uint32_t)(a),(uint64_t)(v))

/* Condition / flag updates */
#define CR0(v)        m3_cr0(&m3_ctx,(int32_t)(v))
#define CRF(f,v)      m3_cr_set(&m3_ctx,(f),(v))
#define CMPS(f,a,b)   m3_cmp_s(&m3_ctx,(f),(int32_t)(a),(int32_t)(b))
#define CMPU(f,a,b)   m3_cmp_u(&m3_ctx,(f),(uint32_t)(a),(uint32_t)(b))
#define FCMP(f,a,b)   m3_fcmp(&m3_ctx,(f),(a),(b))
#define CRB(b)        m3_crbit(&m3_ctx,(b))
#define CRB_SET(b,v)  m3_crbit_set(&m3_ctx,(b),(v))
#define CA            m3_xer_ca(&m3_ctx)
#define SET_CA(c)     m3_set_ca(&m3_ctx,(c))
#define SET_OV(o)     m3_set_ov(&m3_ctx,(o))

#define ROTL32(v,n)   m3_rotl32((uint32_t)(v),(n))
#define MASK(mb,me)   m3_mask((mb),(me))
#define CNTLZW(v)     m3_cntlzw((uint32_t)(v))

/* Single-precision ops round through float; they do not truncate. */
#define F32(d)        ((double)(float)(d))

static inline double m3_fabs(double d)  { return fabs(d); }
static inline double m3_sqrt(double d)  { return sqrt(d); }
/* fctiw honours FPSCR[RN]; every Model 3 title leaves it at round-to-nearest,
 * which is what nearbyint gives under the default host mode. */
static inline double m3_round(double d) { return nearbyint(d); }

uint32_t m3_timebase(unsigned spr);

/* Indirect control transfer. Every target the lifter could not resolve
 * statically lands here and is looked up at runtime. */
#define CALL(a)       func_table_call((uint32_t)(a))

/* An instruction the lifter does not translate. Greppable and countable --
 * never a silent wrong answer. See tools/ppc_lifter.py --stats. */
void m3_unimplemented(uint32_t addr, uint32_t raw, const char *mn);
#define UNIMPL(a,r,m) m3_unimplemented((a),(r),(m))

#endif /* MODEL3RECOMP_LIFT_H */

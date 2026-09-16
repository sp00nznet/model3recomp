/* model3recomp -- Model 3 tilemap generator.
 *
 * Four 8x8 tile layers over the 496x384 display: the service menu, the HUD,
 * and the flat parts of attract mode. Called once per field, before the
 * framebuffer is presented.
 */
#ifndef MODEL3RECOMP_TILEGEN_H
#define MODEL3RECOMP_TILEGEN_H

#ifdef __cplusplus
extern "C" {
#endif

void tilegen_render(void);

/* For bring-up: isolate a layer when working out which one holds what. */
void tilegen_enable_layer(int layer, int on);

#ifdef __cplusplus
}
#endif
#endif

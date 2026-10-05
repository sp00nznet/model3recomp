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

/* One priority pass: the layers the guest has put behind the 3D, or the
 * ones it has put in front. */
void tilegen_render_pass(int above);

/* For bring-up: isolate a layer when working out which one holds what. */
void tilegen_enable_layer(int layer, int on);

#ifdef __cplusplus
}
#endif
#endif

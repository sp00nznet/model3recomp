/* model3recomp -- two-player netplay, lockstep over TCP.
 *
 * The recompiled board is deterministic: fields are paced by guest work, not
 * by the clock, so two machines running the same build from power-on and
 * fed the same inputs on the same fields stay identical. Netplay is
 * therefore nothing more than trading inputs. Each side sends its own a few
 * fields ahead (the input delay) and, before every field, waits for the
 * other's. The host plays player 1, the joiner player 2.
 *
 * A session starts at power-on: the guest cannot be reset in place, so the
 * menu's Host and Join relaunch the program. From a script, set
 *
 *     M3_NETPLAY=host:7777            listen on port 7777
 *     M3_NETPLAY=join:100.64.0.5:7777 connect to a host
 *     M3_NET_DELAY=3                  input delay in fields (host decides)
 *
 * which is all a LAN, a Tailscale address or a forwarded port needs.
 */
#ifndef MODEL3RECOMP_NETPLAY_H
#define MODEL3RECOMP_NETPLAY_H

#include <stdint.h>
#include "model3recomp/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a session if M3_NETPLAY asks for one; blocks until the peer is
 * there or the user closes the window. 0 if it asked and failed. */
int  netplay_init_from_env(void);
void netplay_shutdown(void);

/* 1 while a session is running; the role is 1 for host, 2 for joiner. */
int  netplay_active(void);
int  netplay_role(void);
const char *netplay_status(void);   /* one line for the title bar */

/* Trade inputs for field f. Returns 0 if there is no session, in which
 * case the caller uses its local record as is. */
int  netplay_exchange(const m3_input_t *local, m3_input_t *out, uint64_t f);

#ifdef __cplusplus
}
#endif
#endif

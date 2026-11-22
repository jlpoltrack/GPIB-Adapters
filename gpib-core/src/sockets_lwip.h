#ifndef SOCKETS_LWIP_H_
#define SOCKETS_LWIP_H_

/* Raw SCPI on TCP 5025 and Prologix on TCP 1234, one client each, and the web page on TCP 80 (http.c), a few
 * clients, on lwIP raw TCP (NO_SYS). A new connection to a full port replaces its oldest, so a client that vanished
 * without closing can't lock it. */

#define SOCKETS_RAW_PORT      5025
#define SOCKETS_PROLOGIX_PORT 1234
#define SOCKETS_HTTP_CONNS    3 /* a browser opens two or three at once */

void sockets_lwip_web_init(void); /* web page: with the network, before the instrument search (updates need no instrument) */
void sockets_lwip_web_task(void); /* from the main loop and while searching */
void sockets_lwip_init(void);     /* raw SCPI and Prologix: once the instrument is found */
void sockets_lwip_task(void);     /* from the main loop */

#endif /* SOCKETS_LWIP_H_ */

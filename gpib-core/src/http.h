#ifndef HTTP_H_
#define HTTP_H_

/* A small HTTP server on a port_t, one connection each: the adapter's page ("/"), the LXI identification document
 * ("/lxi/identification", LXI Identification schema 1.0), the network settings form (POST /net, via netcfg) and,
 * where the board supports it, a firmware upload (POST /update, X-Update-Password: netcfg's password, if set).
 * A POST from a browser must come from the adapter's own page: an Origin or Host that isn't the adapter gets 403
 * (another site's script, DNS rebinding); clients without them (curl) are unaffected.
 * Every response closes its connection. Nothing is buffered on the way out: the response is rendered again for each
 * piece TCP takes, and only that window goes to the port. */

#include <stdbool.h>
#include <stdint.h>
#include "port.h"

#define HTTP_PORT     80
#define HTTP_LINE_MAX 128 /* request line; header lines are only scanned for Content-Length, the password, Host, Origin */
#define HTTP_HOST_MAX 48  /* Host / Origin kept for the same-site check on POST */
#define HTTP_BODY_MAX 256 /* the settings form; an upload is streamed to the board instead */

typedef struct {
	const port_t *port;
	uint8_t       state, method, page, msg;
	bool          has_len, pw_ok;
	uint16_t      line_len, body_len;
	uint32_t      content_len, left; /* left: upload bytes still to come */
	char          line[HTTP_LINE_MAX];
	char          host[HTTP_HOST_MAX], origin[HTTP_HOST_MAX + 8];
	char          body[HTTP_BODY_MAX + 1];
	uint32_t      sent, total, body_size;
} http_t;

/* Firmware upload hooks: the board streams POST /update's body to flash. Weak defaults in http.c: no support. */
enum { HTTP_UPD_OK, HTTP_UPD_TOO_LARGE, HTTP_UPD_BAD_IMAGE, HTTP_UPD_FLASH };
bool board_update_supported(void);
int  board_update_begin(uint32_t len);                   /* HTTP_UPD_*; drops any upload in progress */
int  board_update_write(const uint8_t *buf, uint32_t n); /* the next n bytes of the body */
int  board_update_end(void);                             /* checks the whole image: HTTP_UPD_OK if it is staged */
void board_update_done(void);                            /* the reply is queued: install it (reset shortly) */

void        http_init(http_t *h, const port_t *port);
void        http_reset(http_t *h); /* a new connection */
void        http_run(http_t *h);   /* from the main loop */
const char *http_firmware(void);   /* "1.2.3" from APP_USB_BCD, or APP_FW_VERSION */

#endif /* HTTP_H_ */

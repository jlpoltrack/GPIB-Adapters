#include <string.h>
#include "mdns_lwip.h"
#include "lwip/opt.h"

#if LWIP_MDNS_RESPONDER
#include "lwip/apps/mdns.h"
#include "instr.h"
#include "http.h"
#include "board.h"
#include "app_config.h"

static char s_host[MDNS_LABEL_MAXLEN + 1];
static bool s_services;

static void txt(struct mdns_service *svc, const char *key, const char *val)
{
	char item[64];
	size_t k = strlen(key), v = strlen(val);
	if (k + v > sizeof(item))
		v = sizeof(item) - k;
	memcpy(item, key, k);
	memcpy(&item[k], val, v);
	mdns_resp_add_service_txtitem(svc, item, (u8_t)(k + v));
}

static void http_txt(struct mdns_service *svc, void *arg)
{
	(void)arg;
	txt(svc, "path=", "/");
}

/* the keys the LXI Device Specification asks of _lxi._tcp */
static void lxi_txt(struct mdns_service *svc, void *arg)
{
	char serial[BOARD_SERIAL_LEN + 1];
	(void)arg;
	board_get_uid_hex(serial);
	txt(svc, "txtvers=", "1");
	txt(svc, "Manufacturer=", APP_USB_MANUFACTURER);
	txt(svc, "Model=", APP_USB_PRODUCT);
	txt(svc, "SerialNumber=", serial);
	txt(svc, "FirmwareVersion=", http_firmware());
}

void mdns_lwip_init(struct netif *nif, const char *hostname)
{
	strncpy(s_host, hostname, sizeof(s_host) - 1);
	mdns_resp_init();
	mdns_resp_add_netif(nif, s_host);
}

/* Instance "<instrument ID> (<hostname>)", cut to a DNS label: two of the same instrument stay apart */
void mdns_lwip_services(struct netif *nif)
{
	char name[MDNS_LABEL_MAXLEN + 1];
	size_t h = strlen(s_host) + 3, n = strlen(instr_id());

	if (s_services)
		return;
	s_services = true;
	if (n > sizeof(name) - 1 - h)
		n = sizeof(name) - 1 - h;
	memcpy(name, instr_id(), n);
	name[n] = '\0';
	if (n) {
		strcat(name, " (");
		strcat(name, s_host);
		strcat(name, ")");
	} else {
		strcpy(name, s_host);
	}
	mdns_resp_add_service(nif, name, "_vxi-11", DNSSD_PROTO_TCP, 111, NULL, NULL);
	mdns_resp_add_service(nif, name, "_scpi-raw", DNSSD_PROTO_TCP, 5025, NULL, NULL);
	mdns_resp_add_service(nif, name, "_lxi", DNSSD_PROTO_TCP, HTTP_PORT, lxi_txt, NULL);
	mdns_resp_add_service(nif, name, "_http", DNSSD_PROTO_TCP, HTTP_PORT, http_txt, NULL);
	mdns_resp_announce(nif);
}

#else

void mdns_lwip_init(struct netif *nif, const char *hostname)
{
	(void)nif;
	(void)hostname;
}

void mdns_lwip_services(struct netif *nif)
{
	(void)nif;
}

#endif

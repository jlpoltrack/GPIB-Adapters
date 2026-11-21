#include "image.h"

/* Nibble table: 64 bytes of flash instead of 1 KB; roughly 60 ms for a 100 KB app at the 20 MHz reset clock */
static const uint32_t s_crc_nibble[16] = {
	0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
	0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t image_crc32(uint32_t crc, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		crc = (crc >> 4) ^ s_crc_nibble[crc & 0xFu];
		crc = (crc >> 4) ^ s_crc_nibble[crc & 0xFu];
	}
	return ~crc;
}

bool image_valid(const app_header_t *hdr, const uint8_t *image)
{
	if ((hdr->magic != APP_MAGIC) || (hdr->hdr_version != APP_HDR_VER))
		return false;
	if ((hdr->length == 0) || (hdr->length > APP_MAX_LEN))
		return false;
	return image_crc32(0, image, hdr->length) == hdr->crc;
}

uint32_t image_block_addr(uint16_t block, uint16_t len)
{
	uint32_t addr = APP_HDR_ADDR + (uint32_t)block * DFU_BLOCK;

	if ((len == 0) || (len > DFU_BLOCK) || (addr >= APP_END) || (len > APP_END - addr))
		return 0;
	return addr;
}

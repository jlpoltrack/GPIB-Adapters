#ifndef UPDATE_H_
#define UPDATE_H_

/* Firmware update over the network (update.c): the hooks behind gpib-core's POST /update */

void update_task(void); /* main loop: resets into the bootloader's install once a staged image's reply is out */

#endif /* UPDATE_H_ */

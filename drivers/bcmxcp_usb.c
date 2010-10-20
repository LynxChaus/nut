#include "main.h"
#include "bcmxcp.h"
#include "bcmxcp_io.h"
#include "common.h"
#include "usb-common.h"
#include "timehead.h"
#include "nut_stdint.h" /* for uint16_t */
#include <ctype.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#include <usb.h>

#define SUBDRIVER_NAME	"USB communication subdriver"
#define SUBDRIVER_VERSION	"0.18"

/* communication driver description structure */
upsdrv_info_t comm_upsdrv_info = {
	SUBDRIVER_NAME,
	SUBDRIVER_VERSION,
	NULL,
	0,
	{ NULL }
};

/* Powerware */
#define POWERWARE	0x0592

/* Phoenixtec Power Co., Ltd */
#define PHOENIXTEC	0x06da

/* Hewlett Packard */
#define HP_VENDORID	0x03f0

/* USB functions */
usb_dev_handle *nutusb_open(const char *port);
int nutusb_close(usb_dev_handle *dev_h, const char *port);
/* unified failure reporting: call these often */
void nutusb_comm_fail(const char *fmt, ...)
	__attribute__ ((__format__ (__printf__, 1, 2)));
void nutusb_comm_good(void);
/* function pointer, set depending on which device is used */
int (*usb_set_descriptor)(usb_dev_handle *udev, unsigned char type,
	unsigned char index, void *buf, int size);

/* usb_set_descriptor() for Powerware devices */
static int usb_set_powerware(usb_dev_handle *udev, unsigned char type, unsigned char index, void *buf, int size)
{
	return usb_control_msg(udev, USB_ENDPOINT_OUT, USB_REQ_SET_DESCRIPTOR, (type << 8) + index, 0, buf, size, 1000);
}

static void *powerware_ups(void) {
	usb_set_descriptor = &usb_set_powerware;
	return NULL;
}

/* usb_set_descriptor() for Phoenixtec devices */
static int usb_set_phoenixtec(usb_dev_handle *udev, unsigned char type, unsigned char index, void *buf, int size)
{
	return usb_control_msg(udev, 0x42, 0x0d, (0x00 << 8) + 0x0, 0, buf, size, 1000);
}

static void *phoenixtec_ups(void) {
	usb_set_descriptor = &usb_set_phoenixtec;
	return NULL;
}

/* USB IDs device table */
static usb_device_id_t pw_usb_device_table[] = {
	/* various models */
	{ USB_DEVICE(POWERWARE, 0x0002), &powerware_ups },

	/* various models */
	{ USB_DEVICE(PHOENIXTEC, 0x0002), &phoenixtec_ups },

	/* T500 */
	{ USB_DEVICE(HP_VENDORID, 0x1f01), &phoenixtec_ups },
	/* T750 */
	{ USB_DEVICE(HP_VENDORID, 0x1f02), &phoenixtec_ups },
	
	/* Terminating entry */
	{ -1, -1, NULL }
};

/* limit the amount of spew that goes in the syslog when we lose the UPS */
#define USB_ERR_LIMIT 10	/* start limiting after 10 in a row  */
#define USB_ERR_RATE 10		/* then only print every 10th error */
#define XCP_USB_TIMEOUT 5000

/* global variables */
usb_dev_handle *upsdev = NULL;
extern	int		exit_flag;
static	unsigned int	comm_failures = 0;

/* Functions implementations */
void send_read_command(unsigned char command)
{
        unsigned char buf[4];

	buf[0] = PW_COMMAND_START_BYTE;
	buf[1] = 0x01;                    /* data length */
	buf[2] = command;                 /* command to send */
	buf[3] = calc_checksum(buf);      /* checksum */
	usb_set_descriptor(upsdev, USB_DT_STRING, 4, buf, 4); /* FIXME: Ignore error */
}

void send_write_command(unsigned char *command, int command_length)
{
	unsigned char sbuf[128];

	/* Prepare the send buffer */
	sbuf[0] = PW_COMMAND_START_BYTE;
	sbuf[1] = (unsigned char)(command_length);
	memcpy(sbuf+2, command, command_length);
	command_length += 2;

	/* Add checksum */
	sbuf[command_length] = calc_checksum(sbuf);
	command_length += 1;
	usb_set_descriptor(upsdev, USB_DT_STRING, 4, sbuf, command_length);  /* FIXME: Ignore error */
}

/* get the answer of a command from the ups. And check that the answer is for this command */
int get_answer(unsigned char *data, unsigned char command)
{
	unsigned char buf[1024], *my_buf = buf;
	int length, end_length, res, endblock, bytes_read, ellapsed_time;
	unsigned char block_number, sequence, seq_num;
	struct timeval start_time, now;

	length = 1;			/* non zero to enter the read loop */
	end_length = 0;		/* total length of sequence(s), not counting header(s) */
	endblock = 0;		/* signal the last sequence in the block */
	bytes_read = 0;		/* total length of data read, including XCP header */
	res = 0;
	ellapsed_time = 0;
	seq_num = 1;		/* current theoric sequence */

	upsdebugx(1, "entering get_answer(%x)", command);

	/* Store current time */
	gettimeofday(&start_time, NULL);

	while ( (!endblock) && ((XCP_USB_TIMEOUT - ellapsed_time)  > 0) ) {

		/* Get (more) data if needed */
		if ((length - bytes_read) > 0) {
			res = usb_interrupt_read(upsdev, 0x81,
				(char *)&buf[bytes_read],
				(PW_ANSWER_MAX_SIZE - bytes_read),
				(XCP_USB_TIMEOUT - ellapsed_time));

			/* Update time */
			gettimeofday(&now, NULL);
			ellapsed_time = (now.tv_sec - start_time.tv_sec)*1000 +
					(now.tv_usec - start_time.tv_usec)/1000;

			/* Check libusb return value */
			if (res < 0)
			{
				/* Clear any possible endpoint stalls */
				usb_clear_halt(upsdev, 0x81);
				//continue; // FIXME: seems a break would be better!
				break;
			}

			/* this seems to occur on XSlot USB card */
			if (res == 0)
			{
				/* FIXME: */
				continue;
			}
			
			/* Else, we got some input bytes */ 
			bytes_read += res;
			upsdebug_hex(1, "get_answer", buf, bytes_read);
		}

		/* Now validate XCP frame */
		/* Check header */
		if ( my_buf[0] != 0xAB ) {
			upsdebugx(2, "get_answer: wrong header");
			return -1;
		}

		/* These validations seem not needed! */
		/* Read block number byte */
		block_number = my_buf[1];
		upsdebugx(1, "get_answer: block_number = %x", block_number);
#if 0
		if (command <= 0x43) {
			if ((command - 0x30) != block_number){
				nutusb_comm_fail("Receive error (Request command): BLOCK: %x (instead of %x), COMMAND: %x!\n",
					block_number, (command - 0x30), command);
				return -1;
			}
		}

		if (command >= 0x89) {
			if ((command == 0xA0) && (block_number != 0x01)){
				nutusb_comm_fail("Receive error (Request command): BLOCK: %x (instead of 0x01), COMMAND: %x!\n", block_number, command);
				return -1;
			}
			else if ((command != 0xA0) && (block_number != 0x09)){
				nutusb_comm_fail("Receive error (Request command): BLOCK: %x (instead of 0x09), COMMAND: %x!\n", block_number, command);
				return -1;
			}
		}
#endif /* if 0 */

		/* Check data length byte (remove the header length) */
		length = my_buf[2];
		upsdebugx(3, "get_answer: data length = %d", length);
		if ((bytes_read - 5) < length) {
			upsdebugx(2, "get_answer: need to read %d more data", length - (bytes_read - 5));
			continue;
		}
		/* Check if Length conforms to XCP (121 for normal, 140 for Test mode) */
		/* Use the more generous length for testing */
		if (length > 140 ) {
			upsdebugx(2, "get_answer: bad length");
			return -1;
		}

		/* Test the Sequence # */
		sequence = my_buf[3];
		if ((sequence & PW_SEQ_MASK) != seq_num) {
			nutusb_comm_fail("get_answer: not the right sequence received %x!!!\n", (sequence & PW_SEQ_MASK));
			return -1;
		}
		else {
			upsdebugx(2, "get_answer: sequence number (%x) is ok", (sequence & PW_SEQ_MASK));
		}

		/* Validate checksum */
		if (!checksum_test(my_buf)) {
			nutusb_comm_fail("get_answer: checksum error! ");
			return -1;
		}
		else {
			upsdebugx(2, "get_answer: checksum is ok");
		}

		/* Check if it's the last sequence */
		if (sequence & PW_LAST_SEQ) {
			/* we're done receiving data */
			upsdebugx(2, "get_answer: all data received");
			endblock = 1;
		}
		else {
			seq_num++;
		}

		/* copy the current valid XCP frame back */
		memcpy(data+end_length, my_buf+4, length);
		/* increment pointers to process the next sequence */
		end_length += length;
		my_buf += length + 5;
	}
	upsdebugx(4, "get_answer: exiting (len=%d)", end_length);
	return end_length;
}

/* Sends a single command (length=1). and get the answer */
int command_read_sequence(unsigned char command, unsigned char *data)
{
	int bytes_read = 0;
	int retry = 0;
	
	while ((bytes_read < 1) && (retry < 5)) {
		send_read_command(command);
		bytes_read = get_answer(data, command);
		retry++;
	}

	if (bytes_read < 1) {
		nutusb_comm_fail("Error executing command");
		dstate_datastale();
		return -1;
	}

	return bytes_read;
}

/* Sends a setup command (length > 1) */
int command_write_sequence(unsigned char *command, int command_length, unsigned	char *answer)
{
	int bytes_read = 0;
	int retry = 0;
	
	while ((bytes_read < 1) && (retry < 5)) {
		send_write_command(command, command_length);
		bytes_read = get_answer(answer, command[0]);		
		retry ++;
	}

	if (bytes_read < 1) {
		nutusb_comm_fail("Error executing command");
		dstate_datastale();
		return -1;
	}

	return bytes_read;
}


void upsdrv_comm_good(void)
{
	nutusb_comm_good();
}

void upsdrv_initups(void)
{
	upsdev = nutusb_open("USB");
}

void upsdrv_cleanup(void)
{
	upslogx(LOG_ERR, "CLOSING\n");
	nutusb_close(upsdev, "USB");
}

void upsdrv_reconnect(void)
{
	
	upslogx(LOG_WARNING, "RECONNECT USB DEVICE\n");
	nutusb_close(upsdev, "USB");
	upsdev = NULL;
	sleep(3);
	upsdrv_initups();	
}

/* USB functions */
static void nutusb_open_error(const char *port)
{
	printf("Unable to find POWERWARE UPS device on USB bus (%s)\n\n", port);

	printf("Things to try:\n\n");
	printf(" - Connect UPS device to USB bus\n\n");
	printf(" - Run this driver as another user (upsdrvctl -u or 'user=...' in ups.conf).\n");
	printf("   See upsdrvctl(8) and ups.conf(5).\n\n");

	fatalx(EXIT_FAILURE, "Fatal error: unusable configuration");
}

/* FIXME: this part of the opening can go into common... */
static usb_dev_handle *open_powerware_usb(void)
{
	struct usb_bus *busses = usb_get_busses();  
	struct usb_bus *bus;
    
	for (bus = busses; bus; bus = bus->next)
	{
		struct usb_device *dev;
    
		for (dev = bus->devices; dev; dev = dev->next)
		{
			if (dev->descriptor.bDeviceClass != USB_CLASS_PER_INTERFACE) {
				continue;
			}

			if (is_usb_device_supported(pw_usb_device_table,
				dev->descriptor.idVendor, dev->descriptor.idProduct) == SUPPORTED) {
				return usb_open(dev);
			}
		}
	}
	return 0;
}

usb_dev_handle *nutusb_open(const char *port)
{
	static int     libusb_init = 0;
	int            dev_claimed = 0;
	usb_dev_handle *dev_h = NULL;
	int            retry;
	
	if (!libusb_init)
	{
		/* Initialize Libusb */
		usb_init();
		libusb_init = 1;
		usb_find_busses();
		usb_find_devices();
	}

	for (retry = 0; dev_h == NULL && retry < 32; retry++)
	{
		struct timespec t = {5, 0};

		dev_h = open_powerware_usb();
		if (!dev_h) {
			upslogx(LOG_WARNING, "Can't open POWERWARE USB device, retrying ...");
			if (nanosleep(&t, NULL) < 0 && errno == EINTR)
				break;
		}
	}
	
	if (!dev_h)
	{
		upslogx(LOG_ERR, "Can't open POWERWARE USB device");
		goto errout;
	}
	else
		upsdebugx(1, "device %s opened successfully", usb_device(dev_h)->filename);

	if (usb_claim_interface(dev_h, 0) < 0)
	{
		upslogx(LOG_ERR, "Can't claim POWERWARE USB interface: %s", usb_strerror());
		goto errout;
	}
	else
		dev_claimed = 1;
/* FIXME: this part of the opening can go into common... up to here at least */

	if (usb_clear_halt(dev_h, 0x81) < 0)
	{
		upslogx(LOG_ERR, "Can't reset POWERWARE USB endpoint: %s", usb_strerror());
		goto errout;
	}
	return dev_h;

errout:
	if (dev_h && dev_claimed)
		usb_release_interface(dev_h, 0);
	if (dev_h)
		usb_close(dev_h);

	nutusb_open_error(port);
	return 0;
}

/* FIXME: this part can go into common... */
int nutusb_close(usb_dev_handle *dev_h, const char *port)
{
	if (dev_h)
	{
		usb_release_interface(dev_h, 0);
		return usb_close(dev_h);
	}
	
	return 0;
}

void nutusb_comm_fail(const char *fmt, ...)
{
	int	ret;
	char	why[SMALLBUF];
	va_list	ap;

	/* this means we're probably here because select was interrupted */
	if (exit_flag != 0)
		return;		/* ignored, since we're about to exit anyway */

	comm_failures++;

	if ((comm_failures == USB_ERR_LIMIT) ||
		((comm_failures % USB_ERR_RATE) == 0))
	{
		upslogx(LOG_WARNING, "Warning: excessive comm failures, "
			"limiting error reporting");
	}

	/* once it's past the limit, only log once every USB_ERR_LIMIT calls */
	if ((comm_failures > USB_ERR_LIMIT) &&
		((comm_failures % USB_ERR_LIMIT) != 0))
		return;

	/* generic message if the caller hasn't elaborated */
	if (!fmt)
	{
		upslogx(LOG_WARNING, "Communications with UPS lost"
			" - check cabling");
		return;
	}

	va_start(ap, fmt);
	ret = vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);

	if ((ret < 1) || (ret >= (int) sizeof(why)))
		upslogx(LOG_WARNING, "usb_comm_fail: vsnprintf needed "
			"more than %d bytes", (int)sizeof(why));

	upslogx(LOG_WARNING, "Communications with UPS lost: %s", why);
}

void nutusb_comm_good(void)
{
	if (comm_failures == 0)
		return;

	upslogx(LOG_NOTICE, "Communications with UPS re-established");
	comm_failures = 0;
}

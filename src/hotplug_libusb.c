/*
 * MUSCLE SmartCard Development ( https://pcsclite.apdu.fr/ )
 *
 * Copyright (C) 2001-2004
 *  David Corcoran <corcoran@musclecard.com>
 * Copyright (C) 2003-2011
 *  Ludovic Rousseau <ludovic.rousseau@free.fr>
 * Copyright (C) 2003
 *  Toni Andjelkovic <toni@soth.at>
 * Copyright (C) 2003-2004
 *  Damien Sauveron <damien.sauveron@labri.fr>
 *
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * @brief This provides a search API for hot pluggble devices.
 */

#include "config.h"
#ifdef HAVE_LIBUSB

#include <string.h>
#include <sys/types.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <libusb.h>
#include <pthread.h>
#include <signal.h>

#include "misc.h"
#include "wintypes.h"
#include "pcscd.h"
#include "debuglog.h"
#include "parser.h"
#include "readerfactory.h"
#include "winscard_msg.h"
#include "sys_generic.h"
#include "hotplug.h"
#include "utils.h"

#undef DEBUG_HOTPLUG

/* format is "%d:%d:%d", bus_number, device_address, interface */
#define BUS_DEVICE_STRSIZE	10+1+10+1+10+1

#define READER_ABSENT		0
#define READER_PRESENT		1
#define READER_FAILED		2

#define FALSE			0
#define TRUE			1

extern char Add_Interface_In_Name;
extern char Add_Serial_In_Name;

/* we use the default libusb context */
#define ctx NULL

pthread_mutex_t usbNotifierMutex;

static pthread_t usbNotifyThread;
static int driverSize = -1;
static char AraKiriHotPlug = FALSE;
static int rescan_pipe[] = { -1, -1 };
extern int HPForceReaderPolling;

/* values of ifdCapabilities bits */
#define IFD_GENERATE_HOTPLUG 1

/**
 * keep track of drivers in a dynamically allocated array
 */
static struct _driverTracker
{
	unsigned int manuID;
	unsigned int productID;

	char *bundleName;
	char *libraryPath;
	char *readerName;
	int ifdCapabilities;
} *driverTracker = NULL;
#define DRIVER_TRACKER_SIZE_STEP 8

/**
 * keep track of PCSCLITE_MAX_READERS_CONTEXTS simultaneous readers
 */
static struct _readerTracker
{
	char status;
	char bus_device[BUS_DEVICE_STRSIZE];	/**< device name */
	char *fullName;	/**< full reader name (including serial number) */
} readerTracker[PCSCLITE_MAX_READERS_CONTEXTS];

static LONG HPAddHotPluggable(struct libusb_device *dev,
	struct libusb_device_descriptor desc,
	const char bus_device[],
	const struct libusb_interface *idesc,
	struct _driverTracker *driver);
static LONG HPRemoveHotPluggable(int reader_index);

static LONG HPReadBundleValues(void)
{
	LONG rv;
	DIR *hpDir;
	struct dirent *currFP = NULL;
	char fullPath[FILENAME_MAX];
	char fullLibPath[FILENAME_MAX];
	int listCount = 0;

	hpDir = opendir(PCSCLITE_HP_DROPDIR);

	if (hpDir == NULL)
	{
		Log1(PCSC_LOG_ERROR, "Cannot open PC/SC drivers directory: " PCSCLITE_HP_DROPDIR);
		Log1(PCSC_LOG_ERROR, "Disabling USB support for pcscd.");
		return -1;
	}

	/* allocate a first array */
	driverTracker = calloc(DRIVER_TRACKER_SIZE_STEP, sizeof(*driverTracker));
	if (NULL == driverTracker)
	{
		Log1(PCSC_LOG_CRITICAL, "Not enough memory");
		return -1;
	}
	driverSize = DRIVER_TRACKER_SIZE_STEP;

#define GET_KEY(key, values) \
	rv = LTPBundleFindValueWithKey(&plist, key, values); \
	if (rv) \
	{ \
		Log2(PCSC_LOG_ERROR, "Value/Key not defined for " key " in %s", \
			fullPath); \
		continue; \
	}

	while ((currFP = readdir(hpDir)) != 0)
	{
		if (strstr(currFP->d_name, ".bundle") != 0)
		{
			unsigned int alias;
			list_t plist, *values;
			list_t *manuIDs, *productIDs, *readerNames;
			char *libraryPath;
			int ifdCapabilities;

			/*
			 * The bundle exists - let's form a full path name and get the
			 * vendor and product ID's for this particular bundle
			 */
			snprintf(fullPath, sizeof(fullPath), "%s/%s/Contents/Info.plist",
				PCSCLITE_HP_DROPDIR, currFP->d_name);
			fullPath[sizeof(fullPath) - 1] = '\0';

			rv = bundleParse(fullPath, &plist);
			if (rv)
				continue;

			/* get CFBundleExecutable */
			GET_KEY(PCSCLITE_HP_LIBRKEY_NAME, &values)
			libraryPath = list_get_at(values, 0);
			(void)snprintf(fullLibPath, sizeof(fullLibPath),
				"%s/%s/Contents/%s/%s",
				PCSCLITE_HP_DROPDIR, currFP->d_name, PCSC_ARCH,
				libraryPath);
			fullLibPath[sizeof(fullLibPath) - 1] = '\0';

			/* Get ifdCapabilities */
			GET_KEY(PCSCLITE_HP_CPCTKEY_NAME, &values)
			ifdCapabilities = strtol(list_get_at(values, 0), NULL, 16);

			GET_KEY(PCSCLITE_HP_MANUKEY_NAME, &manuIDs)
			GET_KEY(PCSCLITE_HP_PRODKEY_NAME, &productIDs)
			GET_KEY(PCSCLITE_HP_NAMEKEY_NAME, &readerNames)

			/* while we find a nth ifdVendorID in Info.plist */
			for (alias=0; alias<list_size(manuIDs); alias++)
			{
				char *value;

				/* variables entries */
				value = list_get_at(manuIDs, alias);
				driverTracker[listCount].manuID = strtol(value, NULL, 16);

				value = list_get_at(productIDs, alias);
				driverTracker[listCount].productID = strtol(value, NULL, 16);

				driverTracker[listCount].readerName = strdup(list_get_at(readerNames, alias));

				/* constant entries for a same driver */
				driverTracker[listCount].bundleName = strdup(currFP->d_name);
				driverTracker[listCount].libraryPath = strdup(fullLibPath);
				driverTracker[listCount].ifdCapabilities = ifdCapabilities;

#ifdef DEBUG_HOTPLUG
				Log2(PCSC_LOG_INFO, "Found driver for: %s",
					driverTracker[listCount].readerName);
#endif
				listCount++;
				if (listCount >= driverSize)
				{
					int i;

					/* increase the array size */
					driverSize += DRIVER_TRACKER_SIZE_STEP;
#ifdef DEBUG_HOTPLUG
					Log2(PCSC_LOG_INFO,
						"Increase driverTracker to %d entries", driverSize);
#endif
					void* tmp = realloc(driverTracker,
						driverSize * sizeof(*driverTracker));
					if (NULL == tmp)
					{
						free(driverTracker);
						Log1(PCSC_LOG_CRITICAL, "Not enough memory");
						driverSize = -1;
						closedir(hpDir);
						return -1;
					}
					driverTracker = tmp;

					/* clean the newly allocated entries */
					for (i=driverSize-DRIVER_TRACKER_SIZE_STEP; i<driverSize; i++)
					{
						driverTracker[i].manuID = 0;
						driverTracker[i].productID = 0;
						driverTracker[i].bundleName = NULL;
						driverTracker[i].libraryPath = NULL;
						driverTracker[i].readerName = NULL;
						driverTracker[i].ifdCapabilities = 0;
					}
				}
			}
			bundleRelease(&plist);
		}
	}

	driverSize = listCount;
	closedir(hpDir);

	if (driverSize == 0)
	{
		Log1(PCSC_LOG_INFO, "No bundle files in pcsc drivers directory: " PCSCLITE_HP_DROPDIR);
		Log1(PCSC_LOG_INFO, "Disabling USB support for pcscd");
	}
#ifdef DEBUG_HOTPLUG
	else
		Log2(PCSC_LOG_INFO, "Found drivers for %d readers", listCount);
#endif

	return driverSize;
}

static void HPRescanUsbBus(void)
{
	int i, j;
	char bus_device[BUS_DEVICE_STRSIZE];
	libusb_device **devs, *dev;
	ssize_t cnt;

	for (i=0; i < PCSCLITE_MAX_READERS_CONTEXTS; i++)
		/* clear rollcall */
		readerTracker[i].status = READER_ABSENT;

	cnt = libusb_get_device_list(ctx, &devs);
	if (cnt < 0)
	{
		Log2(PCSC_LOG_CRITICAL, "libusb_get_device_list() failed: %s",
			libusb_error_name(cnt));
		return;
	}

	/* For each USB device */
	cnt = 0;
	while ((dev = devs[cnt++]) != NULL)
	{
		struct libusb_device_descriptor desc;
		struct libusb_config_descriptor *config_desc;
		uint8_t bus_number = libusb_get_bus_number(dev);
		uint8_t device_address = libusb_get_device_address(dev);

		int r = libusb_get_device_descriptor(dev, &desc);
		if (r < 0)
		{
			Log4(PCSC_LOG_ERROR,
				"failed to get device descriptor for %d/%d: %s",
				bus_number, device_address, libusb_error_name(r));
			continue;
		}

		r = libusb_get_active_config_descriptor(dev, &config_desc);
		if (r < 0)
		{
			Log4(PCSC_LOG_ERROR, "failed to get device config for %d/%d: %s",
				bus_number, device_address, libusb_error_name(r));
			continue;
		}

		/* check if the device is supported by one driver */
		for (i=0; i<driverSize; i++)
		{
			if (driverTracker[i].libraryPath != NULL &&
				desc.idVendor == driverTracker[i].manuID &&
				desc.idProduct == driverTracker[i].productID)
			{
				int interface;

#ifdef DEBUG_HOTPLUG
				Log3(PCSC_LOG_DEBUG, "Found matching USB device: %d:%d",
					bus_number, device_address);
#endif

				for (interface = 0; interface < config_desc->bNumInterfaces;
					interface++)
				{
					int newreader;

					/* A known device has been found */
					snprintf(bus_device, BUS_DEVICE_STRSIZE, "%d:%d:%d",
						 bus_number, device_address, interface);
					bus_device[BUS_DEVICE_STRSIZE - 1] = '\0';
					newreader = TRUE;

					/* Check if the reader is a new one */
					for (j=0; j<PCSCLITE_MAX_READERS_CONTEXTS; j++)
					{
						if (strncmp(readerTracker[j].bus_device,
							bus_device, BUS_DEVICE_STRSIZE) == 0)
						{
							/* The reader is already known */
							readerTracker[j].status = READER_PRESENT;
							newreader = FALSE;
#ifdef DEBUG_HOTPLUG
							Log2(PCSC_LOG_DEBUG, "Refresh USB device: %s",
								bus_device);
#endif
							break;
						}
					}

					/* New reader found */
					if (newreader)
						HPAddHotPluggable(dev, desc, bus_device,
							&config_desc->interface[interface],
							&driverTracker[i]);
				}
			}
		}
		libusb_free_config_descriptor(config_desc);
	}

	/*
	 * check if all the previously found readers are still present
	 */
	for (i=0; i<PCSCLITE_MAX_READERS_CONTEXTS; i++)
	{
		if ((readerTracker[i].status == READER_ABSENT) &&
			(readerTracker[i].fullName != NULL))
			HPRemoveHotPluggable(i);
	}

	if (AraKiriHotPlug)
	{
		int retval;

		for (i=0; i<driverSize; i++)
		{
			/* free strings allocated by strdup() */
			free(driverTracker[i].bundleName);
			free(driverTracker[i].libraryPath);
			free(driverTracker[i].readerName);
		}
		free(driverTracker);

		Log1(PCSC_LOG_INFO, "Hotplug stopped");
		pthread_exit(&retval);
	}

	/* free the libusb allocated list & devices */
	libusb_free_device_list(devs, 1);
}

static void * HPEstablishUSBNotifications(int pipefd[2])
{
	int i, do_polling;
	int r;
	char c = 42;	/* magic value */

	r = libusb_init(ctx);
	if (r < 0)
	{
		Log2(PCSC_LOG_CRITICAL, "libusb_init failed: %s", libusb_error_name(r));
		/* emergency exit */
		kill(getpid(), SIGTERM);
		return NULL;
	}

	/* scan the USB bus for devices at startup */
	HPRescanUsbBus();

	/* signal that the initially connected readers are now visible */
	if (write(pipefd[1], &c, 1) == -1)
	{
		Log2(PCSC_LOG_ERROR, "write: %s", strerror(errno));
		return NULL;
	}

	/* if at least one driver do not have IFD_GENERATE_HOTPLUG */
	do_polling = FALSE;
	for (i=0; i<driverSize; i++)
		if (driverTracker[i].libraryPath)
			if ((driverTracker[i].ifdCapabilities & IFD_GENERATE_HOTPLUG) == 0)
			{
				Log2(PCSC_LOG_INFO,
					"Driver %s does not support IFD_GENERATE_HOTPLUG. Using active polling instead.",
					driverTracker[i].bundleName);
				if (HPForceReaderPolling < 1)
					HPForceReaderPolling = 1;
				break;
			}

	if (HPForceReaderPolling)
	{
		Log2(PCSC_LOG_INFO,
				"Polling forced every %d second(s)", HPForceReaderPolling);
		do_polling = TRUE;
	}

	if (do_polling)
	{
		while (!AraKiriHotPlug)
		{
			SYS_Sleep(HPForceReaderPolling);
			HPRescanUsbBus();
		}
	}
	else
	{
		char dummy;

		if (pipe(rescan_pipe) == -1)
		{
			Log2(PCSC_LOG_ERROR, "pipe: %s", strerror(errno));
			return NULL;
		}
		while (read(rescan_pipe[0], &dummy, sizeof(dummy)) > 0)
		{
			Log1(PCSC_LOG_INFO, "Reload serial configuration");
			HPRescanUsbBus();
#ifdef USE_SERIAL
			RFReCheckReaderConf();
#endif
			Log1(PCSC_LOG_INFO, "End reload serial configuration");
		}
		close(rescan_pipe[0]);
		rescan_pipe[0] = -1;
	}

	return NULL;
}

LONG HPSearchHotPluggables(void)
{
	int i;

	for (i=0; i<PCSCLITE_MAX_READERS_CONTEXTS; i++)
	{
		readerTracker[i].status = READER_ABSENT;
		readerTracker[i].bus_device[0] = '\0';
		readerTracker[i].fullName = NULL;
	}

	if (HPReadBundleValues() > 0)
	{
		int pipefd[2];
		char c;

		if (pipe(pipefd) == -1)
		{
			Log2(PCSC_LOG_ERROR, "pipe: %s", strerror(errno));
			return -1;
		}

		ThreadCreate(&usbNotifyThread, THREAD_ATTR_DETACHED,
			(PCSCLITE_THREAD_FUNCTION( )) HPEstablishUSBNotifications, pipefd);

		/* Wait for initial readers to setup */
		if (read(pipefd[0], &c, 1) == -1)
		{
			Log2(PCSC_LOG_ERROR, "read: %s", strerror(errno));
			return -1;
		};

		/* cleanup pipe fd */
		close(pipefd[0]);
		close(pipefd[1]);
	}

	return 0;
}

LONG HPStopHotPluggables(void)
{
	AraKiriHotPlug = TRUE;
	if (rescan_pipe[1] >= 0)
	{
		close(rescan_pipe[1]);
		rescan_pipe[1] = -1;
	}

	return 0;
}

static LONG HPAddHotPluggable(struct libusb_device *dev,
	struct libusb_device_descriptor desc,
	const char bus_device[],
	const struct libusb_interface *idesc,
	struct _driverTracker *driver)
{
	int i;
	uint8_t iInterface = 0;
	uint8_t iSerialNumber = 0;
	char deviceName[MAX_DEVICENAME];

	Log2(PCSC_LOG_INFO, "Adding USB device: %s", bus_device);

	snprintf(deviceName, sizeof(deviceName), "usb:%04x/%04x:libusb-1.0:%s",
		desc.idVendor, desc.idProduct, bus_device);

	deviceName[sizeof(deviceName) -1] = '\0';

	pthread_mutex_lock(&usbNotifierMutex);

	/* find a free entry */
	for (i=0; i<PCSCLITE_MAX_READERS_CONTEXTS; i++)
	{
		if (readerTracker[i].fullName == NULL)
			break;
	}

	if (i==PCSCLITE_MAX_READERS_CONTEXTS)
	{
		Log2(PCSC_LOG_ERROR,
			"Not enough reader entries. Already found %d readers", i);
		pthread_mutex_unlock(&usbNotifierMutex);
		return 0;
	}

	strncpy(readerTracker[i].bus_device, bus_device,
		sizeof(readerTracker[i].bus_device));
	readerTracker[i].bus_device[sizeof(readerTracker[i].bus_device) - 1] = '\0';
	readerTracker[i].fullName = NULL;

	if (Add_Interface_In_Name && idesc->num_altsetting > 0)
		iInterface = idesc->altsetting[0].iInterface;

	if (Add_Serial_In_Name)
		iSerialNumber = desc.iSerialNumber;

	if (iSerialNumber != 0 || iInterface != 0)
	{
		libusb_device_handle *device;
		int ret;

		ret = libusb_open(dev, &device);
		if (ret < 0)
		{
			Log2(PCSC_LOG_ERROR, "libusb_open failed: %s",
				libusb_error_name(ret));
		}
		else
		{
			unsigned char interfaceName[MAX_READERNAME];
			unsigned char serialNumber[MAX_READERNAME];
			char fullname[MAX_READERNAME * 3];
			fullname[0] = '\0';
			int ret_interface = 0;
			int ret_serial = 0;

			if (iInterface)
			{
				ret_interface = libusb_get_string_descriptor_ascii(device,
					iInterface, interfaceName, sizeof interfaceName);
				if (ret_interface < 0)
				{
					Log2(PCSC_LOG_ERROR,
						"libusb_get_string_descriptor_ascii failed: %s",
						libusb_error_name(ret_interface));
				}
			}

			if (iSerialNumber)
			{
				ret_serial = libusb_get_string_descriptor_ascii(device,
					iSerialNumber, serialNumber, sizeof serialNumber);
				if (ret_serial < 0)
				{
					Log2(PCSC_LOG_ERROR,
						"libusb_get_string_descriptor_ascii failed: %s",
						libusb_error_name(ret_serial));
				}
			}

			libusb_close(device);

			if (ret_interface > 0 && ret_serial > 0)
			{
				snprintf(fullname, sizeof(fullname), "%s [%s] (%s)",
					driver->readerName, interfaceName, serialNumber);
			}
			else
			{
				if (ret_interface > 0)
				{
					snprintf(fullname, sizeof(fullname), "%s [%s]",
						driver->readerName, interfaceName);
				}
				else
				{
					if (ret_serial > 0)
					{
						snprintf(fullname, sizeof(fullname), "%s (%s)",
							driver->readerName, serialNumber);
					}
				}
			}

			if (fullname[0] != '\0')
				readerTracker[i].fullName = strdup(fullname);
		}
	}

	if (readerTracker[i].fullName == NULL)
		readerTracker[i].fullName = strdup(driver->readerName);

	if (RFAddReader(readerTracker[i].fullName, PCSCLITE_HP_BASE_PORT + i,
		driver->libraryPath, deviceName) == SCARD_S_SUCCESS)
		readerTracker[i].status = READER_PRESENT;
	else
	{
		readerTracker[i].status = READER_FAILED;

		(void)CheckForOpenCT();
	}

	pthread_mutex_unlock(&usbNotifierMutex);

	return 1;
}	/* End of function */

static LONG HPRemoveHotPluggable(int reader_index)
{
	pthread_mutex_lock(&usbNotifierMutex);

	Log3(PCSC_LOG_INFO, "Removing USB device[%d]: %s", reader_index,
		readerTracker[reader_index].bus_device);

	RFRemoveReader(readerTracker[reader_index].fullName,
		PCSCLITE_HP_BASE_PORT + reader_index);
	free(readerTracker[reader_index].fullName);
	readerTracker[reader_index].status = READER_ABSENT;
	readerTracker[reader_index].bus_device[0] = '\0';
	readerTracker[reader_index].fullName = NULL;

	pthread_mutex_unlock(&usbNotifierMutex);

	return 1;
}	/* End of function */

/**
 * Sets up callbacks for device hotplug events.
 */
ULONG HPRegisterForHotplugEvents(void)
{
	(void)pthread_mutex_init(&usbNotifierMutex, NULL);
	return 0;
}

void HPReCheckSerialReaders(void)
{
	Log0(PCSC_LOG_INFO);
	if (rescan_pipe[1] >= 0)
	{
		char dummy = 0;
		if (write(rescan_pipe[1], &dummy, sizeof(dummy)) == -1)
			Log2(PCSC_LOG_ERROR, "write: %s", strerror(errno));
	}
}

#endif


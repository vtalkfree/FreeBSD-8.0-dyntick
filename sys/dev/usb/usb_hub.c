/* $FreeBSD: src/sys/dev/usb/usb_hub.c,v 1.28.2.3.2.1 2009/10/25 01:10:29 kensmith Exp $ */
/*-
 * Copyright (c) 1998 The NetBSD Foundation, Inc. All rights reserved.
 * Copyright (c) 1998 Lennart Augustsson. All rights reserved.
 * Copyright (c) 2008 Hans Petter Selasky. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * USB spec: http://www.usb.org/developers/docs/usbspec.zip 
 */

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/linker_set.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>

#define	USB_DEBUG_VAR uhub_debug

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_request.h>
#include <dev/usb/usb_debug.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_util.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_dynamic.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#define	UHUB_INTR_INTERVAL 250		/* ms */
#define	UHUB_N_TRANSFER 1

#ifdef USB_DEBUG
static int uhub_debug = 0;

SYSCTL_NODE(_hw_usb, OID_AUTO, uhub, CTLFLAG_RW, 0, "USB HUB");
SYSCTL_INT(_hw_usb_uhub, OID_AUTO, debug, CTLFLAG_RW, &uhub_debug, 0,
    "Debug level");
#endif

#if USB_HAVE_POWERD
static int usb_power_timeout = 30;	/* seconds */

SYSCTL_INT(_hw_usb, OID_AUTO, power_timeout, CTLFLAG_RW,
    &usb_power_timeout, 0, "USB power timeout");
#endif

struct uhub_current_state {
	uint16_t port_change;
	uint16_t port_status;
};

struct uhub_softc {
	struct uhub_current_state sc_st;/* current state */
	device_t sc_dev;		/* base device */
	struct mtx sc_mtx;		/* our mutex */
	struct usb_device *sc_udev;	/* USB device */
	struct usb_xfer *sc_xfer[UHUB_N_TRANSFER];	/* interrupt xfer */
	uint8_t	sc_flags;
#define	UHUB_FLAG_DID_EXPLORE 0x01
	char	sc_name[32];
};

#define	UHUB_PROTO(sc) ((sc)->sc_udev->ddesc.bDeviceProtocol)
#define	UHUB_IS_HIGH_SPEED(sc) (UHUB_PROTO(sc) != UDPROTO_FSHUB)
#define	UHUB_IS_SINGLE_TT(sc) (UHUB_PROTO(sc) == UDPROTO_HSHUBSTT)

/* prototypes for type checking: */

static device_probe_t uhub_probe;
static device_attach_t uhub_attach;
static device_detach_t uhub_detach;
static device_suspend_t uhub_suspend;
static device_resume_t uhub_resume;

static bus_driver_added_t uhub_driver_added;
static bus_child_location_str_t uhub_child_location_string;
static bus_child_pnpinfo_str_t uhub_child_pnpinfo_string;

static usb_callback_t uhub_intr_callback;

static void usb_dev_resume_peer(struct usb_device *udev);
static void usb_dev_suspend_peer(struct usb_device *udev);

static const struct usb_config uhub_config[UHUB_N_TRANSFER] = {

	[0] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_ANY,
		.timeout = 0,
		.flags = {.pipe_bof = 1,.short_xfer_ok = 1,},
		.bufsize = 0,	/* use wMaxPacketSize */
		.callback = &uhub_intr_callback,
		.interval = UHUB_INTR_INTERVAL,
	},
};

/*
 * driver instance for "hub" connected to "usb"
 * and "hub" connected to "hub"
 */
static devclass_t uhub_devclass;

static device_method_t uhub_methods[] = {
	DEVMETHOD(device_probe, uhub_probe),
	DEVMETHOD(device_attach, uhub_attach),
	DEVMETHOD(device_detach, uhub_detach),

	DEVMETHOD(device_suspend, uhub_suspend),
	DEVMETHOD(device_resume, uhub_resume),

	DEVMETHOD(bus_child_location_str, uhub_child_location_string),
	DEVMETHOD(bus_child_pnpinfo_str, uhub_child_pnpinfo_string),
	DEVMETHOD(bus_driver_added, uhub_driver_added),
	{0, 0}
};

static driver_t uhub_driver = {
	.name = "uhub",
	.methods = uhub_methods,
	.size = sizeof(struct uhub_softc)
};

DRIVER_MODULE(uhub, usbus, uhub_driver, uhub_devclass, 0, 0);
DRIVER_MODULE(uhub, uhub, uhub_driver, uhub_devclass, NULL, 0);

static void
uhub_intr_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct uhub_softc *sc = usbd_xfer_softc(xfer);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		DPRINTFN(2, "\n");
		/*
		 * This is an indication that some port
		 * has changed status. Notify the bus
		 * event handler thread that we need
		 * to be explored again:
		 */
		usb_needs_explore(sc->sc_udev->bus, 0);

	case USB_ST_SETUP:
		usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
		usbd_transfer_submit(xfer);
		break;

	default:			/* Error */
		if (xfer->error != USB_ERR_CANCELLED) {
			/*
			 * Do a clear-stall. The "stall_pipe" flag
			 * will get cleared before next callback by
			 * the USB stack.
			 */
			usbd_xfer_set_stall(xfer);
			usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
			usbd_transfer_submit(xfer);
		}
		break;
	}
}

/*------------------------------------------------------------------------*
 *	uhub_explore_sub - subroutine
 *
 * Return values:
 *    0: Success
 * Else: A control transaction failed
 *------------------------------------------------------------------------*/
static usb_error_t
uhub_explore_sub(struct uhub_softc *sc, struct usb_port *up)
{
	struct usb_bus *bus;
	struct usb_device *child;
	uint8_t refcount;
	usb_error_t err;

	bus = sc->sc_udev->bus;
	err = 0;

	/* get driver added refcount from USB bus */
	refcount = bus->driver_added_refcount;

	/* get device assosiated with the given port */
	child = usb_bus_port_get_device(bus, up);
	if (child == NULL) {
		/* nothing to do */
		goto done;
	}
	/* check if probe and attach should be done */

	if (child->driver_added_refcount != refcount) {
		child->driver_added_refcount = refcount;
		err = usb_probe_and_attach(child,
		    USB_IFACE_INDEX_ANY);
		if (err) {
			goto done;
		}
	}
	/* start control transfer, if device mode */

	if (child->flags.usb_mode == USB_MODE_DEVICE) {
		usbd_default_transfer_setup(child);
	}
	/* if a HUB becomes present, do a recursive HUB explore */

	if (child->hub) {
		err = (child->hub->explore) (child);
	}
done:
	return (err);
}

/*------------------------------------------------------------------------*
 *	uhub_read_port_status - factored out code
 *------------------------------------------------------------------------*/
static usb_error_t
uhub_read_port_status(struct uhub_softc *sc, uint8_t portno)
{
	struct usb_port_status ps;
	usb_error_t err;

	err = usbd_req_get_port_status(
	    sc->sc_udev, NULL, &ps, portno);

	/* update status regardless of error */

	sc->sc_st.port_status = UGETW(ps.wPortStatus);
	sc->sc_st.port_change = UGETW(ps.wPortChange);

	/* debugging print */

	DPRINTFN(4, "port %d, wPortStatus=0x%04x, "
	    "wPortChange=0x%04x, err=%s\n",
	    portno, sc->sc_st.port_status,
	    sc->sc_st.port_change, usbd_errstr(err));
	return (err);
}

/*------------------------------------------------------------------------*
 *	uhub_reattach_port
 *
 * Returns:
 *    0: Success
 * Else: A control transaction failed
 *------------------------------------------------------------------------*/
static usb_error_t
uhub_reattach_port(struct uhub_softc *sc, uint8_t portno)
{
	struct usb_device *child;
	struct usb_device *udev;
	enum usb_dev_speed speed;
	enum usb_hc_mode mode;
	usb_error_t err;
	uint8_t timeout;

	DPRINTF("reattaching port %d\n", portno);

	err = 0;
	timeout = 0;
	udev = sc->sc_udev;
	child = usb_bus_port_get_device(udev->bus,
	    udev->hub->ports + portno - 1);

repeat:

	/* first clear the port connection change bit */

	err = usbd_req_clear_port_feature(udev, NULL,
	    portno, UHF_C_PORT_CONNECTION);

	if (err) {
		goto error;
	}
	/* detach any existing devices */

	if (child) {
		usb_free_device(child,
		    USB_UNCFG_FLAG_FREE_SUBDEV |
		    USB_UNCFG_FLAG_FREE_EP0);
		child = NULL;
	}
	/* get fresh status */

	err = uhub_read_port_status(sc, portno);
	if (err) {
		goto error;
	}
	/* check if nothing is connected to the port */

	if (!(sc->sc_st.port_status & UPS_CURRENT_CONNECT_STATUS)) {
		goto error;
	}
	/* check if there is no power on the port and print a warning */

	if (!(sc->sc_st.port_status & UPS_PORT_POWER)) {
		DPRINTF("WARNING: strange, connected port %d "
		    "has no power\n", portno);
	}
	/* check if the device is in Host Mode */

	if (!(sc->sc_st.port_status & UPS_PORT_MODE_DEVICE)) {

		DPRINTF("Port %d is in Host Mode\n", portno);

		if (sc->sc_st.port_status & UPS_SUSPEND) {
			DPRINTF("Port %d was still "
			    "suspended, clearing.\n", portno);
			err = usbd_req_clear_port_feature(sc->sc_udev,
			    NULL, portno, UHF_PORT_SUSPEND);
		}
		/* USB Host Mode */

		/* wait for maximum device power up time */

		usb_pause_mtx(NULL, 
		    USB_MS_TO_TICKS(USB_PORT_POWERUP_DELAY));

		/* reset port, which implies enabling it */

		err = usbd_req_reset_port(udev, NULL, portno);

		if (err) {
			DPRINTFN(0, "port %d reset "
			    "failed, error=%s\n",
			    portno, usbd_errstr(err));
			goto error;
		}
		/* get port status again, it might have changed during reset */

		err = uhub_read_port_status(sc, portno);
		if (err) {
			goto error;
		}
		/* check if something changed during port reset */

		if ((sc->sc_st.port_change & UPS_C_CONNECT_STATUS) ||
		    (!(sc->sc_st.port_status & UPS_CURRENT_CONNECT_STATUS))) {
			if (timeout) {
				DPRINTFN(0, "giving up port reset "
				    "- device vanished!\n");
				goto error;
			}
			timeout = 1;
			goto repeat;
		}
	} else {
		DPRINTF("Port %d is in Device Mode\n", portno);
	}

	/*
	 * Figure out the device speed
	 */
	switch (udev->speed) {
	case USB_SPEED_HIGH:
		if (sc->sc_st.port_status & UPS_HIGH_SPEED)
			speed = USB_SPEED_HIGH;
		else if (sc->sc_st.port_status & UPS_LOW_SPEED)
			speed = USB_SPEED_LOW;
		else
			speed = USB_SPEED_FULL;
		break;
	case USB_SPEED_FULL:
		if (sc->sc_st.port_status & UPS_LOW_SPEED)
			speed = USB_SPEED_LOW;
		else
			speed = USB_SPEED_FULL;
		break;
	case USB_SPEED_LOW:
		speed = USB_SPEED_LOW;
		break;
	default:
		/* same speed like parent */
		speed = udev->speed;
		break;
	}
	/*
	 * Figure out the device mode
	 *
	 * NOTE: This part is currently FreeBSD specific.
	 */
	if (sc->sc_st.port_status & UPS_PORT_MODE_DEVICE)
		mode = USB_MODE_DEVICE;
	else
		mode = USB_MODE_HOST;

	/* need to create a new child */
	child = usb_alloc_device(sc->sc_dev, udev->bus, udev,
	    udev->depth + 1, portno - 1, portno, speed, mode);
	if (child == NULL) {
		DPRINTFN(0, "could not allocate new device!\n");
		goto error;
	}
	return (0);			/* success */

error:
	if (child) {
		usb_free_device(child,
		    USB_UNCFG_FLAG_FREE_SUBDEV |
		    USB_UNCFG_FLAG_FREE_EP0);
		child = NULL;
	}
	if (err == 0) {
		if (sc->sc_st.port_status & UPS_PORT_ENABLED) {
			err = usbd_req_clear_port_feature(
			    sc->sc_udev, NULL,
			    portno, UHF_PORT_ENABLE);
		}
	}
	if (err) {
		DPRINTFN(0, "device problem (%s), "
		    "disabling port %d\n", usbd_errstr(err), portno);
	}
	return (err);
}

/*------------------------------------------------------------------------*
 *	uhub_suspend_resume_port
 *
 * Returns:
 *    0: Success
 * Else: A control transaction failed
 *------------------------------------------------------------------------*/
static usb_error_t
uhub_suspend_resume_port(struct uhub_softc *sc, uint8_t portno)
{
	struct usb_device *child;
	struct usb_device *udev;
	uint8_t is_suspend;
	usb_error_t err;

	DPRINTF("port %d\n", portno);

	udev = sc->sc_udev;
	child = usb_bus_port_get_device(udev->bus,
	    udev->hub->ports + portno - 1);

	/* first clear the port suspend change bit */

	err = usbd_req_clear_port_feature(udev, NULL,
	    portno, UHF_C_PORT_SUSPEND);
	if (err) {
		DPRINTF("clearing suspend failed.\n");
		goto done;
	}
	/* get fresh status */

	err = uhub_read_port_status(sc, portno);
	if (err) {
		DPRINTF("reading port status failed.\n");
		goto done;
	}
	/* get current state */

	if (sc->sc_st.port_status & UPS_SUSPEND) {
		is_suspend = 1;
	} else {
		is_suspend = 0;
	}

	DPRINTF("suspended=%u\n", is_suspend);

	/* do the suspend or resume */

	if (child) {
		/*
		 * This code handle two cases: 1) Host Mode - we can only
		 * receive resume here 2) Device Mode - we can receive
		 * suspend and resume here
		 */
		if (is_suspend == 0)
			usb_dev_resume_peer(child);
		else if (child->flags.usb_mode == USB_MODE_DEVICE)
			usb_dev_suspend_peer(child);
	}
done:
	return (err);
}

/*------------------------------------------------------------------------*
 *	uhub_root_interrupt
 *
 * This function is called when a Root HUB interrupt has
 * happened. "ptr" and "len" makes up the Root HUB interrupt
 * packet. This function is called having the "bus_mtx" locked.
 *------------------------------------------------------------------------*/
void
uhub_root_intr(struct usb_bus *bus, const uint8_t *ptr, uint8_t len)
{
	USB_BUS_LOCK_ASSERT(bus, MA_OWNED);

	usb_needs_explore(bus, 0);
}

/*------------------------------------------------------------------------*
 *	uhub_explore
 *
 * Returns:
 *     0: Success
 *  Else: Failure
 *------------------------------------------------------------------------*/
static usb_error_t
uhub_explore(struct usb_device *udev)
{
	struct usb_hub *hub;
	struct uhub_softc *sc;
	struct usb_port *up;
	usb_error_t err;
	uint8_t portno;
	uint8_t x;

	hub = udev->hub;
	sc = hub->hubsoftc;

	DPRINTFN(11, "udev=%p addr=%d\n", udev, udev->address);

	/* ignore hubs that are too deep */
	if (udev->depth > USB_HUB_MAX_DEPTH) {
		return (USB_ERR_TOO_DEEP);
	}

	if (udev->flags.self_suspended) {
		/* need to wait until the child signals resume */
		DPRINTF("Device is suspended!\n");
		return (0);
	}
	for (x = 0; x != hub->nports; x++) {
		up = hub->ports + x;
		portno = x + 1;

		err = uhub_read_port_status(sc, portno);
		if (err) {
			/* most likely the HUB is gone */
			break;
		}
		if (sc->sc_st.port_change & UPS_C_OVERCURRENT_INDICATOR) {
			DPRINTF("Overcurrent on port %u.\n", portno);
			err = usbd_req_clear_port_feature(
			    udev, NULL, portno, UHF_C_PORT_OVER_CURRENT);
			if (err) {
				/* most likely the HUB is gone */
				break;
			}
		}
		if (!(sc->sc_flags & UHUB_FLAG_DID_EXPLORE)) {
			/*
			 * Fake a connect status change so that the
			 * status gets checked initially!
			 */
			sc->sc_st.port_change |=
			    UPS_C_CONNECT_STATUS;
		}
		if (sc->sc_st.port_change & UPS_C_PORT_ENABLED) {
			err = usbd_req_clear_port_feature(
			    udev, NULL, portno, UHF_C_PORT_ENABLE);
			if (err) {
				/* most likely the HUB is gone */
				break;
			}
			if (sc->sc_st.port_change & UPS_C_CONNECT_STATUS) {
				/*
				 * Ignore the port error if the device
				 * has vanished !
				 */
			} else if (sc->sc_st.port_status & UPS_PORT_ENABLED) {
				DPRINTFN(0, "illegal enable change, "
				    "port %d\n", portno);
			} else {

				if (up->restartcnt == USB_RESTART_MAX) {
					/* XXX could try another speed ? */
					DPRINTFN(0, "port error, giving up "
					    "port %d\n", portno);
				} else {
					sc->sc_st.port_change |=
					    UPS_C_CONNECT_STATUS;
					up->restartcnt++;
				}
			}
		}
		if (sc->sc_st.port_change & UPS_C_CONNECT_STATUS) {
			err = uhub_reattach_port(sc, portno);
			if (err) {
				/* most likely the HUB is gone */
				break;
			}
		}
		if (sc->sc_st.port_change & UPS_C_SUSPEND) {
			err = uhub_suspend_resume_port(sc, portno);
			if (err) {
				/* most likely the HUB is gone */
				break;
			}
		}
		err = uhub_explore_sub(sc, up);
		if (err) {
			/* no device(s) present */
			continue;
		}
		/* explore succeeded - reset restart counter */
		up->restartcnt = 0;
	}

	/* initial status checked */
	sc->sc_flags |= UHUB_FLAG_DID_EXPLORE;

	/* return success */
	return (USB_ERR_NORMAL_COMPLETION);
}

static int
uhub_probe(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);

	if (uaa->usb_mode != USB_MODE_HOST) {
		return (ENXIO);
	}
	/*
	 * The subclass for USB HUBs is ignored because it is 0 for
	 * some and 1 for others.
	 */
	if ((uaa->info.bConfigIndex == 0) &&
	    (uaa->info.bDeviceClass == UDCLASS_HUB)) {
		return (0);
	}
	return (ENXIO);
}

static int
uhub_attach(device_t dev)
{
	struct uhub_softc *sc = device_get_softc(dev);
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct usb_device *udev = uaa->device;
	struct usb_device *parent_hub = udev->parent_hub;
	struct usb_hub *hub;
	struct usb_hub_descriptor hubdesc;
	uint16_t pwrdly;
	uint8_t x;
	uint8_t nports;
	uint8_t portno;
	uint8_t removable;
	uint8_t iface_index;
	usb_error_t err;

	sc->sc_udev = udev;
	sc->sc_dev = dev;

	mtx_init(&sc->sc_mtx, "USB HUB mutex", NULL, MTX_DEF);

	snprintf(sc->sc_name, sizeof(sc->sc_name), "%s",
	    device_get_nameunit(dev));

	device_set_usb_desc(dev);

	DPRINTFN(2, "depth=%d selfpowered=%d, parent=%p, "
	    "parent->selfpowered=%d\n",
	    udev->depth,
	    udev->flags.self_powered,
	    parent_hub,
	    parent_hub ?
	    parent_hub->flags.self_powered : 0);

	if (udev->depth > USB_HUB_MAX_DEPTH) {
		DPRINTFN(0, "hub depth, %d, exceeded. HUB ignored!\n",
		    USB_HUB_MAX_DEPTH);
		goto error;
	}
	if (!udev->flags.self_powered && parent_hub &&
	    (!parent_hub->flags.self_powered)) {
		DPRINTFN(0, "bus powered HUB connected to "
		    "bus powered HUB. HUB ignored!\n");
		goto error;
	}
	/* get HUB descriptor */

	DPRINTFN(2, "getting HUB descriptor\n");

	/* assuming that there is one port */
	err = usbd_req_get_hub_descriptor(udev, NULL, &hubdesc, 1);

	nports = hubdesc.bNbrPorts;

	if (!err && (nports >= 8)) {
		/* get complete HUB descriptor */
		err = usbd_req_get_hub_descriptor(udev, NULL, &hubdesc, nports);
	}
	if (err) {
		DPRINTFN(0, "getting hub descriptor failed,"
		    "error=%s\n", usbd_errstr(err));
		goto error;
	}
	if (hubdesc.bNbrPorts != nports) {
		DPRINTFN(0, "number of ports changed!\n");
		goto error;
	}
	if (nports == 0) {
		DPRINTFN(0, "portless HUB!\n");
		goto error;
	}
	hub = malloc(sizeof(hub[0]) + (sizeof(hub->ports[0]) * nports),
	    M_USBDEV, M_WAITOK | M_ZERO);

	if (hub == NULL) {
		goto error;
	}
	udev->hub = hub;

#if USB_HAVE_TT_SUPPORT
	/* init FULL-speed ISOCHRONOUS schedule */
	usbd_fs_isoc_schedule_init_all(hub->fs_isoc_schedule);
#endif
	/* initialize HUB structure */
	hub->hubsoftc = sc;
	hub->explore = &uhub_explore;
	hub->nports = hubdesc.bNbrPorts;
	hub->hubudev = udev;

	/* if self powered hub, give ports maximum current */
	if (udev->flags.self_powered) {
		hub->portpower = USB_MAX_POWER;
	} else {
		hub->portpower = USB_MIN_POWER;
	}

	/* set up interrupt pipe */
	iface_index = 0;
	if (udev->parent_hub == NULL) {
		/* root HUB is special */
		err = 0;
	} else {
		/* normal HUB */
		err = usbd_transfer_setup(udev, &iface_index, sc->sc_xfer,
		    uhub_config, UHUB_N_TRANSFER, sc, &sc->sc_mtx);
	}
	if (err) {
		DPRINTFN(0, "cannot setup interrupt transfer, "
		    "errstr=%s!\n", usbd_errstr(err));
		goto error;
	}
	/* wait with power off for a while */
	usb_pause_mtx(NULL, USB_MS_TO_TICKS(USB_POWER_DOWN_TIME));

	/*
	 * To have the best chance of success we do things in the exact same
	 * order as Windoze98.  This should not be necessary, but some
	 * devices do not follow the USB specs to the letter.
	 *
	 * These are the events on the bus when a hub is attached:
	 *  Get device and config descriptors (see attach code)
	 *  Get hub descriptor (see above)
	 *  For all ports
	 *     turn on power
	 *     wait for power to become stable
	 * (all below happens in explore code)
	 *  For all ports
	 *     clear C_PORT_CONNECTION
	 *  For all ports
	 *     get port status
	 *     if device connected
	 *        wait 100 ms
	 *        turn on reset
	 *        wait
	 *        clear C_PORT_RESET
	 *        get port status
	 *        proceed with device attachment
	 */

	/* XXX should check for none, individual, or ganged power? */

	removable = 0;
	pwrdly = ((hubdesc.bPwrOn2PwrGood * UHD_PWRON_FACTOR) +
	    USB_EXTRA_POWER_UP_TIME);

	for (x = 0; x != nports; x++) {
		/* set up data structures */
		struct usb_port *up = hub->ports + x;

		up->device_index = 0;
		up->restartcnt = 0;
		portno = x + 1;

		/* check if port is removable */
		if (!UHD_NOT_REMOV(&hubdesc, portno)) {
			removable++;
		}
		if (!err) {
			/* turn the power on */
			err = usbd_req_set_port_feature(udev, NULL,
			    portno, UHF_PORT_POWER);
		}
		if (err) {
			DPRINTFN(0, "port %d power on failed, %s\n",
			    portno, usbd_errstr(err));
		}
		DPRINTF("turn on port %d power\n",
		    portno);

		/* wait for stable power */
		usb_pause_mtx(NULL, USB_MS_TO_TICKS(pwrdly));
	}

	device_printf(dev, "%d port%s with %d "
	    "removable, %s powered\n", nports, (nports != 1) ? "s" : "",
	    removable, udev->flags.self_powered ? "self" : "bus");

	/* Start the interrupt endpoint, if any */

	if (sc->sc_xfer[0] != NULL) {
		mtx_lock(&sc->sc_mtx);
		usbd_transfer_start(sc->sc_xfer[0]);
		mtx_unlock(&sc->sc_mtx);
	}

	/* Enable automatic power save on all USB HUBs */

	usbd_set_power_mode(udev, USB_POWER_MODE_SAVE);

	return (0);

error:
	usbd_transfer_unsetup(sc->sc_xfer, UHUB_N_TRANSFER);

	if (udev->hub) {
		free(udev->hub, M_USBDEV);
		udev->hub = NULL;
	}

	mtx_destroy(&sc->sc_mtx);

	return (ENXIO);
}

/*
 * Called from process context when the hub is gone.
 * Detach all devices on active ports.
 */
static int
uhub_detach(device_t dev)
{
	struct uhub_softc *sc = device_get_softc(dev);
	struct usb_hub *hub = sc->sc_udev->hub;
	struct usb_device *child;
	uint8_t x;

	/* detach all children first */
	bus_generic_detach(dev);

	if (hub == NULL) {		/* must be partially working */
		return (0);
	}
	for (x = 0; x != hub->nports; x++) {

		child = usb_bus_port_get_device(sc->sc_udev->bus, hub->ports + x);

		if (child == NULL) {
			continue;
		}
		/*
		 * Subdevices are not freed, because the caller of
		 * uhub_detach() will do that.
		 */
		usb_free_device(child,
		    USB_UNCFG_FLAG_FREE_EP0);
	}

	usbd_transfer_unsetup(sc->sc_xfer, UHUB_N_TRANSFER);

	free(hub, M_USBDEV);
	sc->sc_udev->hub = NULL;

	mtx_destroy(&sc->sc_mtx);

	return (0);
}

static int
uhub_suspend(device_t dev)
{
	DPRINTF("\n");
	/* Sub-devices are not suspended here! */
	return (0);
}

static int
uhub_resume(device_t dev)
{
	DPRINTF("\n");
	/* Sub-devices are not resumed here! */
	return (0);
}

static void
uhub_driver_added(device_t dev, driver_t *driver)
{
	usb_needs_explore_all();
}

struct hub_result {
	struct usb_device *udev;
	uint8_t	portno;
	uint8_t	iface_index;
};

static void
uhub_find_iface_index(struct usb_hub *hub, device_t child,
    struct hub_result *res)
{
	struct usb_interface *iface;
	struct usb_device *udev;
	uint8_t nports;
	uint8_t x;
	uint8_t i;

	nports = hub->nports;
	for (x = 0; x != nports; x++) {
		udev = usb_bus_port_get_device(hub->hubudev->bus,
		    hub->ports + x);
		if (!udev) {
			continue;
		}
		for (i = 0; i != USB_IFACE_MAX; i++) {
			iface = usbd_get_iface(udev, i);
			if (iface &&
			    (iface->subdev == child)) {
				res->iface_index = i;
				res->udev = udev;
				res->portno = x + 1;
				return;
			}
		}
	}
	res->iface_index = 0;
	res->udev = NULL;
	res->portno = 0;
}

static int
uhub_child_location_string(device_t parent, device_t child,
    char *buf, size_t buflen)
{
	struct uhub_softc *sc = device_get_softc(parent);
	struct usb_hub *hub = sc->sc_udev->hub;
	struct hub_result res;

	mtx_lock(&Giant);
	uhub_find_iface_index(hub, child, &res);
	if (!res.udev) {
		DPRINTF("device not on hub\n");
		if (buflen) {
			buf[0] = '\0';
		}
		goto done;
	}
	snprintf(buf, buflen, "port=%u interface=%u",
	    res.portno, res.iface_index);
done:
	mtx_unlock(&Giant);

	return (0);
}

static int
uhub_child_pnpinfo_string(device_t parent, device_t child,
    char *buf, size_t buflen)
{
	struct uhub_softc *sc = device_get_softc(parent);
	struct usb_hub *hub = sc->sc_udev->hub;
	struct usb_interface *iface;
	struct hub_result res;

	mtx_lock(&Giant);
	uhub_find_iface_index(hub, child, &res);
	if (!res.udev) {
		DPRINTF("device not on hub\n");
		if (buflen) {
			buf[0] = '\0';
		}
		goto done;
	}
	iface = usbd_get_iface(res.udev, res.iface_index);
	if (iface && iface->idesc) {
		snprintf(buf, buflen, "vendor=0x%04x product=0x%04x "
		    "devclass=0x%02x devsubclass=0x%02x "
		    "sernum=\"%s\" "
		    "release=0x%04x "
		    "intclass=0x%02x intsubclass=0x%02x",
		    UGETW(res.udev->ddesc.idVendor),
		    UGETW(res.udev->ddesc.idProduct),
		    res.udev->ddesc.bDeviceClass,
		    res.udev->ddesc.bDeviceSubClass,
		    res.udev->serial,
		    UGETW(res.udev->ddesc.bcdDevice),
		    iface->idesc->bInterfaceClass,
		    iface->idesc->bInterfaceSubClass);
	} else {
		if (buflen) {
			buf[0] = '\0';
		}
		goto done;
	}
done:
	mtx_unlock(&Giant);

	return (0);
}

/*
 * The USB Transaction Translator:
 * ===============================
 *
 * When doing LOW- and FULL-speed USB transfers accross a HIGH-speed
 * USB HUB, bandwidth must be allocated for ISOCHRONOUS and INTERRUPT
 * USB transfers. To utilize bandwidth dynamically the "scatter and
 * gather" principle must be applied. This means that bandwidth must
 * be divided into equal parts of bandwidth. With regard to USB all
 * data is transferred in smaller packets with length
 * "wMaxPacketSize". The problem however is that "wMaxPacketSize" is
 * not a constant!
 *
 * The bandwidth scheduler which I have implemented will simply pack
 * the USB transfers back to back until there is no more space in the
 * schedule. Out of the 8 microframes which the USB 2.0 standard
 * provides, only 6 are available for non-HIGH-speed devices. I have
 * reserved the first 4 microframes for ISOCHRONOUS transfers. The
 * last 2 microframes I have reserved for INTERRUPT transfers. Without
 * this division, it is very difficult to allocate and free bandwidth
 * dynamically.
 *
 * NOTE about the Transaction Translator in USB HUBs:
 *
 * USB HUBs have a very simple Transaction Translator, that will
 * simply pipeline all the SPLIT transactions. That means that the
 * transactions will be executed in the order they are queued!
 *
 */

/*------------------------------------------------------------------------*
 *	usb_intr_find_best_slot
 *
 * Return value:
 *   The best Transaction Translation slot for an interrupt endpoint.
 *------------------------------------------------------------------------*/
static uint8_t
usb_intr_find_best_slot(usb_size_t *ptr, uint8_t start, uint8_t end)
{
	usb_size_t max = 0 - 1;
	uint8_t x;
	uint8_t y;

	y = 0;

	/* find the last slot with lesser used bandwidth */

	for (x = start; x < end; x++) {
		if (max >= ptr[x]) {
			max = ptr[x];
			y = x;
		}
	}
	return (y);
}

/*------------------------------------------------------------------------*
 *	usb_intr_schedule_adjust
 *
 * This function will update the bandwith usage for the microframe
 * having index "slot" by "len" bytes. "len" can be negative.  If the
 * "slot" argument is greater or equal to "USB_HS_MICRO_FRAMES_MAX"
 * the "slot" argument will be replaced by the slot having least used
 * bandwidth.
 *
 * Returns:
 *   The slot on which the bandwidth update was done.
 *------------------------------------------------------------------------*/
uint8_t
usb_intr_schedule_adjust(struct usb_device *udev, int16_t len, uint8_t slot)
{
	struct usb_bus *bus = udev->bus;
	struct usb_hub *hub;
	enum usb_dev_speed speed;

	USB_BUS_LOCK_ASSERT(bus, MA_OWNED);

	speed = usbd_get_speed(udev);

	switch (speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
		if (speed == USB_SPEED_LOW) {
			len *= 8;
		}
		/*
	         * The Host Controller Driver should have
	         * performed checks so that the lookup
	         * below does not result in a NULL pointer
	         * access.
	         */

		hub = udev->parent_hs_hub->hub;
		if (slot >= USB_HS_MICRO_FRAMES_MAX) {
			slot = usb_intr_find_best_slot(hub->uframe_usage,
			    USB_FS_ISOC_UFRAME_MAX, 6);
		}
		hub->uframe_usage[slot] += len;
		bus->uframe_usage[slot] += len;
		break;
	default:
		if (slot >= USB_HS_MICRO_FRAMES_MAX) {
			slot = usb_intr_find_best_slot(bus->uframe_usage, 0,
			    USB_HS_MICRO_FRAMES_MAX);
		}
		bus->uframe_usage[slot] += len;
		break;
	}
	return (slot);
}

/*------------------------------------------------------------------------*
 *	usbd_fs_isoc_schedule_init_sub
 *
 * This function initialises an USB FULL speed isochronous schedule
 * entry.
 *------------------------------------------------------------------------*/
#if USB_HAVE_TT_SUPPORT
static void
usbd_fs_isoc_schedule_init_sub(struct usb_fs_isoc_schedule *fss)
{
	fss->total_bytes = (USB_FS_ISOC_UFRAME_MAX *
	    USB_FS_BYTES_PER_HS_UFRAME);
	fss->frame_bytes = (USB_FS_BYTES_PER_HS_UFRAME);
	fss->frame_slot = 0;
}
#endif

/*------------------------------------------------------------------------*
 *	usbd_fs_isoc_schedule_init_all
 *
 * This function will reset the complete USB FULL speed isochronous
 * bandwidth schedule.
 *------------------------------------------------------------------------*/
#if USB_HAVE_TT_SUPPORT
void
usbd_fs_isoc_schedule_init_all(struct usb_fs_isoc_schedule *fss)
{
	struct usb_fs_isoc_schedule *fss_end = fss + USB_ISOC_TIME_MAX;

	while (fss != fss_end) {
		usbd_fs_isoc_schedule_init_sub(fss);
		fss++;
	}
}
#endif

/*------------------------------------------------------------------------*
 *	usb_isoc_time_expand
 *
 * This function will expand the time counter from 7-bit to 16-bit.
 *
 * Returns:
 *   16-bit isochronous time counter.
 *------------------------------------------------------------------------*/
uint16_t
usb_isoc_time_expand(struct usb_bus *bus, uint16_t isoc_time_curr)
{
	uint16_t rem;

	USB_BUS_LOCK_ASSERT(bus, MA_OWNED);

	rem = bus->isoc_time_last & (USB_ISOC_TIME_MAX - 1);

	isoc_time_curr &= (USB_ISOC_TIME_MAX - 1);

	if (isoc_time_curr < rem) {
		/* the time counter wrapped around */
		bus->isoc_time_last += USB_ISOC_TIME_MAX;
	}
	/* update the remainder */

	bus->isoc_time_last &= ~(USB_ISOC_TIME_MAX - 1);
	bus->isoc_time_last |= isoc_time_curr;

	return (bus->isoc_time_last);
}

/*------------------------------------------------------------------------*
 *	usbd_fs_isoc_schedule_isoc_time_expand
 *
 * This function does multiple things. First of all it will expand the
 * passed isochronous time, which is the return value. Then it will
 * store where the current FULL speed isochronous schedule is
 * positioned in time and where the end is. See "pp_start" and
 * "pp_end" arguments.
 *
 * Returns:
 *   Expanded version of "isoc_time".
 *
 * NOTE: This function depends on being called regularly with
 * intervals less than "USB_ISOC_TIME_MAX".
 *------------------------------------------------------------------------*/
#if USB_HAVE_TT_SUPPORT
uint16_t
usbd_fs_isoc_schedule_isoc_time_expand(struct usb_device *udev,
    struct usb_fs_isoc_schedule **pp_start,
    struct usb_fs_isoc_schedule **pp_end,
    uint16_t isoc_time)
{
	struct usb_fs_isoc_schedule *fss_end;
	struct usb_fs_isoc_schedule *fss_a;
	struct usb_fs_isoc_schedule *fss_b;
	struct usb_hub *hs_hub;

	isoc_time = usb_isoc_time_expand(udev->bus, isoc_time);

	hs_hub = udev->parent_hs_hub->hub;

	if (hs_hub != NULL) {

		fss_a = hs_hub->fs_isoc_schedule +
		    (hs_hub->isoc_last_time % USB_ISOC_TIME_MAX);

		hs_hub->isoc_last_time = isoc_time;

		fss_b = hs_hub->fs_isoc_schedule +
		    (isoc_time % USB_ISOC_TIME_MAX);

		fss_end = hs_hub->fs_isoc_schedule + USB_ISOC_TIME_MAX;

		*pp_start = hs_hub->fs_isoc_schedule;
		*pp_end = fss_end;

		while (fss_a != fss_b) {
			if (fss_a == fss_end) {
				fss_a = hs_hub->fs_isoc_schedule;
				continue;
			}
			usbd_fs_isoc_schedule_init_sub(fss_a);
			fss_a++;
		}

	} else {

		*pp_start = NULL;
		*pp_end = NULL;
	}
	return (isoc_time);
}
#endif

/*------------------------------------------------------------------------*
 *	usbd_fs_isoc_schedule_alloc
 *
 * This function will allocate bandwidth for an isochronous FULL speed
 * transaction in the FULL speed schedule. The microframe slot where
 * the transaction should be started is stored in the byte pointed to
 * by "pstart". The "len" argument specifies the length of the
 * transaction in bytes.
 *
 * Returns:
 *    0: Success
 * Else: Error
 *------------------------------------------------------------------------*/
#if USB_HAVE_TT_SUPPORT
uint8_t
usbd_fs_isoc_schedule_alloc(struct usb_fs_isoc_schedule *fss,
    uint8_t *pstart, uint16_t len)
{
	uint8_t slot = fss->frame_slot;

	/* Compute overhead and bit-stuffing */

	len += 8;

	len *= 7;
	len /= 6;

	if (len > fss->total_bytes) {
		*pstart = 0;		/* set some dummy value */
		return (1);		/* error */
	}
	if (len > 0) {

		fss->total_bytes -= len;

		while (len >= fss->frame_bytes) {
			len -= fss->frame_bytes;
			fss->frame_bytes = USB_FS_BYTES_PER_HS_UFRAME;
			fss->frame_slot++;
		}

		fss->frame_bytes -= len;
	}
	*pstart = slot;
	return (0);			/* success */
}
#endif

/*------------------------------------------------------------------------*
 *	usb_bus_port_get_device
 *
 * This function is NULL safe.
 *------------------------------------------------------------------------*/
struct usb_device *
usb_bus_port_get_device(struct usb_bus *bus, struct usb_port *up)
{
	if ((bus == NULL) || (up == NULL)) {
		/* be NULL safe */
		return (NULL);
	}
	if (up->device_index == 0) {
		/* nothing to do */
		return (NULL);
	}
	return (bus->devices[up->device_index]);
}

/*------------------------------------------------------------------------*
 *	usb_bus_port_set_device
 *
 * This function is NULL safe.
 *------------------------------------------------------------------------*/
void
usb_bus_port_set_device(struct usb_bus *bus, struct usb_port *up,
    struct usb_device *udev, uint8_t device_index)
{
	if (bus == NULL) {
		/* be NULL safe */
		return;
	}
	/*
	 * There is only one case where we don't
	 * have an USB port, and that is the Root Hub!
         */
	if (up) {
		if (udev) {
			up->device_index = device_index;
		} else {
			device_index = up->device_index;
			up->device_index = 0;
		}
	}
	/*
	 * Make relationships to our new device
	 */
	if (device_index != 0) {
#if USB_HAVE_UGEN
		mtx_lock(&usb_ref_lock);
#endif
		bus->devices[device_index] = udev;
#if USB_HAVE_UGEN
		mtx_unlock(&usb_ref_lock);
#endif
	}
	/*
	 * Debug print
	 */
	DPRINTFN(2, "bus %p devices[%u] = %p\n", bus, device_index, udev);
}

/*------------------------------------------------------------------------*
 *	usb_needs_explore
 *
 * This functions is called when the USB event thread needs to run.
 *------------------------------------------------------------------------*/
void
usb_needs_explore(struct usb_bus *bus, uint8_t do_probe)
{
	uint8_t do_unlock;

	DPRINTF("\n");

	if (bus == NULL) {
		DPRINTF("No bus pointer!\n");
		return;
	}
	if ((bus->devices == NULL) ||
	    (bus->devices[USB_ROOT_HUB_ADDR] == NULL)) {
		DPRINTF("No root HUB\n");
		return;
	}
	if (mtx_owned(&bus->bus_mtx)) {
		do_unlock = 0;
	} else {
		USB_BUS_LOCK(bus);
		do_unlock = 1;
	}
	if (do_probe) {
		bus->do_probe = 1;
	}
	if (usb_proc_msignal(&bus->explore_proc,
	    &bus->explore_msg[0], &bus->explore_msg[1])) {
		/* ignore */
	}
	if (do_unlock) {
		USB_BUS_UNLOCK(bus);
	}
}

/*------------------------------------------------------------------------*
 *	usb_needs_explore_all
 *
 * This function is called whenever a new driver is loaded and will
 * cause that all USB busses are re-explored.
 *------------------------------------------------------------------------*/
void
usb_needs_explore_all(void)
{
	struct usb_bus *bus;
	devclass_t dc;
	device_t dev;
	int max;

	DPRINTFN(3, "\n");

	dc = usb_devclass_ptr;
	if (dc == NULL) {
		DPRINTFN(0, "no devclass\n");
		return;
	}
	/*
	 * Explore all USB busses in parallell.
	 */
	max = devclass_get_maxunit(dc);
	while (max >= 0) {
		dev = devclass_get_device(dc, max);
		if (dev) {
			bus = device_get_softc(dev);
			if (bus) {
				usb_needs_explore(bus, 1);
			}
		}
		max--;
	}
}

/*------------------------------------------------------------------------*
 *	usb_bus_power_update
 *
 * This function will ensure that all USB devices on the given bus are
 * properly suspended or resumed according to the device transfer
 * state.
 *------------------------------------------------------------------------*/
#if USB_HAVE_POWERD
void
usb_bus_power_update(struct usb_bus *bus)
{
	usb_needs_explore(bus, 0 /* no probe */ );
}
#endif

/*------------------------------------------------------------------------*
 *	usbd_transfer_power_ref
 *
 * This function will modify the power save reference counts and
 * wakeup the USB device associated with the given USB transfer, if
 * needed.
 *------------------------------------------------------------------------*/
#if USB_HAVE_POWERD
void
usbd_transfer_power_ref(struct usb_xfer *xfer, int val)
{
	static const usb_power_mask_t power_mask[4] = {
		[UE_CONTROL] = USB_HW_POWER_CONTROL,
		[UE_BULK] = USB_HW_POWER_BULK,
		[UE_INTERRUPT] = USB_HW_POWER_INTERRUPT,
		[UE_ISOCHRONOUS] = USB_HW_POWER_ISOC,
	};
	struct usb_device *udev;
	uint8_t needs_explore;
	uint8_t needs_hw_power;
	uint8_t xfer_type;

	udev = xfer->xroot->udev;

	if (udev->device_index == USB_ROOT_HUB_ADDR) {
		/* no power save for root HUB */
		return;
	}
	USB_BUS_LOCK(udev->bus);

	xfer_type = xfer->endpoint->edesc->bmAttributes & UE_XFERTYPE;

	udev->pwr_save.last_xfer_time = ticks;
	udev->pwr_save.type_refs[xfer_type] += val;

	if (xfer->flags_int.control_xfr) {
		udev->pwr_save.read_refs += val;
		if (xfer->flags_int.usb_mode == USB_MODE_HOST) {
			/*
			 * it is not allowed to suspend during a control
			 * transfer
			 */
			udev->pwr_save.write_refs += val;
		}
	} else if (USB_GET_DATA_ISREAD(xfer)) {
		udev->pwr_save.read_refs += val;
	} else {
		udev->pwr_save.write_refs += val;
	}

	if (udev->flags.self_suspended)
		needs_explore =
		    (udev->pwr_save.write_refs != 0) ||
		    ((udev->pwr_save.read_refs != 0) &&
		    (usb_peer_can_wakeup(udev) == 0));
	else
		needs_explore = 0;

	if (!(udev->bus->hw_power_state & power_mask[xfer_type])) {
		DPRINTF("Adding type %u to power state\n", xfer_type);
		udev->bus->hw_power_state |= power_mask[xfer_type];
		needs_hw_power = 1;
	} else {
		needs_hw_power = 0;
	}

	USB_BUS_UNLOCK(udev->bus);

	if (needs_explore) {
		DPRINTF("update\n");
		usb_bus_power_update(udev->bus);
	} else if (needs_hw_power) {
		DPRINTF("needs power\n");
		if (udev->bus->methods->set_hw_power != NULL) {
			(udev->bus->methods->set_hw_power) (udev->bus);
		}
	}
}
#endif

/*------------------------------------------------------------------------*
 *	usb_bus_powerd
 *
 * This function implements the USB power daemon and is called
 * regularly from the USB explore thread.
 *------------------------------------------------------------------------*/
#if USB_HAVE_POWERD
void
usb_bus_powerd(struct usb_bus *bus)
{
	struct usb_device *udev;
	usb_ticks_t temp;
	usb_ticks_t limit;
	usb_ticks_t mintime;
	usb_size_t type_refs[5];
	uint8_t x;
	uint8_t rem_wakeup;

	limit = usb_power_timeout;
	if (limit == 0)
		limit = hz;
	else if (limit > 255)
		limit = 255 * hz;
	else
		limit = limit * hz;

	DPRINTF("bus=%p\n", bus);

	USB_BUS_LOCK(bus);

	/*
	 * The root HUB device is never suspended
	 * and we simply skip it.
	 */
	for (x = USB_ROOT_HUB_ADDR + 1;
	    x != bus->devices_max; x++) {

		udev = bus->devices[x];
		if (udev == NULL)
			continue;

		rem_wakeup = usb_peer_can_wakeup(udev);

		temp = ticks - udev->pwr_save.last_xfer_time;

		if ((udev->power_mode == USB_POWER_MODE_ON) ||
		    (udev->pwr_save.type_refs[UE_ISOCHRONOUS] != 0) ||
		    (udev->pwr_save.write_refs != 0) ||
		    ((udev->pwr_save.read_refs != 0) &&
		    (rem_wakeup == 0))) {

			/* check if we are suspended */
			if (udev->flags.self_suspended != 0) {
				USB_BUS_UNLOCK(bus);
				usb_dev_resume_peer(udev);
				USB_BUS_LOCK(bus);
			}
		} else if (temp >= limit) {

			/* check if we are not suspended */
			if (udev->flags.self_suspended == 0) {
				USB_BUS_UNLOCK(bus);
				usb_dev_suspend_peer(udev);
				USB_BUS_LOCK(bus);
			}
		}
	}

	/* reset counters */

	mintime = 0 - 1;
	type_refs[0] = 0;
	type_refs[1] = 0;
	type_refs[2] = 0;
	type_refs[3] = 0;
	type_refs[4] = 0;

	/* Re-loop all the devices to get the actual state */

	for (x = USB_ROOT_HUB_ADDR + 1;
	    x != bus->devices_max; x++) {

		udev = bus->devices[x];
		if (udev == NULL)
			continue;

		/* we found a non-Root-Hub USB device */
		type_refs[4] += 1;

		/* "last_xfer_time" can be updated by a resume */
		temp = ticks - udev->pwr_save.last_xfer_time;

		/*
		 * Compute minimum time since last transfer for the complete
		 * bus:
		 */
		if (temp < mintime)
			mintime = temp;

		if (udev->flags.self_suspended == 0) {
			type_refs[0] += udev->pwr_save.type_refs[0];
			type_refs[1] += udev->pwr_save.type_refs[1];
			type_refs[2] += udev->pwr_save.type_refs[2];
			type_refs[3] += udev->pwr_save.type_refs[3];
		}
	}

	if (mintime >= (1 * hz)) {
		/* recompute power masks */
		DPRINTF("Recomputing power masks\n");
		bus->hw_power_state = 0;
		if (type_refs[UE_CONTROL] != 0)
			bus->hw_power_state |= USB_HW_POWER_CONTROL;
		if (type_refs[UE_BULK] != 0)
			bus->hw_power_state |= USB_HW_POWER_BULK;
		if (type_refs[UE_INTERRUPT] != 0)
			bus->hw_power_state |= USB_HW_POWER_INTERRUPT;
		if (type_refs[UE_ISOCHRONOUS] != 0)
			bus->hw_power_state |= USB_HW_POWER_ISOC;
		if (type_refs[4] != 0)
			bus->hw_power_state |= USB_HW_POWER_NON_ROOT_HUB;
	}
	USB_BUS_UNLOCK(bus);

	if (bus->methods->set_hw_power != NULL) {
		/* always update hardware power! */
		(bus->methods->set_hw_power) (bus);
	}
	return;
}
#endif

/*------------------------------------------------------------------------*
 *	usb_dev_resume_peer
 *
 * This function will resume an USB peer and do the required USB
 * signalling to get an USB device out of the suspended state.
 *------------------------------------------------------------------------*/
static void
usb_dev_resume_peer(struct usb_device *udev)
{
	struct usb_bus *bus;
	int err;

	/* be NULL safe */
	if (udev == NULL)
		return;

	/* check if already resumed */
	if (udev->flags.self_suspended == 0)
		return;

	/* we need a parent HUB to do resume */
	if (udev->parent_hub == NULL)
		return;

	DPRINTF("udev=%p\n", udev);

	if ((udev->flags.usb_mode == USB_MODE_DEVICE) &&
	    (udev->flags.remote_wakeup == 0)) {
		/*
		 * If the host did not set the remote wakeup feature, we can
		 * not wake it up either!
		 */
		DPRINTF("remote wakeup is not set!\n");
		return;
	}
	/* get bus pointer */
	bus = udev->bus;

	/* resume parent hub first */
	usb_dev_resume_peer(udev->parent_hub);

	/* resume current port (Valid in Host and Device Mode) */
	err = usbd_req_clear_port_feature(udev->parent_hub,
	    NULL, udev->port_no, UHF_PORT_SUSPEND);
	if (err) {
		DPRINTFN(0, "Resuming port failed!\n");
		return;
	}
	/* resume settle time */
	usb_pause_mtx(NULL, USB_MS_TO_TICKS(USB_PORT_RESUME_DELAY));

	if (bus->methods->device_resume != NULL) {
		/* resume USB device on the USB controller */
		(bus->methods->device_resume) (udev);
	}
	USB_BUS_LOCK(bus);
	/* set that this device is now resumed */
	udev->flags.self_suspended = 0;
#if USB_HAVE_POWERD
	/* make sure that we don't go into suspend right away */
	udev->pwr_save.last_xfer_time = ticks;

	/* make sure the needed power masks are on */
	if (udev->pwr_save.type_refs[UE_CONTROL] != 0)
		bus->hw_power_state |= USB_HW_POWER_CONTROL;
	if (udev->pwr_save.type_refs[UE_BULK] != 0)
		bus->hw_power_state |= USB_HW_POWER_BULK;
	if (udev->pwr_save.type_refs[UE_INTERRUPT] != 0)
		bus->hw_power_state |= USB_HW_POWER_INTERRUPT;
	if (udev->pwr_save.type_refs[UE_ISOCHRONOUS] != 0)
		bus->hw_power_state |= USB_HW_POWER_ISOC;
#endif
	USB_BUS_UNLOCK(bus);

	if (bus->methods->set_hw_power != NULL) {
		/* always update hardware power! */
		(bus->methods->set_hw_power) (bus);
	}

	usbd_enum_lock(udev);

	/* notify all sub-devices about resume */
	err = usb_suspend_resume(udev, 0);

	usbd_enum_unlock(udev);

	/* check if peer has wakeup capability */
	if (usb_peer_can_wakeup(udev)) {
		/* clear remote wakeup */
		err = usbd_req_clear_device_feature(udev,
		    NULL, UF_DEVICE_REMOTE_WAKEUP);
		if (err) {
			DPRINTFN(0, "Clearing device "
			    "remote wakeup failed: %s!\n",
			    usbd_errstr(err));
		}
	}
	return;
}

/*------------------------------------------------------------------------*
 *	usb_dev_suspend_peer
 *
 * This function will suspend an USB peer and do the required USB
 * signalling to get an USB device into the suspended state.
 *------------------------------------------------------------------------*/
static void
usb_dev_suspend_peer(struct usb_device *udev)
{
	struct usb_device *child;
	int err;
	uint8_t x;
	uint8_t nports;

repeat:
	/* be NULL safe */
	if (udev == NULL)
		return;

	/* check if already suspended */
	if (udev->flags.self_suspended)
		return;

	/* we need a parent HUB to do suspend */
	if (udev->parent_hub == NULL)
		return;

	DPRINTF("udev=%p\n", udev);

	/* check if the current device is a HUB */
	if (udev->hub != NULL) {
		nports = udev->hub->nports;

		/* check if all devices on the HUB are suspended */
		for (x = 0; x != nports; x++) {

			child = usb_bus_port_get_device(udev->bus,
			    udev->hub->ports + x);

			if (child == NULL)
				continue;

			if (child->flags.self_suspended)
				continue;

			DPRINTFN(1, "Port %u is busy on the HUB!\n", x + 1);
			return;
		}
	}

	usbd_enum_lock(udev);

	/* notify all sub-devices about suspend */
	err = usb_suspend_resume(udev, 1);

	usbd_enum_unlock(udev);

	if (usb_peer_can_wakeup(udev)) {
		/* allow device to do remote wakeup */
		err = usbd_req_set_device_feature(udev,
		    NULL, UF_DEVICE_REMOTE_WAKEUP);
		if (err) {
			DPRINTFN(0, "Setting device "
			    "remote wakeup failed!\n");
		}
	}
	USB_BUS_LOCK(udev->bus);
	/*
	 * Set that this device is suspended. This variable must be set
	 * before calling USB controller suspend callbacks.
	 */
	udev->flags.self_suspended = 1;
	USB_BUS_UNLOCK(udev->bus);

	if (udev->bus->methods->device_suspend != NULL) {
		usb_timeout_t temp;

		/* suspend device on the USB controller */
		(udev->bus->methods->device_suspend) (udev);

		/* do DMA delay */
		temp = usbd_get_dma_delay(udev->bus);
		usb_pause_mtx(NULL, USB_MS_TO_TICKS(temp));

	}
	/* suspend current port */
	err = usbd_req_set_port_feature(udev->parent_hub,
	    NULL, udev->port_no, UHF_PORT_SUSPEND);
	if (err) {
		DPRINTFN(0, "Suspending port failed\n");
		return;
	}

	udev = udev->parent_hub;
	goto repeat;
}

/*------------------------------------------------------------------------*
 *	usbd_set_power_mode
 *
 * This function will set the power mode, see USB_POWER_MODE_XXX for a
 * USB device.
 *------------------------------------------------------------------------*/
void
usbd_set_power_mode(struct usb_device *udev, uint8_t power_mode)
{
	/* filter input argument */
	if ((power_mode != USB_POWER_MODE_ON) &&
	    (power_mode != USB_POWER_MODE_OFF)) {
		power_mode = USB_POWER_MODE_SAVE;
	}
	udev->power_mode = power_mode;	/* update copy of power mode */

#if USB_HAVE_POWERD
	usb_bus_power_update(udev->bus);
#endif
}

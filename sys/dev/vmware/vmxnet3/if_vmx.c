/*-
 * Copyright (c) 2013 Tsubai Masanari
 * Copyright (c) 2013 Bryan Venteicher <bryanv@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $OpenBSD: src/sys/dev/pci/if_vmx.c,v 1.11 2013/06/22 00:28:10 uebayasi Exp $
 */

/* Driver for VMware vmxnet3 virtual ethernet devices. */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/endian.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/if_vlan_var.h>

#include <net/bpf.h>

#include <net/if_types.h>
#include <net/if_vlan_var.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "if_vmxreg.h"
#include "if_vmxvar.h"

#include "opt_inet.h"
#include "opt_inet6.h"

static int	vmxnet3_probe(device_t);
static int	vmxnet3_attach(device_t);
static int	vmxnet3_detach(device_t);
static int	vmxnet3_shutdown(device_t);

static int	vmxnet3_alloc_resources(struct vmxnet3_softc *);
static void	vmxnet3_free_resources(struct vmxnet3_softc *);
static int	vmxnet3_check_version(struct vmxnet3_softc *);
static void	vmxnet3_initial_config(struct vmxnet3_softc *);

static int	vmxnet3_init_rxq(struct vmxnet3_softc *, int);
static int	vmxnet3_init_txq(struct vmxnet3_softc *, int);
static int	vmxnet3_alloc_rxtx_queues(struct vmxnet3_softc *);
static void	vmxnet3_destroy_rxq(struct vmxnet3_rxqueue *);
static void 	vmxnet3_destroy_txq(struct vmxnet3_txqueue *);
static void	vmxnet3_free_rxtx_queues(struct vmxnet3_softc *);

static int	vmxnet3_alloc_shared_data(struct vmxnet3_softc *);
static void	vmxnet3_free_shared_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_txq_data(struct vmxnet3_softc *);
static void	vmxnet3_free_txq_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_rxq_data(struct vmxnet3_softc *);
static void	vmxnet3_free_rxq_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_queue_data(struct vmxnet3_softc *);
static void	vmxnet3_free_queue_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_mcast_table(struct vmxnet3_softc *);
static void	vmxnet3_init_shared_data(struct vmxnet3_softc *);
static void	vmxnet3_reinit_shared_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_data(struct vmxnet3_softc *);
static void	vmxnet3_free_data(struct vmxnet3_softc *);
static int	vmxnet3_setup_interface(struct vmxnet3_softc *);

static void	vmxnet3_evintr(struct vmxnet3_softc *);
static void	vmxnet3_txeof(struct vmxnet3_softc *, struct vmxnet3_txqueue *);
static void	vmxnet3_rx_csum(struct vmxnet3_rxcompdesc *, struct mbuf *);
static int	vmxnet3_newbuf(struct vmxnet3_softc *, struct vmxnet3_rxring *);
static void	vmxnet3_rxeof(struct vmxnet3_softc *, struct vmxnet3_rxqueue *);
static void	vmxnet3_legacy_intr(void *);
static int	vmxnet3_setup_interrupts(struct vmxnet3_softc *);

static void	vmxnet3_txstop(struct vmxnet3_softc *, struct vmxnet3_txqueue *);
static void	vmxnet3_rxstop(struct vmxnet3_softc *, struct vmxnet3_rxqueue *);
static void	vmxnet3_stop(struct vmxnet3_softc *);

static void	vmxnet3_txinit(struct vmxnet3_softc *, struct vmxnet3_txqueue *);
static int	vmxnet3_rxinit(struct vmxnet3_softc *, struct vmxnet3_rxqueue *);
static int	vmxnet3_reinit_queues(struct vmxnet3_softc *);
static int	vmxnet3_enable_device(struct vmxnet3_softc *);
static void	vmxnet3_reinit_rxfilters(struct vmxnet3_softc *);
static int 	vmxnet3_reinit(struct vmxnet3_softc *);
static void	vmxnet3_init_locked(struct vmxnet3_softc *);
static void	vmxnet3_init(void *);

static int	vmxnet3_encap_offload_ctx(struct mbuf *, int *, int *, int *);
static int	vmxnet3_encap_load_mbuf(struct vmxnet3_softc *,
		    struct vmxnet3_txring *, struct mbuf **, bus_dmamap_t,
		    bus_dma_segment_t [], int *);
static void	vmxnet3_encap_unload_mbuf(struct vmxnet3_softc *,
		    struct vmxnet3_txring *, bus_dmamap_t);
static int	vmxnet3_encap(struct vmxnet3_softc *, struct vmxnet3_txqueue *,
		    struct mbuf **);
static void	vmxnet3_start_locked(struct ifnet *);
static void	vmxnet3_start(struct ifnet *);

static void	vmxnet3_update_vlan_filter(struct vmxnet3_softc *, int,
		    uint16_t);
static void 	vmxnet3_register_vlan(void *, struct ifnet *, uint16_t);
static void 	vmxnet3_unregister_vlan(void *, struct ifnet *, uint16_t);
static void	vmxnet3_set_rxfilter(struct vmxnet3_softc *);
static int	vmxnet3_change_mtu(struct vmxnet3_softc *, int);
static int	vmxnet3_ioctl(struct ifnet *, u_long, caddr_t);

static void	vmxnet3_watchdog(struct vmxnet3_softc *);
static void	vmxnet3_tick(void *);
static void	vmxnet3_link_status(struct vmxnet3_softc *);
static void	vmxnet3_media_status(struct ifnet *, struct ifmediareq *);
static int	vmxnet3_media_change(struct ifnet *);
static void	vmxnet3_set_lladdr(struct vmxnet3_softc *);
static void	vmxnet3_get_lladdr(struct vmxnet3_softc *);

static uint32_t	vmxnet3_read_bar0(struct vmxnet3_softc *, bus_size_t);
static void	vmxnet3_write_bar0(struct vmxnet3_softc *, bus_size_t,
		    uint32_t);
static uint32_t	vmxnet3_read_bar1(struct vmxnet3_softc *, bus_size_t);
static void	vmxnet3_write_bar1(struct vmxnet3_softc *, bus_size_t,
		    uint32_t);
static void	vmxnet3_write_cmd(struct vmxnet3_softc *, uint32_t);
static uint32_t	vmxnet3_read_cmd(struct vmxnet3_softc *, uint32_t);

static void	vmxnet3_enable_intr(struct vmxnet3_softc *, int);
static void	vmxnet3_disable_intr(struct vmxnet3_softc *, int);
static void	vmxnet3_enable_all_intrs(struct vmxnet3_softc *);
static void	vmxnet3_disable_all_intrs(struct vmxnet3_softc *);

static int	vmxnet3_dma_malloc(struct vmxnet3_softc *, bus_size_t,
		    bus_size_t, struct vmxnet3_dma_alloc *);
static void	vmxnet3_dma_free(struct vmxnet3_softc *,
		    struct vmxnet3_dma_alloc *);

static device_method_t vmxnet3_methods[] = {
	/* Device interface. */
	DEVMETHOD(device_probe,		vmxnet3_probe),
	DEVMETHOD(device_attach,	vmxnet3_attach),
	DEVMETHOD(device_detach,	vmxnet3_detach),
	DEVMETHOD(device_shutdown,	vmxnet3_shutdown),

	DEVMETHOD_END
};

static driver_t vmxnet3_driver = {
	"vmx", vmxnet3_methods, sizeof(struct vmxnet3_softc)
};

static devclass_t vmxnet3_devclass;
DRIVER_MODULE(vmx, pci, vmxnet3_driver, vmxnet3_devclass, 0, 0);

MODULE_DEPEND(vmx, pci, 1, 1, 1);
MODULE_DEPEND(vmx, ether, 1, 1, 1);

#define VMXNET3_VMWARE_VENDOR_ID	0x15AD
#define VMXNET3_VMWARE_DEVICE_ID	0x07B0

static int
vmxnet3_probe(device_t dev)
{

	if (pci_get_vendor(dev) == VMXNET3_VMWARE_VENDOR_ID &&
	    pci_get_device(dev) == VMXNET3_VMWARE_DEVICE_ID) {
		device_set_desc(dev, "VMware VMXNET3 Ethernet Adapter");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
vmxnet3_attach(device_t dev)
{
	struct vmxnet3_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->vmx_dev = dev;

	VMXNET3_CORE_LOCK_INIT(sc, device_get_nameunit(dev));
	callout_init_mtx(&sc->vmx_tick, &sc->vmx_mtx, 0);

	vmxnet3_initial_config(sc);

	error = vmxnet3_alloc_resources(sc);
	if (error)
		goto fail;

	error = vmxnet3_check_version(sc);
	if (error)
		goto fail;

	error = vmxnet3_alloc_rxtx_queues(sc);
	if (error)
		goto fail;

	error = vmxnet3_alloc_data(sc);
	if (error)
		goto fail;

	error = vmxnet3_setup_interface(sc);
	if (error)
		goto fail;

	error = vmxnet3_setup_interrupts(sc);
	if (error) {
		ether_ifdetach(sc->vmx_ifp);
		device_printf(dev, "could not set up interrupt\n");
		goto fail;
	}

	vmxnet3_link_status(sc);

fail:
	if (error)
		vmxnet3_detach(dev);

	return (error);
}

static int
vmxnet3_detach(device_t dev)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;	

	sc = device_get_softc(dev);
	ifp = sc->vmx_ifp;

	if (device_is_attached(dev)) {
		ether_ifdetach(ifp);
		VMXNET3_CORE_LOCK(sc);
		vmxnet3_stop(sc);
		VMXNET3_CORE_UNLOCK(sc);
		callout_drain(&sc->vmx_tick);
	}

	if (sc->vmx_intrhand != NULL) {
		bus_teardown_intr(dev, sc->vmx_irq, sc->vmx_intrhand);
		sc->vmx_intrhand = NULL;
	}

	if (sc->vmx_vlan_attach != NULL) {
		EVENTHANDLER_DEREGISTER(vlan_config, sc->vmx_vlan_attach);
		sc->vmx_vlan_attach = NULL;
	}
	if (sc->vmx_vlan_detach != NULL) {
		EVENTHANDLER_DEREGISTER(vlan_config, sc->vmx_vlan_detach);
		sc->vmx_vlan_detach = NULL;
	}

	if (ifp != NULL) {
		if_free(ifp);
		sc->vmx_ifp = NULL;
	}

	ifmedia_removeall(&sc->vmx_media);

	if (sc->vmx_irq != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->vmx_irq);
		sc->vmx_irq = NULL;
	}

	vmxnet3_free_data(sc);
	vmxnet3_free_resources(sc);
	vmxnet3_free_rxtx_queues(sc);

	VMXNET3_CORE_LOCK_DESTROY(sc);

	return (0);
}

static int
vmxnet3_shutdown(device_t dev)
{

	return (0);
}

static int
vmxnet3_alloc_resources(struct vmxnet3_softc *sc)
{
	device_t dev;
	int rid;

	dev = sc->vmx_dev;

	rid = PCIR_BAR(0);
	sc->vmx_res0 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->vmx_res0 == NULL) {
		device_printf(dev,
		    "could not map BAR0 memory\n");
		return (ENXIO);
	}

	sc->vmx_iot0 = rman_get_bustag(sc->vmx_res0);
	sc->vmx_ioh0 = rman_get_bushandle(sc->vmx_res0);

	rid = PCIR_BAR(1);
	sc->vmx_res1 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->vmx_res1 == NULL) {
		device_printf(dev,
		    "could not map BAR1 memory\n");
		return (ENXIO);
	}

	sc->vmx_iot1 = rman_get_bustag(sc->vmx_res1);
	sc->vmx_ioh1 = rman_get_bushandle(sc->vmx_res1);

	/*
	 * XXX This really doesn't belong here, but is fine until
	 * we support MSI/MSIX.
	 */
	rid = 0;
	sc->vmx_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	   RF_SHAREABLE | RF_ACTIVE);
	if (sc->vmx_irq == NULL) {
		device_printf(dev, "could not allocate interrupt resource\n");
		return (ENXIO);
	}

	return (0);
}

static void
vmxnet3_free_resources(struct vmxnet3_softc *sc)
{
	device_t dev;
	int rid;

	dev = sc->vmx_dev;

	if (sc->vmx_res0 != NULL) {
		rid = PCIR_BAR(0);
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->vmx_res0);
		sc->vmx_res0 = NULL;
	}

	if (sc->vmx_res1 != NULL) {
		rid = PCIR_BAR(1);
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->vmx_res1);
		sc->vmx_res1 = NULL;
	}
}

static int
vmxnet3_check_version(struct vmxnet3_softc *sc)
{
	device_t dev;
	uint32_t version;

	dev = sc->vmx_dev;

	version = vmxnet3_read_bar1(sc, VMXNET3_BAR1_VRRS);
	if ((version & 0x01) == 0) {
		device_printf(dev, "unsupported hardware version %#x\n",
		    version);
		return (ENOTSUP);
	} else
		vmxnet3_write_bar1(sc, VMXNET3_BAR1_VRRS, 1);

	version = vmxnet3_read_bar1(sc, VMXNET3_BAR1_UVRS);
	if ((version & 0x01) == 0) {
		device_printf(dev, "unsupported UPT version %#x\n", version);
		return (ENOTSUP);
	} else 
		vmxnet3_write_bar1(sc, VMXNET3_BAR1_UVRS, 1);

	return (0);
}

static void
vmxnet3_initial_config(struct vmxnet3_softc *sc)
{

	sc->vmx_ntxqueues = 1;
	sc->vmx_nrxqueues = 1;
	sc->vmx_ntxdescs = VMXNET3_MAX_TX_NDESC;
	sc->vmx_nrxdescs = VMXNET3_MAX_RX_NDESC;
	sc->vmx_max_rxsegs = 1;
}

static int
vmxnet3_init_rxq(struct vmxnet3_softc *sc, int q)
{
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	int i;

	rxq = &sc->vmx_rxq[q];

	snprintf(rxq->vxrxq_name, sizeof(rxq->vxrxq_name), "%s-rx%d",
	    device_get_nameunit(sc->vmx_dev), q);
	mtx_init(&rxq->vxrxq_mtx, rxq->vxrxq_name, NULL, MTX_DEF);

	rxq->vxrxq_sc = sc;
	rxq->vxrxq_id = q;

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_rid = i;
		rxr->vxrxr_ndesc = sc->vmx_nrxdescs;
	}

	rxq->vxrxq_comp_ring.vxcr_ndesc =
	    sc->vmx_nrxdescs * VMXNET3_RXRINGS_PERQ;

	return (0);
}

static int
vmxnet3_init_txq(struct vmxnet3_softc *sc, int q)
{
	struct vmxnet3_txqueue *txq;

	txq = &sc->vmx_txq[q];

	snprintf(txq->vxtxq_name, sizeof(txq->vxtxq_name), "%s-tx%d",
	    device_get_nameunit(sc->vmx_dev), q);
	mtx_init(&txq->vxtxq_mtx, txq->vxtxq_name, NULL, MTX_DEF);

	txq->vxtxq_sc = sc;
	txq->vxtxq_id = q;

	txq->vxtxq_cmd_ring.vxtxr_ndesc = sc->vmx_ntxdescs;
	txq->vxtxq_comp_ring.vxcr_ndesc = sc->vmx_ntxdescs;

	return (0);
}

static int
vmxnet3_alloc_rxtx_queues(struct vmxnet3_softc *sc)
{
	int i, error;

	sc->vmx_rxq = malloc(sizeof(struct vmxnet3_rxqueue) *
	    sc->vmx_nrxqueues, M_DEVBUF, M_NOWAIT | M_ZERO);
	sc->vmx_txq = malloc(sizeof(struct vmxnet3_txqueue) *
	    sc->vmx_ntxqueues, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (sc->vmx_rxq == NULL || sc->vmx_txq == NULL)
		return (ENOMEM);

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		error = vmxnet3_init_rxq(sc, i);
		if (error)
			return (error);
	}

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		error = vmxnet3_init_txq(sc, i);
		if (error)
			return (error);
	}

	return (0);
}

static void
vmxnet3_destroy_rxq(struct vmxnet3_rxqueue *rxq)
{

	rxq->vxrxq_sc = NULL;
	rxq->vxrxq_id = -1;

	if (mtx_initialized(&rxq->vxrxq_mtx) != 0)
		mtx_destroy(&rxq->vxrxq_mtx);
}

static void
vmxnet3_destroy_txq(struct vmxnet3_txqueue *txq)
{

	txq->vxtxq_sc = NULL;
	txq->vxtxq_id = -1;

	if (mtx_initialized(&txq->vxtxq_mtx) != 0)
		mtx_destroy(&txq->vxtxq_mtx);

}

static void
vmxnet3_free_rxtx_queues(struct vmxnet3_softc *sc)
{
	int i;

	if (sc->vmx_rxq != NULL) {
		for (i = 0; i < sc->vmx_nrxqueues; i++)
			vmxnet3_destroy_rxq(&sc->vmx_rxq[i]);
		free(sc->vmx_rxq, M_DEVBUF);
		sc->vmx_rxq = NULL;
	}

	if (sc->vmx_txq != NULL) {
		for (i = 0; i < sc->vmx_ntxqueues; i++)
			vmxnet3_destroy_txq(&sc->vmx_txq[i]);
		free(sc->vmx_txq, M_DEVBUF);
		sc->vmx_txq = NULL;
	}
}

static int
vmxnet3_alloc_shared_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	uint8_t *kva;
	size_t size;
	int i, error;

	dev = sc->vmx_dev;

	size = sizeof(struct vmxnet3_driver_shared);
	error = vmxnet3_dma_malloc(sc, size, 1, &sc->vmx_ds_dma);
	if (error) {
		device_printf(dev, "cannot alloc shared memory\n");
		return (error);
	}
	sc->vmx_ds = (struct vmxnet3_driver_shared *) sc->vmx_ds_dma.dma_vaddr;

	size = sc->vmx_ntxqueues * sizeof(struct vmxnet3_txq_shared) +
	    sc->vmx_nrxqueues * sizeof(struct vmxnet3_rxq_shared);
	error = vmxnet3_dma_malloc(sc, size, 128, &sc->vmx_qs_dma);
	if (error) {
		device_printf(dev, "cannot alloc queue shared memory\n");
		return (error);
	}
	sc->vmx_qs = (void *) sc->vmx_qs_dma.dma_vaddr;
	kva = sc->vmx_qs;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		sc->vmx_txq[i].vxtxq_ts = (struct vmxnet3_txq_shared *) kva;
		kva += sizeof(struct vmxnet3_txq_shared);
	}
	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		sc->vmx_rxq[i].vxrxq_rs = (struct vmxnet3_rxq_shared *) kva;
		kva += sizeof(struct vmxnet3_rxq_shared);
	}

	return (0);
}

static void
vmxnet3_free_shared_data(struct vmxnet3_softc *sc)
{

	if (sc->vmx_qs != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_qs_dma);
		sc->vmx_qs = NULL;
	}

	if (sc->vmx_ds != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_ds_dma);
		sc->vmx_ds = NULL;
	}
}

static int
vmxnet3_alloc_txq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	size_t descsz, compsz;
	int i, q, error;

	dev = sc->vmx_dev;
	descsz = sc->vmx_ntxdescs * sizeof(struct vmxnet3_txdesc);
	compsz = sc->vmx_ntxdescs * sizeof(struct vmxnet3_txcompdesc);

	for (q = 0; q < sc->vmx_ntxqueues; q++) {
		txq = &sc->vmx_txq[q];
		txr = &txq->vxtxq_cmd_ring;
		txc = &txq->vxtxq_comp_ring;

		/*
		 * XXX BMV Need better way to determine the maximum
		 * size/segments/segsize arguments.
		 */
		error = bus_dma_tag_create(bus_get_dma_tag(dev),
		    1, 0,			/* alignment, boundary */
		    BUS_SPACE_MAXADDR,		/* lowaddr */
		    BUS_SPACE_MAXADDR,		/* highaddr */
		    NULL, NULL,			/* filter, filterarg */
		    VMXNET3_TSO_MAXSIZE,	/* maxsize */
		    VMXNET3_TX_MAXSEGS,		/* nsegments */
		    PAGE_SIZE,			/* maxsegsize */
		    0,				/* flags */
		    NULL, NULL,			/* lockfunc, lockarg */
		    &txr->vxtxr_txtag);
		if (error) {
			device_printf(dev,
			    "unable to create Tx buffer tag for queue %d\n", q);
			return (error);
		}

		error = vmxnet3_dma_malloc(sc, descsz, 512, &txr->vxtxr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Tx descriptors for "
			    "queue %d error %d\n", q, error);
			return (error);
		}
		txr->vxtxr_txd =
		    (struct vmxnet3_txdesc *) txr->vxtxr_dma.dma_vaddr;

		error = vmxnet3_dma_malloc(sc, compsz, 512, &txc->vxcr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Tx comp descriptors "
			   "for queue %d error %d\n", q, error);
			return (error);
		}
		txc->vxcr_u.txcd =
		    (struct vmxnet3_txcompdesc *) txc->vxcr_dma.dma_vaddr;

		for (i = 0; i < sc->vmx_ntxdescs; i++) {
			error = bus_dmamap_create(txr->vxtxr_txtag, 0,
			    &txr->vxtxr_dmap[i]);
			if (error) {
				device_printf(dev, "unable to create Tx buf "
				    "dmamap for queue %d idx %d\n", q, i);
				return (error);
			}
		}
	}

	return (0);
}

static void
vmxnet3_free_txq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	int i, q;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_ntxqueues; q++) {
		txq = &sc->vmx_txq[q];
		txr = &txq->vxtxq_cmd_ring;
		txc = &txq->vxtxq_comp_ring;

		for (i = 0; i < txr->vxtxr_ndesc; i++) {
			if (txr->vxtxr_dmap[i] != NULL) {
				bus_dmamap_destroy(txr->vxtxr_txtag,
				    txr->vxtxr_dmap[i]);
				txr->vxtxr_dmap[i] = NULL;
			}
		}

		if (txc->vxcr_u.txcd != NULL) {
			vmxnet3_dma_free(sc, &txc->vxcr_dma);
			txc->vxcr_u.txcd = NULL;
		}

		if (txr->vxtxr_txd != NULL) {
			vmxnet3_dma_free(sc, &txr->vxtxr_dma);
			txr->vxtxr_txd = NULL;
		}

		if (txr->vxtxr_txtag != NULL) {
			bus_dma_tag_destroy(txr->vxtxr_txtag);
			txr->vxtxr_txtag = NULL;
		}
	}
}

static int
vmxnet3_alloc_rxq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	int descsz, compsz;
	int i, j, q, error;

	dev = sc->vmx_dev;
	descsz = sc->vmx_nrxdescs * sizeof(struct vmxnet3_rxdesc);
	compsz = sc->vmx_nrxdescs * sizeof(struct vmxnet3_rxcompdesc) *
	    VMXNET3_RXRINGS_PERQ;

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		rxq = &sc->vmx_rxq[q];
		rxc = &rxq->vxrxq_comp_ring;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			error = bus_dma_tag_create(bus_get_dma_tag(dev),
			    1, 0,		/* alignment, boundary */
			    BUS_SPACE_MAXADDR,	/* lowaddr */
			    BUS_SPACE_MAXADDR,	/* highaddr */
			    NULL, NULL,		/* filter, filterarg */
			    MJUMPAGESIZE,	/* maxsize */
			    1,			/* nsegments */
			    MJUMPAGESIZE,	/* maxsegsize */
			    0,			/* flags */
			    NULL, NULL,		/* lockfunc, lockarg */
			    &rxr->vxrxr_rxtag);
			if (error) {
				device_printf(dev,
				    "unable to create Rx buffer tag for "
				    "queue %d\n", q);
				return (error);
			}

			error = vmxnet3_dma_malloc(sc, descsz, 512,
			    &rxr->vxrxr_dma);
			if (error) {
				device_printf(dev, "cannot allocate Rx "
				    "descriptors for queue %d/%d error %d\n",
				    i, q, error);
				return (error);
			}
			rxr->vxrxr_rxd =
			    (struct vmxnet3_rxdesc *) rxr->vxrxr_dma.dma_vaddr;
		}

		error = vmxnet3_dma_malloc(sc, compsz, 512, &rxc->vxcr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Rx comp descriptors "
			    "for queue %d error %d\n", q, error);
			return (error);
		}
		rxc->vxcr_u.rxcd =
		    (struct vmxnet3_rxcompdesc *) rxc->vxcr_dma.dma_vaddr;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			error = bus_dmamap_create(rxr->vxrxr_rxtag, 0,
			    &rxr->vxrxr_spare_dmap);
			if (error) {
				device_printf(dev, "unable to create spare "
				    "dmamap for queue %d/%d error %d\n",
				    q, i, error);
				return (error);
			}

			for (j = 0; j < sc->vmx_nrxdescs; j++) {
				error = bus_dmamap_create(rxr->vxrxr_rxtag, 0,
				    &rxr->vxrxr_dmap[j]);
				if (error) {
					device_printf(dev, "unable to create "
					    "dmamap for queue %d/%d slot %d "
					    "error %d\n",
					    q, i, j, error);
					return (error);
				}
			}
		}
	}

	return (0);
}

static void
vmxnet3_free_rxq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	int i, j, q;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		rxq = &sc->vmx_rxq[q];
		rxc = &rxq->vxrxq_comp_ring;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			if (rxr->vxrxr_spare_dmap != NULL) {
				bus_dmamap_destroy(rxr->vxrxr_rxtag,
				    rxr->vxrxr_spare_dmap);
				rxr->vxrxr_spare_dmap = NULL;
			}

			for (j = 0; j < rxr->vxrxr_ndesc; j++) {
				if (rxr->vxrxr_dmap[j] != NULL) {
					bus_dmamap_destroy(rxr->vxrxr_rxtag,
					    rxr->vxrxr_dmap[j]);
					rxr->vxrxr_dmap[j] = NULL;
				}
			}
		}

		if (rxc->vxcr_u.rxcd != NULL) {
			vmxnet3_dma_free(sc, &rxc->vxcr_dma);
			rxc->vxcr_u.rxcd = NULL;
		}

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			if (rxr->vxrxr_rxd != NULL) {
				vmxnet3_dma_free(sc, &rxr->vxrxr_dma);
				rxr->vxrxr_rxd = NULL;
			}

			if (rxr->vxrxr_rxtag != NULL) {
				bus_dma_tag_destroy(rxr->vxrxr_rxtag);
				rxr->vxrxr_rxtag = NULL;
			}
		}
	}
}

static int
vmxnet3_alloc_queue_data(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_alloc_txq_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_rxq_data(sc);
	if (error)
		return (error);

	return (0);
}

static void
vmxnet3_free_queue_data(struct vmxnet3_softc *sc)
{

	vmxnet3_free_rxq_data(sc);
	vmxnet3_free_txq_data(sc);
}

static int
vmxnet3_alloc_mcast_table(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_dma_malloc(sc, VMXNET3_MULTICAST_MAX * ETHER_ADDR_LEN,
	    32, &sc->vmx_mcast_dma);
	if (error)
		device_printf(sc->vmx_dev, "unable to alloc multicast table\n");
	else
		sc->vmx_mcast = sc->vmx_mcast_dma.dma_vaddr;

	return (error);
}

static void
vmxnet3_free_mcast_table(struct vmxnet3_softc *sc)
{

	if (sc->vmx_mcast != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_mcast_dma);
		sc->vmx_mcast = NULL;
	}
}

static void
vmxnet3_init_shared_data(struct vmxnet3_softc *sc)
{
	struct vmxnet3_driver_shared *ds;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txq_shared *txs;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxq_shared *rxs;
	int i;

	ds = sc->vmx_ds;

	/*
	 * Initialize fields of the shared data that remains the same across
	 * reinits. Note the shared data is zero'd when allocated.
	 */

	ds->magic = VMXNET3_REV1_MAGIC;

	/* DriverInfo */
	ds->version = VMXNET3_DRIVER_VERSION;
	ds->guest = VMXNET3_GOS_FREEBSD | VMXNET3_GUEST_OS_VERSION |
#ifdef __LP64__
	    VMXNET3_GOS_64BIT;
#else
	    VMXNET3_GOS_32BIT;
#endif
	ds->vmxnet3_revision = 1;
	ds->upt_version = 1;

	/* Misc. conf */
	ds->driver_data = vtophys(sc);
	ds->driver_data_len = sizeof(struct vmxnet3_softc);
	ds->queue_shared = sc->vmx_qs_dma.dma_paddr;
	ds->queue_shared_len = sc->vmx_qs_dma.dma_size;
	ds->nrxsg_max = sc->vmx_max_rxsegs;

	/* Interrupt control. */
	ds->automask = 1;
	ds->nintr = VMXNET3_NINTR;
	ds->evintr = 0;
	ds->ictrl = VMXNET3_ICTRL_DISABLE_ALL;

	for (i = 0; i < VMXNET3_NINTR; i++)
		ds->modlevel[i] = UPT1_IMOD_ADAPTIVE;

	/* Receive filter. */
	ds->mcast_table = sc->vmx_mcast_dma.dma_paddr;
	ds->mcast_tablelen = sc->vmx_mcast_dma.dma_size;

	/* Tx queues */
	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_txq[i];
		txs = txq->vxtxq_ts;

		txs->cmd_ring = txq->vxtxq_cmd_ring.vxtxr_dma.dma_paddr;
		txs->cmd_ring_len = sc->vmx_ntxdescs;
		txs->comp_ring = txq->vxtxq_comp_ring.vxcr_dma.dma_paddr;
		txs->comp_ring_len = sc->vmx_ntxdescs;
		txs->driver_data = vtophys(txq);
		txs->driver_data_len = sizeof(struct vmxnet3_txqueue);
	}

	/* Rx queues */
	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		rxq = &sc->vmx_rxq[i];
		rxs = rxq->vxrxq_rs;

		rxs->cmd_ring[0] = rxq->vxrxq_cmd_ring[0].vxrxr_dma.dma_paddr;
		rxs->cmd_ring_len[0] = sc->vmx_nrxdescs;
		rxs->cmd_ring[1] = rxq->vxrxq_cmd_ring[1].vxrxr_dma.dma_paddr;
		rxs->cmd_ring_len[1] = sc->vmx_nrxdescs;
		rxs->comp_ring = rxq->vxrxq_comp_ring.vxcr_dma.dma_paddr;
		rxs->comp_ring_len = sc->vmx_nrxdescs * VMXNET3_RXRINGS_PERQ;
		rxs->driver_data = vtophys(rxq);
		rxs->driver_data_len = sizeof(struct vmxnet3_rxqueue);
	}
}

static void
vmxnet3_reinit_shared_data(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	struct vmxnet3_driver_shared *ds;

	ifp = sc->vmx_ifp;
	ds = sc->vmx_ds;

	/* Use the current MAC address. */
	bcopy(IF_LLADDR(sc->vmx_ifp), sc->vmx_lladdr, ETHER_ADDR_LEN);
	vmxnet3_set_lladdr(sc);

	ds->upt_features = 0;
	if (ifp->if_capenable & IFCAP_VLAN_HWTAGGING)
		ds->upt_features |= UPT1_F_VLAN;
	if (ifp->if_capenable & IFCAP_RXCSUM)
		ds->upt_features |= UPT1_F_CSUM;

	ds->mtu = ifp->if_mtu;
	ds->ntxqueue = sc->vmx_ntxqueues;
	ds->nrxqueue = sc->vmx_nrxqueues;

	vmxnet3_write_bar1(sc, VMXNET3_BAR1_DSL, sc->vmx_ds_dma.dma_paddr);
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_DSH,
	    sc->vmx_ds_dma.dma_paddr >> 32);
}

static int
vmxnet3_alloc_data(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_alloc_shared_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_queue_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_mcast_table(sc);
	if (error)
		return (error);

	vmxnet3_init_shared_data(sc);

	return (0);
}

static void
vmxnet3_free_data(struct vmxnet3_softc *sc)
{

	vmxnet3_free_mcast_table(sc);
	vmxnet3_free_queue_data(sc);
	vmxnet3_free_shared_data(sc);
}

static int
vmxnet3_setup_interface(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct ifnet *ifp;

	dev = sc->vmx_dev;

	ifp = sc->vmx_ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		device_printf(dev, "cannot allocate ifnet structure\n");
		return (ENOSPC);
	}

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	if_initbaudrate(ifp, IF_Gbps(10)); /* Approx. */
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = vmxnet3_init;
	ifp->if_ioctl = vmxnet3_ioctl;
	ifp->if_start = vmxnet3_start;
	ifp->if_snd.ifq_drv_maxlen = sc->vmx_ntxdescs - 1;
	IFQ_SET_MAXLEN(&ifp->if_snd, sc->vmx_ntxdescs - 1);
	IFQ_SET_READY(&ifp->if_snd);

	vmxnet3_get_lladdr(sc);
	ether_ifattach(ifp, sc->vmx_lladdr);

	ifp->if_capabilities |= IFCAP_VLAN_MTU | IFCAP_VLAN_HWTAGGING;
	ifp->if_capabilities |= IFCAP_RXCSUM | IFCAP_TXCSUM;
	ifp->if_hwassist |= VMXNET3_CSUM_FEATURES;

	ifp->if_capenable = ifp->if_capabilities;
	
	/*
	 * Capabilities after here are not enabled by default.
	 */

	ifp->if_capabilities |= IFCAP_VLAN_HWFILTER;
	sc->vmx_vlan_attach = EVENTHANDLER_REGISTER(vlan_config,
	    vmxnet3_register_vlan, sc, EVENTHANDLER_PRI_FIRST);
	sc->vmx_vlan_detach = EVENTHANDLER_REGISTER(vlan_config,
	    vmxnet3_unregister_vlan, sc, EVENTHANDLER_PRI_FIRST);

	ifmedia_init(&sc->vmx_media, 0, vmxnet3_media_change,
	    vmxnet3_media_status);
	ifmedia_add(&sc->vmx_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->vmx_media, IFM_ETHER | IFM_AUTO);

	return (0);
}

static void
vmxnet3_evintr(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct ifnet *ifp;
	struct vmxnet3_txq_shared *ts;
	struct vmxnet3_rxq_shared *rs;
	uint32_t event;
	int reset;

	dev = sc->vmx_dev;
	ifp = sc->vmx_ifp;
	event = sc->vmx_ds->event;
	reset = 0;

	/* Clear events. */
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_EVENT, event);

	VMXNET3_CORE_LOCK(sc);

	if (event & VMXNET3_EVENT_LINK)
		vmxnet3_link_status(sc);

	if (event & (VMXNET3_EVENT_TQERROR | VMXNET3_EVENT_RQERROR)) {
		reset = 1;
		vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_STATUS);
		ts = sc->vmx_txq[0].vxtxq_ts;
		if (ts->stopped != 0)
			device_printf(dev, "Tx queue error %#x\n", ts->error);
		rs = sc->vmx_rxq[0].vxrxq_rs;
		if (rs->stopped != 0)
			device_printf(dev, "Rx queue error %#x\n", rs->error);
		device_printf(dev, "Rx/Tx queue error event ... resetting\n");
	}

	if (event & VMXNET3_EVENT_DIC)
		device_printf(dev, "device implementation change event\n");
	if (event & VMXNET3_EVENT_DEBUG)
		device_printf(dev, "debug event\n");

	if (reset != 0) {
		ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
		vmxnet3_init_locked(sc);
	}

	VMXNET3_CORE_UNLOCK(sc);
}

static void
vmxnet3_txeof(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq)
{
	struct ifnet *ifp;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	struct vmxnet3_txcompdesc *txcd;
	u_int sop;

	ifp = sc->vmx_ifp;
	txr = &txq->vxtxq_cmd_ring;
	txc = &txq->vxtxq_comp_ring;

	VMXNET3_TXQ_LOCK_ASSERT(txq);

	for (;;) {
		txcd = &txc->vxcr_u.txcd[txc->vxcr_next];
		if (txcd->gen != txc->vxcr_gen)
			break;

		if (++txc->vxcr_next == txc->vxcr_ndesc) {
			txc->vxcr_next = 0;
			txc->vxcr_gen ^= 1;
		}

		sop = txr->vxtxr_next;
		if (txr->vxtxr_m[sop] != NULL) {
			bus_dmamap_sync(txr->vxtxr_txtag, txr->vxtxr_dmap[sop],
				BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(txr->vxtxr_txtag,
			    txr->vxtxr_dmap[sop]);

			m_freem(txr->vxtxr_m[sop]);
			txr->vxtxr_m[sop] = NULL;

			ifp->if_opackets++;
		}

		txr->vxtxr_next = (txcd->eop_idx + 1) % txr->vxtxr_ndesc;
	}

	if (txr->vxtxr_head == txr->vxtxr_next)
		sc->vmx_watchdog_timer = 0;
}

static void
vmxnet3_rx_csum(struct vmxnet3_rxcompdesc *rxcd, struct mbuf *m)
{

	if (rxcd->ipv4 && rxcd->ipcsum_ok)
		m->m_pkthdr.csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;

	if (!rxcd->fragment) {
		if (rxcd->csum_ok && (rxcd->tcp || rxcd->udp)) {
			m->m_pkthdr.csum_flags |= CSUM_DATA_VALID |
			    CSUM_PSEUDO_HDR;
			m->m_pkthdr.csum_data = 0xFFFF;
		}
	}
}

static int
vmxnet3_newbuf(struct vmxnet3_softc *sc, struct vmxnet3_rxring *rxr)
{
	struct ifnet *ifp;
	struct mbuf *m;
	struct vmxnet3_rxdesc *rxd;
	bus_dma_tag_t tag;
	bus_dmamap_t dmap;
	bus_dma_segment_t segs[1];
	int nsegs, idx, btype, error;

	ifp = sc->vmx_ifp;
	tag = rxr->vxrxr_rxtag;
	dmap = rxr->vxrxr_spare_dmap;
	idx = rxr->vxrxr_fill;
	rxd = &rxr->vxrxr_rxd[idx];
	btype = rxr->vxrxr_rid == 0 ? VMXNET3_BTYPE_HEAD : VMXNET3_BTYPE_BODY;

	/* XXX Do not populate ring 2 yet. */
	if (rxr->vxrxr_rid != 0)
		return (0);

	m = m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR, MCLBYTES);
	if (m == NULL)
		return (ENOBUFS);
	m->m_pkthdr.len = m->m_len = MCLBYTES;
	if (btype == VMXNET3_BTYPE_BODY)
		m_adj(m, ETHER_ALIGN);

	error = bus_dmamap_load_mbuf_sg(tag, dmap, m, &segs[0], &nsegs,
	    BUS_DMA_NOWAIT);
	if (error) {
		m_freem(m);
		return (error);
	}
	KASSERT(nsegs == 1,
	    ("%s: mbuf %p with too many segments %d", __func__, m, nsegs));

	if (rxr->vxrxr_m[idx] != NULL) {
		bus_dmamap_sync(tag, rxr->vxrxr_dmap[idx], BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(tag, rxr->vxrxr_dmap[idx]);
	}

	rxr->vxrxr_spare_dmap = rxr->vxrxr_dmap[idx];
	rxr->vxrxr_dmap[idx] = dmap;
	rxr->vxrxr_m[idx] = m;

	rxd->addr = segs[0].ds_addr;
	rxd->len = segs[0].ds_len;
	rxd->btype = btype;
	rxd->gen = rxr->vxrxr_gen;

	if (++idx == rxr->vxrxr_ndesc) {
		idx = 0;
		rxr->vxrxr_gen ^= 1;
	}
	rxr->vxrxr_fill = idx;

	return (0);
}

static void
vmxnet3_rxeof_discard(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq,
   struct vmxnet3_rxring *rxr, int idx)
{
	struct vmxnet3_rxdesc *rxd;

	rxd = &rxr->vxrxr_rxd[idx];

	rxd->gen = rxr->vxrxr_gen;
}

static void
vmxnet3_rxeof(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq)
{
	struct ifnet *ifp;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	struct vmxnet3_rxdesc *rxd;
	struct vmxnet3_rxcompdesc *rxcd;
	struct mbuf *m;
	int idx, length;

	ifp = sc->vmx_ifp;
	rxr = &rxq->vxrxq_cmd_ring[0];
	rxc = &rxq->vxrxq_comp_ring;

	VMXNET3_RXQ_LOCK_ASSERT(rxq);

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
		return;

	for (;;) {
		rxcd = &rxc->vxcr_u.rxcd[rxc->vxcr_next];
		if (rxcd->gen != rxc->vxcr_gen)
			break;

		if (++rxc->vxcr_next == rxc->vxcr_ndesc) {
			rxc->vxcr_next = 0;
			rxc->vxcr_gen ^= 1;
		}

		idx = rxcd->rxd_idx;
		length = rxcd->len;
		if (rxcd->qid < sc->vmx_nrxqueues)
			rxr = &rxq->vxrxq_cmd_ring[0];
		else
			rxr = &rxq->vxrxq_cmd_ring[1];
		rxd = &rxr->vxrxr_rxd[idx];

		m = rxr->vxrxr_m[idx];
		KASSERT(m != NULL, ("%s: queue %d idx %d without mbuf",
		    __func__, rxcd->qid, idx));

		if (rxd->btype != VMXNET3_BTYPE_HEAD) {
			vmxnet3_rxeof_discard(sc, rxq, rxr, idx);
			ifp->if_iqdrops++;
			goto nextp;
		} else if (rxcd->error) {
			vmxnet3_rxeof_discard(sc, rxq, rxr, idx);
			ifp->if_ierrors++;
			goto nextp;
		} else if (vmxnet3_newbuf(sc, rxr) != 0) {
			vmxnet3_rxeof_discard(sc, rxq, rxr, idx);
			ifp->if_iqdrops++;
			goto nextp;
		}

		m->m_pkthdr.rcvif = ifp;
		m->m_pkthdr.len = m->m_len = length;
		m->m_pkthdr.csum_flags = 0;

		if (ifp->if_capenable & IFCAP_RXCSUM && !rxcd->no_csum)
			vmxnet3_rx_csum(rxcd, m);

		if (rxcd->vlan) {
			m->m_flags |= M_VLANTAG;
			m->m_pkthdr.ether_vtag = rxcd->vtag;
		}

		ifp->if_ipackets++;
		VMXNET3_RXQ_UNLOCK(rxq);
		(*ifp->if_input)(ifp, m);
		VMXNET3_RXQ_LOCK(rxq);

		/* Must recheck the state after dropping the Rx lock. */
		if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
			break;

nextp:
		if (rxq->vxrxq_rs->update_rxhead) {
			int qid = rxcd->qid;
			bus_size_t r;

			/*
			 * XXX BMV This looks pretty odd.
			 */
			idx = (idx + 1) % rxr->vxrxr_ndesc;
			if (qid >= sc->vmx_nrxqueues) {
				qid -= sc->vmx_nrxqueues;
				r = VMXNET3_BAR0_RXH2(qid);
			} else
				r = VMXNET3_BAR0_RXH1(qid);
			vmxnet3_write_bar0(sc, r, idx);
		}
	}
}

static void
vmxnet3_legacy_intr(void *xsc)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_txqueue *txq;

	sc = xsc;
	rxq = &sc->vmx_rxq[0];
	txq = &sc->vmx_txq[0];
	ifp = sc->vmx_ifp;

	if (vmxnet3_read_bar1(sc, VMXNET3_BAR1_INTR) == 0)
		return;

	if (sc->vmx_ds->event != 0)
		vmxnet3_evintr(sc);

	VMXNET3_RXQ_LOCK(rxq);
	vmxnet3_rxeof(sc, rxq);
	VMXNET3_RXQ_UNLOCK(rxq);

	VMXNET3_TXQ_LOCK(txq);
	vmxnet3_txeof(sc, txq);
	if (!IFQ_DRV_IS_EMPTY(&ifp->if_snd))
		vmxnet3_start_locked(ifp);
	VMXNET3_TXQ_UNLOCK(txq);

	vmxnet3_enable_intr(sc, 0);
}

static int
vmxnet3_setup_interrupts(struct vmxnet3_softc *sc)
{
	device_t dev;
	int error;

	dev = sc->vmx_dev;

	/*
	 * Only support a single legacy interrupt for now.
	 * Add MSI/MSIx later.
	 */
	error = bus_setup_intr(dev, sc->vmx_irq, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, vmxnet3_legacy_intr, sc, &sc->vmx_intrhand);

	return (error);
}

static void
vmxnet3_txstop(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;
	int i;

	txr = &txq->vxtxq_cmd_ring;

	for (i = 0; i < txr->vxtxr_ndesc; i++) {
		if (txr->vxtxr_m[i] == NULL)
			continue;

		bus_dmamap_sync(txr->vxtxr_txtag, txr->vxtxr_dmap[i],
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(txr->vxtxr_txtag, txr->vxtxr_dmap[i]);
		m_freem(txr->vxtxr_m[i]);
		txr->vxtxr_m[i] = NULL;
	}
}

static void
vmxnet3_rxstop(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_rxring *rxr;
	int i, j;

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];

		for (j = 0; j < rxr->vxrxr_ndesc; j++) {
			if (rxr->vxrxr_m[j] == NULL)
				continue;
			bus_dmamap_sync(rxr->vxrxr_rxtag, rxr->vxrxr_dmap[j],
			    BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(rxr->vxrxr_rxtag, rxr->vxrxr_dmap[j]);
			m_freem(rxr->vxrxr_m[j]);
			rxr->vxrxr_m[j] = NULL;
		}
	}
}

static void
vmxnet3_stop_rendezvous(struct vmxnet3_softc *sc)
{
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_txqueue *txq;
	int i;

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		rxq = &sc->vmx_rxq[i];
		VMXNET3_RXQ_LOCK(rxq);
		VMXNET3_RXQ_UNLOCK(rxq);
	}

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_txq[i];
		VMXNET3_TXQ_LOCK(txq);
		VMXNET3_TXQ_UNLOCK(txq);
	}
}

static void
vmxnet3_stop(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	int q;

	ifp = sc->vmx_ifp;
	VMXNET3_CORE_LOCK_ASSERT(sc);

	ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
	sc->vmx_link_active = 0;
	callout_stop(&sc->vmx_tick);

	/* Disable interrupts. */
	vmxnet3_disable_all_intrs(sc);
	vmxnet3_write_cmd(sc, VMXNET3_CMD_DISABLE);

	vmxnet3_stop_rendezvous(sc);

	for (q = 0; q < sc->vmx_ntxqueues; q++)
		vmxnet3_txstop(sc, &sc->vmx_txq[q]);
	for (q = 0; q < sc->vmx_nrxqueues; q++)
		vmxnet3_rxstop(sc, &sc->vmx_rxq[q]);

	vmxnet3_write_cmd(sc, VMXNET3_CMD_RESET);
}

static void
vmxnet3_txinit(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;

	txr = &txq->vxtxq_cmd_ring;
	txr->vxtxr_head = 0;
	txr->vxtxr_next = 0;
	txr->vxtxr_gen = VMXNET3_INIT_GEN;
	bzero(txr->vxtxr_txd,
	    txr->vxtxr_ndesc * sizeof(struct vmxnet3_txdesc));

	txc = &txq->vxtxq_comp_ring;
	txc->vxcr_next = 0;
	txc->vxcr_gen = VMXNET3_INIT_GEN;
	bzero(txc->vxcr_u.txcd,
	    txc->vxcr_ndesc * sizeof(struct vmxnet3_txcompdesc));
}

static int
vmxnet3_rxinit(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	int i, idx, error;

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_fill = 0;
		rxr->vxrxr_gen = VMXNET3_INIT_GEN;
		bzero(rxr->vxrxr_rxd,
		    rxr->vxrxr_ndesc * sizeof(struct vmxnet3_rxdesc));

		for (idx = 0; idx < rxr->vxrxr_ndesc; idx++) {
			error = vmxnet3_newbuf(sc, rxr);
			if (error)
				return (error);
		}
	}

	rxc = &rxq->vxrxq_comp_ring;
	rxc->vxcr_next = 0;
	rxc->vxcr_gen = VMXNET3_INIT_GEN;
	bzero(rxc->vxcr_u.rxcd,
	     rxc->vxcr_ndesc * sizeof(struct vmxnet3_rxcompdesc));

	return (0);
}

static int
vmxnet3_reinit_queues(struct vmxnet3_softc *sc)
{
	device_t dev;
	int q, error;
	
	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_ntxqueues; q++)
		vmxnet3_txinit(sc, &sc->vmx_txq[q]);

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		error = vmxnet3_rxinit(sc, &sc->vmx_rxq[q]);
		if (error) {
			device_printf(dev, "cannot populate Rx queue %d\n", q);
			return (error);
		}
	}

	return (0);
}

static int
vmxnet3_enable_device(struct vmxnet3_softc *sc)
{
	int q;

	if (vmxnet3_read_cmd(sc, VMXNET3_CMD_ENABLE) != 0) {
		device_printf(sc->vmx_dev, "device enable command failed!\n");
		return (1);
	}

	/* Reset the Rx queue heads. */
	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_RXH1(q), 0);
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_RXH2(q), 0);
	}

	return (0);
}

static void
vmxnet3_reinit_rxfilters(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;

	ifp = sc->vmx_ifp;

	vmxnet3_set_rxfilter(sc);

	if (ifp->if_capenable & IFCAP_VLAN_HWFILTER)
		bcopy(sc->vmx_vlan_filter, sc->vmx_ds->vlan_filter,
		    sizeof(sc->vmx_ds->vlan_filter));
	else
		bzero(sc->vmx_ds->vlan_filter,
		    sizeof(sc->vmx_ds->vlan_filter));
	vmxnet3_write_cmd(sc, VMXNET3_CMD_VLAN_FILTER);
}

static int
vmxnet3_reinit(struct vmxnet3_softc *sc)
{

	vmxnet3_reinit_shared_data(sc);

	if (vmxnet3_reinit_queues(sc) != 0)
		return (ENXIO);

	if (vmxnet3_enable_device(sc) != 0)
		return (ENXIO);

	vmxnet3_reinit_rxfilters(sc);

	return (0);
}

static void
vmxnet3_init_locked(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;

	ifp = sc->vmx_ifp;

	if (ifp->if_drv_flags & IFF_DRV_RUNNING)
		return;

	vmxnet3_stop(sc);

	if (vmxnet3_reinit(sc) != 0) {
		vmxnet3_stop(sc);
		return;
	}

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	vmxnet3_link_status(sc);

	vmxnet3_enable_all_intrs(sc);
	callout_reset(&sc->vmx_tick, hz, vmxnet3_tick, sc);
}

static void
vmxnet3_init(void *xsc)
{
	struct vmxnet3_softc *sc;

	sc = xsc;

	VMXNET3_CORE_LOCK(sc);
	vmxnet3_init_locked(sc);
	VMXNET3_CORE_UNLOCK(sc);
}

/*
 * BMV: Much of this can go away once we finally have offsets in
 * the mbuf packet header. Bug andre@.
 */
static int
vmxnet3_encap_offload_ctx(struct mbuf *m, int *etype, int *proto, int *start)
{
	struct ether_vlan_header *evh;
	int offset;

	evh = mtod(m, struct ether_vlan_header *);
	if (evh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		/* BMV: We should handle nested VLAN tags too. */
		*etype = ntohs(evh->evl_proto);
		offset = sizeof(struct ether_vlan_header);
	} else {
		*etype = ntohs(evh->evl_encap_proto);
		offset = sizeof(struct ether_header);
	}

	switch (*etype) {
#if defined(INET)
	case ETHERTYPE_IP: {
		struct ip *ip, iphdr;
		if (__predict_false(m->m_len < offset + sizeof(struct ip))) {
			m_copydata(m, offset, sizeof(struct ip),
			    (caddr_t) &iphdr);
			ip = &iphdr;
		} else
			ip = (struct ip *)(m->m_data + offset);
		*proto = ip->ip_p;
		*start = offset + (ip->ip_hl << 2);
		break;
	}
#endif
#if defined(INET6)
	case ETHERTYPE_IPV6:
		*proto = -1;
		*start = ip6_lasthdr(m, offset, IPPROTO_IPV6, proto);
		/* Assert the network stack sent us a valid packet. */
		KASSERT(*start > offset,
		    ("%s: mbuf %p start %d offset %d proto %d", __func__, m,
		    *start, offset, *proto));
		break;
#endif
	default:
		return (EINVAL);
	}

	return (0);
}

static int
vmxnet3_encap_load_mbuf(struct vmxnet3_softc *sc, struct vmxnet3_txring *txr,
    struct mbuf **m0, bus_dmamap_t dmap, bus_dma_segment_t segs[], int *nsegs)
{
	struct mbuf *m;
	bus_dma_tag_t tag;
	int maxsegs, error;

	m = *m0;
	tag = txr->vxtxr_txtag;
	maxsegs = VMXNET3_TX_MAXSEGS;

	error = bus_dmamap_load_mbuf_sg(tag, dmap, m, segs, nsegs, 0);
	if (error == 0 || error != EFBIG)
		return (error);

	m = m_collapse(m, M_NOWAIT, maxsegs);
	if (m != NULL) {
		*m0 = m;
		error = bus_dmamap_load_mbuf_sg(tag, dmap, m, segs, nsegs, 0);
	} else
		error = ENOBUFS;

	if (error) {
		m_freem(*m0);
		*m0 = NULL;
	}

	return (error);
}

static void
vmxnet3_encap_unload_mbuf(struct vmxnet3_softc *sc, struct vmxnet3_txring *txr,
    bus_dmamap_t dmap)
{

	bus_dmamap_unload(txr->vxtxr_txtag, dmap);
}

static int
vmxnet3_encap(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq,
    struct mbuf **m0)
{
	struct ifnet *ifp;
	struct vmxnet3_txring *txr;
	struct vmxnet3_txdesc *txd, *sop;
	struct mbuf *m;
	bus_dmamap_t dmap;
	bus_dma_segment_t segs[VMXNET3_TX_MAXSEGS];
	int i, gen, nsegs, etype, proto, start, error;

	ifp = sc->vmx_ifp;
	txr = &txq->vxtxq_cmd_ring;
	dmap = txr->vxtxr_dmap[txr->vxtxr_head];

	error = vmxnet3_encap_load_mbuf(sc, txr, m0, dmap, segs, &nsegs);
	if (error)
		return (error);

	m = *m0;
	M_ASSERTPKTHDR(m);
	KASSERT(nsegs <= VMXNET3_TX_MAXSEGS,
	    ("%s: mbuf %p with too many segments %d", __func__, m, nsegs));

	if (VMXNET3_TXRING_AVAIL(txr) < nsegs) {
		vmxnet3_encap_unload_mbuf(sc, txr, dmap);
		return (ENOSPC);
	} else if (m->m_pkthdr.csum_flags & VMXNET3_CSUM_FEATURES) {
		error = vmxnet3_encap_offload_ctx(m, &etype, &proto, &start);
		if (error) {
			vmxnet3_encap_unload_mbuf(sc, txr, dmap);
			m_freem(m);
			*m0 = NULL;
			return (error);
		}
	}

	txr->vxtxr_m[txr->vxtxr_head] = m = *m0;
	sop = &txr->vxtxr_txd[txr->vxtxr_head];
	gen = txr->vxtxr_gen ^ 1;	/* Owned by cpu (yet) */

	for (i = 0; i < nsegs; i++) {
		txd = &txr->vxtxr_txd[txr->vxtxr_head];

		txd->addr = segs[i].ds_addr;
		txd->len = segs[i].ds_len;
		txd->gen = gen;
		txd->dtype = 0;
		txd->offload_mode = VMXNET3_OM_NONE;
		txd->offload_pos = 0;
		txd->hlen = 0;
		txd->eop = 0;
		txd->compreq = 0;
		txd->vtag_mode = 0;
		txd->vtag = 0;

		if (++txr->vxtxr_head == txr->vxtxr_ndesc) {
			txr->vxtxr_head = 0;
			txr->vxtxr_gen ^= 1;
		}
		gen = txr->vxtxr_gen;
	}
	txd->eop = 1;
	txd->compreq = 1;

	if (m->m_flags & M_VLANTAG) {
		sop->vtag_mode = 1;
		sop->vtag = m->m_pkthdr.ether_vtag;
	}

	if (m->m_pkthdr.csum_flags & VMXNET3_CSUM_FEATURES) {
		sop->offload_mode = VMXNET3_OM_CSUM;
		sop->hlen = start;
		sop->offload_pos = start + m->m_pkthdr.csum_data;
	}

	/* Finally, change the ownership. */
	sop->gen ^= 1;

	return (0);
}

static void
vmxnet3_start_locked(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct mbuf *m_head;
	int tx;

	sc = ifp->if_softc;
	txq = &sc->vmx_txq[0];
	txr = &txq->vxtxq_cmd_ring;
	tx = 0;

	VMXNET3_TXQ_LOCK_ASSERT(txq);

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0 ||
	    sc->vmx_link_active == 0)
		return;

	while (VMXNET3_TXRING_AVAIL(txr) > 0) {
		IFQ_DRV_DEQUEUE(&ifp->if_snd, m_head);
		if (m_head == NULL)
			break;

		if (vmxnet3_encap(sc, txq, &m_head) != 0) {
			if (m_head == NULL)
				break;
			IFQ_DRV_PREPEND(&ifp->if_snd, m_head);
			break;
		}

		tx++;
		ETHER_BPF_MTAP(ifp, m_head);
	}

	if (tx > 0) {
		/* bus_dmamap_sync() ? */
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_TXH(0), txr->vxtxr_head);
		sc->vmx_watchdog_timer = VMXNET3_WATCHDOG_TIMEOUT;
	}
}

static void
vmxnet3_start(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;

	sc = ifp->if_softc;
	txq = &sc->vmx_txq[0];

	VMXNET3_TXQ_LOCK(txq);
	vmxnet3_start_locked(ifp);
	VMXNET3_TXQ_UNLOCK(txq);
}

static void
vmxnet3_update_vlan_filter(struct vmxnet3_softc *sc, int add, uint16_t tag)
{
	struct ifnet *ifp;
	int idx, bit;

	ifp = sc->vmx_ifp;
	idx = (tag >> 5) & 0x7F;
	bit = tag & 0x1F;

	if (tag == 0 || tag > 4095)
		return;

	VMXNET3_CORE_LOCK(sc);

	/* Update our private VLAN bitvector. */
	if (add)
		sc->vmx_vlan_filter[idx] |= (1 << bit);
	else
		sc->vmx_vlan_filter[idx] &= ~(1 << bit);

	if (ifp->if_capenable & IFCAP_VLAN_HWFILTER) {
		if (add)
			sc->vmx_ds->vlan_filter[idx] |= (1 << bit);
		else
			sc->vmx_ds->vlan_filter[idx] &= ~(1 << bit);
		vmxnet3_write_cmd(sc, VMXNET3_CMD_VLAN_FILTER);
	}

	VMXNET3_CORE_UNLOCK(sc);
}

static void
vmxnet3_register_vlan(void *arg, struct ifnet *ifp, uint16_t tag)
{

	if (ifp->if_softc == arg)
		vmxnet3_update_vlan_filter(arg, 1, tag);
}

static void
vmxnet3_unregister_vlan(void *arg, struct ifnet *ifp, uint16_t tag)
{

	if (ifp->if_softc == arg)
		vmxnet3_update_vlan_filter(arg, 0, tag);
}

static void
vmxnet3_set_rxfilter(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	struct vmxnet3_driver_shared *ds;
	struct ifmultiaddr *ifma;
	u_int mode;
	int cnt;

	ifp = sc->vmx_ifp;
	ds = sc->vmx_ds;
	cnt = 0;

	mode = VMXNET3_RXMODE_UCAST;
	if (ifp->if_flags & IFF_BROADCAST)
		mode |= VMXNET3_RXMODE_BCAST;
	if (ifp->if_flags & IFF_PROMISC)
		mode |= VMXNET3_RXMODE_PROMISC;
	if (ifp->if_flags & IFF_ALLMULTI)
		mode |= VMXNET3_RXMODE_ALLMULTI;
	else {
		if_maddr_rlock(ifp);
		TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
			if (ifma->ifma_addr->sa_family != AF_LINK)
				continue;
			else if (cnt == VMXNET3_MULTICAST_MAX)
				break;

			bcopy(LLADDR((struct sockaddr_dl *)ifma->ifma_addr),
			   &sc->vmx_mcast[cnt*ETHER_ADDR_LEN], ETHER_ADDR_LEN);
			cnt++;
		}
		if_maddr_runlock(ifp);

		if (cnt >= VMXNET3_MULTICAST_MAX) {
			cnt = 0;
			mode |= VMXNET3_RXMODE_ALLMULTI;
		} else if (cnt > 0)
			mode |= VMXNET3_RXMODE_MCAST;
		ds->mcast_tablelen = cnt * ETHER_ADDR_LEN;
	}

	vmxnet3_write_cmd(sc, VMXNET3_CMD_SET_FILTER);
	ds->rxmode = mode;
	vmxnet3_write_cmd(sc, VMXNET3_CMD_SET_RXMODE);
}

static int
vmxnet3_change_mtu(struct vmxnet3_softc *sc, int mtu)
{
	struct ifnet *ifp;

	ifp = sc->vmx_ifp;

	if (mtu < VMXNET3_MIN_MTU || mtu > VMXNET3_MAX_MTU)
		return (EINVAL);

	ifp->if_mtu = mtu;
	sc->vmx_ds->mtu = mtu;

	if (ifp->if_drv_flags & IFF_DRV_RUNNING) {
		ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
		vmxnet3_init_locked(sc);
	}

	return (0);
}

static int
vmxnet3_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct vmxnet3_softc *sc;
	struct ifreq *ifr;
	int reinit, mask, error;

	sc = ifp->if_softc;
	ifr = (struct ifreq *) data;
	error = 0;

	switch (cmd) {
	case SIOCSIFMTU:
		if (ifp->if_mtu != ifr->ifr_mtu) {
			VMXNET3_CORE_LOCK(sc);
			error = vmxnet3_change_mtu(sc, ifr->ifr_mtu);
			VMXNET3_CORE_UNLOCK(sc);
		}
		break;

	case SIOCSIFFLAGS:
		VMXNET3_CORE_LOCK(sc);
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_drv_flags & IFF_DRV_RUNNING)) {
				if ((ifp->if_flags ^ sc->vmx_if_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) {
					vmxnet3_set_rxfilter(sc);
				}
			} else
				vmxnet3_init_locked(sc);
		} else {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING)
				vmxnet3_stop(sc);
		}
		sc->vmx_if_flags = ifp->if_flags;
		VMXNET3_CORE_UNLOCK(sc);
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		VMXNET3_CORE_LOCK(sc);
		if (ifp->if_drv_flags & IFF_DRV_RUNNING)
			vmxnet3_set_rxfilter(sc);
		VMXNET3_CORE_UNLOCK(sc);
		break;

	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->vmx_media, cmd);
		break;

	case SIOCSIFCAP:
		VMXNET3_CORE_LOCK(sc);
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;

		if (mask & IFCAP_TXCSUM)
			ifp->if_capenable ^= IFCAP_TXCSUM;
		if (mask & IFCAP_TXCSUM_IPV6)
			ifp->if_capenable ^= IFCAP_TXCSUM_IPV6;
		if (mask & IFCAP_TSO4)
			ifp->if_capenable ^= IFCAP_TSO4;
		if (mask & IFCAP_TSO6)
			ifp->if_capenable ^= IFCAP_TSO6;

		if (mask & (IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6 | IFCAP_LRO |
		    IFCAP_VLAN_HWFILTER)) {
			/* These Rx features require us to renegotiate. */
			reinit = 1;

			if (mask & IFCAP_RXCSUM)
				ifp->if_capenable ^= IFCAP_RXCSUM;
			if (mask & IFCAP_RXCSUM_IPV6)
				ifp->if_capenable ^= IFCAP_RXCSUM_IPV6;
			if (mask & IFCAP_LRO)
				ifp->if_capenable ^= IFCAP_LRO;
			if (mask & IFCAP_VLAN_HWFILTER)
				ifp->if_capenable ^= IFCAP_VLAN_HWFILTER;
		} else
			reinit = 0;

		if (mask & IFCAP_VLAN_HWTSO)
			ifp->if_capenable ^= IFCAP_VLAN_HWTSO;
		if (mask & IFCAP_VLAN_HWTAGGING)
			ifp->if_capenable ^= IFCAP_VLAN_HWTAGGING;

		if (reinit && (ifp->if_drv_flags & IFF_DRV_RUNNING)) {
			ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
			vmxnet3_init_locked(sc);
		}

		VMXNET3_CORE_UNLOCK(sc);
		VLAN_CAPABILITIES(ifp);
		break;

	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	VMXNET3_CORE_LOCK_ASSERT_NOTOWNED(sc);

	return (error);
}

static void
vmxnet3_watchdog(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	struct vmxnet3_txqueue *txq;

	ifp = sc->vmx_ifp;
	txq = &sc->vmx_txq[0];

	VMXNET3_TXQ_LOCK(txq);
	if (sc->vmx_watchdog_timer == 0 || --sc->vmx_watchdog_timer) {
		VMXNET3_TXQ_UNLOCK(txq);
		return;
	}
	VMXNET3_TXQ_UNLOCK(txq);

	if_printf(ifp, "watchdog timeout -- resetting\n");
	ifp->if_oerrors++;
	ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
	vmxnet3_init_locked(sc);
}

static void
vmxnet3_tick(void *xsc)
{
	struct vmxnet3_softc *sc;

	sc = xsc;

	vmxnet3_watchdog(sc);
	callout_reset(&sc->vmx_tick, hz, vmxnet3_tick, sc);
}

static int
vmxnet3_link_is_up(struct vmxnet3_softc *sc)
{
	uint32_t status;

	/* Also update the link speed while here. */
	status = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_LINK);
	sc->vmx_link_speed = status >> 16;
	return !!(status & 0x1);
}

static void
vmxnet3_link_status(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	int link;

	ifp = sc->vmx_ifp;
	link = vmxnet3_link_is_up(sc);

	if (link != 0 && sc->vmx_link_active == 0) {
		sc->vmx_link_active = 1;
		if_link_state_change(ifp, LINK_STATE_UP);
	} else if (link == 0 && sc->vmx_link_active != 0) {
		sc->vmx_link_active = 0;
		if_link_state_change(ifp, LINK_STATE_DOWN);
	}
}

static void
vmxnet3_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct vmxnet3_softc *sc;

	sc = ifp->if_softc;

	ifmr->ifm_active = IFM_ETHER | IFM_AUTO;
	ifmr->ifm_status = IFM_AVALID;

	VMXNET3_CORE_LOCK(sc);
	if (vmxnet3_link_is_up(sc) != 0)
		ifmr->ifm_status |= IFM_ACTIVE;
	else
		ifmr->ifm_status |= IFM_NONE;
	VMXNET3_CORE_UNLOCK(sc);
}

static int
vmxnet3_media_change(struct ifnet *ifp)
{
	/* Ignore. */
	return (0);
}

static void
vmxnet3_set_lladdr(struct vmxnet3_softc *sc)
{
	uint32_t ml, mh;

	ml  = sc->vmx_lladdr[0];
	ml |= sc->vmx_lladdr[1] << 8;
	ml |= sc->vmx_lladdr[2] << 16;
	ml |= sc->vmx_lladdr[3] << 24;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_MACL, ml);

	mh  = sc->vmx_lladdr[4];
	mh |= sc->vmx_lladdr[5] << 8;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_MACH, mh);
}

static void
vmxnet3_get_lladdr(struct vmxnet3_softc *sc)
{
	uint32_t ml, mh;

	ml = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_MACL);
	mh = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_MACH);

	sc->vmx_lladdr[0] = ml;
	sc->vmx_lladdr[1] = ml >> 8;
	sc->vmx_lladdr[2] = ml >> 16;
	sc->vmx_lladdr[3] = ml >> 24;
	sc->vmx_lladdr[4] = mh;
	sc->vmx_lladdr[5] = mh >> 8;
}

static uint32_t __unused
vmxnet3_read_bar0(struct vmxnet3_softc *sc, bus_size_t r)
{

	bus_space_barrier(sc->vmx_iot0, sc->vmx_ioh0, r, 4,
	    BUS_SPACE_BARRIER_READ);
	return (bus_space_read_4(sc->vmx_iot0, sc->vmx_ioh0, r));
}

static void
vmxnet3_write_bar0(struct vmxnet3_softc *sc, bus_size_t r, uint32_t v)
{

	bus_space_write_4(sc->vmx_iot0, sc->vmx_ioh0, r, v);
	bus_space_barrier(sc->vmx_iot0, sc->vmx_ioh0, r, 4,
	    BUS_SPACE_BARRIER_WRITE);
}

static uint32_t
vmxnet3_read_bar1(struct vmxnet3_softc *sc, bus_size_t r)
{

	bus_space_barrier(sc->vmx_iot1, sc->vmx_ioh1, r, 4,
	    BUS_SPACE_BARRIER_READ);
	return (bus_space_read_4(sc->vmx_iot1, sc->vmx_ioh1, r));
}

static void
vmxnet3_write_bar1(struct vmxnet3_softc *sc, bus_size_t r, uint32_t v)
{

	bus_space_write_4(sc->vmx_iot1, sc->vmx_ioh1, r, v);
	bus_space_barrier(sc->vmx_iot1, sc->vmx_ioh1, r, 4,
	    BUS_SPACE_BARRIER_WRITE);
}

static void
vmxnet3_write_cmd(struct vmxnet3_softc *sc, uint32_t cmd)
{

	vmxnet3_write_bar1(sc, VMXNET3_BAR1_CMD, cmd);
}

static uint32_t
vmxnet3_read_cmd(struct vmxnet3_softc *sc, uint32_t cmd)
{

	vmxnet3_write_cmd(sc, cmd);
	return (vmxnet3_read_bar1(sc, VMXNET3_BAR1_CMD));
}


static void
vmxnet3_enable_intr(struct vmxnet3_softc *sc, int irq)
{

	vmxnet3_write_bar0(sc, VMXNET3_BAR0_IMASK(irq), 0);
}

static void
vmxnet3_disable_intr(struct vmxnet3_softc *sc, int irq)
{

	vmxnet3_write_bar0(sc, VMXNET3_BAR0_IMASK(irq), 1);
}

static void
vmxnet3_enable_all_intrs(struct vmxnet3_softc *sc)
{
	int i;

	sc->vmx_ds->ictrl &= ~VMXNET3_ICTRL_DISABLE_ALL;
	for (i = 0; i < VMXNET3_NINTR; i++)
		vmxnet3_enable_intr(sc, i);
}

static void
vmxnet3_disable_all_intrs(struct vmxnet3_softc *sc)
{
	int i;

	sc->vmx_ds->ictrl |= VMXNET3_ICTRL_DISABLE_ALL;
	for (i = 0; i < VMXNET3_NINTR; i++)
		vmxnet3_disable_intr(sc, i);
}

static void
vmxnet3_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *baddr = arg;

	if (error == 0)
		*baddr = segs->ds_addr;
}

static int
vmxnet3_dma_malloc(struct vmxnet3_softc *sc, bus_size_t size, bus_size_t align,
    struct vmxnet3_dma_alloc *dma)
{
	device_t dev;
	int error;

	dev = sc->vmx_dev;
	bzero(dma, sizeof(struct vmxnet3_dma_alloc));

	error = bus_dma_tag_create(bus_get_dma_tag(dev),
	    align, 0,		/* alignment, bounds */
	    BUS_SPACE_MAXADDR,	/* lowaddr */
	    BUS_SPACE_MAXADDR,	/* highaddr */
	    NULL, NULL,		/* filter, filterarg */
	    size,		/* maxsize */
	    1,			/* nsegments */
	    size,		/* maxsegsize */
	    BUS_DMA_ALLOCNOW,	/* flags */
	    NULL,		/* lockfunc */
	    NULL,		/* lockfuncarg */
	    &dma->dma_tag);
	if (error) {
		device_printf(dev, "bus_dma_tag_create failed: %d\n", error);
		goto fail;
	}

	error = bus_dmamem_alloc(dma->dma_tag, (void **)&dma->dma_vaddr,
	    BUS_DMA_ZERO | BUS_DMA_NOWAIT, &dma->dma_map);
	if (error) {
		device_printf(dev, "bus_dmamem_alloc failed: %d\n", error);
		goto fail;
	}

	error = bus_dmamap_load(dma->dma_tag, dma->dma_map, dma->dma_vaddr,
	    size, vmxnet3_dmamap_cb, &dma->dma_paddr, BUS_DMA_NOWAIT);
	if (error) {
		device_printf(dev,"bus_dmamap_load failed: %d\n", error);
		goto fail;
	}

	dma->dma_size = size;

fail:
	if (error)
		vmxnet3_dma_free(sc, dma);

	return (error);
}

static void
vmxnet3_dma_free(struct vmxnet3_softc *sc, struct vmxnet3_dma_alloc *dma)
{

	if (dma->dma_tag != NULL) {
		if (dma->dma_map != NULL) {
			bus_dmamap_sync(dma->dma_tag, dma->dma_map,
			    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(dma->dma_tag, dma->dma_map);
		}

		if (dma->dma_vaddr != NULL) {
			bus_dmamem_free(dma->dma_tag, dma->dma_vaddr,
			    dma->dma_map);
		}

		bus_dma_tag_destroy(dma->dma_tag);
	}
	bzero(dma, sizeof(struct vmxnet3_dma_alloc));
}

/*-
 * Copyright (C) 2011, Romain Tartière
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

/*
 * $Id$
 */

#include "config.h"

#include <sys/types.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "llcp_log.h"
#include "llcp_parameters.h"
#include "llc_link.h"
#include "llc_service.h"
#include "llc_service_llc.h"
#include "llc_service_sdp.h"

#define LOG_LLC_LINK "libnfc-llcp.llc.link"
#define LLC_LINK_MSG(priority, message) llcp_log_log (LOG_LLC_LINK, priority, "%s", message)
#define LLC_LINK_LOG(priority, format, ...) llcp_log_log (LOG_LLC_LINK, priority, format, __VA_ARGS__)

struct llc_link *
llc_link_new (void)
{
    struct llc_link *link;

    if ((link = malloc (sizeof (*link)))) {
	link->version.major = LLCP_VERSION_MAJOR;
	link->version.minor = LLCP_VERSION_MINOR;
	link->opt = LINK_SERVICE_CLASS_3;
	memset (link->services, '\0', sizeof (link->services));
	link->cut_test_context = NULL;

	struct llc_service *llcp_service = llc_service_new (llc_service_llc_thread);
	if (!llcp_service) {
	    LLC_LINK_MSG (LLC_PRIORITY_FATAL, "Cannot create LLC service 0");
	    llc_link_free (link);
	    return NULL;
	}

	llc_service_set_user_data (llcp_service, link);
	llc_service_set_mq_down_non_blocking (llcp_service);

	if (llc_link_service_bind (link, llcp_service, 0) < 0) {
	    LLC_LINK_MSG (LLC_PRIORITY_FATAL, "Cannot bind LLC service 0");
	    llc_link_free (link);
	    return NULL;
	}

	struct llc_service *sdp_service = llc_service_new_with_uri (llc_service_sdp_thread, LLCP_SDP_URI);

	llc_service_set_user_data (sdp_service, link);
	if (llc_link_service_bind (link, sdp_service, LLCP_SDP_SAP) < 0) {
	    LLC_LINK_MSG (LLC_PRIORITY_FATAL, "Cannot bind LLC service 1");
	    llc_link_free (link);
	    return NULL;
	}
    }

    return link;
}

int
llc_link_service_bind (struct llc_link *link, struct llc_service *service, int8_t sap)
{
    assert (link);
    assert (service);
    assert (sap <= MAX_LLC_LINK_SERVICE);

    if (SAP_AUTO == sap) {
	for (sap = 0x10; link->services[sap] && (sap <= 0x1F); sap++);
	if (sap > 0x1F) {
	    LLC_LINK_MSG (LLC_PRIORITY_ERROR, "No space left for service");
	    return -1;
	}
    }

    if (link->services[sap]) {
	LLC_LINK_LOG (LLC_PRIORITY_ERROR, "SAP %d already bound", sap);
	return -1;
    }

    service->cut_test_context = link->cut_test_context;
    service->sap = sap;
    link->services[sap] = service;

    LLC_LINK_LOG (LLC_PRIORITY_TRACE, "service %p bound to SAP %d", (void *) service, sap);

    return sap;
}

void
llc_link_service_unbind (struct llc_link *link, uint8_t sap)
{
    if (link->services[sap]) {
	link->services[sap]->cut_test_context = NULL;
	link->services[sap]->sap = -1;
	link->services[sap] = NULL;
    }
}

uint16_t
llc_link_get_wks (const struct llc_link *link)
{
    uint16_t wks = 0x0000;
    for (int i = 0; i < 16; i++) {
	wks |= (link->services[i] ? 1 : 0) << i;
    }

    return wks;
}

int
llc_link_activate (struct llc_link *link, uint8_t flags, const uint8_t *parameters, size_t length)
{
    assert (link);
    assert (flags == (flags & 0x03));

    link->role = flags & 0x01;
    link->version.major = LLCP_VERSION_MAJOR;
    link->version.minor = LLCP_VERSION_MINOR;
    link->local_miu  = LLC_DEFAULT_MIU;
    link->remote_miu = LLC_DEFAULT_MIU;
    link->remote_wks = 0x0001;
    link->local_lto.tv_sec  = 0;
    link->local_lto.tv_nsec = 100000000;
    link->remote_lto.tv_sec  = 0;
    link->remote_lto.tv_nsec = 100000000;
    link->local_lsc  = 3;
    link->remote_lsc = 3;

    if (llc_link_configure (link, parameters, length) < 0) {
	LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Link configuration failed");
	llc_link_deactivate (link);
	return -1;
    }

    switch (flags & 0x01) {
    case LLC_INITIATOR:
    case LLC_TARGET:
	break;
    }

    if (!(flags & LLC_PAX_PDU_PROHIBITED)) {
	/* FIXME: Exchange PAX PDU */
    }

    for (int i = 0; i <= MAX_LLC_LINK_SERVICE; i++) {
	if (link->services[i]) {
	    LLC_LINK_LOG (LLC_PRIORITY_INFO, "Starting service %d", i);
	    if (llc_service_start (link->services[i]) < 0) {
		LLC_LINK_LOG (LLC_PRIORITY_ERROR, "Can't start service %d", i);
		return -1;
	    }
	}
    }

    return 0;
}

int
llc_link_configure (struct llc_link *link, const uint8_t *parameters, size_t length)
{
    struct llcp_version version;
    uint16_t miux;
    uint8_t lto;
    uint8_t opt;

    LLC_LINK_LOG (LLC_PRIORITY_TRACE, "llc_link_configure (%p, %p, %d)", (void *)link, (void *) parameters, length);

    size_t offset = 0;
    while (offset < length) {
	if (offset > length - 2) {
	    LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Incomplete TLV field in parameters list");
	    return -1;
	}
	if (offset + 2 + parameters[offset+1] > length) {
	    LLC_LINK_LOG (LLC_PRIORITY_ERROR, "Incomplete TLV value in parameters list (expected %d bytes but only %d left)", parameters[offset+1], length - (offset + 2));
	    return -1;
	}
	switch (parameters[offset]) {
	case LLCP_PARAMETER_VERSION:
	    if (parameter_decode_version (parameters + offset, 2 + parameters[offset+1], &version) < 0) {
		LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Invalid Version TLV parameter");
		return -1;
	    }
	    LLC_LINK_LOG (LLC_PRIORITY_DEBUG, "Version: %d.%d (remote)", version.major, version.minor);
	    if (llcp_version_agreement (link, version) < 0) {
		LLC_LINK_MSG (LLC_PRIORITY_WARN, "LLCP Version Agreement Procedure failed");
		return -1;
	    }
	    LLC_LINK_LOG (LLC_PRIORITY_DEBUG, "Version: %d.%d (agreed)", version.major, version.minor);
	    break;
	case LLCP_PARAMETER_MIUX:
	    if (parameter_decode_miux (parameters + offset, 2 + parameters[offset+1], &miux) < 0) {
		LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Invalid MIUX TLV parameter");
		return -1;
	    }
	    link->remote_miu = miux + 128;
	    LLC_LINK_LOG (LLC_PRIORITY_DEBUG, "MIUX: %d (0x%02x)", miux + 128, miux);
	    break;
	case LLCP_PARAMETER_WKS:
	    if (parameter_decode_wks (parameters + offset, 2 + parameters[offset+1], &link->remote_wks) < 0) {
		LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Invalid WKS TLV parameter");
		return -1;
	    }
	    LLC_LINK_LOG (LLC_PRIORITY_DEBUG, "WKS: 0x%04x", link->remote_wks);
	    break;
	case LLCP_PARAMETER_LTO:
	    if (parameter_decode_lto (parameters + offset, 2 + parameters[offset+1], &lto) < 0) {
		LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Invalid LTO TLV parameter");
		return -1;
	    }
	    link->remote_lto.tv_sec = (lto * 10 * 1000000) / 1000000000;
	    link->remote_lto.tv_nsec = (lto * 10 * 1000000) % 1000000000;
	    LLC_LINK_LOG (LLC_PRIORITY_DEBUG, "LTO: %d ms (0x%02x)", 10 * lto, lto);
	    break;
	case LLCP_PARAMETER_OPT:
	    if (parameter_decode_opt (parameters + offset, 2 + parameters[offset+1], &opt) < 0) {
		LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Invalid OPT TLV parameter");
		return -1;
	    }
	    link->remote_lsc = opt & 0x03;
	    LLC_LINK_LOG (LLC_PRIORITY_DEBUG, "OPT: 0x%02x", opt);
	    break;
	default:
	    LLC_LINK_LOG (LLC_PRIORITY_INFO, "Unknown TLV Field 0x%02x (length: %d)",
			  parameters[offset], parameters[offset+1]);
	}
	offset += 2 + parameters[offset+1];
    }
    if (offset != length) {
	LLC_LINK_MSG (LLC_PRIORITY_ERROR, "Unprocessed TLV parameters");
	return -1;
    }
    return 0;
}

int
llc_link_encode_parameters (const struct llc_link *link, uint8_t *parameters, size_t length)
{
    int n;
    uint8_t *parameter = parameters;

    if ((n = parameter_encode_version (parameter, length, link->version)) < 0)
	return -1;
    parameter += n; length -= n;

    if ((n = parameter_encode_miux (parameter, length, link->local_miu)) < 0)
	return -1;
    parameter += n; length -= n;

    if ((n = parameter_encode_wks (parameter, length, llc_link_get_wks(link))) < 0)
	return -1;
    parameter += n; length -= n;

    uint8_t lto = link->local_lto.tv_sec * 100 + link->local_lto.tv_nsec / 10000000;
    if ((n = parameter_encode_lto (parameter, length, lto)) < 0)
	return -1;
    parameter += n; length -= n;

    if ((n = parameter_encode_opt (parameter, length, link->opt)) < 0)
	return -1;
    parameter += n; length -= n;

    return parameter - parameters;
}

void
llc_link_deactivate (struct llc_link *link)
{
    assert (link);

    for (int i = 0; i <= MAX_LLC_LINK_SERVICE; i++) {
	if (link->services[i]) {
	    LLC_LINK_LOG (LLC_PRIORITY_INFO, "Stopping service %d", i);
	    llc_service_stop (link->services[i]);
	    LLC_LINK_LOG (LLC_PRIORITY_INFO, "Service %d stopped", i);
	}
    }
}

void
llc_link_free (struct llc_link *link)
{
    assert (link);

    for (int i = MAX_LLC_LINK_SERVICE; i >= 0; i--) {
	if (link->services[i]) {
	    LLC_LINK_LOG (LLC_PRIORITY_INFO, "Freeing service %d", i);
	    llc_service_free (link->services[i]);
	}
    }

    free (link);
}
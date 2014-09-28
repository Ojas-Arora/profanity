/*
 * iq.c
 *
 * Copyright (C) 2012 - 2014 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

#include "config.h"

#ifdef HAVE_GIT_VERSION
#include "gitversion.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <strophe.h>

#include "log.h"
#include "muc.h"
#include "profanity.h"
#include "config/preferences.h"
#include "server_events.h"
#include "xmpp/capabilities.h"
#include "xmpp/connection.h"
#include "xmpp/stanza.h"
#include "xmpp/form.h"
#include "roster_list.h"
#include "xmpp/xmpp.h"

#define HANDLE(ns, type, func) xmpp_handler_add(conn, func, ns, STANZA_NAME_IQ, type, ctx)

static int _error_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _ping_get_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _version_get_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _disco_info_get_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _disco_info_response_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _version_result_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _disco_items_result_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _disco_items_get_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _destroy_room_result_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _room_config_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _room_config_submit_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _manual_pong_handler(xmpp_conn_t *const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _ping_timed_handler(xmpp_conn_t * const conn,
    void * const userdata);
static int _caps_response_handler(xmpp_conn_t *const conn,
    xmpp_stanza_t * const stanza, void * const userdata);

void
iq_add_handlers(void)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    HANDLE(NULL,                STANZA_TYPE_ERROR,  _error_handler);

    HANDLE(XMPP_NS_DISCO_INFO,  STANZA_TYPE_GET,    _disco_info_get_handler);

    HANDLE(XMPP_NS_DISCO_ITEMS, STANZA_TYPE_GET,    _disco_items_get_handler);
    HANDLE(XMPP_NS_DISCO_ITEMS, STANZA_TYPE_RESULT, _disco_items_result_handler);

    HANDLE(STANZA_NS_VERSION,   STANZA_TYPE_GET,    _version_get_handler);
    HANDLE(STANZA_NS_VERSION,   STANZA_TYPE_RESULT, _version_result_handler);

    HANDLE(STANZA_NS_PING,      STANZA_TYPE_GET,    _ping_get_handler);

    if (prefs_get_autoping() != 0) {
        int millis = prefs_get_autoping() * 1000;
        xmpp_timed_handler_add(conn, _ping_timed_handler, millis, ctx);
    }
}

static void
_iq_set_autoping(const int seconds)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    if (jabber_get_connection_status() == JABBER_CONNECTED) {
        xmpp_timed_handler_delete(conn, _ping_timed_handler);

        if (seconds != 0) {
            int millis = seconds * 1000;
            xmpp_timed_handler_add(conn, _ping_timed_handler, millis,
                ctx);
        }
    }
}

static void
_iq_room_list_request(gchar *conferencejid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_disco_items_iq(ctx, "confreq", conferencejid);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_disco_info_request(gchar *jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    char *id = create_unique_id("disco_info");
    xmpp_stanza_t *iq = stanza_create_disco_info_iq(ctx, id, jid, NULL);

    xmpp_id_handler_add(conn, _disco_info_response_handler, id, NULL);

    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_send_caps_request(const char * const to, const char * const id,
    const char * const node, const char * const ver)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    if (!node) {
        log_error("Could not create caps request, no node");
        return;
    }
    if (!ver) {
        log_error("Could not create caps request, no ver");
        return;
    }

    GString *node_str = g_string_new("");
    g_string_printf(node_str, "%s#%s", node, ver);
    xmpp_stanza_t *iq = stanza_create_disco_info_iq(ctx, id, to, node_str->str);
    g_string_free(node_str, TRUE);

    xmpp_id_handler_add(conn, _caps_response_handler, id, NULL);

    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_disco_items_request(gchar *jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_disco_items_iq(ctx, "discoitemsreq", jid);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_send_software_version(const char * const fulljid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_software_version_iq(ctx, fulljid);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_confirm_instant_room(const char * const room_jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_instant_room_request_iq(ctx, room_jid);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_destroy_instant_room(const char * const room_jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_instant_room_destroy_iq(ctx, room_jid);

    char *id = xmpp_stanza_get_id(iq);
    xmpp_id_handler_add(conn, _destroy_room_result_handler, id, NULL);

    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_request_room_config_form(const char * const room_jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_room_config_request_iq(ctx, room_jid);

    char *id = xmpp_stanza_get_id(iq);
    xmpp_id_handler_add(conn, _room_config_handler, id, NULL);

    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_submit_room_config(const char * const room, DataForm *form)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_room_config_submit_iq(ctx, room, form);

    char *id = xmpp_stanza_get_id(iq);
    xmpp_id_handler_add(conn, _room_config_submit_handler, id, NULL);

    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_room_config_cancel(const char * const room_jid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_room_config_cancel_iq(ctx, room_jid);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static void
_iq_send_ping(const char * const target)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_ping_iq(ctx, target);
    char *id = xmpp_stanza_get_id(iq);

    GDateTime *now = g_date_time_new_now_local();
    xmpp_id_handler_add(conn, _manual_pong_handler, id, now);

    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

static int
_error_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    char *error_msg = stanza_get_error_message(stanza);

    if (id != NULL) {
        log_debug("IQ error handler fired, id: %s, error: %s", id, error_msg);
        log_error("IQ error received, id: %s, error: %s", id, error_msg);
    } else {
        log_debug("IQ error handler fired, error: %s", error_msg);
        log_error("IQ error received, error: %s", error_msg);
    }

    free(error_msg);

    return 1;
}

static int
_pong_handler(xmpp_conn_t *const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    char *id = xmpp_stanza_get_id(stanza);
    char *type = xmpp_stanza_get_type(stanza);

    if (id != NULL) {
        log_debug("IQ pong handler fired, id: %s.", id);
    } else {
        log_debug("IQ pong handler fired.");
    }

    if (id != NULL && type != NULL) {
        // show warning if error
        if (strcmp(type, STANZA_TYPE_ERROR) == 0) {
            char *error_msg = stanza_get_error_message(stanza);
            log_warning("Server ping (id=%s) responded with error: %s", id, error_msg);
            free(error_msg);

            // turn off autoping if error type is 'cancel'
            xmpp_stanza_t *error = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_ERROR);
            if (error != NULL) {
                char *errtype = xmpp_stanza_get_type(error);
                if (errtype != NULL) {
                    if (strcmp(errtype, "cancel") == 0) {
                        log_warning("Server ping (id=%s) error type 'cancel', disabling autoping.", id);
                        handle_autoping_cancel();
                        xmpp_timed_handler_delete(conn, _ping_timed_handler);
                    }
                }
            }
        }
    }

    // remove this handler
    return 0;
}

static int
_caps_response_handler(xmpp_conn_t *const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);

    if (id) {
        log_info("Capabilities response handler fired for id %s", id);
    } else {
        log_info("Capabilities response handler fired");
    }

    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    if (!from) {
        log_info("No from attribute");
        return 0;
    }

    char *type = xmpp_stanza_get_type(stanza);
    // handle error responses
    if (g_strcmp0(type, STANZA_TYPE_ERROR) == 0) {
        char *error_message = stanza_get_error_message(stanza);
        log_warning("Error received for capabilities response from %s: ", from, error_message);
        free(error_message);
        return 0;
    }

    if (query == NULL) {
        log_warning("No query element found.");
        return 0;
    }

    char *node = xmpp_stanza_get_attribute(query, STANZA_ATTR_NODE);
    if (node == NULL) {
        log_warning("No node attribute found");
        return 0;
    }

    // validate sha1
    gchar **split = g_strsplit(node, "#", -1);
    char *given_sha1 = split[1];
    char *generated_sha1 = caps_create_sha1_str(query);

    if (g_strcmp0(given_sha1, generated_sha1) != 0) {
        log_warning("Generated sha-1 does not match given:");
        log_warning("Generated : %s", generated_sha1);
        log_warning("Given     : %s", given_sha1);
    } else {
        log_info("Valid SHA-1 hash found: %s", given_sha1);

        if (caps_contains(given_sha1)) {
            log_info("Capabilties cached: %s", given_sha1);
        } else {
            log_info("Capabilities not cached: %s, storing", given_sha1);
            Capabilities *capabilities = caps_create(query);
            caps_add(given_sha1, capabilities);
            caps_destroy(capabilities);
        }

        caps_map(from, given_sha1);
    }

    g_free(generated_sha1);
    g_strfreev(split);
    return 0;
}

static int
_manual_pong_handler(xmpp_conn_t *const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    char *type = xmpp_stanza_get_type(stanza);

    // handle error responses
    if (g_strcmp0(type, STANZA_TYPE_ERROR) == 0) {
        char *error_message = stanza_get_error_message(stanza);
        handle_ping_error_result(from, error_message);
        free(error_message);
        return 0;
    }

    GDateTime *sent = (GDateTime *)userdata;
    GDateTime *now = g_date_time_new_now_local();

    GTimeSpan elapsed = g_date_time_difference(now, sent);
    int elapsed_millis = elapsed / 1000;

    g_date_time_unref(sent);
    g_date_time_unref(now);

    handle_ping_result(from, elapsed_millis);

    return 0;
}

static int
_ping_timed_handler(xmpp_conn_t * const conn, void * const userdata)
{
    xmpp_ctx_t *ctx = (xmpp_ctx_t *)userdata;

    if (jabber_get_connection_status() == JABBER_CONNECTED) {

        xmpp_stanza_t *iq = stanza_create_ping_iq(ctx, NULL);
        char *id = xmpp_stanza_get_id(iq);

        // add pong handler
        xmpp_id_handler_add(conn, _pong_handler, id, ctx);

        xmpp_send(conn, iq);
        xmpp_stanza_release(iq);
    }

    return 1;
}

static int
_version_result_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    char *id = xmpp_stanza_get_id(stanza);

    if (id != NULL) {
        log_debug("IQ version result handler fired, id: %s.", id);
    } else {
        log_debug("IQ version result handler fired.");
    }

    const char *jid = xmpp_stanza_get_attribute(stanza, "from");

    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);
    if (query == NULL) {
        return 1;
    }

    char *ns = xmpp_stanza_get_ns(query);
    if (g_strcmp0(ns, STANZA_NS_VERSION) != 0) {
        return 1;
    }

    char *name_str = NULL;
    char *version_str = NULL;
    char *os_str = NULL;
    xmpp_stanza_t *name = xmpp_stanza_get_child_by_name(query, "name");
    xmpp_stanza_t *version = xmpp_stanza_get_child_by_name(query, "version");
    xmpp_stanza_t *os = xmpp_stanza_get_child_by_name(query, "os");

    if (name != NULL) {
        name_str = xmpp_stanza_get_text(name);
    }
    if (version != NULL) {
        version_str = xmpp_stanza_get_text(version);
    }
    if (os != NULL) {
        os_str = xmpp_stanza_get_text(os);
    }

    PContact contact;
    Jid *jidp = jid_create(jid);
    if (muc_active(jidp->barejid)) {
        contact = muc_roster_item(jidp->barejid, jidp->resourcepart);
    } else {
        contact = roster_get_contact(jidp->barejid);
    }

    Resource *resource = p_contact_get_resource(contact, jidp->resourcepart);
    const char *presence = string_from_resource_presence(resource->presence);
    handle_software_version_result(jid, presence, name_str, version_str, os_str);

    jid_destroy(jidp);

    return 1;
}

static int
_ping_get_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    xmpp_ctx_t *ctx = (xmpp_ctx_t *)userdata;
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    const char *to = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_TO);
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    if (id != NULL) {
        log_debug("IQ ping get handler fired, id: %s.", id);
    } else {
        log_debug("IQ ping get handler fired.");
    }

    if ((from == NULL) || (to == NULL)) {
        return 1;
    }

    xmpp_stanza_t *pong = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(pong, STANZA_NAME_IQ);
    xmpp_stanza_set_attribute(pong, STANZA_ATTR_TO, from);
    xmpp_stanza_set_attribute(pong, STANZA_ATTR_FROM, to);
    xmpp_stanza_set_attribute(pong, STANZA_ATTR_TYPE, STANZA_TYPE_RESULT);

    if (id != NULL) {
        xmpp_stanza_set_attribute(pong, STANZA_ATTR_ID, id);
    }

    xmpp_send(conn, pong);
    xmpp_stanza_release(pong);

    return 1;
}

static int
_version_get_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    xmpp_ctx_t *ctx = (xmpp_ctx_t *)userdata;
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    if (id != NULL) {
        log_debug("IQ version get handler fired, id: %s.", id);
    } else {
        log_debug("IQ version get handler fired.");
    }

    if (from != NULL) {
        xmpp_stanza_t *response = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(response, STANZA_NAME_IQ);
        if (id != NULL) {
            xmpp_stanza_set_id(response, id);
        }
        xmpp_stanza_set_attribute(response, STANZA_ATTR_TO, from);
        xmpp_stanza_set_type(response, STANZA_TYPE_RESULT);

        xmpp_stanza_t *query = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(query, STANZA_NAME_QUERY);
        xmpp_stanza_set_ns(query, STANZA_NS_VERSION);

        xmpp_stanza_t *name = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(name, "name");
        xmpp_stanza_t *name_txt = xmpp_stanza_new(ctx);
        xmpp_stanza_set_text(name_txt, "Profanity");
        xmpp_stanza_add_child(name, name_txt);

        xmpp_stanza_t *version = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(version, "version");
        xmpp_stanza_t *version_txt = xmpp_stanza_new(ctx);
        GString *version_str = g_string_new(PACKAGE_VERSION);
        if (strcmp(PACKAGE_STATUS, "development") == 0) {
#ifdef HAVE_GIT_VERSION
            g_string_append(version_str, "dev.");
            g_string_append(version_str, PROF_GIT_BRANCH);
            g_string_append(version_str, ".");
            g_string_append(version_str, PROF_GIT_REVISION);
#else
            g_string_append(version_str, "dev");
#endif
        }
        xmpp_stanza_set_text(version_txt, version_str->str);
        xmpp_stanza_add_child(version, version_txt);

        xmpp_stanza_add_child(query, name);
        xmpp_stanza_add_child(query, version);
        xmpp_stanza_add_child(response, query);

        xmpp_send(conn, response);

        g_string_free(version_str, TRUE);
        xmpp_stanza_release(name_txt);
        xmpp_stanza_release(version_txt);
        xmpp_stanza_release(name);
        xmpp_stanza_release(version);
        xmpp_stanza_release(query);
        xmpp_stanza_release(response);
    }

    return 1;
}

static int
_disco_items_get_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    xmpp_ctx_t *ctx = (xmpp_ctx_t *)userdata;
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    if (id != NULL) {
        log_debug("IQ disco items get handler fired, id: %s.", id);
    } else {
        log_debug("IQ disco items get handler fired.");
    }

    if (from != NULL) {
        xmpp_stanza_t *response = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(response, STANZA_NAME_IQ);
        xmpp_stanza_set_id(response, xmpp_stanza_get_id(stanza));
        xmpp_stanza_set_attribute(response, STANZA_ATTR_TO, from);
        xmpp_stanza_set_type(response, STANZA_TYPE_RESULT);
        xmpp_stanza_t *query = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(query, STANZA_NAME_QUERY);
        xmpp_stanza_set_ns(query, XMPP_NS_DISCO_ITEMS);
        xmpp_stanza_add_child(response, query);
        xmpp_send(conn, response);

        xmpp_stanza_release(response);
    }

    return 1;
}


static int
_disco_info_get_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    xmpp_ctx_t *ctx = (xmpp_ctx_t *)userdata;
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    xmpp_stanza_t *incoming_query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);
    const char *node_str = xmpp_stanza_get_attribute(incoming_query, STANZA_ATTR_NODE);

    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);

    if (id != NULL) {
        log_debug("IQ disco info get handler fired, id: %s.", id);
    } else {
        log_debug("IQ disco info get handler fired.");
    }

    if (from != NULL) {
        xmpp_stanza_t *response = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(response, STANZA_NAME_IQ);
        xmpp_stanza_set_id(response, xmpp_stanza_get_id(stanza));
        xmpp_stanza_set_attribute(response, STANZA_ATTR_TO, from);
        xmpp_stanza_set_type(response, STANZA_TYPE_RESULT);
        xmpp_stanza_t *query = caps_create_query_response_stanza(ctx);
        if (node_str != NULL) {
            xmpp_stanza_set_attribute(query, STANZA_ATTR_NODE, node_str);
        }
        xmpp_stanza_add_child(response, query);
        xmpp_send(conn, response);

        xmpp_stanza_release(query);
        xmpp_stanza_release(response);
    }

    return 1;
}

static int
_destroy_room_result_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);

    if (id != NULL) {
        log_debug("IQ destroy room result handler fired, id: %s.", id);
    } else {
        log_debug("IQ destroy room result handler fired.");
    }

    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    if (from == NULL) {
        log_error("No from attribute for IQ destroy room result");
    } else {
        handle_room_destroy(from);
    }

    return 0;
}

static int
_room_config_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    const char *type = xmpp_stanza_get_type(stanza);
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    if (id != NULL) {
        log_debug("IQ room config handler fired, id: %s.", id);
    } else {
        log_debug("IQ room config handler fired.");
    }

    // handle error responses
    if (g_strcmp0(type, STANZA_TYPE_ERROR) == 0) {
        char *error_message = stanza_get_error_message(stanza);
        handle_room_configuration_form_error(from, error_message);
        free(error_message);
        return 0;
    }

    if (from == NULL) {
        log_warning("No from attribute for IQ config request result");
        handle_room_configuration_form_error(from, "No from attribute for room cofig response.");
        return 0;
    }

    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);
    if (query == NULL) {
        log_warning("No query element found parsing room config response");
        handle_room_configuration_form_error(from, "No query element found parsing room config response");
        return 0;
    }

    xmpp_stanza_t *x = xmpp_stanza_get_child_by_ns(query, STANZA_NS_DATA);
    if (x == NULL) {
        log_warning("No x element found with %s namespace parsing room config response", STANZA_NS_DATA);
        handle_room_configuration_form_error(from, "No form configuration options available");
        return 0;
    }

    char *form_type = xmpp_stanza_get_attribute(x, STANZA_ATTR_TYPE);
    if (g_strcmp0(form_type, "form") != 0) {
        log_warning("x element not of type 'form' parsing room config response");
        handle_room_configuration_form_error(from, "Form not of type 'form' parsing room config response.");
        return 0;
    }

    DataForm *form = form_create(x);
    handle_room_configure(from, form);

    return 0;
}

static int
_room_config_submit_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    const char *type = xmpp_stanza_get_type(stanza);
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);

    if (id != NULL) {
        log_debug("IQ room config submit handler fired, id: %s.", id);
    } else {
        log_debug("IQ room config submit handler fired.");
    }

    // handle error responses
    if (g_strcmp0(type, STANZA_TYPE_ERROR) == 0) {
        char *error_message = stanza_get_error_message(stanza);
        handle_room_config_submit_result_error(from, error_message);
        free(error_message);
        return 0;
    }

    handle_room_config_submit_result(from);

    return 0;
}

static void
_identity_destroy(DiscoIdentity *identity)
{
    if (identity != NULL) {
        free(identity->name);
        free(identity->type);
        free(identity->category);
        free(identity);
    }
}

static void
_item_destroy(DiscoItem *item)
{
    if (item != NULL) {
        free(item->jid);
        free(item->name);
        free(item);
    }
}

static int
_disco_info_response_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    log_info("Received diso#info response");

    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    const char *type = xmpp_stanza_get_type(stanza);

    // handle error responses
    if (g_strcmp0(type, STANZA_TYPE_ERROR) == 0) {
        char *error_message = stanza_get_error_message(stanza);
        handle_disco_info_error(from, error_message);
        free(error_message);
        return 0;
    }

    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);

    if (query != NULL) {
        xmpp_stanza_t *child = xmpp_stanza_get_children(query);
        GSList *identities = NULL;
        GSList *features = NULL;
        while (child != NULL) {
            const char *stanza_name = xmpp_stanza_get_name(child);
            if (g_strcmp0(stanza_name, STANZA_NAME_FEATURE) == 0) {
                const char *var = xmpp_stanza_get_attribute(child, STANZA_ATTR_VAR);
                if (var != NULL) {
                    features = g_slist_append(features, strdup(var));
                }
            } else if (g_strcmp0(stanza_name, STANZA_NAME_IDENTITY) == 0) {
                const char *name = xmpp_stanza_get_attribute(child, STANZA_ATTR_NAME);
                const char *type = xmpp_stanza_get_attribute(child, STANZA_ATTR_TYPE);
                const char *category = xmpp_stanza_get_attribute(child, STANZA_ATTR_CATEGORY);

                if ((name != NULL) || (category != NULL) || (type != NULL)) {
                    DiscoIdentity *identity = malloc(sizeof(struct disco_identity_t));

                    if (name != NULL) {
                        identity->name = strdup(name);
                    } else {
                        identity->name = NULL;
                    }
                    if (category != NULL) {
                        identity->category = strdup(category);
                    } else {
                        identity->category = NULL;
                    }
                    if (type != NULL) {
                        identity->type = strdup(type);
                    } else {
                        identity->type = NULL;
                    }

                    identities = g_slist_append(identities, identity);
                }
            }

            child = xmpp_stanza_get_next(child);
        }

        handle_disco_info(from, identities, features);
        g_slist_free_full(features, free);
        g_slist_free_full(identities, (GDestroyNotify)_identity_destroy);
    }
    return 1;
}

static int
_disco_items_result_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{

    log_debug("Received diso#items response");
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    GSList *items = NULL;

    if ((g_strcmp0(id, "confreq") == 0) || (g_strcmp0(id, "discoitemsreq") == 0)) {
        log_debug("Response to query: %s", id);
        xmpp_stanza_t *query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);

        if (query != NULL) {
            xmpp_stanza_t *child = xmpp_stanza_get_children(query);
            while (child != NULL) {
                const char *stanza_name = xmpp_stanza_get_name(child);
                if ((stanza_name != NULL) && (g_strcmp0(stanza_name, STANZA_NAME_ITEM) == 0)) {
                    const char *item_jid = xmpp_stanza_get_attribute(child, STANZA_ATTR_JID);
                    if (item_jid != NULL) {
                        DiscoItem *item = malloc(sizeof(struct disco_item_t));
                        item->jid = strdup(item_jid);
                        const char *item_name = xmpp_stanza_get_attribute(child, STANZA_ATTR_NAME);
                        if (item_name != NULL) {
                            item->name = strdup(item_name);
                        } else {
                            item->name = NULL;
                        }
                        items = g_slist_append(items, item);
                    }
                }

                child = xmpp_stanza_get_next(child);
            }
        }
    }

    if (g_strcmp0(id, "confreq") == 0) {
        handle_room_list(items, from);
    } else if (g_strcmp0(id, "discoitemsreq") == 0) {
        handle_disco_items(items, from);
    }

    g_slist_free_full(items, (GDestroyNotify)_item_destroy);

    return 1;
}

void
iq_init_module(void)
{
    iq_room_list_request = _iq_room_list_request;
    iq_disco_info_request = _iq_disco_info_request;
    iq_disco_items_request = _iq_disco_items_request;
    iq_send_software_version = _iq_send_software_version;
    iq_set_autoping = _iq_set_autoping;
    iq_confirm_instant_room = _iq_confirm_instant_room;
    iq_destroy_instant_room = _iq_destroy_instant_room;
    iq_send_ping = _iq_send_ping;
    iq_request_room_config_form = _iq_request_room_config_form;
    iq_room_config_cancel = _iq_room_config_cancel;
    iq_submit_room_config = _iq_submit_room_config;
    iq_send_caps_request = _iq_send_caps_request;
}

/* vi: se sw=4 ts=4 tw=80 fo+=t cin cino=(0t0 : */
/*
 * LIBGAMI - Library for using the Asterisk Manager Interface with GObject
 * Copyright (C) 2008-2009 Florian Müllner, EuropeSIP Communications S.L.
 * 
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library;  if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#include <glib.h>
#include <glib-object.h>

#ifdef G_OS_WIN32
#  include <windef.h>
#  include <ws2tcpip.h>
#  define CLOSESOCKET(S) closesocket (S)
#  define G_SOCKET_IO_CHANNEL_NEW(S) g_io_channel_win32_new_socket (S)
#else
#  include <sys/socket.h>
#  include <netdb.h>
#  define CLOSESOCKET(S) close (S)
#  define G_SOCKET_IO_CHANNEL_NEW(S) g_io_channel_unix_new (S)
#endif

#ifndef SOCKET
#  define SOCKET gint
#endif

#ifndef INVALID_SOCKET
#  define INVALID_SOCKET -1
#endif

#ifndef HAVE_GAI_STRERROR
#  undef gai_strerror
#  define gai_strerror(C) ""
#endif

#include <gami-manager.h>

/**
 * SECTION: libgami-manager
 * @short_description: An GObject based implementation of the Asterisk
 *         Manager Interface
 * @title: GamiManager
 * @stability: Unstable
 *
 * GamiManager is an implementation of the Asterisk Manager Interface based 
 * on GObject. It supports both synchronious and asynchronious operation 
 * and integrates well with glib's signal / callback system.
 *
 * Each manager action has both a synchronous and an asynchronous version.
 * Actions return either a #gboolean, a string (#gchar *), a #GHashTable or a
 * #GSList. The synchronous function returns these directly, while the 
 * asynchronous version takes a callback parameter, which will be called with 
 * the return value once the action has finished, along the user_data pointer
 * which may be passed to asynchronous actions to access your application's
 * objects.
 * All functions support an optional ActionID as supported by the underlying
 * Asterisk Manager API. Note that an ActionID will be randomly assigned if
 * not provided as a parameter.
 * Errors are reported via an optional #GError parameter.
 * 
 * Asynchronious callbacks and events require the use of #GMainLoop (or derived
 * implementations as gtk_main().
 */

typedef enum {
    GAMI_RESPONSE_TYPE_BOOL,
    GAMI_RESPONSE_TYPE_STRING,
    GAMI_RESPONSE_TYPE_HASH,
    GAMI_RESPONSE_TYPE_LIST
} GamiResponseType;

typedef struct _GamiActionHook GamiActionHook;
struct _GamiActionHook
{
    GamiResponseType type;

    union {
        GamiBoolResponseFunc   bool_func;
        GamiStringResponseFunc string_func;
        GamiHashResponseFunc   hash_func;
        GamiListResponseFunc   list_func;
    } user_func;

    gpointer  handler_data;
    gpointer  user_data;
};

typedef struct _GamiManagerPrivate GamiManagerPrivate;
struct _GamiManagerPrivate
{
    GIOChannel *socket;
    gboolean connected;
    gchar *host;
    gchar *port;

    GHashTable *action_hooks;
    GQueue *buffer;
};

#define GAMI_MANAGER_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
                                                            GAMI_TYPE_MANAGER, \
                                                            GamiManagerPrivate))

typedef struct _GamiManagerNewAsyncData GamiManagerNewAsyncData;
struct _GamiManagerNewAsyncData {
    const gchar *host;
    const gchar *port;
    GamiManagerNewAsyncFunc func;
    gpointer data;
};

enum {
    HOST_PROP = 1,
    PORT_PROP
};

enum {
    CONNECTED,
    DISCONNECTED,
    EVENT,
    LAST_SIGNAL
};

static guint signals [LAST_SIGNAL];

G_DEFINE_TYPE (GamiManager, gami_manager, G_TYPE_OBJECT);

static gboolean gami_manager_new_async_cb (GamiManagerNewAsyncData *data);
static gboolean parse_connection_string (GamiManager *ami, GError **error);
static gchar *event_string_from_mask (GamiManager *ami, GamiEventMask mask);

static gchar *get_action_id (const gchar *action_id);
static gboolean reconnect_socket (GamiManager *ami);

static GIOStatus send_command (GIOChannel *c, const gchar *cmd, GError **e);

static gboolean dispatch_ami (GIOChannel *chan,
                              GIOCondition cond,
                              GamiManager *ami);
static gboolean process_packets (GamiManager *manager);
static void add_action_hook (GamiManager *manager, gchar *action_id,
                             GamiActionHook *hook);

/* internal response functions to feed callbacks */
static gboolean process_bool_response (GHashTable *packet, gpointer expected);
static gchar *process_string_response (GHashTable *packet, gpointer return_key);
static GHashTable *process_hash_response (GHashTable *packet);
static gboolean process_list_response (GHashTable *packet,
                                       gpointer stop_event, GSList **resp);

/* response callbacks used internally in synchronous mode */
static void set_bool_response   (gboolean response, gboolean *store);
static void set_string_response (gchar *response, gchar **store);
static void set_hash_response   (GHashTable *response, GHashTable **store);
static void set_list_response   (GSList *response, GSList **store);

/* initialize action hooks */
static GamiActionHook *bool_action_hook_new (GamiBoolResponseFunc user_func,
                                             gpointer user_data,
                                             gpointer handler_data);
static GamiActionHook *string_action_hook_new (GamiStringResponseFunc user_func,
                                               gpointer user_data,
                                               gpointer handler_data);
static GamiActionHook *hash_action_hook_new (GamiHashResponseFunc user_func,
                                             gpointer user_data);
static GamiActionHook *list_action_hook_new (GamiListResponseFunc user_func,
                                             gpointer user_data,
                                             gpointer handler_data);

/* various helper funcs */
static gboolean check_response (GHashTable *p, const gchar *expected_value);
static void join_originate_vars (gchar *key, gchar *value, GString *s);
static void join_originate_vars_legacy (gchar *key, gchar *value, GString *s);
static void join_user_event_headers (gchar *key, gchar *value, GString *s);

/*
 * Public API
 */


/**
 * gami_manager_new:
 * @host: Asterisk manager host.
 * @port: Asterisk manager port.
 *
 * This function creates an instance of %GAMI_TYPE_MANAGER connected to
 * @host:@port.
 *
 * Returns: A new #GamiManager
 */
GamiManager *
gami_manager_new (const gchar *host, const gchar *port)
{
    GamiManager *ami;
    GamiManagerPrivate *priv;
    GError  *error = NULL;

    ami = g_object_new (GAMI_TYPE_MANAGER,
                        "host", host,
                        "port", port,
                        NULL);
    priv = GAMI_MANAGER_PRIVATE (ami);

    if (! gami_manager_connect (ami, &error)) {
        g_warning ("Failed to connect to the server%s%s",
                   error ? ": " : "",
                   error ? error->message : "");

        g_object_unref (ami);
        return NULL;
    }

    return ami;
}

/**
 * gami_manager_new_async:
 * @host: Asterisk manager host.
 * @port: Asterisk manager port.
 * @func: Callback function called when object has been created
 * @user_data: data to pass to @func
 *
 * Asynchronously create a #GamiManager connected to @host:@port. The new 
 * object will be passed as a parameter to @func when finished.
 */
void
gami_manager_new_async (const gchar *host, const gchar *port,
                        GamiManagerNewAsyncFunc func, gpointer user_data)
{
    GamiManagerNewAsyncData *data;
    data = g_new0 (GamiManagerNewAsyncData, 1);
    data->host = host;
    data->port = port;
    data->func = func;
    data->data = user_data;

    if (g_thread_supported ())
        g_thread_create ((GThreadFunc) gami_manager_new_async_cb, data,
                         FALSE, NULL);
    else
        g_idle_add ((GSourceFunc) gami_manager_new_async_cb, data);
}

/**
 * gami_manager_connect:
 * @ami: #GamiManager
 * @error: A location to return an error of type #GIOChannelError
 *
 * Connect #GamiManager with the Asterisk server defined by the object 
 * properties #GamiManager:host and #GamiManager:port.
 *
 * Note that it is not usually necessary to call this function, as it is called
 * by gami_manager_new() and gami_manager_new_async(). Use it only in classes 
 * inheritting from #GamiManager.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_connect (GamiManager *ami, GError **error)
{
    GamiManagerPrivate *priv;
    struct addrinfo hints;
    struct addrinfo *rp, *result = NULL;
    int rv;

    SOCKET sock = INVALID_SOCKET;

    g_assert (error == NULL || *error == NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    memset (&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if ((rv = getaddrinfo (priv->host, priv->port, &hints, &result)) != 0) {
        g_warning ("Error resolving host '%s': %s", priv->host,
                   gai_strerror (rv));
        return FALSE;
    }

    for (rp = result; rp; rp = rp->ai_next) {
        sock = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sock == INVALID_SOCKET)
            continue;

        if (connect (sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break;   /* Bingo! */

        CLOSESOCKET (sock);
        sock = INVALID_SOCKET;
    }


    if (rp == NULL) {
        /* Error */
        freeaddrinfo (result);

        return FALSE;
    }

    freeaddrinfo (result);

    priv->socket = G_SOCKET_IO_CHANNEL_NEW (sock);

    if (parse_connection_string (ami, error)) {
        priv->connected = TRUE;
        g_signal_emit (ami, signals [CONNECTED], 0);
    }

    g_io_channel_set_flags (priv->socket, G_IO_FLAG_NONBLOCK, error);
    g_io_add_watch (priv->socket, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP,
                    (GIOFunc) dispatch_ami, ami);

    return priv->connected;
}

/*
 * Login/Logoff
 */

/**
 * gami_manager_login:
 * @ami: #GamiManager
 * @username: Username to use for authentification
 * @secret: Password to use for authentification
 * @auth_type: (allow-none): AuthType to use for authentification - if set
 *             to "md5", @secret is expected to contain an MD5 hash of the
 *             result string of gami_manager_challenge() and the user's password
 * @events: Flags of type %GamiEventMask, indicating which events should be
 *          received initially. It is possible to modify this setting using the
 *          gami_manager_events() action
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Authenticate to asterisk and open a new manager session
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_login (GamiManager *ami, const gchar *username,
                    const gchar *secret, const gchar *auth_type,
                    GamiEventMask events, const gchar *action_id,
                    GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_login_async (ami, username, secret, auth_type,
                                         events, action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_login_async:
 * @ami: #GamiManager
 * @username: Username to use for authentification
 * @secret: Password to use for authentification
 * @auth_type: (allow-none): AuthType to use for authentification - if set
 *             to "md5", @secret is expected to contain an MD5 hash of the
 *             result string of gami_manager_challenge() and the user's password
 * @events: Flags of type %GamiEventMask, indicating which events should be
 *          received initially. It is possible to modify this setting using the
 *          gami_manager_events() action
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Authenticate to asterisk and open a new manager session
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_login_async (GamiManager *ami, const gchar *username,
                          const gchar *secret, const gchar *auth_type,
                          GamiEventMask events, const gchar *action_id,
                          GamiBoolResponseFunc response_func,
                          gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString  *action;
    gchar    *action_str;
    gchar    *action_id_new;
    gchar    *event_str;
    GIOStatus iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (username != NULL && secret != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Login\r\n");
    if (auth_type)
        g_string_append_printf (action, "AuthType: %s\r\n", auth_type);

    g_string_append_printf (action, "Username: %s\r\n%s: %s\r\n",
                            username, (auth_type) ? "Key" : "Secret", secret);

    event_str = event_string_from_mask (ami, events);
    g_string_append_printf (action, "Events: %s\r\n", event_str);
    g_free (event_str);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_logoff:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Close the manager session and disconnect from asterisk
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_logoff (GamiManager *ami, const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_logoff_async (ami, action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_logoff_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Close the manager session and disconnect from asterisk
 *
 * Returns: #GIOStatus of the send operation
 */
GIOStatus
gami_manager_logoff_async (GamiManager *ami, const gchar *action_id,
                           GamiBoolResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Logoff\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           (ami->api_major
                                            && ami->api_minor) ? "Success"
                                                               : "Goodbye"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 *  Get/Set Variables
 */

/**
 * gami_manager_get_var:
 * @ami: #GamiManager
 * @channel: (allow-none): Channel to retrieve variable from
 * @variable: Name of the variable to retrieve
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get value of @variable (either from @channel or as global)
 *
 * Returns: value of @variable or %NULL
 */
gchar *
gami_manager_get_var (GamiManager *ami, const gchar *channel,
                      const gchar *variable, const gchar *action_id,
                      GError **error)
{
    gchar *rv = NULL;
    GIOStatus  iostatus;

    GamiStringResponseFunc func = (GamiStringResponseFunc) set_string_response;
    iostatus = gami_manager_get_var_async (ami, channel, variable, action_id,
                                           func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return NULL;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_get_var_async:
 * @ami: #GamiManager
 * @channel: (allow-none): Channel to retrieve variable from
 * @variable: Name of the variable to retrieve
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get value of @variable (either from @channel or as global)
 *
 * Returns: #GIOStatus of the send operation
 */
GIOStatus
gami_manager_get_var_async (GamiManager *ami, const gchar *channel,
                            const gchar *variable, const gchar *action_id,
                            GamiStringResponseFunc response_func,
                            gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (variable != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: GetVar\r\n");
    g_string_append_printf (action, "Variable: %s\r\n", variable);

    if (channel != NULL)
        g_string_append_printf (action, "Channel: %s\r\n", channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     string_action_hook_new (response_func, response_data,
                                             "Value"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_set_var:
 * @ami: #GamiManager
 * @channel: (allow-none): Channel to set variable for
 * @variable: Name of the variable to set
 * @value: New value for @variable
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set @variable (optionally on channel @channel) to @value
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_set_var (GamiManager *ami, const gchar *channel,
                      const gchar *variable, const gchar *value,
                      const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_set_var_async (ami, channel, variable, value,
                                           action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_set_var_async:
 * @ami: #GamiManager
 * @channel: (allow-none): Channel to set variable for
 * @variable: Name of the variable to set
 * @value: New value for @variable
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set @variable (optionally on channel @channel) to @value
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_set_var_async (GamiManager *ami, const gchar *channel,
                            const gchar *variable, const gchar *value,
                            const gchar *action_id,
                            GamiBoolResponseFunc response_func,
                            gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (variable != NULL && value != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: SetVar\r\n");

    if (channel)
        g_string_append_printf (action, "Channel: %s\r\n", channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\n\n", action_id_new);

    g_string_append_printf (action, "Variable: %s\r\nValue: %s\r\n\r\n",
                            variable, value);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Module handling
 */

/**
 * gami_manager_module_check:
 * @ami: #GamiManager
 * @module: Asterisk module name (not including extension)
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Check whether @module is loaded
 *
 * Returns: %TRUE if @module is loaded, %FALSE otherwise
 */
gboolean
gami_manager_module_check (GamiManager *ami, const gchar *module,
                           const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_module_check_async (ami, module, action_id,
                                                func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_module_check_async:
 * @ami: #GamiManager
 * @module: Asterisk module name (not including extension)
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Check whether @module is loaded
 *
 * Returns: %TRUE if @module is loaded, %FALSE otherwise
 */
GIOStatus
gami_manager_module_check_async (GamiManager *ami, const gchar *module,
                                 const gchar *action_id,
                                 GamiBoolResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (module != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ModuleCheck\r\n");
    g_string_append_printf (action, "Module: %s\r\n", module);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_module_load:
 * @ami: #GamiManager
 * @module: Asterisk module name (not including extension)
 * @load_type: Load action to perform (load, reload or unload)
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Perform action indicated by @load_type for @module
 *
 * Returns: %TRUE if @module is loaded, %FALSE otherwise
 */
gboolean
gami_manager_module_load (GamiManager *ami, const gchar *module,
                          GamiModuleLoadType load_type, const gchar *action_id,
                          GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_module_load_async (ami, module, load_type,
                                               action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_module_load_async:
 * @ami: #GamiManager
 * @module: Asterisk module name (not including extension)
 * @load_type: Load action to perform (load, reload or unload)
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Perform action indicated by @load_type for @module
 *
 * Returns: %TRUE if @module is loaded, %FALSE otherwise
 */
GIOStatus
gami_manager_module_load_async (GamiManager *ami, const gchar *module,
                                GamiModuleLoadType load_type,
                                const gchar *action_id,
                                GamiBoolResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ModuleCheck\r\n");

    if (module)
        g_string_append_printf (action, "Module: %s\r\n", module);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    switch (load_type) {
        case GAMI_MODULE_LOAD:
            g_string_append (action, "LoadType: load\r\n");
            break;
        case GAMI_MODULE_RELOAD:
            g_string_append (action, "LoadType: reload\r\n");
            break;
        case GAMI_MODULE_UNLOAD:
            g_string_append (action, "LoadType: unload\r\n");
            break;
    }

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Monitor channels
 */

/**
 * gami_manager_monitor:
 * @ami: #GamiManager
 * @channel: Channel to start monitoring
 * @file: (allow-none): Filename to use for recording
 * @format: (allow-none): Format to use for recording
 * @mix: (allow-none): Whether to mix in / out channel into one file
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Start monitoring @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_monitor (GamiManager *ami, const gchar *channel, const gchar *file,
                      const gchar *format, gboolean mix, const gchar *action_id,
                      GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_monitor_async (ami, channel, file, format, mix,
                                           action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_monitor_async:
 * @ami: #GamiManager
 * @channel: Channel to start monitoring
 * @file: (allow-none): Filename to use for recording
 * @format: (allow-none): Format to use for recording
 * @mix: (allow-none): Whether to mix in / out channel into one file
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Start monitoring @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_monitor_async (GamiManager *ami, const gchar *channel,
                            const gchar *file, const gchar *format,
                            gboolean mix, const gchar *action_id,
                            GamiBoolResponseFunc response_func,
                            gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Monitor\r\n");
    g_string_append_printf (action, "Channel: %s\r\n", channel);

    if (file != NULL)
        g_string_append_printf (action, "File: %s\r\n", file);
    if (format != NULL)
        g_string_append_printf (action, "Format: %s\r\n", format);
    if (mix)
        g_string_append (action, "Mix: 1\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_change_monitor:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @file: New filename to use for recording
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Change the file name of the recording occuring on @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_change_monitor (GamiManager *ami, const gchar *channel,
                             const gchar *file, const gchar *action_id,
                             GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_change_monitor_async (ami, channel, file, action_id,
                                                  func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_change_monitor_async:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @file: New filename to use for recording
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Change the file name of the recording occuring on @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_change_monitor_async (GamiManager *ami, const gchar *channel,
                                   const gchar *file, const gchar *action_id,
                                   GamiBoolResponseFunc response_func,
                                   gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL && file != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ChangeMonitor\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Channel: %s\r\nFile: %s\r\n\r\n",
                            channel, file);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_stop_monitor:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Stop monitoring @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_stop_monitor (GamiManager *ami, const gchar *channel,
                           const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_stop_monitor_async (ami, channel, action_id,
                                                func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_stop_monitor_async:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Stop monitoring @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_stop_monitor_async (GamiManager *ami, const gchar *channel,
                                 const gchar *action_id,
                                 GamiBoolResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: StopMonitor\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Channel: %s\r\n\r\n", channel);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_pause_monitor:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Pause monitoring of @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_pause_monitor (GamiManager *ami, const gchar *channel,
                            const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_pause_monitor_async (ami, channel, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_pause_monitor_async:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Pause monitoring of @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_pause_monitor_async (GamiManager *ami, const gchar *channel,
                                  const gchar *action_id,
                                  GamiBoolResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: PauseMonitor\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Channel: %s\r\n\r\n", channel);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_unpause_monitor:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Continue monitoring of @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_unpause_monitor (GamiManager *ami, const gchar *channel,
                              const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_unpause_monitor_async (ami, channel, action_id,
                                                   func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_unpause_monitor_async:
 * @ami: #GamiManager
 * @channel: Monitored channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Continue monitoring of @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_unpause_monitor_async (GamiManager *ami, const gchar *channel,
                                    const gchar *action_id,
                                    GamiBoolResponseFunc response_func, 
                                    gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: UnpauseMonitor\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Channel: %s\r\n\r\n", channel);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Meetme
 */

/**
 * gami_manager_meetme_mute:
 * @ami: #GamiManager
 * @meetme: The MeetMe conference bridge number
 * @user_num: The user number in the specified bridge
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Mutes @user_num in conference @meetme
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_meetme_mute (GamiManager *ami, const gchar *meetme,
                          const gchar *user_num, const gchar *action_id,
                          GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_meetme_mute_async (ami, meetme, user_num, action_id,
                                               func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_meetme_mute_async:
 * @ami: #GamiManager
 * @meetme: The MeetMe conference bridge number
 * @user_num: The user number in the specified bridge
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Mutes @user_num in conference @meetme
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_meetme_mute_async (GamiManager *ami, const gchar *meetme,
                                const gchar *user_num, const gchar *action_id,
                                GamiBoolResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (meetme != NULL && user_num != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: MeetmeMute\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Meetme: %s\r\nUserNum: %s\r\n\r\n",
                            meetme, user_num);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_meetme_unmute:
 * @ami: #GamiManager
 * @meetme: The MeetMe conference bridge number
 * @user_num: The user number in the specified bridge
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Unmutes @user_num in conference @meetme
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_meetme_unmute (GamiManager *ami, const gchar *meetme,
                            const gchar *user_num, const gchar *action_id,
                            GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_meetme_unmute_async (ami, meetme, user_num,
                                                 action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_meetme_unmute_async:
 * @ami: #GamiManager
 * @meetme: The MeetMe conference bridge number
 * @user_num: The user number in the specified bridge
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Unmutes @user_num in conference @meetme
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_meetme_unmute_async (GamiManager *ami, const gchar *meetme,
                                  const gchar *user_num, const gchar *action_id,
                                  GamiBoolResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (meetme != NULL && user_num != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: MeetmeUnmute\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Meetme: %s\r\nUserNum: %s\r\n\r\n",
                            meetme, user_num);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_meetme_list:
 * @ami: #GamiManager
 * @meetme: The MeetMe conference bridge number
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * List al users in conference @meetme
 *
 * Returns: #GSList of user information (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_meetme_list (GamiManager *ami, const gchar *meetme,
                          const gchar *action_id, GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_meetme_list_async (ami, meetme, action_id,
                                               func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return NULL;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_meetme_list_async:
 * @ami: #GamiManager
 * @meetme: The MeetMe conference bridge number
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * List al users in conference @meetme
 *
 * Returns: #GSList of user information (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_meetme_list_async (GamiManager *ami, const gchar *meetme,
                                const gchar *action_id,
                                GamiListResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: MeetmeList\r\n");

    if (meetme)
        g_string_append_printf (action, "Conference: %s", meetme);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "MeetMeListComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Queue management
 */

/**
 * gami_manager_queue_add:
 * @ami: #GamiManager
 * @queue: Existing queue to add member
 * @iface: Member interface to add to @queue
 * @penalty: Penalty for new member
 * @paused: whether @iface should be initially paused
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Add @iface to @queue
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_queue_add (GamiManager *ami, const gchar *queue,
                        const gchar *iface, guint penalty, gboolean paused,
                        const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_queue_add_async (ami, queue, iface, penalty,
                                             paused, action_id, func, &rv,
                                             error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_queue_add_async:
 * @ami: #GamiManager
 * @queue: Existing queue to add member
 * @iface: Member interface to add to @queue
 * @penalty: Penalty for new member
 * @paused: whether @iface should be initially paused
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Add @iface to @queue
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_queue_add_async (GamiManager *ami, const gchar *queue,
                              const gchar *iface, guint penalty,
                              gboolean paused, const gchar *action_id,
                              GamiBoolResponseFunc response_func,
                              gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (queue != NULL && iface != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: QueueAdd\r\n");
    g_string_append_printf (action, "Queue: %s\r\nInterface: %s\r\n",
                            queue, iface);

    if (penalty)
        g_string_append_printf (action, "Penalty: %d\r\n", penalty);
    if (paused)
        g_string_append (action, "Paused: 1\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_queue_remove:
 * @ami: #GamiManager
 * @queue: Existing queue to remove member from
 * @iface: Member interface to remove from @queue
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Remove @iface from @queue
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_queue_remove (GamiManager *ami, const gchar *queue,
                           const gchar *iface, const gchar *action_id,
                           GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_queue_remove_async (ami, queue, iface, action_id,
                                                func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_queue_remove_async:
 * @ami: #GamiManager
 * @queue: Existing queue to remove member from
 * @iface: Member interface to remove from @queue
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Remove @iface from @queue
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_queue_remove_async (GamiManager *ami, const gchar *queue,
                                 const gchar *iface, const gchar *action_id,
                                 GamiBoolResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (queue != NULL && iface != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: QueueRemove\r\n");
    g_string_append_printf (action, "Queue: %s\r\nInterface: %s\r\n",
                            queue, iface);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_queue_pause:
 * @ami: #GamiManager
 * @queue: (allow-none): Existing queue for which @iface should be (un)paused
 * @iface: Member interface (un)pause
 * @paused: Whether to pause or unpause @iface
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * (Un)pause @iface
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_queue_pause (GamiManager *ami, const gchar *queue,
                          const gchar *iface, gboolean paused,
                          const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_queue_pause_async (ami, queue, iface, paused,
                                               action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_queue_pause_async:
 * @ami: #GamiManager
 * @queue: (allow-none): Existing queue for which @iface should be (un)paused
 * @iface: Member interface (un)pause
 * @paused: Whether to pause or unpause @iface
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * (Un)pause @iface
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_queue_pause_async (GamiManager *ami, const gchar *queue,
                                const gchar *iface, gboolean paused,
                                const gchar *action_id,
                                GamiBoolResponseFunc response_func, 
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (iface != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: QueuePause\r\n");
    g_string_append_printf (action, "Interface: %s\r\nPaused: %d\r\n",
                            iface, paused ? 1: 0);

    if (queue)
        g_string_append_printf (action, "Queue: %s\r\n", queue);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_queue_penalty:
 * @ami: #GamiManager
 * @queue: (allow-none): Limit @penalty change to existing queue
 * @iface: Member interface change penalty for
 * @penalty: New penalty to set for @iface
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Change the penalty value of @iface
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_queue_penalty (GamiManager *ami, const gchar *queue,
                            const gchar *iface, guint penalty,
                            const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_queue_penalty_async (ami, queue, iface, penalty,
                                                 action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_queue_penalty_async:
 * @ami: #GamiManager
 * @queue: (allow-none): Limit @penalty change to existing queue
 * @iface: Member interface change penalty for
 * @penalty: New penalty to set for @iface
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Change the penalty value of @iface
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_queue_penalty_async (GamiManager *ami, const gchar *queue,
                                  const gchar *iface, guint penalty,
                                  const gchar *action_id,
                                  GamiBoolResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (iface != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: QueuePenalty\r\n");
    g_string_append_printf (action, "Interface: %s\r\nPenalty: %d\r\n",
                            iface, penalty);

    if (queue)
        g_string_append_printf (action, "Queue: %s\r\n", queue);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_queue_summary:
 * @ami: #GamiManager
 * @queue: (allow-none): Only send summary information for @queue
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get summary of queue statistics
 *
 * Returns: #GSList of queue statistics (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_queue_summary (GamiManager *ami, const gchar *queue,
                            const gchar *action_id, GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_queue_summary_async (ami, queue, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_queue_summary_async:
 * @ami: #GamiManager
 * @queue: (allow-none): Only send summary information for @queue
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get summary of queue statistics
 *
 * Returns: #GSList of queue statistics (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_queue_summary_async (GamiManager *ami, const gchar *queue,
                                  const gchar *action_id,
                                  GamiListResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: QueueSummary\r\n");

    if (queue)
        g_string_append_printf (action, "Queue: %s\r\n", queue);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "QueueSummaryComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_queue_log:
 * @ami: #GamiManager
 * @queue: Queue to generate queue_log entry for
 * @event: Log event to generate
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Generate a queue_log entry for @queue
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_queue_log (GamiManager *ami, const gchar *queue,
                        const gchar *event, const gchar *action_id,
                        GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_queue_log_async (ami, queue, event, action_id,
                                             func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_queue_log_async:
 * @ami: #GamiManager
 * @queue: Queue to generate queue_log entry for
 * @event: Log event to generate
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Generate a queue_log entry for @queue
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_queue_log_async (GamiManager *ami, const gchar *queue,
                              const gchar *event, const gchar *action_id,
                              GamiBoolResponseFunc response_func,
                              gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (queue != NULL && event != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: QueueLog\r\n");
    g_string_append_printf (action, "Queue: %s\r\nEvent: %s\r\n",
                            queue, event);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

#if 0
    GSList *
gami_manager_queue_status (GamiManager *ami, const gchar *queue,
                           const gchar *action_id, GError **error)
{
    GamiManagerPrivate *priv;
    GString *action;
    gchar *action_str;

    GSList *list = NULL;
    gboolean list_complete = FALSE;

    g_return_val_if_fail (ami != NULL && GAMI_IS_MANAGER (ami), FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    priv = GAMI_MANAGER_PRIVATE (ami);

    action = g_string_new ("Action: QueueStatus\r\n");

    if (queue)
        g_string_append_printf (action, "Queue: %s\r\n", queue);
    if (action_id)
        g_string_append_printf (action, "ActionID: %s\r\n", action_id);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    if (send_command (priv->socket, action_str, error) != G_IO_STATUS_NORMAL) {
        g_assert (error == NULL || *error != NULL);
        return NULL;
    }

    g_assert (error == NULL || *error == NULL);

    g_free (action_str);

    if (! check_response (priv->socket, "Response", "Success", error))
        return NULL;

    g_assert (error == NULL || *error == NULL);

    while (! list_complete) {
        GIOStatus status;
        GHashTable *packet = NULL;
        gchar *event;

        while ((status = receive_packet (priv->socket,
                                         &packet,
                                         error)) == G_IO_STATUS_AGAIN);

        if (status != G_IO_STATUS_NORMAL) {
            g_assert (error == NULL || *error != NULL);

            if (list) {
                g_slist_foreach (list, (GFunc)g_hash_table_destroy, NULL);
                g_slist_free (list);
            }

            return NULL;
        }

        g_assert (error == NULL || *error == NULL);

        event = g_hash_table_lookup (packet, "Event");
        if (! strcmp (event, "QueueParam")) {
            g_hash_table_remove (packet, "Event");
            list = g_slist_prepend (list, packet);
            packet = NULL;
        } else if (! strcmp (event, "QueueStatusComplete")) {
            list_complete = TRUE;
            g_hash_table_destroy (packet);
        } else {
            /* this is just a test, we should get rid of this longterm */
            //printf ("Ups, unexpected event in get_response_list(): %s\n",
            //        event);
            g_hash_table_destroy (packet);
            packet = NULL;
        }
    }

    list = g_slist_reverse (list);

    return list;
}
#endif


/*
 * ZAP Channels
 */

/**
 * gami_manager_zap_dial_offhook
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel on which to dial @number
 * @number: The number to dial
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Dial over ZAP channel while offhook
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_zap_dial_offhook (GamiManager *ami, const gchar *zap_channel,
                               const gchar *number, const gchar *action_id,
                               GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_zap_dial_offhook_async (ami, zap_channel, number,
                                                    action_id, func, &rv,
                                                    error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_zap_dial_offhook_async
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel on which to dial @number
 * @number: The number to dial
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Dial over ZAP channel while offhook
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_zap_dial_offhook_async (GamiManager *ami, const gchar *zap_channel,
                                     const gchar *number,
                                     const gchar *action_id,
                                     GamiBoolResponseFunc response_func,
                                     gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (zap_channel != NULL && number != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ZapDialOffhook\r\n");
    g_string_append_printf (action, "ZapChannel: %s\r\nNumber: %s\r\n",
                            zap_channel, number);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_zap_hangup:
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel to hang up
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Hangup ZAP channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_zap_hangup (GamiManager *ami, const gchar *zap_channel,
                         const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_zap_hangup_async (ami, zap_channel, action_id,
                                              func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_zap_hangup_async:
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel to hang up
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Hangup ZAP channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_zap_hangup_async (GamiManager *ami, const gchar *zap_channel,
                         const gchar *action_id,
                         GamiBoolResponseFunc response_func,
                         gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (zap_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ZapHangup\r\n");
    g_string_append_printf (action, "ZapChannel: %s\r\n", zap_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_zap_dnd_on:
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel on which to turn on DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @zap_channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_zap_dnd_on (GamiManager *ami, const gchar *zap_channel,
                         const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_zap_dnd_on_async (ami, zap_channel, action_id,
                                              func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_zap_dnd_on_async:
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel on which to turn on DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @zap_channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_zap_dnd_on_async (GamiManager *ami, const gchar *zap_channel,
                               const gchar *action_id,
                               GamiBoolResponseFunc response_func,
                               gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (zap_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ZapDNDOn\r\n");
    g_string_append_printf (action, "ZapChannel: %s\r\n", zap_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_zap_dnd_off:
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel on which to turn off DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @zap_channel to off
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_zap_dnd_off (GamiManager *ami, const gchar *zap_channel,
                          const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_zap_dnd_off_async (ami, zap_channel, action_id,
                                               func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_zap_dnd_off_async:
 * @ami: #GamiManager
 * @zap_channel: The ZAP channel on which to turn off DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @zap_channel to off
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_zap_dnd_off_async (GamiManager *ami, const gchar *zap_channel,
                                const gchar *action_id,
                                GamiBoolResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (zap_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ZapDNDOff\r\n");
    g_string_append_printf (action, "ZapChannel: %s\r\n", zap_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_zap_show_channels:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Show the status of all ZAP channels
 *
 * Returns: #GSList of ZAP channels (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_zap_show_channels (GamiManager *ami, const gchar *action_id,
                                GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_zap_show_channels_async (ami, action_id,
                                                     func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_zap_show_channels_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Show the status of all ZAP channels
 *
 * Returns: #GSList of ZAP channels (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_zap_show_channels_async (GamiManager *ami, const gchar *action_id,
                                      GamiListResponseFunc response_func,
                                      gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ZapShowChannels\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "ZapShowChannelsComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_zap_transfer:
 * @ami: #GamiManager
 * @zap_channel: The channel to be transferred
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Transfer ZAP channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_zap_transfer (GamiManager *ami, const gchar *zap_channel,
                           const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_zap_transfer_async (ami, zap_channel, action_id,
                                               func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_zap_transfer_async:
 * @ami: #GamiManager
 * @zap_channel: The channel to be transferred
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Transfer ZAP channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_zap_transfer_async (GamiManager *ami, const gchar *zap_channel,
                                 const gchar *action_id, 
                                 GamiBoolResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (zap_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ZapTransfer\r\n");
    g_string_append_printf (action, "ZapChannel: %s\r\n", zap_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_zap_restart:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Restart ZAP channels. Any active calls will be terminated
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_zap_restart (GamiManager *ami, const gchar *action_id,
                          GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_zap_restart_async (ami, action_id,
                                               func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_zap_restart_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Restart ZAP channels. Any active calls will be terminated
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_zap_restart_async (GamiManager *ami, const gchar *action_id,
                                GamiBoolResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ZapRestart\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * DAHDI
 */

/**
 * gami_manager_dahdi_dial_offhook:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel on which to dial @number
 * @number: The number to dial
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Dial over DAHDI channel while offhook
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_dahdi_dial_offhook (GamiManager *ami, const gchar *dahdi_channel,
                                 const gchar *number, const gchar *action_id,
                                 GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_dahdi_dial_offhook_async (ami, dahdi_channel,
                                                      number, action_id,
                                                      func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_dahdi_dial_offhook_async:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel on which to dial @number
 * @number: The number to dial
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Dial over DAHDI channel while offhook
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_dahdi_dial_offhook_async (GamiManager *ami,
                                       const gchar *dahdi_channel,
                                       const gchar *number,
                                       const gchar *action_id,
                                       GamiBoolResponseFunc response_func,
                                       gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (dahdi_channel != NULL && number != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DAHDIDialOffhook\r\n");
    g_string_append_printf (action, "DAHDIChannel: %s\r\nNumber: %s\r\n",
                            dahdi_channel, number);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_dahdi_hangup:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel to hang up
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Hangup DAHDI channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_dahdi_hangup (GamiManager *ami, const gchar *dahdi_channel,
                           const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_dahdi_hangup_async (ami, dahdi_channel, action_id,
                                                func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_dahdi_hangup_async:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel to hang up
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Hangup DAHDI channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_dahdi_hangup_async (GamiManager *ami, const gchar *dahdi_channel,
                                 const gchar *action_id,
                                 GamiBoolResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (dahdi_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DAHDIHangup\r\n");
    g_string_append_printf (action, "DAHDIChannel: %s\r\n", dahdi_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_dahdi_dnd_on:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel on which to turn on DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @dahdi_channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_dahdi_dnd_on (GamiManager *ami, const gchar *dahdi_channel,
                           const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_dahdi_dnd_on_async (ami, dahdi_channel, action_id,
                                                func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_dahdi_dnd_on_async:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel on which to turn on DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @dahdi_channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_dahdi_dnd_on_async (GamiManager *ami, const gchar *dahdi_channel,
                                 const gchar *action_id,
                                 GamiBoolResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (dahdi_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DAHDIDNDOn\r\n");
    g_string_append_printf (action, "DAHDIChannel: %s\r\n", dahdi_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_dahdi_dnd_off:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel on which to turn off DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @dahdi_channel to off
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_dahdi_dnd_off (GamiManager *ami, const gchar *dahdi_channel,
                            const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_dahdi_dnd_off_async (ami, dahdi_channel, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_dahdi_dnd_off_async:
 * @ami: #GamiManager
 * @dahdi_channel: The DAHDI channel on which to turn off DND status
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set DND (Do Not Disturb) status on @dahdi_channel to off
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_dahdi_dnd_off_async (GamiManager *ami, const gchar *dahdi_channel,
                                  const gchar *action_id,
                                  GamiBoolResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (dahdi_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DAHDIDNDOff\r\n");
    g_string_append_printf (action, "DAHDIChannel: %s\r\n", dahdi_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_dahdi_show_channels:
 * @ami: #GamiManager
 * @dahdi_channel: (allow-none): Limit status information to this channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Show the status of all DAHDI channels
 *
 * Returns: #GSList of DAHDI channels (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_dahdi_show_channels (GamiManager *ami, const gchar *dahdi_channel,
                                  const gchar *action_id, GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_dahdi_show_channels_async (ami, dahdi_channel,
                                                       action_id, func, &rv,
                                                       error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_dahdi_show_channels_async:
 * @ami: #GamiManager
 * @dahdi_channel: (allow-none): Limit status information to this channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Show the status of all DAHDI channels
 *
 * Returns: #GSList of DAHDI channels (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_dahdi_show_channels_async (GamiManager *ami,
                                        const gchar *dahdi_channel,
                                        const gchar *action_id,
                                        GamiListResponseFunc response_func,
                                        gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DAHDIShowChannels\r\n");

    if (dahdi_channel)
        g_string_append_printf (action, "DAHDIChannel: %s\r\n", dahdi_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "DAHDIShowChannelsComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_dahdi_transfer:
 * @ami: #GamiManager
 * @dahdi_channel: The channel to be transferred
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Transfer DAHDI channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_dahdi_transfer (GamiManager *ami, const gchar *dahdi_channel,
                             const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_dahdi_transfer_async (ami, dahdi_channel, action_id,
                                                  func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_dahdi_transfer_async:
 * @ami: #GamiManager
 * @dahdi_channel: The channel to be transferred
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Transfer DAHDI channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_dahdi_transfer_async (GamiManager *ami, const gchar *dahdi_channel,
                                   const gchar *action_id,
                                   GamiBoolResponseFunc response_func,
                                   gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (dahdi_channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DAHDITransfer\r\n");
    g_string_append_printf (action, "DAHDIChannel: %s\r\n", dahdi_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_dahdi_restart:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Restart DAHDI channels. Any active calls will be terminated
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_dahdi_restart (GamiManager *ami, const gchar *action_id,
                            GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_dahdi_restart_async (ami, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_dahdi_restart_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Restart DAHDI channels. Any active calls will be terminated
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_dahdi_restart_async (GamiManager *ami, const gchar *action_id,
                                  GamiBoolResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DAHDIRestart\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Agents
 */

/**
 * gami_manager_agents:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * List information about all configured agents and their status
 *
 * Returns: #GSList of agents (stored as #GHashTable) on success,
 *           %NULL on failure
 */
GSList *
gami_manager_agents (GamiManager *ami, const gchar *action_id, GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_agents_async (ami, action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_agents_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * List information about all configured agents and their status
 *
 * Returns: #GSList of agents (stored as #GHashTable) on success,
 *           %NULL on failure
 */
GIOStatus
gami_manager_agents_async (GamiManager *ami, const gchar *action_id, 
                           GamiListResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Agents\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "AgentsComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_agent_callback_login:
 * @ami: #GamiManager
 * @agent: The ID of the agent to log in
 * @exten: The extension to use as callback
 * @context: (allow-none): The context to use as callback
 * @ack_call: (allow-none): Whether calls should be acknowledged by the agent
 *            (by pressing #)
 * @wrapup_time: (allow-none): The minimum amount of time after hangup before
 *            the agent will receive a new call
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Log in @agent and register callback to @exten (note that the action has 
 * been deprecated in asterisk-1.4 and was removed in asterisk-1.6)
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_agent_callback_login (GamiManager *ami, const gchar *agent,
                                   const gchar *exten, const gchar *context,
                                   gboolean ack_call, guint wrapup_time,
                                   const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_agent_callback_login_async (ami, agent, exten,
                                                        context, ack_call,
                                                        wrapup_time, action_id,
                                                        func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_agent_callback_login_async:
 * @ami: #GamiManager
 * @agent: The ID of the agent to log in
 * @exten: The extension to use as callback
 * @context: (allow-none): The context to use as callback
 * @ack_call: (allow-none): Whether calls should be acknowledged by the agent
 *            (by pressing #)
 * @wrapup_time: (allow-none): The minimum amount of time after hangup before
 *            the agent will receive a new call
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Log in @agent and register callback to @exten (note that the action has 
 * been deprecated in asterisk-1.4 and was removed in asterisk-1.6)
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_agent_callback_login_async (GamiManager *ami, const gchar *agent,
                                         const gchar *exten,
                                         const gchar *context,
                                         gboolean ack_call, guint wrapup_time,
                                         const gchar *action_id,
                                         GamiBoolResponseFunc response_func,
                                         gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (agent != NULL && exten != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: AgentCallbackLogin\r\n");
    g_string_append_printf (action, "Agent: %s\r\nExten: %s\r\n", agent, exten);

    if (context)
        g_string_append_printf (action, "Context: %s\r\n", context);
    if (ack_call)
        g_string_append (action, "AckCall: 1\r\n");
    if (wrapup_time)
        g_string_append_printf (action, "WrapupTime: %d\r\n", wrapup_time);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_agent_logoff:
 * @ami: #GamiManager
 * @agent: The ID of the agent to log off
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Log off @agent
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_agent_logoff (GamiManager *ami, const gchar *agent,
                           const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_agent_logoff_async (ami, agent, action_id,
                                                func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_agent_logoff_async:
 * @ami: #GamiManager
 * @agent: The ID of the agent to log off
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Log off @agent
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_agent_logoff_async (GamiManager *ami, const gchar *agent,
                                 const gchar *action_id,
                                 GamiBoolResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (agent != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: AgentLogoff\r\n");
    g_string_append_printf (action, "Agent: %s\r\n", agent);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/*
 * DB
 */

/**
 * gami_manager_db_get:
 * @ami: #GamiManager
 * @family: The AstDB key family from which to retrieve the value
 * @key: The name of the AstDB key
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve value of AstDB entry @family/@key
 *
 * Returns: the value of @family/@key on success, %NULL on failure
 */
gchar *
gami_manager_db_get (GamiManager *ami, const gchar *family, const gchar *key,
                     const gchar *action_id, GError **error)
{
    gchar     *rv = NULL;
    GIOStatus  iostatus;

    GamiStringResponseFunc func = (GamiStringResponseFunc) set_string_response;
    iostatus = gami_manager_db_get_async (ami, family, key, action_id,
                                          func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_db_get_async:
 * @ami: #GamiManager
 * @family: The AstDB key family from which to retrieve the value
 * @key: The name of the AstDB key
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve value of AstDB entry @family/@key
 *
 * Returns: the value of @family/@key on success, %FALSE on failure
 */
GIOStatus
gami_manager_db_get_async (GamiManager *ami, const gchar *family,
                           const gchar *key, const gchar *action_id,
                           GamiStringResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (family != NULL && key != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DBGet\r\n");
    g_string_append_printf (action, "Family: %s\r\nKey: %s\r\n", family, key);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     string_action_hook_new (response_func, response_data,
                                             "Val"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_db_put:
 * @ami: #GamiManager
 * @family: The AstDB key family in which to set the value
 * @key: The name of the AstDB key
 * @val: The value to assign to the key
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set AstDB entry @family/@key to @value
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_db_put (GamiManager *ami, const gchar *family, const gchar *key,
                     const gchar *val, const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_db_put_async (ami, family, key, val, action_id,
                                          func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_db_put_async:
 * @ami: #GamiManager
 * @family: The AstDB key family in which to set the value
 * @key: The name of the AstDB key
 * @val: The value to assign to the key
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set AstDB entry @family/@key to @value
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_db_put_async (GamiManager *ami, const gchar *family,
                           const gchar *key, const gchar *val,
                           const gchar *action_id,
                           GamiBoolResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (family != NULL && key != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DBPut\r\n");
    g_string_append_printf (action, "Family: %s\r\nKey: %s\r\n", family, key);

    if (val)
        g_string_append_printf (action, "Val: %s\r\n", val);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_db_del:
 * @ami: #GamiManager
 * @family: The AstDB key family in which to delete the key
 * @key: The name of the AstDB key to delete
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Remove AstDB entry @family/@key
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_db_del (GamiManager *ami, const gchar *family, const gchar *key,
                     const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_db_del_async (ami, family, key, action_id,
                                          func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_db_del_async:
 * @ami: #GamiManager
 * @family: The AstDB key family in which to delete the key
 * @key: The name of the AstDB key to delete
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Remove AstDB entry @family/@key
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_db_del_async (GamiManager *ami, const gchar *family,
                           const gchar *key, const gchar *action_id,
                           GamiBoolResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (family != NULL && key != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DBDel\r\n");
    g_string_append_printf (action, "Family: %s\r\nKey: %s\r\n", family, key);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_db_del_tree:
 * @ami: #GamiManager
 * @family: The AstDB key family to delete
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Remove AstDB key family
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_db_del_tree (GamiManager *ami, const gchar *family,
                          const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_db_del_tree_async (ami, family, action_id,
                                               func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_db_del_tree_async:
 * @ami: #GamiManager
 * @family: The AstDB key family to delete
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Remove AstDB key family
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_db_del_tree_async (GamiManager *ami, const gchar *family,
                                const gchar *action_id,
                                GamiBoolResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (family != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: DBDelTree\r\n");
    g_string_append_printf (action, "Family: %s\r\n", family);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Call Parking
 */

/**
 * gami_manager_park:
 * @ami: #GamiManager
 * @channel: Channel name to park
 * @channel2: Channel to announce park info to (and return the call to if the
 *            parking times out)
 * @timeout: (allow-none): Milliseconds to wait before callback
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Park a channel in the parking lot
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_park (GamiManager *ami, const gchar *channel,
                   const gchar *channel2, guint timeout, const gchar *action_id,
                   GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_park_async (ami, channel, channel2, timeout,
                                        action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_park_async:
 * @ami: #GamiManager
 * @channel: Channel name to park
 * @channel2: Channel to announce park info to (and return the call to if the
 *            parking times out)
 * @timeout: (allow-none): Milliseconds to wait before callback
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Park a channel in the parking lot
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_park_async (GamiManager *ami, const gchar *channel,
                         const gchar *channel2, guint timeout,
                         const gchar *action_id,
                         GamiBoolResponseFunc response_func,
                         gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL && channel2 != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Park\r\n");
    g_string_append_printf (action, "Channel: %s\r\nChannel2: %s\r\n",
                            channel, channel2);

    if (timeout)
        g_string_append_printf (action, "Timeout: %d\r\n", timeout);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_parked_calls:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of parked calls
 *
 * Returns: #GSList of parked calls (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_parked_calls (GamiManager *ami, const gchar *action_id,
                           GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_parked_calls_async (ami, action_id,
                                                func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_parked_calls_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of parked calls
 *
 * Returns: #GSList of parked calls (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_parked_calls_async (GamiManager *ami, const gchar *action_id,
                                 GamiListResponseFunc response_func,
                                 gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ParkedCalls\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "ParkedCallsComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Mailboxes
 */

/**
 * gami_manager_voicemail_users_list:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of voicemail users
 *
 * Returns: #GSList of voicemail users (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_voicemail_users_list (GamiManager *ami, const gchar *action_id,
                                   GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_voicemail_users_list_async (ami, action_id,
                                                        func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_voicemail_users_list_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of voicemail users
 *
 * Returns: #GSList of voicemail users (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_voicemail_users_list_async (GamiManager *ami,
                                         const gchar *action_id,
                                         GamiListResponseFunc response_func,
                                         gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: VoicemailUsersList\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "VoicemailUserEntryComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_mailbox_count:
 * @ami: #GamiManager
 * @mailbox: The mailbox to check messages for
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve count of new and old messages in @mailbox
 *
 * Returns: #GHashTable with message counts on success, %NULL on failure
 */
GHashTable *
gami_manager_mailbox_count (GamiManager *ami, const gchar *mailbox,
                            const gchar *action_id, GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_mailbox_count_async (ami, mailbox, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_mailbox_count_async:
 * @ami: #GamiManager
 * @mailbox: The mailbox to check messages for
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve count of new and old messages in @mailbox
 *
 * Returns: #GHashTable with message counts on success, %NULL on failure
 */
GIOStatus
gami_manager_mailbox_count_async (GamiManager *ami, const gchar *mailbox,
                                  const gchar *action_id,
                                  GamiHashResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (mailbox != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: MailboxCount\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Mailbox: %s\r\n\r\n", mailbox);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_mailbox_status:
 * @ami: #GamiManager
 * @mailbox: The mailbox to check status for
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Check the status of @mailbox
 *
 * Returns: #GHashTable with status variables on success, %NULL on failure
 */
GHashTable *
gami_manager_mailbox_status (GamiManager *ami, const gchar *mailbox,
                             const gchar *action_id, GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_mailbox_status_async (ami, mailbox, action_id,
                                                  func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_mailbox_status_async:
 * @ami: #GamiManager
 * @mailbox: The mailbox to check status for
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Check the status of @mailbox
 *
 * Returns: #GHashTable with status variables on success, %NULL on failure
 */
GIOStatus
gami_manager_mailbox_status_async (GamiManager *ami, const gchar *mailbox,
                                   const gchar *action_id,
                                   GamiHashResponseFunc response_func,
                                   gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (mailbox != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: MailboxStatus\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Mailbox: %s\r\n\r\n", mailbox);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Core
 */

/**
 * gami_manager_core_status:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve information about the current PBX core status (as active calls,
 * startup time etc.)
 *
 * Returns: #GHashTable with status variables on success, %NULL on failure
 */
GHashTable *
gami_manager_core_status (GamiManager *ami, const gchar *action_id,
                          GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_core_status_async (ami, action_id,
                                               func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_core_status_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve information about the current PBX core status (as active calls,
 * startup time etc.)
 *
 * Returns: #GHashTable with status variables on success, %NULL on failure
 */
GIOStatus
gami_manager_core_status_async (GamiManager *ami, const gchar *action_id,
                                GamiHashResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: CoreStatus\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_core_show_channels:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of currently active channels
 *
 * Returns: #GSList of active channels (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_core_show_channels (GamiManager *ami, const gchar *action_id,
                                 GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_core_show_channels_async (ami, action_id,
                                                      func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_core_show_channels_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of currently active channels
 *
 * Returns: #GSList of active channels (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_core_show_channels_async (GamiManager *ami, const gchar *action_id,
                                       GamiListResponseFunc response_func,
                                       gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: CoreShowChannels\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "CoreShowChannelsComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_core_settings:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve information about PBX core settings (as Asterisk/GAMI version etc.)
 *
 * Returns: #GHashTable with settings variables on success, %NULL on failure
 */
GHashTable *
gami_manager_core_settings (GamiManager *ami, const gchar *action_id,
                            GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_core_settings_async (ami, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_core_settings_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve information about PBX core settings (as Asterisk/GAMI version etc.)
 *
 * Returns: #GHashTable with settings variables on success, %NULL on failure
 */
GIOStatus
gami_manager_core_settings_async (GamiManager *ami, const gchar *action_id,
                                  GamiHashResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: CoreSettings\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/*
 * Misc (TODO: Sort these out and order properly)
 */

/**
 * gami_manager_iax_peer_list:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of IAX2 peers
 *
 * Returns: #GSList of IAX2 peers (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_iax_peer_list (GamiManager *ami, const gchar *action_id,
                            GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_iax_peer_list_async (ami, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_iax_peer_list_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of IAX2 peers
 *
 * Returns: #GSList of IAX2 peers (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_iax_peer_list_async (GamiManager *ami, const gchar *action_id,
                                  GamiListResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: IAXpeerlist\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "PeerlistComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_sip_peers:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of SIP peers
 *
 * Returns: #GSList of SIP peers (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_sip_peers (GamiManager *ami, const gchar *action_id,
                        GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_sip_peers_async (ami, action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_sip_peers_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a list of SIP peers
 *
 * Returns: #GSList of SIP peers (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_sip_peers_async (GamiManager *ami, const gchar *action_id,
                              GamiListResponseFunc response_func,
                              gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: SIPpeers\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "PeerlistComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_sip_show_peer:
 * @ami: #GamiManager
 * @peer: SIP peer to get status information for
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve status information for @peer
 *
 * Returns: #GHashTable of peer status information on success, %NULL on failure
 */
GHashTable *
gami_manager_sip_show_peer (GamiManager *ami, const gchar *peer,
                            const gchar *action_id, GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_sip_show_peer_async (ami, peer, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_sip_show_peer_async:
 * @ami: #GamiManager
 * @peer: SIP peer to get status information for
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve status information for @peer
 *
 * Returns: #GHashTable of peer status information on success, %NULL on failure
 */
GIOStatus
gami_manager_sip_show_peer_async (GamiManager *ami, const gchar *peer,
                                  const gchar *action_id,
                                  GamiHashResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (peer != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: SIPShowPeer\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Peer: %s\r\n\r\n", peer);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_sip_show_registry:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve registry information of SIP peers
 *
 * Returns: #GSList of registry information (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_sip_show_registry (GamiManager *ami, const gchar *action_id,
                                GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_sip_show_registry_async (ami, action_id,
                                                     func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_sip_show_registry_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve registry information of SIP peers
 *
 * Returns: #GSList of registry information (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_sip_show_registry_async (GamiManager *ami, const gchar *action_id,
                                      GamiListResponseFunc response_func,
                                      gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: SIPshowregistry\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "RegistrationsComplete"));

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_status:
 * @ami: #GamiManager
 * @channel: (allow-none): Only retrieve status information for this channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve status information of active channels (or @channel)
 *
 * Returns: #GSList of status information (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GSList *
gami_manager_status (GamiManager *ami, const gchar *channel,
                     const gchar *action_id, GError **error)
{
    GSList    *rv = NULL;
    GIOStatus  iostatus;

    GamiListResponseFunc func = (GamiListResponseFunc) set_list_response;
    iostatus = gami_manager_status_async (ami, channel, action_id,
                                          func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_status_async:
 * @ami: #GamiManager
 * @channel: (allow-none): Only retrieve status information for this channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve status information of active channels (or @channel)
 *
 * Returns: #GSList of status information (stored as #GHashTable) on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_status_async (GamiManager *ami, const gchar *channel,
                           const gchar *action_id,
                           GamiListResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Status\r\n");

    if (channel)
        g_string_append_printf (action, "Channel: %s\r\n", channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     list_action_hook_new (response_func, response_data,
                                           "StatusComplete"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_extension_state:
 * @ami: #GamiManager
 * @exten: The name of the extension to check
 * @context: The context of the extension to check
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Check extension state of @exten@@context - if hints are properly configured
 * on the server, the action will report the status of the device connected to
 * @exten
 *
 * Returns: #GHashTable of status information on success, %NULL on failure
 */
GHashTable *
gami_manager_extension_state (GamiManager *ami, const gchar *exten,
                              const gchar *context, const gchar *action_id,
                              GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_extension_state_async (ami, exten, context,
                                                   action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_extension_state_async:
 * @ami: #GamiManager
 * @exten: The name of the extension to check
 * @context: The context of the extension to check
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Check extension state of @exten@@context - if hints are properly configured
 * on the server, the action will report the status of the device connected to
 * @exten
 *
 * Returns: #GHashTable of status information on success, %NULL on failure
 */
GIOStatus
gami_manager_extension_state_async (GamiManager *ami, const gchar *exten,
                                    const gchar *context,
                                    const gchar *action_id,
                                    GamiHashResponseFunc response_func,
                                    gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (exten != NULL && context != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ExtensionState\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Exten: %s\r\nContext: %s\r\n\r\n",
                            exten, context);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_ping:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Query the Asterisk server to make sure it is still responding. May be used
 * to keep the connection alive
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_ping (GamiManager *ami, const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_ping_async (ami, action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_ping_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Query the Asterisk server to make sure it is still responding. May be used
 * to keep the connection alive
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_ping_async (GamiManager *ami, const gchar *action_id,
                         GamiBoolResponseFunc response_func,
                         gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Ping\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           (ami->api_major
                                            && ami->api_minor) ? "Success"
                                                               : "Pong"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);

    return iostatus;
}

/**
 * gami_manager_absolute_timeout:
 * @ami: #GamiManager
 * @channel: The name of the channel to set the timeout for
 * @timeout: The maximum duration of the current call, in seconds
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set timeout for call on @channel to @timeout seconds
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_absolute_timeout (GamiManager *ami, const gchar *channel,
                               gint timeout, const gchar *action_id,
                               GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_absolute_timeout_async (ami, channel, timeout,
                                                    action_id, func, &rv,
                                                    error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_absolute_timeout_async:
 * @ami: #GamiManager
 * @channel: The name of the channel to set the timeout for
 * @timeout: The maximum duration of the current call, in seconds
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set timeout for call on @channel to @timeout seconds
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_absolute_timeout_async (GamiManager *ami, const gchar *channel,
                                     gint timeout, const gchar *action_id,
                                     GamiBoolResponseFunc response_func,
                                     gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: AbsoluteTimeout\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Channel: %s\r\nTimeout: %d\r\n\r\n",
                            channel, timeout);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_challenge:
 * @ami: #GamiManager
 * @auth_type: The authentification type to generate challenge for (e.g. "md5")
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a challenge string to use for authentification of type @auth_type
 *
 * Returns: the generated challenge on success, %FALSE on failure
 */
gchar *
gami_manager_challenge (GamiManager *ami, const gchar *auth_type,
                        const gchar *action_id, GError **error)
{
    gchar     *rv = NULL;
    GIOStatus  iostatus;

    GamiStringResponseFunc func = (GamiStringResponseFunc) set_string_response;
    iostatus = gami_manager_challenge_async (ami, auth_type, action_id,
                                             func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_challenge_async:
 * @ami: #GamiManager
 * @auth_type: The authentification type to generate challenge for (e.g. "md5")
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Retrieve a challenge string to use for authentification of type @auth_type
 *
 * Returns: the generated challenge on success, %FALSE on failure
 */
GIOStatus
gami_manager_challenge_async (GamiManager *ami, const gchar *auth_type,
                              const gchar *action_id,
                              GamiStringResponseFunc response_func,
                              gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (auth_type != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Challenge\r\n");
    g_string_append_printf (action, "AuthType: %s\r\n", auth_type);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     string_action_hook_new (response_func, response_data,
                                             "Challenge"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_set_cdr_user_field:
 * @ami: #GamiManager
 * @channel: The name of the channel to set @user_field for
 * @user_field: The value for the CDR user field
 * @append: (allow-none): Whether to append @user_field to current value
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set CDR user field for @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_set_cdr_user_field (GamiManager *ami, const gchar *channel,
                                 const gchar *user_field, gboolean append,
                                 const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_set_cdr_user_field_async (ami, channel, user_field,
                                                      append, action_id,
                                                      func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_set_cdr_user_field_async:
 * @ami: #GamiManager
 * @channel: The name of the channel to set @user_field for
 * @user_field: The value for the CDR user field
 * @append: (allow-none): Whether to append @user_field to current value
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set CDR user field for @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_set_cdr_user_field_async (GamiManager *ami, const gchar *channel,
                                       const gchar *user_field, gboolean append,
                                       const gchar *action_id,
                                       GamiBoolResponseFunc response_func,
                                       gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL && user_field != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: SetCDRUserField\r\n");

    if (append)
        g_string_append (action, "Append: 1\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Channel: %s\r\nUserField: %s\r\n\r\n",
                            channel, user_field);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_reload:
 * @ami: #GamiManager
 * @module: (allow-none): The name of the module to reload (not including
 *           extension)
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Reload @module or all modules
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_reload (GamiManager *ami, const gchar *module,
                     const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_reload_async (ami, module, action_id,
                                          func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_reload_async:
 * @ami: #GamiManager
 * @module: (allow-none): The name of the module to reload (not including
 *           extension)
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Reload @module or all modules
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_reload_async (GamiManager *ami, const gchar *module,
                           const gchar *action_id,
                           GamiBoolResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Reload\r\n");

    if (module)
        g_string_append_printf (action, "Module: %s\r\n", module);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_hangup:
 * @ami: #GamiManager
 * @channel: The name of the channel to hang up
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Hang up @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_hangup (GamiManager *ami, const gchar *channel,
                     const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_hangup_async (ami, channel, action_id,
                                          func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_hangup_async:
 * @ami: #GamiManager
 * @channel: The name of the channel to hang up
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Hang up @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_hangup_async (GamiManager *ami, const gchar *channel,
                           const gchar *action_id,
                           GamiBoolResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Hangup\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Channel: %s\r\n\r\n", channel);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_redirect:
 * @ami: #GamiManager
 * @channel: The name of the channel redirect
 * @extra_channel: (allow-none): Second call leg to transfer
 * @exten: The extension @channel should be redirected to
 * @context: The context @channel should be redirected to
 * @priority: The priority @channel should be redirected to
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Redirect @channel to @exten@@context:@priority
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_redirect (GamiManager *ami, const gchar *channel,
                       const gchar *extra_channel, const gchar *exten,
                       const gchar *context, const gchar *priority,
                       const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_redirect_async (ami, channel, extra_channel, exten,
                                            context, priority, action_id,
                                            func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_redirect_async:
 * @ami: #GamiManager
 * @channel: The name of the channel redirect
 * @extra_channel: (allow-none): Second call leg to transfer
 * @exten: The extension @channel should be redirected to
 * @context: The context @channel should be redirected to
 * @priority: The priority @channel should be redirected to
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Redirect @channel to @exten@@context:@priority
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_redirect_async (GamiManager *ami, const gchar *channel,
                             const gchar *extra_channel, const gchar *exten,
                             const gchar *context, const gchar *priority,
                             const gchar *action_id,
                             GamiBoolResponseFunc response_func,
                             gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);
    g_assert (exten != NULL && context != NULL && priority != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Redirect\r\n");
    g_string_append_printf (action, "Channel: %s\r\n", channel);

    if (extra_channel)
        g_string_append_printf (action, "ExtraChannel: %s\r\n", extra_channel);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Exten: %s\r\nContext: %s\r\n"
                            "Priority: %s\r\n\r\n", exten, context, priority);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_bridge:
 * @ami: #GamiManager
 * @channel1: The name of the channel to bridge to @channel2
 * @channel2: The name of the channel to bridge to @channel1
 * @tone: Whether to play courtesy tone to @channel2
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Bridge together the existing channels @channel1 and @channel2
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_bridge (GamiManager *ami, const gchar *channel1,
                     const gchar *channel2, gboolean tone,
                     const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_bridge_async (ami, channel1, channel2, tone,
                                          action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_bridge_async:
 * @ami: #GamiManager
 * @channel1: The name of the channel to bridge to @channel2
 * @channel2: The name of the channel to bridge to @channel1
 * @tone: Whether to play courtesy tone to @channel2
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Bridge together the existing channels @channel1 and @channel2
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_bridge_async (GamiManager *ami, const gchar *channel1,
                           const gchar *channel2, gboolean tone,
                           const gchar *action_id,
                           GamiBoolResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel1 != NULL && channel2 != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Bridge\r\n");
    g_string_append_printf (action, "Channel1: %s\r\nChannel2: %s\r\n",
                            channel1, channel2);

    g_string_append_printf (action, "Tone: %s\r\n", tone ? "Yes" : "No");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_agi:
 * @ami: #GamiManager
 * @channel: The name of the channel to execute @command in
 * @command: The name of the AGI command to execute
 * @command_id: (allow-none): CommandID for matching in AGI notification events
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Execute AGI command @command in @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_agi (GamiManager *ami, const gchar *channel, const gchar *command,
                  const gchar *command_id, const gchar *action_id,
                  GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_agi_async (ami, channel, command, command_id,
                                       action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_agi_async:
 * @ami: #GamiManager
 * @channel: The name of the channel to execute @command in
 * @command: The name of the AGI command to execute
 * @command_id: (allow-none): CommandID for matching in AGI notification events
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Execute AGI command @command in @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_agi_async (GamiManager *ami, const gchar *channel,
                        const gchar *command, const gchar *command_id,
                        const gchar *action_id,
                        GamiBoolResponseFunc response_func,
                        gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL && command != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: AGI\r\n");
    g_string_append_printf (action, "Channel: %s\r\nCommand: %s\r\n",
                            channel, command);

    if (command_id)
        g_string_append_printf (action, "CommandID: %s\r\n", command_id);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_send_text:
 * @ami: #GamiManager
 * @channel: The name of the channel to send @message to
 * @message: The message to send to @channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Send @message to @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_send_text (GamiManager *ami, const gchar *channel,
                        const gchar *message, const gchar *action_id,
                        GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_send_text_async (ami, channel, message,
                                             action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_send_text_async:
 * @ami: #GamiManager
 * @channel: The name of the channel to send @message to
 * @message: The message to send to @channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Send @message to @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_send_text_async (GamiManager *ami, const gchar *channel,
                              const gchar *message, const gchar *action_id,
                              GamiBoolResponseFunc response_func,
                              gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL && message != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: SendText\r\n");
    g_string_append_printf (action, "Channel: %s\r\nMessage: %s\r\n",
                            channel, message);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_jabber_send:
 * @ami: #GamiManager
 * @jabber: Jabber / GTalk account to send message from
 * @screen_name: Jabber / GTalk account to send message to
 * @message: The message to send
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Send @message from Jabber / GTalk account @jabber to account @screen_name
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_jabber_send (GamiManager *ami, const gchar *jabber,
                          const gchar *screen_name, const gchar *message,
                          const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_jabber_send_async (ami, jabber, screen_name,
                                               message, action_id, func,
                                               &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_jabber_send_async:
 * @ami: #GamiManager
 * @jabber: Jabber / GTalk account to send message from
 * @screen_name: Jabber / GTalk account to send message to
 * @message: The message to send
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Send @message from Jabber / GTalk account @jabber to account @screen_name
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_jabber_send_async (GamiManager *ami, const gchar *jabber,
                                const gchar *screen_name, const gchar *message,
                                const gchar *action_id,
                                GamiBoolResponseFunc response_func,
                                gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (jabber != NULL && screen_name != NULL && message != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: JabberSend\r\n");
    g_string_append_printf (action, "Jabber: %s\r\nScreenName: %s\r\n",
                            jabber, screen_name);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Message: %s\r\n\r\n", message);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_play_dtmf:
 * @ami: #GamiManager
 * @channel: The name of the channel to send @digit to
 * @digit: The DTMF digit to play on @channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Play a DTMF digit @digit on @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_play_dtmf (GamiManager *ami, const gchar *channel, gchar digit,
                        const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_play_dtmf_async (ami, channel, digit, action_id,
                                             func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_play_dtmf_async:
 * @ami: #GamiManager
 * @channel: The name of the channel to send @digit to
 * @digit: The DTMF digit to play on @channel
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Play a DTMF digit @digit on @channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_play_dtmf_async (GamiManager *ami, const gchar *channel,
                              gchar digit, const gchar *action_id,
                              GamiBoolResponseFunc response_func,
                              gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: PlayDTMF\r\n");
    g_string_append_printf (action, "Channel: %s\r\n", channel);

    if (digit)
        g_string_append_printf (action, "Digit: %c\r\n", digit);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_list_commands:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * List available Asterisk manager commands - the available actions may vary
 * between different versions of Asterisk and due to the set of loaded modules
 *
 * Returns: A #GHashTable of action names / descriptions on success, 
 *          %NULL on failure
 */
GHashTable *
gami_manager_list_commands (GamiManager *ami, const gchar *action_id,
                            GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_list_commands_async (ami, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_list_commands_async:
 * @ami: #GamiManager
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * List available Asterisk manager commands - the available actions may vary
 * between different versions of Asterisk and due to the set of loaded modules
 *
 * Returns: A #GHashTable of action names / descriptions on success, 
 *          %NULL on failure
 */
GIOStatus
gami_manager_list_commands_async (GamiManager *ami, const gchar *action_id,
                                  GamiHashResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ListCommands\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_list_categories:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to list categories for
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * List categories in @filename
 *
 * Returns: A #GHashTable of category number / name on success, 
 *          %NULL on failure
 */
GHashTable *
gami_manager_list_categories (GamiManager *ami, const gchar *filename,
                              const gchar *action_id, GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_list_categories_async (ami, filename, action_id,
                                                   func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_list_categories_async:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to list categories for
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * List categories in @filename
 *
 * Returns: A #GHashTable of category number / name on success, 
 *          %NULL on failure
 */
GIOStatus
gami_manager_list_categories_async (GamiManager *ami, const gchar *filename,
                                    const gchar *action_id,
                                    GamiHashResponseFunc response_func,
                                    gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (filename != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: ListCategories\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Filename: %s\r\n\r\n", filename);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_get_config:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to get content for
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get content of configuration file @filename
 *
 * Returns: A #GHashTable of line number / values on success, 
 *          %NULL on failure
 */
GHashTable *
gami_manager_get_config (GamiManager *ami, const gchar *filename,
                         const gchar *action_id, GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_get_config_async (ami, filename, action_id,
                                              func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_get_config_async:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to get content for
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get content of configuration file @filename
 *
 * Returns: A #GHashTable of line number / values on success, 
 *          %NULL on failure
 */
GIOStatus
gami_manager_get_config_async (GamiManager *ami, const gchar *filename,
                               const gchar *action_id,
                               GamiHashResponseFunc response_func,
                               gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (filename != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: GetConfig\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Filename: %s\r\n\r\n", filename);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_get_config_json:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to get content for
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get content of configuration file @filename as JS hash for use with JSON
 *
 * Returns: A #GHashTable with file dump on success,
 *          %NULL on failure
 */
GHashTable *
gami_manager_get_config_json (GamiManager *ami, const gchar *filename,
                              const gchar *action_id, GError **error)
{
    GHashTable *rv = NULL;
    GIOStatus   iostatus;

    GamiHashResponseFunc func = (GamiHashResponseFunc) set_hash_response;
    iostatus = gami_manager_get_config_json_async (ami, filename, action_id,
                                                   func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == NULL)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_get_config_json_async:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to get content for
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Get content of configuration file @filename as JS hash for use with JSON
 *
 * Returns: A #GHashTable with file dump on success,
 *          %NULL on failure
 */
GIOStatus
gami_manager_get_config_json_async (GamiManager *ami, const gchar *filename,
                                    const gchar *action_id,
                                    GamiHashResponseFunc response_func,
                                    gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (filename != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: GetConfigJSON\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     hash_action_hook_new (response_func, response_data));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append_printf (action, "Filename: %s\r\n\r\n", filename);

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_create_config:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to create
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Create an empty configurion file @filename
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_create_config (GamiManager *ami, const gchar *filename,
                            const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_create_config_async (ami, filename, action_id,
                                                 func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_create_config_async:
 * @ami: #GamiManager
 * @filename: The name of the configuration file to create
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Create an empty configurion file @filename
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_create_config_async (GamiManager *ami, const gchar *filename,
                                  const gchar *action_id,
                                  GamiBoolResponseFunc response_func,
                                  gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (filename != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: CreateConfig\r\n");
    g_string_append_printf (action, "Filename: %s\r\n", filename);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_originate:
 * @ami: #GamiManager
 * @channel: The name of the channel to call. Once the channel has answered,
 *           the call will be passed to the specified exten/context/priority or
 *           application/data
 * @application_exten: Extension to dial or application to call (depending on
 *                     @priority)
 * @data_context: Context to dial or data to pass to application (depending on
 *                @priority)
 * @priority: (allow-none): Priority to dial - if %NULL, @application_exten will
 *            be interpretated as application and @data_context as data
 * @timeout: (allow-none): Time to wait for @channel to answer in milliseconds
 * @caller_id: (allow-none): CallerID to set on the outgoing channel
 * @account: (allow-none): AccountCode to set for the call
 * @variables: (allow-none): A #GHashTable with name / value pairs to pass as 
 *             channel variables
 * @async: (allow-none): Whether to originate call asynchronously - this allows
 *         to originate further calls before a response is received
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Generate an outbound call from Asterisk and connect the channel to
 * Exten / Context / Priority or execute Application (Data) on the channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_originate (GamiManager *ami, const gchar *channel,
                        const gchar *application_exten,
                        const gchar *data_context, const gchar *priority,
                        guint timeout, const gchar *caller_id,
                        const gchar *account, const GHashTable *variables,
                        gboolean async, const gchar *action_id,
                        GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_originate_async (ami, channel, application_exten,
                                             data_context, priority, timeout,
                                             caller_id, account, variables,
                                             async, action_id,
                                             func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_originate_async:
 * @ami: #GamiManager
 * @channel: The name of the channel to call. Once the channel has answered,
 *           the call will be passed to the specified exten/context/priority or
 *           application/data
 * @application_exten: Extension to dial or application to call (depending on
 *                     @priority)
 * @data_context: Context to dial or data to pass to application (depending on
 *                @priority)
 * @priority: (allow-none): Priority to dial - if %NULL, @application_exten will
 *            be interpretated as application and @data_context as data
 * @timeout: (allow-none): Time to wait for @channel to answer in milliseconds
 * @caller_id: (allow-none): CallerID to set on the outgoing channel
 * @account: (allow-none): AccountCode to set for the call
 * @variables: (allow-none): A #GHashTable with name / value pairs to pass as 
 *             channel variables
 * @async: (allow-none): Whether to originate call asynchronously - this allows
 *         to originate further calls before a response is received
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Generate an outbound call from Asterisk and connect the channel to
 * Exten / Context / Priority or execute Application (Data) on the channel
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_originate_async (GamiManager *ami, const gchar *channel,
                              const gchar *application_exten,
                              const gchar *data_context, const gchar *priority,
                              guint timeout, const gchar *caller_id,
                              const gchar *account, const GHashTable *variables,
                              gboolean async, const gchar *action_id,
                              GamiBoolResponseFunc response_func,
                              gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (channel != NULL);
    g_assert (application_exten != NULL && data_context != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Originate\r\n");
    g_string_append_printf (action, "Channel: %s\r\n", channel);

    if (priority)
        g_string_append_printf (action, "Exten: %s\r\nContext: %s\r\n"
                                "Priority: %s\r\n", application_exten,
                                data_context, priority);
    else
        g_string_append_printf (action, "Application: %s\r\nData: %s\r\n",
                                application_exten, data_context);
    if (timeout)
        g_string_append_printf (action, "Timeout: %d\r\n", timeout);
    if (caller_id)
        g_string_append_printf (action, "CallerID: %s\r\n", caller_id);
    if (account)
        g_string_append_printf (action, "Account: %s\r\n", account);
    if (variables) {
        GHFunc join_func;
        GString *vars = g_string_new ("");
        gchar *var_str;

        join_func = (ami->api_major
                     && ami->api_minor) ? (GHFunc) join_originate_vars
                                        : (GHFunc) join_originate_vars_legacy;
        g_hash_table_foreach ((GHashTable *) variables, join_func, vars);
        var_str = g_string_free (vars, FALSE);
        g_string_append_printf (action, "Variable: %s\r\n", var_str);
        g_free (var_str);
    }
    if (async)
        g_string_append (action, "Async: 1\r\n");

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_events:
 * @ami: #GamiManager
 * @event_mask: #GamiEventMask to set for the connection
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set #GamiEventMask for the connection to control which events shall be
 * received
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_events (GamiManager *ami, const GamiEventMask event_mask,
                     const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_events_async (ami, event_mask, action_id,
                                          func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_events_async:
 * @ami: #GamiManager
 * @event_mask: #GamiEventMask to set for the connection
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Set #GamiEventMask for the connection to control which events shall be
 * received
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_events_async (GamiManager *ami, const GamiEventMask event_mask,
                           const gchar *action_id,
                           GamiBoolResponseFunc response_func,
                           gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    gchar     *event_str;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: Events\r\n");

    event_str = event_string_from_mask (ami, event_mask);
    g_string_append_printf (action, "EventMask: %s\r\n", event_str);
    g_free (event_str);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           (ami->api_major
                                            && ami->api_minor) ? "Success"
                                                               : "Events Off"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_user_event:
 * @ami: #GamiManager
 * @user_event: The user defined event to send
 * @headers: (allow-none): Optional header to add to the event
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Send the user defined event @user_event with an optional payload of @headers
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_user_event (GamiManager *ami, const gchar *user_event,
                         const GHashTable *headers, const gchar *action_id,
                         GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_user_event_async (ami, user_event, headers,
                                              action_id, func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_user_event_async:
 * @ami: #GamiManager
 * @user_event: The user defined event to send
 * @headers: (allow-none): Optional header to add to the event
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Send the user defined event @user_event with an optional payload of @headers
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_user_event_async (GamiManager *ami, const gchar *user_event,
                               const GHashTable *headers,
                               const gchar *action_id,
                               GamiBoolResponseFunc response_func,
                               gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));
    g_assert (user_event != NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: UserEvent\r\n");
    g_string_append_printf (action, "UserEvent: %s\r\n", user_event);

    if (headers) {
        GString *header = g_string_new ("");
        gchar *header_str;

        g_hash_table_foreach ((GHashTable *) headers,
                              (GHFunc) join_user_event_headers, header);
        header_str = g_string_free (header, FALSE);
        g_string_append_printf (action, "%s", header_str);
        g_free (header_str);
    }

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}

/**
 * gami_manager_wait_event:
 * @ami: #GamiManager
 * @timeout: (allow-none): Maximum time to wait for events in seconds
 * @action_id: (allow-none): ActionID to ease response matching
 * @error: A location to return an error of type #GIOChannelError
 *
 * Wait for an event to occur
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gami_manager_wait_event (GamiManager *ami, guint timeout,
                         const gchar *action_id, GError **error)
{
    gboolean   rv = -1;
    GIOStatus  iostatus;

    GamiBoolResponseFunc func = (GamiBoolResponseFunc) set_bool_response;
    iostatus = gami_manager_wait_event_async (ami, timeout, action_id,
                                              func, &rv, error);

    if (iostatus != G_IO_STATUS_NORMAL)
        return FALSE;

    while (rv == -1)
        g_main_context_iteration (NULL, TRUE);

    return rv;
}

/**
 * gami_manager_wait_event_async:
 * @ami: #GamiManager
 * @timeout: (allow-none): Maximum time to wait for events in seconds
 * @action_id: (allow-none): ActionID to ease response matching
 * @response_func: Callback for asynchronious operation.
 * @response_data: User data to pass to the callback.
 * @error: A location to return an error of type #GIOChannelError
 *
 * Wait for an event to occur
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
GIOStatus
gami_manager_wait_event_async (GamiManager *ami, guint timeout,
                               const gchar *action_id,
                               GamiBoolResponseFunc response_func,
                               gpointer response_data, GError **error)
{
    GamiManagerPrivate *priv;
    GString   *action;
    gchar     *action_str;
    gchar     *action_id_new;
    GIOStatus  iostatus;

    g_assert (error == NULL || *error == NULL);
    g_assert (ami   != NULL && GAMI_IS_MANAGER (ami));

    priv = GAMI_MANAGER_PRIVATE (ami);

    g_assert (priv->connected == TRUE);

    action = g_string_new ("Action: WaitEvent\r\n");

    if (timeout)
        g_string_append_printf (action, "Timeout: %d\r\n", timeout);

    action_id_new = get_action_id (action_id);
    add_action_hook (ami, action_id_new,
                     bool_action_hook_new (response_func, response_data,
                                           "Success"));
    g_string_append_printf (action, "ActionID: %s\r\n", action_id_new);

    g_string_append (action, "\r\n");

    action_str = g_string_free (action, FALSE);

    iostatus = send_command (priv->socket, action_str, error);
    g_free (action_str);

    return iostatus;
}


/*
 * Private API
 */

static gboolean
gami_manager_new_async_cb (GamiManagerNewAsyncData *data)
{
    GamiManager *gami;

    gami = gami_manager_new (data->host, data->port);
    data->func (gami, data->data);

    return FALSE; /* for g_idle_add() */
}

static gboolean
parse_connection_string (GamiManager *ami, GError **error)
{
    GamiManagerPrivate *priv;
    GIOStatus status;
    /* read welcome message and set API */
    gchar   *welcome_message;
    gchar  **split_version;

    g_assert (ami != NULL && GAMI_IS_MANAGER (ami));
    g_assert (error == NULL || *error == NULL);

    priv = GAMI_MANAGER_PRIVATE (ami);

    status = g_io_channel_read_line (priv->socket, &welcome_message,
                     NULL, NULL, error);

    if (status != G_IO_STATUS_NORMAL) {
        return FALSE;
    }

    ami->api_version = g_strdup (g_strchomp (g_strrstr (welcome_message,
                                                        "/") + 1));
    g_free (welcome_message);

    split_version = g_strsplit (ami->api_version, ".", 2);
    ami->api_major = atoi (split_version [0]);
    ami->api_minor = atoi (split_version [1]);
    g_free (split_version);

    return TRUE;
}

static gchar *
event_string_from_mask (GamiManager *mgr, GamiEventMask mask)
{
    GString *events;

    events = g_string_new ("");
    if (mask == GAMI_EVENT_MASK_NONE)
        g_string_append (events, "off");
    else if (mask & GAMI_EVENT_MASK_ALL)
        g_string_append (events, "on");
    else if (mgr->api_major && mgr->api_minor) {
        gboolean first = TRUE;

        if (mask & GAMI_EVENT_MASK_CALL) {
            g_string_append_printf (events, "%scall", first ? "" : ",");
            first = FALSE;
        }
        if (mask & GAMI_EVENT_MASK_SYSTEM) {
            g_string_append_printf (events, "%ssystem", first ? "" : ",");
            first = FALSE;
        }
        if (mask & GAMI_EVENT_MASK_AGENT) {
            g_string_append_printf (events, "%sagent", first ? "" : ",");
            first = FALSE;
        }
        if (mask & GAMI_EVENT_MASK_LOG) {
            g_string_append_printf (events, "%slog", first ? "" : ",");
            first = FALSE;
        }
        if (mask & GAMI_EVENT_MASK_USER) {
            g_string_append_printf (events, "%suser", first ? "" : ",");
            first = FALSE;
        }
        if (mask & GAMI_EVENT_MASK_CDR) {
            g_string_append_printf (events, "%scdr", first ? "" : ",");
            first = FALSE;
        }
    } else switch (mask) {
        case GAMI_EVENT_MASK_CALL:
        case GAMI_EVENT_MASK_CDR:
            g_string_printf (events, "call");
            break;
        case GAMI_EVENT_MASK_SYSTEM:
            g_string_printf (events, "system");
            break;
        case GAMI_EVENT_MASK_AGENT:
            g_string_printf (events, "agent");
            break;
        case GAMI_EVENT_MASK_LOG:
            g_string_printf (events, "log");
            break;
        case GAMI_EVENT_MASK_USER:
            g_string_printf (events, "user");
            break;
        default:
            g_string_printf (events, "on");
            break;
    }

    return g_string_free (events, FALSE);
}

static gchar *
get_action_id (const gchar *action_id)
{
    gchar *template;

    if (action_id)
        return g_strdup (action_id);

#ifndef G_OS_WIN32
    template = g_strdup_printf ("XXXXXX");
    mktemp (template);
#else
    template = g_strdup_printf ("%d", g_random_int_range (100000, 999999));
#endif

    return template;
}

static GIOStatus
send_command (GIOChannel *channel, const gchar *command, GError **error)
{
    GIOStatus status;
    gchar **cmd_lines, **cmd_line;

    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    g_debug ("Sending GAMI command");

    cmd_lines = g_strsplit (command, "\r", 0);
    for (cmd_line = cmd_lines; *cmd_line; cmd_line++)
        if (g_strchug (*cmd_line) && g_strcmp0 (*cmd_line, ""))
            g_debug ("   %s", *cmd_line);
    g_strfreev (cmd_lines);

    while ((status = g_io_channel_write_chars (channel,
                                               command,
                                               -1,
                                               NULL,
                                               error)) == G_IO_STATUS_AGAIN);

    g_debug ("GAMI command sent");

    if (status != G_IO_STATUS_NORMAL) {
        g_assert (error == NULL || *error != NULL);
        return status;
    }

    g_assert (error == NULL || *error == NULL);

    while ((status = g_io_channel_flush (channel, error)) == G_IO_STATUS_AGAIN);

    return status;
}

static gboolean
reconnect_socket (GamiManager *ami)
{
    GamiManagerPrivate *priv;
    GError *error = NULL;

    priv = GAMI_MANAGER_PRIVATE (ami);

    close (g_io_channel_unix_get_fd (priv->socket));
    g_io_channel_shutdown (priv->socket, TRUE, NULL);
    g_io_channel_unref (priv->socket);

    return ! gami_manager_connect (ami, &error); /* try again if connection
                                                    failed */
}

static gboolean
dispatch_ami (GIOChannel *chan, GIOCondition cond, GamiManager *mgr)
{
    GamiManagerPrivate *priv;
    GIOStatus           status = G_IO_STATUS_NORMAL;

    priv = GAMI_MANAGER_PRIVATE (mgr);

    if (cond & (G_IO_IN | G_IO_PRI)) {
        GError *error  = NULL;

        do {
            static GHashTable *packet = NULL;
            gchar             *line;

            status = g_io_channel_read_line (chan, &line, NULL, NULL, &error);

            if (status == G_IO_STATUS_NORMAL) {
                gchar **tokens;

                if (! packet) {
                    packet = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, g_free);

                    g_debug ("Reveiving an GAMI packet");
                }

                tokens = g_strsplit (line, ": ", 2);
                if (g_strv_length (tokens) == 2) {
                    gchar *key, *value;

                    key = g_strdup (g_strchomp (tokens [0]));
                    value = g_strdup (g_strchomp (tokens [1]));

                    g_debug ("   %s: %s", key, value);

                    g_hash_table_insert (packet, key, value);
                }
                g_strfreev (tokens);

                if (g_str_has_prefix (line, "\r\n")) {
                    g_debug ("GAMI packet received.");

                    g_queue_push_tail (priv->buffer, packet);
                    packet = NULL;
                }
            }

            g_free (line);

        } while (g_io_channel_get_buffer_condition (chan) & G_IO_IN);

        if (status == G_IO_STATUS_ERROR) {
            g_warning ("An error occurred during package reception%s%s\n",
                       error ? ": " : "",
                       error ? error->message : "");
            if (error)
                g_error_free (error);
        }

        if (! g_queue_is_empty (priv->buffer))
            g_timeout_add (0, (GSourceFunc) process_packets, mgr);
    }

    if (cond & (G_IO_HUP | G_IO_ERR) || status == G_IO_STATUS_EOF) {

        priv->connected = FALSE;
        g_signal_emit (mgr, signals [DISCONNECTED], 0);
        g_idle_add ((GSourceFunc) reconnect_socket, mgr);

        return FALSE;

    }

    return TRUE;
}

static gboolean
process_packets (GamiManager *mgr)
{
    GamiManagerPrivate *priv;
    GHashTable         *packet;
    gchar              *action_id;

    priv = GAMI_MANAGER_PRIVATE (mgr);

    if (! (packet = g_queue_pop_head (priv->buffer)))
		return FALSE;

    action_id = g_hash_table_lookup (packet, "ActionID");
    if (action_id || g_hash_table_lookup (packet, "Response")) {
        GamiActionHook *hook;

        hook = action_id ? g_hash_table_lookup (priv->action_hooks, action_id)
                         : g_hash_table_lookup (priv->action_hooks, "current");
        if (hook) {
            switch (hook->type) {
                gboolean    bool_resp;
                gchar      *str_resp;
                GHashTable *hash_resp;
                GSList     *list_resp;

                case GAMI_RESPONSE_TYPE_BOOL:
                    bool_resp = process_bool_response (packet,
                                                       hook->handler_data);
                    hook->user_func.bool_func (bool_resp, hook->user_data);
                    break;
                case GAMI_RESPONSE_TYPE_STRING:
                    str_resp = process_string_response (packet,
                                                        hook->handler_data);
                    hook->user_func.string_func (str_resp, hook->user_data);
                    break;
                case GAMI_RESPONSE_TYPE_HASH:
                    hash_resp = process_hash_response (packet);
                    hook->user_func.hash_func (hash_resp, hook->user_data);
                    break;
                case GAMI_RESPONSE_TYPE_LIST:
                    list_resp = NULL;
                    if (process_list_response (packet, hook->handler_data,
                                               &list_resp))
                        hook->user_func.list_func (list_resp, hook->user_data);
                    break;
            }
        }
    } else if (g_hash_table_lookup (packet, "Event"))
        g_signal_emit (mgr, signals [EVENT], 0, packet);

    return ! g_queue_is_empty (priv->buffer);
}

static GamiActionHook *
bool_action_hook_new (GamiBoolResponseFunc user_func, gpointer user_data,
                      gpointer handler_data)
{
    GamiActionHook *hook;

    hook = g_new0 (GamiActionHook, 1);
    hook->type = GAMI_RESPONSE_TYPE_BOOL;
    hook->user_data = user_data;
    hook->user_func.bool_func = user_func;
    hook->handler_data = handler_data;

    return hook;
}

static GamiActionHook *
string_action_hook_new (GamiStringResponseFunc user_func, gpointer user_data,
                        gpointer handler_data)
{
    GamiActionHook *hook;

    hook = g_new0 (GamiActionHook, 1);
    hook->type = GAMI_RESPONSE_TYPE_STRING;
    hook->user_data = user_data;
    hook->user_func.string_func = user_func;
    hook->handler_data = handler_data;

    return hook;
}

static GamiActionHook *
hash_action_hook_new (GamiHashResponseFunc user_func, gpointer user_data)
{
    GamiActionHook *hook;

    hook = g_new0 (GamiActionHook, 1);
    hook->type = GAMI_RESPONSE_TYPE_HASH;
    hook->user_data = user_data;
    hook->user_func.hash_func = user_func;
    hook->handler_data = NULL;

    return hook;
}

static GamiActionHook *
list_action_hook_new (GamiListResponseFunc user_func, gpointer user_data,
                      gpointer handler_data)
{
    GamiActionHook *hook;

    hook = g_new0 (GamiActionHook, 1);
    hook->type = GAMI_RESPONSE_TYPE_LIST;
    hook->user_data = user_data;
    hook->user_func.list_func = user_func;
    hook->handler_data = handler_data;

    return hook;
}

static void
add_action_hook (GamiManager *mgr, gchar *action_id, GamiActionHook *hook)
{
    GamiManagerPrivate *priv;

    priv = GAMI_MANAGER_PRIVATE (mgr);
        
    g_hash_table_insert (priv->action_hooks, action_id, hook);
    g_hash_table_insert (priv->action_hooks, g_strdup ("current"),
                         g_memdup (hook, sizeof (GamiActionHook)));
}

static gboolean
process_bool_response (GHashTable *packet, gpointer expected)
{
    return check_response (packet, (gchar *) expected);
}

static gchar *
process_string_response (GHashTable *packet, gpointer return_key)
{
    if (! check_response (packet, "Success"))
        return NULL;

    return g_strdup (g_hash_table_lookup (packet, (gchar *) return_key));
}

static GHashTable *
process_hash_response (GHashTable *packet)
{
    if (! check_response (packet, "Success"))
        return NULL;

    g_hash_table_remove (packet, "Response");
    g_hash_table_remove (packet, "Message");

    return g_hash_table_ref (packet);
}

static gboolean
process_list_response (GHashTable *packet, gpointer stop_event, GSList **resp)
{
    static GSList *list = NULL;
    gchar         *event;

    if (g_hash_table_lookup (packet, "Response")) {
        if (list) {              /* clean up left overs */
            g_slist_foreach (list, (GFunc) g_hash_table_destroy, NULL);
            g_slist_free (list);

            list = NULL;
        }

        if (! check_response (packet, "Success"))
            return TRUE;   /* FIXME: errors, empty lists */

        return FALSE;
    }

    event = g_hash_table_lookup (packet, "Event");
    if (! strcmp (event, (gchar *) stop_event)) {

        *resp = g_slist_reverse (list);
        list = NULL;

        return TRUE;

    } else {
        if (event)
            g_hash_table_remove (packet, "Event");

        list = g_slist_prepend (list, g_hash_table_ref (packet));
    }

    return FALSE; /* list not complete, wait for more packets */
}

static void
set_bool_response   (gboolean response, gboolean *store)
{
    *store = response;
}

static void
set_string_response (gchar *response, gchar **store)
{
    *store = response;
}

static void
set_hash_response   (GHashTable *response, GHashTable **store)
{
    *store = response;
}

static void
set_list_response   (GSList *response, GSList **store)
{
    *store = response;
}

static gboolean
check_response (GHashTable *pkt, const gchar *value)
{
    g_return_val_if_fail (pkt != NULL, FALSE);
    g_return_val_if_fail (value != NULL, FALSE);

    if (g_strcmp0 (g_hash_table_lookup (pkt, "Response"), value) != 0) {
        return FALSE;
    }
    return TRUE;
}

static void join_originate_vars (gchar *key, gchar *value, GString *s)
{
    g_string_append_printf (s, "%s%s=%s", (s->len == 0)?"":",", key, value);
}

static void join_originate_vars_legacy (gchar *key, gchar *value, GString *s)
{
    g_string_append_printf (s, "%s%s=%s", (s->len == 0)?"":"|", key, value);
}

static void join_user_event_headers (gchar *key, gchar *value, GString *s)
{
    g_string_append_printf (s, "%s: %s\r\n", key, value);
}


/*
 * GObject boilerplate
 */

static void
gami_manager_init (GamiManager *object)
{
    GamiManagerPrivate *priv;

    priv = GAMI_MANAGER_PRIVATE (object);
    priv->connected = FALSE;
    priv->buffer = g_queue_new ();
    priv->action_hooks = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
}

static void
gami_manager_finalize (GObject *object)
{
    GamiManagerPrivate *priv;

    priv = GAMI_MANAGER_PRIVATE (object);

    while (g_source_remove_by_user_data (object));

    if (priv->socket) {
        g_io_channel_shutdown (priv->socket, TRUE, NULL);
        g_io_channel_unref (priv->socket);
    }

    g_queue_foreach (priv->buffer, (GFunc) g_hash_table_unref, NULL);
    g_queue_free (priv->buffer);

    g_hash_table_destroy (priv->action_hooks);

    g_free (priv->host);
    g_free (priv->port);

    if (GAMI_MANAGER (object)->api_version)
        g_free ((gchar *) GAMI_MANAGER (object)->api_version);

    G_OBJECT_CLASS (gami_manager_parent_class)->finalize (object);
}

static void
gami_manager_get_property (GObject *obj, guint prop_id,
                           GValue *value, GParamSpec *pspec)
{
    GamiManager        *gami = (GamiManager *) obj;
    GamiManagerPrivate *priv = GAMI_MANAGER_PRIVATE (gami);

    switch (prop_id) {
        case HOST_PROP:
            g_value_set_string (value, priv->host);
            break;
        case PORT_PROP:
            g_value_set_string (value, priv->port);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
            break;
    }
}

static void
gami_manager_set_property (GObject *obj, guint prop_id,
                           const GValue *value, GParamSpec *pspec)
{
    GamiManager        *gami = (GamiManager *) obj;
    GamiManagerPrivate *priv = GAMI_MANAGER_PRIVATE (gami);

    switch (prop_id) {
        case HOST_PROP:
            g_free (priv->host);
            priv->host = g_value_dup_string (value);
            break;
        case PORT_PROP:
            g_free (priv->port);
            priv->port = g_value_dup_string (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
            break;
    }
}

static void
gami_manager_class_init (GamiManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GParamSpec   *pspec;

    g_type_class_add_private (klass, sizeof (GamiManagerPrivate));

    object_class->set_property = gami_manager_set_property;
    object_class->get_property = gami_manager_get_property;
    object_class->finalize = gami_manager_finalize;

    /**
     * GamiManager:host:
     *
     * The Asterisk manager host to connect to
     **/
    pspec = g_param_spec_string ("host",
                                 "Asterisk manager host",
                                 "Set Asterisk manager host to connect to",
                                 "localhost",
                                 G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE);
    g_object_class_install_property (object_class, HOST_PROP, pspec);

    /**
     * GamiManager:port:
     *
     * The Asterisk manager port to connect to
     **/
    pspec = g_param_spec_string ("port",
                                 "Asterisk manager port",
                                 "Set Asterisk manager port to connect to",
                                 "5038",
                                 G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PORT_PROP, pspec);

    /**
     * GamiManager::connected:
     * @ami: The #GamiManager that received the signal
     *
     * The ::connected signal is emitted after successfully establishing 
     * a connection to the Asterisk server
     */
    signals [CONNECTED] = g_signal_new ("connected",
                                        G_TYPE_FROM_CLASS (object_class),
                                        G_SIGNAL_RUN_LAST,
                                        0,
                                        NULL,
                                        NULL,
                                        g_cclosure_marshal_VOID__VOID,
                                        G_TYPE_NONE,
                                        0);

    /**
     * GamiManager::disconnected
     * @ami: The #GamiManager that received the signal
     *
     * The ::disconnected event is emitted each time the connection to the 
     * Asterisk server is lost
     */
    signals [DISCONNECTED] = g_signal_new ("disconnected",
                                           G_TYPE_FROM_CLASS (object_class),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL,
                                           NULL,
                                           g_cclosure_marshal_VOID__VOID,
                                           G_TYPE_NONE,
                                           0);

    /**
     * GamiManager::event:
     * @ami: The #GamiManager that received the signal
     * @event: The event that occurred (stored as a #GHashTable)
     *
     * The ::event signal is emitted each time Asterisk emits an event
     */
    signals [EVENT] = g_signal_new ("event",
                                    G_TYPE_FROM_CLASS (object_class),
                                    G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL,
                                    NULL,
                                    g_cclosure_marshal_VOID__BOXED,
                                    G_TYPE_NONE,
                                    1, G_TYPE_HASH_TABLE);
}

GType
gami_event_mask_get_type (void)
{
    static GType etype = 0;
    if (etype == 0) {
        static const GEnumValue values [] = {
            { GAMI_EVENT_MASK_NONE, "GAMI_EVENT_MASK_NONE", "none" },
            { GAMI_EVENT_MASK_CALL, "GAMI_EVENT_MASK_CALL", "call" },
            { GAMI_EVENT_MASK_SYSTEM, "GAMI_EVENT_MASK_SYSTEM", "system" },
            { GAMI_EVENT_MASK_AGENT, "GAMI_EVENT_MASK_AGENT", "agent" },
            { GAMI_EVENT_MASK_LOG, "GAMI_EVENT_MASK_LOG", "log" },
            { GAMI_EVENT_MASK_USER, "GAMI_EVENT_MASK_USER", "user" },
            { GAMI_EVENT_MASK_ALL, "GAMI_EVENT_MASK_ALL", "all"},
            { 0, NULL, NULL }
        };
        etype = g_enum_register_static ("GamiEventMask", values);
    }
    return etype;
}

GType
gami_module_load_type_get_type (void)
{
    static GType etype = 0;
    if (etype == 0) {
        static const GEnumValue values [] = {
            { GAMI_MODULE_LOAD, "GAMI_MODULE_LOAD", "load" },
            { GAMI_MODULE_RELOAD, "GAMI_MODULE_RELOAD", "reload" },
            { GAMI_MODULE_UNLOAD, "GAMI_MODULE_UNLOAD", "unload" },
            { 0, NULL, NULL }
        };
        etype = g_enum_register_static ("GamiModuleLoadType", values);
    }
    return etype;
}

/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 Vincent Untz
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Authors:
 *	Vincent Untz <vuntz@gnome.org>
 */

#include <config.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <upower.h>

#include "gsm-logout-dialog.h"
#include "gsm-consolekit.h"
#include "mdm.h"

#define GSM_LOGOUT_DIALOG_GET_PRIVATE(o)                                \
        (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSM_TYPE_LOGOUT_DIALOG, GsmLogoutDialogPrivate))

#define AUTOMATIC_ACTION_TIMEOUT 60

#define GSM_ICON_LOGOUT    "system-log-out"
#define GSM_ICON_SWITCH    "system-users"
#define GSM_ICON_SHUTDOWN  "system-shutdown"
#define GSM_ICON_REBOOT    "view-refresh"
/* TODO: use gpm icons? */
#define GSM_ICON_HIBERNATE "drive-harddisk"
#define GSM_ICON_SLEEP     "sleep"

typedef enum {
        GSM_DIALOG_LOGOUT_TYPE_LOGOUT,
        GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN
} GsmDialogLogoutType;

struct _GsmLogoutDialogPrivate
{
        GsmDialogLogoutType  type;

        UpClient            *up_client;
        GsmConsolekit       *consolekit;

        GtkWidget           *info_label;
        GtkWidget           *cancel_button;

        int                  timeout;
        unsigned int         timeout_id;

        unsigned int         default_response;
};

static GsmLogoutDialog *current_dialog = NULL;

static void gsm_logout_dialog_set_timeout  (GsmLogoutDialog *logout_dialog,
                                            int              seconds);

static void gsm_logout_dialog_destroy  (GsmLogoutDialog *logout_dialog,
                                        gpointer         data);

static void gsm_logout_dialog_show     (GsmLogoutDialog *logout_dialog,
                                        gpointer         data);

static void gsm_logout_set_info_text   (GsmLogoutDialog *logout_dialog,
                                        int              seconds);

G_DEFINE_TYPE (GsmLogoutDialog, gsm_logout_dialog, GTK_TYPE_DIALOG);

static void
gsm_logout_dialog_class_init (GsmLogoutDialogClass *klass)
{
        GObjectClass *gobject_class;

        gobject_class = G_OBJECT_CLASS (klass);

        g_type_class_add_private (klass, sizeof (GsmLogoutDialogPrivate));
}

static void
gsm_logout_dialog_init (GsmLogoutDialog *logout_dialog)
{
        logout_dialog->priv = GSM_LOGOUT_DIALOG_GET_PRIVATE (logout_dialog);

        logout_dialog->priv->timeout_id = 0;
        logout_dialog->priv->timeout = 0;
        logout_dialog->priv->default_response = GTK_RESPONSE_CANCEL;
        logout_dialog->priv->info_label = NULL;

        gtk_window_set_resizable (GTK_WINDOW (logout_dialog), FALSE);
        gtk_dialog_set_has_separator (GTK_DIALOG (logout_dialog), FALSE);
        gtk_window_set_keep_above (GTK_WINDOW (logout_dialog), TRUE);
        gtk_window_stick (GTK_WINDOW (logout_dialog));

        /* use HIG spacings */
        gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (logout_dialog)->vbox), 12);
        gtk_container_set_border_width (GTK_CONTAINER (logout_dialog), 6);

        gtk_dialog_add_button (GTK_DIALOG (logout_dialog), GTK_STOCK_HELP, 
                               GTK_RESPONSE_HELP);
        logout_dialog->priv->cancel_button =
                gtk_dialog_add_button (GTK_DIALOG (logout_dialog),
                                       GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);

        logout_dialog->priv->up_client = up_client_new ();

        logout_dialog->priv->consolekit = gsm_get_consolekit ();

        g_signal_connect (logout_dialog,
                          "destroy",
                          G_CALLBACK (gsm_logout_dialog_destroy),
                          NULL);

        g_signal_connect (logout_dialog,
                          "show",
                          G_CALLBACK (gsm_logout_dialog_show),
                          NULL);
}

static void
gsm_logout_dialog_destroy (GsmLogoutDialog *logout_dialog,
                           gpointer         data)
{
        if (logout_dialog->priv->timeout_id != 0) {
                g_source_remove (logout_dialog->priv->timeout_id);
                logout_dialog->priv->timeout_id = 0;
        }

        if (logout_dialog->priv->up_client) {
                g_object_unref (logout_dialog->priv->up_client);
                logout_dialog->priv->up_client = NULL;
        }

        if (logout_dialog->priv->consolekit) {
                g_object_unref (logout_dialog->priv->consolekit);
                logout_dialog->priv->consolekit = NULL;
        }

        current_dialog = NULL;
}

static gboolean
gsm_logout_supports_system_suspend (GsmLogoutDialog *logout_dialog)
{
        return up_client_get_can_suspend (logout_dialog->priv->up_client);
}

static gboolean
gsm_logout_supports_system_hibernate (GsmLogoutDialog *logout_dialog)
{
        return up_client_get_can_hibernate (logout_dialog->priv->up_client);
}

static gboolean
gsm_logout_supports_switch_user (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

        ret = gsm_consolekit_can_switch_user (logout_dialog->priv->consolekit);

        return ret;
}

static gboolean
gsm_logout_supports_reboot (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

        ret = gsm_consolekit_can_restart (logout_dialog->priv->consolekit);
        if (!ret) {
                ret = mdm_supports_logout_action (MDM_LOGOUT_ACTION_REBOOT);
        }

        return ret;
}

static gboolean
gsm_logout_supports_shutdown (GsmLogoutDialog *logout_dialog)
{
        gboolean ret;

        ret = gsm_consolekit_can_stop (logout_dialog->priv->consolekit);

        if (!ret) {
                ret = mdm_supports_logout_action (MDM_LOGOUT_ACTION_SHUTDOWN);
        }

        return ret;
}

static void
gsm_logout_dialog_show (GsmLogoutDialog *logout_dialog,
                        gpointer         user_data)
{
        gsm_logout_set_info_text (logout_dialog, AUTOMATIC_ACTION_TIMEOUT);

        if (logout_dialog->priv->default_response != GTK_RESPONSE_CANCEL)
                gsm_logout_dialog_set_timeout (logout_dialog,
                                               AUTOMATIC_ACTION_TIMEOUT);
}

static void
gsm_logout_set_info_text (GsmLogoutDialog *logout_dialog,
                          int              seconds)
{
        const char *info_text;
        char       *markup = NULL;
        static char     *session_type = NULL;

        switch (logout_dialog->priv->default_response) {
        case GSM_LOGOUT_RESPONSE_LOGOUT:
                info_text = ngettext ("You will be automatically logged "
                                      "out in %d second.",
                                      "You will be automatically logged "
                                      "out in %d seconds.",
                                      seconds);
                break;

        case GSM_LOGOUT_RESPONSE_SHUTDOWN:
                info_text = ngettext ("This system will be automatically "
                                      "shut down in %d second.",
                                      "This system will be automatically "
                                      "shut down in %d seconds.",
                                      seconds);
                break;

        case GTK_RESPONSE_CANCEL:
                info_text = NULL;
                break;

        default:
                g_assert_not_reached ();
        }
        
        if (session_type == NULL) {
		GsmConsolekit *consolekit;

                consolekit = gsm_get_consolekit ();
                session_type = gsm_consolekit_get_current_session_type (consolekit);
                g_object_unref (consolekit);
        }
        
        if (g_strcmp0 (session_type, GSM_CONSOLEKIT_SESSION_TYPE_LOGIN_WINDOW) != 0) {
                char *name, *buf, *buf2;
                name = g_locale_to_utf8 (g_get_real_name (), -1, NULL, NULL, NULL);

                if (!name || name[0] == '\0' || strcmp (name, "Unknown") == 0) {
                   name = g_locale_to_utf8 (g_get_user_name (), -1 , NULL, NULL, NULL);
                }
        
                if (!name) {
                        name = g_strdup (g_get_user_name ());
                }

                buf = g_strdup_printf (_("You are currently logged in as \"%s\"."), name);
                if (info_text != NULL) {
                        buf2 = g_strdup_printf (info_text, seconds);
                        markup = g_markup_printf_escaped ("<i>%s</i>", g_strconcat (buf, "\n", buf2, NULL));
                        g_free (buf2);
                } else {
                        markup = g_markup_printf_escaped ("<i>%s</i>", buf);
                }                
                g_free (buf);
                g_free (name);
	} else if (info_text != NULL) {
	                char *buf2;
	                buf2 = g_strdup_printf (info_text, seconds);
	                markup = g_markup_printf_escaped ("<i>%s</i>", buf2);
	                g_free (buf2);
	}
        
        gtk_label_set_markup (GTK_LABEL (logout_dialog->priv->info_label),
                        markup ? markup : "");
            
        if (markup != NULL)
                g_free (markup);
}

static gboolean
gsm_logout_dialog_timeout (gpointer data)
{
        GsmLogoutDialog *logout_dialog;
        char            *seconds_warning;
        char            *secondary_text;
        int              seconds_to_show;
        static char     *session_type = NULL;

        logout_dialog = (GsmLogoutDialog *) data;

        if (!logout_dialog->priv->timeout) {
                gtk_dialog_response (GTK_DIALOG (logout_dialog),
                                     logout_dialog->priv->default_response);

                return FALSE;
        }

        if (logout_dialog->priv->timeout <= 30) {
                seconds_to_show = logout_dialog->priv->timeout;
        } else {
                seconds_to_show = (logout_dialog->priv->timeout/10) * 10;

                if (logout_dialog->priv->timeout % 10)
                        seconds_to_show += 10;
        }

        gsm_logout_set_info_text (logout_dialog, seconds_to_show);

        logout_dialog->priv->timeout--;
        
        return TRUE;
}

static void
gsm_logout_dialog_set_timeout (GsmLogoutDialog *logout_dialog,
                               int              seconds)
{
        logout_dialog->priv->timeout = seconds;

        if (logout_dialog->priv->timeout_id != 0) {
                g_source_remove (logout_dialog->priv->timeout_id);
        }

        logout_dialog->priv->timeout_id = g_timeout_add (1000,
                                                         gsm_logout_dialog_timeout,
                                                         logout_dialog);
}

static GtkWidget *
gsm_logout_tile_new (const char *icon_name,
                     const char *title,
                     const char *description)
{
        GtkWidget *button;
        GtkWidget *alignment;
        GtkWidget *hbox;
        GtkWidget *vbox;
        GtkWidget *image;
        GtkWidget *label;
        char      *markup;

        g_assert (title != NULL);

        button = GTK_WIDGET (gtk_button_new ());
        gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);

        alignment = gtk_alignment_new (0, 0.5, 0, 0);
        gtk_alignment_set_padding (GTK_ALIGNMENT (alignment), 0, 0, 6, 6);
        gtk_container_add (GTK_CONTAINER (button), alignment);

        hbox = gtk_hbox_new (FALSE, 12);
        gtk_container_add (GTK_CONTAINER (alignment), hbox);
        if (icon_name != NULL) {
                image = gtk_image_new_from_icon_name (icon_name,
                                                      GTK_ICON_SIZE_DIALOG);
                gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
        }

        vbox = gtk_vbox_new (FALSE, 2);

        markup = g_markup_printf_escaped ("<span weight=\"bold\">%s</span>",
                                          title);
        label = gtk_label_new (markup);
        g_free (markup);

        gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
        gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
        gtk_label_set_use_underline (GTK_LABEL (label), TRUE);

        gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

        if (description != NULL) {
                gchar     *markup;
                GdkColor  *color;
                GtkWidget *label;

                color = &GTK_WIDGET (button)->style->fg[GTK_STATE_INSENSITIVE];
                markup = g_markup_printf_escaped ("<span size=\"small\" foreground=\"#%.2x%.2x%.2x\">%s</span>", 
                                                  color->red,
                                                  color->green,
                                                  color->blue,
                                                  description);
                label = gtk_label_new (markup);
                g_free (markup);

                gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
                gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
                gtk_label_set_use_underline (GTK_LABEL (label), TRUE);
                gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);

                gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);
        }

        gtk_box_pack_start (GTK_BOX (hbox), vbox, FALSE, FALSE, 0);

        return button;
}

static void
gsm_logout_tile_clicked (GtkWidget *tile,
                         gpointer   response_p)
{
        GtkWidget *dialog;

        dialog = gtk_widget_get_toplevel (tile);
        g_assert (GTK_IS_DIALOG (dialog));
        gtk_dialog_response (GTK_DIALOG (dialog),
                             GPOINTER_TO_UINT (response_p));
}

static GtkWidget *
gsm_logout_append_tile (GtkWidget    *vbox,
                        unsigned int  response,
                        const char   *icon_name,
                        const char   *title,
                        const char   *description)
{
        GtkWidget *tile;

        tile = gsm_logout_tile_new (icon_name, title, description);
        gtk_box_pack_start (GTK_BOX (vbox), tile, TRUE, TRUE, 0);
        gtk_widget_show_all (tile);

        g_signal_connect (tile,
                          "clicked",
                          G_CALLBACK (gsm_logout_tile_clicked),
                          GUINT_TO_POINTER (response));

        return tile;
}

static GtkWidget *
gsm_get_dialog (GsmDialogLogoutType type,
                GdkScreen          *screen,
                guint32             activate_time)
{
        GsmLogoutDialog *logout_dialog;
        GtkWidget       *dialog_image;
        GtkWidget       *vbox;
        GtkWidget       *tile;
        const char      *icon_name;
        const char      *title;

        if (current_dialog != NULL) {
                gtk_widget_destroy (GTK_WIDGET (current_dialog));
        }

        logout_dialog = g_object_new (GSM_TYPE_LOGOUT_DIALOG, NULL);

        current_dialog = logout_dialog;

        vbox = gtk_vbox_new (FALSE, 12);
        gtk_box_pack_start (GTK_BOX (GTK_DIALOG (logout_dialog)->vbox), vbox,
                            FALSE, FALSE, 0);
        gtk_container_set_border_width (GTK_CONTAINER (vbox), 6);
        gtk_widget_show (vbox);

        icon_name = NULL;
        title = NULL;

        switch (type) {
        case GSM_DIALOG_LOGOUT_TYPE_LOGOUT:
                icon_name    = GSM_ICON_LOGOUT;
                title        = _("Log Out of the Session");

                logout_dialog->priv->default_response = GSM_LOGOUT_RESPONSE_LOGOUT;


                gsm_logout_append_tile (vbox, GSM_LOGOUT_RESPONSE_LOGOUT,
                                        GSM_ICON_LOGOUT, _("_Log Out"),
                                        _("Ends your session and logs you "
                                          "out."));

                tile = gsm_logout_append_tile (vbox,
                                               GSM_LOGOUT_RESPONSE_SWITCH_USER,
                                               GSM_ICON_SWITCH,
                                               _("_Switch User"),
                                               _("Suspends your session, "
                                                 "allowing another user to "
                                                 "log in and use the "
                                                 "computer."));
                if (!gsm_logout_supports_switch_user (logout_dialog)) {
                        gtk_widget_set_sensitive (tile, FALSE);
                }

                break;
        case GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN:
                icon_name    = GSM_ICON_SHUTDOWN;
                title        = _("Shut Down the Computer");

                logout_dialog->priv->default_response = GSM_LOGOUT_RESPONSE_SHUTDOWN;

                tile = gsm_logout_append_tile (vbox,
                                               GSM_LOGOUT_RESPONSE_SHUTDOWN,
                                               GSM_ICON_SHUTDOWN,
                                               _("_Shut Down"),
                                               _("Ends your session and turns "
                                                 "off the computer."));
                if (!gsm_logout_supports_shutdown (logout_dialog)) {
                        gtk_widget_set_sensitive (tile, FALSE);
                        /* If shutdown is not available, let's just fallback
                         * on cancel as the default action. We could fallback
                         * on reboot first, then suspend and then hibernate
                         * but it's not that useful, really */
                        logout_dialog->priv->default_response = GTK_RESPONSE_CANCEL;
                }

                tile = gsm_logout_append_tile (vbox,
                                               GSM_LOGOUT_RESPONSE_REBOOT,
                                               GSM_ICON_REBOOT, _("_Restart"),
                                               _("Ends your session and "
                                                 "restarts the computer."));
                if (!gsm_logout_supports_reboot (logout_dialog)) {
                        gtk_widget_set_sensitive (tile, FALSE);
                }

                /* We don't set those options insensitive if they are no
                 * supported (like we do for shutdown/restart) since some
                 * hardware just don't support suspend/hibernate. So we
                 * don't show those options in this case. */
                if (gsm_logout_supports_system_suspend (logout_dialog)) {
                        gsm_logout_append_tile (vbox,
                                                GSM_LOGOUT_RESPONSE_SLEEP,
                                                GSM_ICON_SLEEP, _("S_uspend"),
                                                _("Suspends your session "
                                                  "quickly, using minimal "
                                                  "power while the computer "
                                                  "stands by."));      
                }

                if (gsm_logout_supports_system_hibernate (logout_dialog)) {
                        gsm_logout_append_tile (vbox,
                                                GSM_LOGOUT_RESPONSE_HIBERNATE,
                                                GSM_ICON_HIBERNATE,
                                                _("Hiber_nate"),
                                                _("Suspends your session, "
                                                  "using no power until the "
                                                  "computer is restarted."));
                }
                break;
        default:
                g_assert_not_reached ();
        }

        logout_dialog->priv->info_label = gtk_label_new ("");
        gtk_label_set_line_wrap (GTK_LABEL (logout_dialog->priv->info_label),
                                 TRUE);
        gtk_box_pack_start (GTK_BOX (vbox), logout_dialog->priv->info_label,
                            TRUE, TRUE, 0);
        gtk_widget_show (logout_dialog->priv->info_label);
        gtk_window_set_icon_name (GTK_WINDOW (logout_dialog), icon_name);
        gtk_window_set_title (GTK_WINDOW (logout_dialog), title);
        gtk_window_set_position (GTK_WINDOW (logout_dialog),
                                 GTK_WIN_POS_CENTER_ALWAYS);

        gtk_dialog_set_default_response (GTK_DIALOG (logout_dialog),
                                         logout_dialog->priv->default_response);
        /* Note that focus is on the widget for the default response by default
         * (since they're the first widget, except when it's Cancel */
        if (logout_dialog->priv->default_response == GTK_RESPONSE_CANCEL)
                gtk_window_set_focus (GTK_WINDOW (logout_dialog),
                                      logout_dialog->priv->cancel_button);

        gtk_window_set_screen (GTK_WINDOW (logout_dialog), screen);

        return GTK_WIDGET (logout_dialog);
}

GtkWidget *
gsm_get_shutdown_dialog (GdkScreen *screen,
                         guint32    activate_time)
{
        return gsm_get_dialog (GSM_DIALOG_LOGOUT_TYPE_SHUTDOWN,
                               screen,
                               activate_time);
}

GtkWidget *
gsm_get_logout_dialog (GdkScreen *screen,
                       guint32    activate_time)
{
        return gsm_get_dialog (GSM_DIALOG_LOGOUT_TYPE_LOGOUT,
                               screen,
                               activate_time);
}

/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Novell, Inc.
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
 */

#include "config.h"

#include <stdlib.h>

#include <gtk/gtk.h>

#include "gsm-logout-dialog.h"

static void
logout_dialog_response (GsmLogoutDialog *logout_dialog,
                        guint            response_id,
                        gpointer         data)
{
        /* In case of dialog cancel, switch user, hibernate and
         * suspend, we just perform the respective action and return,
         * without shutting down the session. */
        switch (response_id) {
        case GTK_RESPONSE_CANCEL:
        case GTK_RESPONSE_NONE:
        case GTK_RESPONSE_DELETE_EVENT:
                g_print ("Cancel\n");
                break;
        case GTK_RESPONSE_HELP:
                g_print ("Help\n");
                break;
        case GSM_LOGOUT_RESPONSE_SWITCH_USER:
                g_print ("Switch user\n");
                break;
        case GSM_LOGOUT_RESPONSE_HIBERNATE:
                g_print ("Hibernate\n");
                break;
        case GSM_LOGOUT_RESPONSE_SLEEP:
                g_print ("Suspend\n");
                break;
        case GSM_LOGOUT_RESPONSE_SHUTDOWN:
                g_print ("Shutdown\n");
                break;
        case GSM_LOGOUT_RESPONSE_REBOOT:
                g_print ("Reboot\n");
                break;
        case GSM_LOGOUT_RESPONSE_LOGOUT:
                g_print ("Logout\n");
                break;
        default:
                g_assert_not_reached ();
                break;
        }

        gtk_main_quit ();
}


int
main (int   argc,
      char *argv[])
{
        GtkWidget *dialog;
        GError    *error;

        static gboolean logout;
        static gboolean shutdown;

        static GOptionEntry entries[] = {
                { "logout", 'l', 0, G_OPTION_ARG_NONE, &logout, "Test logout dialog", NULL },
                { "shutdown", 's', 0, G_OPTION_ARG_NONE, &shutdown, "Test shutdown dialog", NULL },
                { NULL, 0, 0, 0, NULL, NULL, NULL }
        };

        logout = shutdown = FALSE;
        error = NULL;

        gtk_init_with_args (&argc, &argv,
                            (char *) " - test logout/shutdown dialogs",
                            entries, NULL,
                            &error);
        if (error != NULL) {
                g_warning ("%s", error->message);
                g_error_free (error);
                exit (1);
        }

        if (!shutdown)
                dialog = gsm_get_logout_dialog (gdk_screen_get_default (),
                                                GDK_CURRENT_TIME);
        else
                dialog = gsm_get_shutdown_dialog (gdk_screen_get_default (),
                                                  GDK_CURRENT_TIME);

        g_signal_connect (dialog, "response",
                          G_CALLBACK (logout_dialog_response), NULL);
        gtk_widget_show (dialog);

        gtk_main ();

        return 0;
}

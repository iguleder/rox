/*
 * $Id: main.h,v 1.17 2001/11/07 12:54:14 tal197 Exp $
 *
 * ROX-Filer, filer for the ROX desktop project
 * By Thomas Leonard, <tal197@users.sourceforge.net>.
 */

#ifndef _MAIN_H
#define _MAIN_H

#include <sys/types.h>
#include <gtk/gtk.h>

typedef struct _Callback Callback;
typedef void (*CallbackFn)(gpointer data);

#ifndef _TOOLBAR
extern GtkTooltips *tooltips;
#endif

struct _Callback
{
	CallbackFn	callback;
	gpointer	data;
};

extern int number_of_windows;
extern gboolean override_redirect;

extern uid_t euid;
extern gid_t egid;
extern int ngroups;			/* Number of supplemental groups */
extern gid_t *supplemental_groups;
extern guchar *show_user_message;
extern int home_dir_len;
extern char *home_dir, *app_dir;

/* Prototypes */
int main(int argc, char **argv);
void on_child_death(gint child, CallbackFn callback, gpointer data);

#endif /* _MAIN_H */

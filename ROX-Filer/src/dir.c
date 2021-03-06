/*
 * $Id: dir.c,v 1.69 2002/01/21 15:44:32 tal197 Exp $
 *
 * Copyright (C) 2002, the ROX-Filer team.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* dir.c - directory scanning and caching */

/* How it works:
 *
 * A Directory contains a list DirItems, each having a name and some details
 * (size, image, owner, etc).
 *
 * There is a list of file names that need to be rechecked. While this
 * list is non-empty, items are taken from the list in an idle callback
 * and checked. Missing items are removed from the Directory, new items are
 * added and existing items are updated if they've changed.
 *
 * When a whole directory is to be rescanned:
 * 
 * - A list of all filenames in the directory is fetched, without any
 *   of the extra details.
 * - This list is compared to the current DirItems, removing any that are now
 *   missing.
 * - The recheck list is replaced with this new list.
 *
 * This system is designed to get the number of items and their names quickly,
 * so that the auto-sizer can make a good guess.
 *
 * To get the Directory object, use dir_cache, which will automatically
 * trigger a rescan if needed.
 *
 * To get notified when the Directory changes, use the dir_attach() and
 * dir_detach() functions.
 */

#include "config.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "global.h"

#include "dir.h"
#include "diritem.h"
#include "support.h"
#include "gui_support.h"
#include "dir.h"
#include "fscache.h"
#include "mount.h"
#include "pixmaps.h"
#include "type.h"
#include "usericons.h"

GFSCache *dir_cache = NULL;

/* Static prototypes */
static Directory *load(char *pathname, gpointer data);
static void ref(Directory *dir, gpointer data);
static void unref(Directory *dir, gpointer data);
static int getref(Directory *dir, gpointer data);
static void update(Directory *dir, gchar *pathname, gpointer data);
static void destroy(Directory *dir);
static void set_idle_callback(Directory *dir);
static DirItem *insert_item(Directory *dir, guchar *leafname);
static void remove_missing(Directory *dir, GPtrArray *keep);
static void dir_recheck(Directory *dir, guchar *path, guchar *leafname);
static GPtrArray *hash_to_array(GHashTable *hash);
static void dir_force_update_item(Directory *dir, gchar *leaf);

/****************************************************************
 *			EXTERNAL INTERFACE			*
 ****************************************************************/

void dir_init(void)
{
	dir_cache = g_fscache_new((GFSLoadFunc) load,
				(GFSRefFunc) ref,
				(GFSRefFunc) unref,
				(GFSGetRefFunc) getref,
				(GFSUpdateFunc) update, NULL);
}

/* Periodically calls callback to notify about changes to the contents
 * of the directory.
 * Before this function returns, it calls the callback once to add all
 * the items currently in the directory (unless the dir is empty).
 * If we are not scanning, it also calls update(DIR_END_SCAN).
 */
void dir_attach(Directory *dir, DirCallback callback, gpointer data)
{
	DirUser	*user;
	GPtrArray *items;

	g_return_if_fail(dir != NULL);
	g_return_if_fail(callback != NULL);

	user = g_new(DirUser, 1);
	user->callback = callback;
	user->data = data;
	
	dir->users = g_list_prepend(dir->users, user);

	ref(dir, NULL);

	items = hash_to_array(dir->known_items);
	if (items->len)
		callback(dir, DIR_ADD, items, data);
	g_ptr_array_free(items, TRUE);

	if (dir->needs_update && !dir->scanning)
		dir_rescan(dir, dir->pathname);

	/* May start scanning if noone was watching before */
	set_idle_callback(dir);

	if (!dir->scanning)
		callback(dir, DIR_END_SCAN, NULL, data);

	if (dir->error)
		delayed_error(_("Error scanning '%s':\n%s"),
				dir->pathname, dir->error);
}

/* Undo the effect of dir_attach */
void dir_detach(Directory *dir, DirCallback callback, gpointer data)
{
	DirUser	*user;
	GList	*list = dir->users;

	g_return_if_fail(dir != NULL);
	g_return_if_fail(callback != NULL);

	while (list)
	{
		user = (DirUser *) list->data;
		if (user->callback == callback && user->data == data)
		{
			g_free(user);
			dir->users = g_list_remove(dir->users, user);
			unref(dir, NULL);

			/* May stop scanning if noone's watching */
			set_idle_callback(dir);

			return;
		}
		list = list->next;
	}

	g_warning("dir_detach: Callback/data pair not attached!\n");
}

void dir_update(Directory *dir, gchar *pathname)
{
	update(dir, pathname, NULL);
}

int dir_item_cmp(const void *a, const void *b)
{
	DirItem *aa = *((DirItem **) a);
	DirItem *bb = *((DirItem **) b);

	return strcmp(aa->leafname, bb->leafname);
}

/* Rescan this directory */
void refresh_dirs(char *path)
{
	g_fscache_update(dir_cache, path);
}

/* When something has happened to a particular object, call this
 * and all appropriate changes will be made.
 */
void dir_check_this(guchar *path)
{
	guchar	*real_path;
	guchar	*dir_path;
	Directory *dir;

	dir_path = g_dirname(path);
	real_path = pathdup(dir_path);
	g_free(dir_path);

	dir = g_fscache_lookup_full(dir_cache, real_path,
					FSCACHE_LOOKUP_PEEK, NULL);
	if (dir)
		dir_recheck(dir, real_path, g_basename(path));
	
	g_free(real_path);
}

/* Tell watchers that this item has changed, but don't rescan.
 * (used when thumbnail has been created for an item)
 */
void dir_force_update_path(gchar *path)
{
	gchar	*dir_path;
	Directory *dir;

	g_return_if_fail(path[0] == '/');

	dir_path = g_dirname(path);

	dir = g_fscache_lookup_full(dir_cache, dir_path, FSCACHE_LOOKUP_PEEK,
			NULL);
	if (dir)
		dir_force_update_item(dir, g_basename(path));
	
	g_free(dir_path);
}

/* Ensure that 'leafname' is up-to-date. Returns the new/updated
 * DirItem, or NULL if the file no longer exists.
 */
DirItem *dir_update_item(Directory *dir, gchar *leafname)
{
	DirItem *item;
	
	item = insert_item(dir, leafname);
	dir_merge_new(dir);

	return item;
}

static int sort_names(const void *a, const void *b)
{
	return strcmp(*((char **) a), *((char **) b));
}

static void free_recheck_list(Directory *dir)
{
	GList	*next;

	for (next = dir->recheck_list; next; next = next->next)
		g_free(next->data);

	g_list_free(dir->recheck_list);

	dir->recheck_list = NULL;
}

/* If scanning state has changed then notify all filer windows */
void dir_set_scanning(Directory *dir, gboolean scanning)
{
	GList	*next;

	if (scanning == dir->scanning)
		return;

	dir->scanning = scanning;

	for (next = dir->users; next; next = next->next)
	{
		DirUser *user = (DirUser *) next->data;

		user->callback(dir,
				scanning ? DIR_START_SCAN : DIR_END_SCAN,
				NULL, user->data);
	}
}

/* This is called in the background when there are items on the
 * dir->recheck_list to process.
 */
static gboolean recheck_callback(gpointer data)
{
	Directory *dir = (Directory *) data;
	GList	*next;
	guchar	*leaf;
	
	g_return_val_if_fail(dir != NULL, FALSE);
	g_return_val_if_fail(dir->recheck_list != NULL, FALSE);

	/* Remove the first name from the list */
	next = dir->recheck_list;
	dir->recheck_list = g_list_remove_link(dir->recheck_list, next);
	leaf = (guchar *) next->data;
	g_list_free_1(next);

	/* usleep(800); */

	insert_item(dir, leaf);

	g_free(leaf);

	if (dir->recheck_list)
		return TRUE;	/* Call again */

	/* The recheck_list list empty. Stop scanning, unless
	 * needs_update, in which case we start scanning again.
	 */

	dir_merge_new(dir);
	
	dir->have_scanned = TRUE;
	dir_set_scanning(dir, FALSE);
	gtk_idle_remove(dir->idle_callback);
	dir->idle_callback = 0;

	if (dir->needs_update)
		dir_rescan(dir, dir->pathname);

	return FALSE;
}

/* Get the names of all files in the directory.
 * Remove any DirItems that are no longer listed.
 * Replace the recheck_list with the items found.
 */
void dir_rescan(Directory *dir, guchar *pathname)
{
	GPtrArray	*names;
	DIR		*d;
	struct dirent	*ent;
	int		i;

	g_return_if_fail(dir != NULL);
	g_return_if_fail(pathname != NULL);

	dir->needs_update = FALSE;

	names = g_ptr_array_new();

	read_globicons();
	mount_update(FALSE);
	if (dir->error)
	{
		g_free(dir->error);
		dir->error = NULL;
	}

	d = mc_opendir(pathname);
	if (!d)
	{
		dir->error = g_strdup_printf(_("Can't open directory: %s"),
				g_strerror(errno));
		return;		/* Report on attach */
	}

	dir_set_scanning(dir, TRUE);
	dir_merge_new(dir);
	gdk_flush();

	/* Make a list of all the names in the directory */
	while ((ent = mc_readdir(d)))
	{
		if (ent->d_name[0] == '.')
		{
			if (ent->d_name[1] == '\0')
				continue;		/* Ignore '.' */
			if (ent->d_name[1] == '.' && ent->d_name[2] == '\0')
				continue;		/* Ignore '..' */
		}
		
		g_ptr_array_add(names, g_strdup(ent->d_name));
	}

	/* Sort, so the names are scanned in a sensible order */
	qsort(names->pdata, names->len, sizeof(guchar *), sort_names);

	/* Compare the list with the current DirItems, removing
	 * any that are missing.
	 */
	remove_missing(dir, names);

	free_recheck_list(dir);

	/* For each name found, add it to the recheck_list.
	 * If the item is new, put a blank place-holder item in the directory.
	 */
	for (i = 0; i < names->len; i++)
	{
		guchar *name = names->pdata[i];
		dir->recheck_list = g_list_prepend(dir->recheck_list, name);
		if (!g_hash_table_lookup(dir->known_items, name))
		{
			DirItem *new;

			new = diritem_new(name);
			g_ptr_array_add(dir->new_items, new);
		}
	}
	dir_merge_new(dir);
	
	dir->recheck_list = g_list_reverse(dir->recheck_list);

	g_ptr_array_free(names, TRUE);
	mc_closedir(d);

	set_idle_callback(dir);
}

/* Add all the new items to the items array.
 * Notify everyone who is watching us.
 */
void dir_merge_new(Directory *dir)
{
	GList *list = dir->users;
	GPtrArray	*new = dir->new_items;
	GPtrArray	*up = dir->up_items;
	GPtrArray	*gone = dir->gone_items;
	int	i;

	while (list)
	{
		DirUser *user = (DirUser *) list->data;

		if (new->len)
			user->callback(dir, DIR_ADD, new, user->data);
		if (up->len)
			user->callback(dir, DIR_UPDATE, up, user->data);
		if (gone->len)
			user->callback(dir, DIR_REMOVE, gone, user->data);
		
		list = list->next;
	}

	for (i = 0; i < new->len; i++)
	{
		DirItem *item = (DirItem *) new->pdata[i];

		g_hash_table_insert(dir->known_items, item->leafname, item);
	}

	for (i = 0; i < gone->len; i++)
	{
		DirItem	*item = (DirItem *) gone->pdata[i];

		diritem_free(item);
	}
	
	g_ptr_array_set_size(gone, 0);
	g_ptr_array_set_size(new, 0);
	g_ptr_array_set_size(up, 0);
}

/****************************************************************
 *			INTERNAL FUNCTIONS			*
 ****************************************************************/

static void free_items_array(GPtrArray *array)
{
	int	i;

	for (i = 0; i < array->len; i++)
	{
		DirItem	*item = (DirItem *) array->pdata[i];

		diritem_free(item);
	}

	g_ptr_array_free(array, TRUE);
}

/* Tell everyone watching that these items have gone */
static void notify_deleted(Directory *dir, GPtrArray *deleted)
{
	GList	*next;
	
	if (!deleted->len)
		return;

	for (next = dir->users; next; next = next->next)
	{
		DirUser *user = (DirUser *) next->data;

		user->callback(dir, DIR_REMOVE, deleted, user->data);
	}
}

static void mark_unused(gpointer key, gpointer value, gpointer data)
{
	DirItem	*item = (DirItem *) value;

	item->may_delete = TRUE;
}

static void keep_deleted(gpointer key, gpointer value, gpointer data)
{
	DirItem	*item = (DirItem *) value;
	GPtrArray *deleted = (GPtrArray *) data;

	if (item->may_delete)
		g_ptr_array_add(deleted, item);
}

static gboolean check_unused(gpointer key, gpointer value, gpointer data)
{
	DirItem	*item = (DirItem *) value;

	return item->may_delete;
}

/* Remove all the old items that have gone.
 * Notify everyone who is watching us of the removed items.
 */
static void remove_missing(Directory *dir, GPtrArray *keep)
{
	GPtrArray	*deleted;
	int		i;

	deleted = g_ptr_array_new();

	/* Mark all current items as may_delete */
	g_hash_table_foreach(dir->known_items, mark_unused, NULL);

	/* Unmark all items also in 'keep' */
	for (i = 0; i < keep->len; i++)
	{
		guchar	*leaf = (guchar *) keep->pdata[i];
		DirItem *item;

		item = g_hash_table_lookup(dir->known_items, leaf);

		if (item)
			item->may_delete = FALSE;
	}

	/* Add each item still marked to 'deleted' */
	g_hash_table_foreach(dir->known_items, keep_deleted, deleted);

	/* Remove all items still marked */
	g_hash_table_foreach_remove(dir->known_items, check_unused, NULL);

	notify_deleted(dir, deleted);

	free_items_array(deleted);
}

static gint notify_timeout(gpointer data)
{
	Directory	*dir = (Directory *) data;

	g_return_val_if_fail(dir->notify_active == TRUE, FALSE);

	dir_merge_new(dir);

	dir->notify_active = FALSE;
	unref(dir, NULL);

	return FALSE;
}

/* Call dir_merge_new() after a while. */
static void delayed_notify(Directory *dir)
{
	if (dir->notify_active)
		return;
	ref(dir, NULL);
	gtk_timeout_add(500, notify_timeout, dir);
	dir->notify_active = TRUE;
}

/* Stat this item and add, update or remove it.
 * Returns the new/updated item, if any.
 * (leafname may be from the current DirItem item)
 */
static DirItem *insert_item(Directory *dir, guchar *leafname)
{
	static GString  *tmp = NULL;

	DirItem		*item;
	DirItem		old;
	gboolean	do_compare = FALSE;	/* (old is filled in) */

	tmp = make_path(dir->pathname, leafname);
	item = g_hash_table_lookup(dir->known_items, leafname);

	if (item)
	{
		if (item->base_type != TYPE_UNKNOWN)
		{
			/* Preserve the old details so we can compare */
			memcpy(&old, item, sizeof(DirItem));
			pixmap_ref(old.image);
			do_compare = TRUE;
		}
		diritem_restat(tmp->str, item);
	}
	else
	{
		/* Item isn't already here. This won't normally happen,
		 * because blank items are added when scanning, before
		 * we get here.
		 */
		item = diritem_new(leafname);
		diritem_restat(tmp->str, item);
		if (item->base_type == TYPE_ERROR &&
				item->lstat_errno == ENOENT)
		{
			diritem_free(item);
			return NULL;
		}
		g_ptr_array_add(dir->new_items, item);

	}

	if (item->base_type == TYPE_ERROR && item->lstat_errno == ENOENT)
	{
		/* Item has been deleted */
		g_hash_table_remove(dir->known_items, item->leafname);
		g_ptr_array_add(dir->gone_items, item);
		if (do_compare)
			pixmap_unref(old.image);
		delayed_notify(dir);
		return NULL;
	}

	if (do_compare)
	{
		if (item->lstat_errno == old.lstat_errno
		 && item->base_type == old.base_type
		 && item->flags == old.flags
		 && item->size == old.size
		 && item->mode == old.mode
		 && item->atime == old.atime
		 && item->ctime == old.ctime
		 && item->mtime == old.mtime
		 && item->uid == old.uid
		 && item->gid == old.gid
		 && item->image == old.image
		 && item->mime_type == old.mime_type)
		{
			pixmap_unref(old.image);
			return item;
		}
		pixmap_unref(old.image);

	}

	g_ptr_array_add(dir->up_items, item);
	delayed_notify(dir);

	return item;
}

static Directory *load(char *pathname, gpointer data)
{
	Directory *dir;

	dir = g_new(Directory, 1);
	dir->ref = 1;
	dir->known_items = g_hash_table_new(g_str_hash, g_str_equal);
	dir->recheck_list = NULL;
	dir->idle_callback = 0;
	dir->scanning = FALSE;
	dir->have_scanned = FALSE;
	
	dir->users = NULL;
	dir->needs_update = TRUE;
	dir->notify_active = FALSE;
	dir->pathname = g_strdup(pathname);
	dir->error = NULL;

	dir->new_items = g_ptr_array_new();
	dir->up_items = g_ptr_array_new();
	dir->gone_items = g_ptr_array_new();
	
	return dir;
}

/* Note: dir_cache is never purged, so this shouldn't get called */
static void destroy(Directory *dir)
{
	GPtrArray *items;

	g_return_if_fail(dir->users == NULL);

	g_print("[ destroy %p ]\n", dir);

	free_recheck_list(dir);
	set_idle_callback(dir);

	dir_merge_new(dir);	/* Ensures new, up and gone are empty */

	g_ptr_array_free(dir->up_items, TRUE);
	g_ptr_array_free(dir->new_items, TRUE);
	g_ptr_array_free(dir->gone_items, TRUE);

	items = hash_to_array(dir->known_items);
	free_items_array(items);
	g_hash_table_destroy(dir->known_items);
	
	g_free(dir->error);
	g_free(dir->pathname);
	g_free(dir);
}

static void ref(Directory *dir, gpointer data)
{
	dir->ref++;
}
	
static void unref(Directory *dir, gpointer data)
{
	if (--dir->ref == 0)
		destroy(dir);
}

static int getref(Directory *dir, gpointer data)
{
	return dir->ref;
}

static void update(Directory *dir, gchar *pathname, gpointer data)
{
	g_free(dir->pathname);
	dir->pathname = pathdup(pathname);

	if (dir->scanning)
		dir->needs_update = TRUE;
	else
		dir_rescan(dir, pathname);

	if (dir->error)
		delayed_error(_("Error scanning '%s':\n%s"),
				dir->pathname, dir->error);
}

/* If there is work to do, set the idle callback.
 * Otherwise, stop scanning and unset the idle callback.
 */
static void set_idle_callback(Directory *dir)
{
	if (dir->recheck_list && dir->users)
	{
		/* Work to do, and someone's watching */
		dir_set_scanning(dir, TRUE);
		if (dir->idle_callback)
			return;
		dir->idle_callback = gtk_idle_add(recheck_callback, dir);
	}
	else
	{
		dir_set_scanning(dir, FALSE);
		if (dir->idle_callback)
		{
			gtk_idle_remove(dir->idle_callback);
			dir->idle_callback = 0;
		}
	}
}

/* See dir_force_update_path() */
static void dir_force_update_item(Directory *dir, gchar *leaf)
{
	GList *list = dir->users;
	GPtrArray *items;
	DirItem *item;

	items = g_ptr_array_new();

	item = g_hash_table_lookup(dir->known_items, leaf);
	if (!item)
		goto out;

	g_ptr_array_add(items, item);

	while (list)
	{
		DirUser *user = (DirUser *) list->data;

		user->callback(dir, DIR_UPDATE, items, user->data);
		
		list = list->next;
	}

out:
	g_ptr_array_free(items, TRUE);
}

static void dir_recheck(Directory *dir, guchar *path, guchar *leafname)
{
	guchar *old = dir->pathname;

	dir->pathname = g_strdup(path);
	g_free(old);

	insert_item(dir, leafname);
}

static void to_array(gpointer key, gpointer value, gpointer data)
{
	GPtrArray *array = (GPtrArray *) data;

	g_ptr_array_add(array, value);
}

/* Convert a hash table to an unsorted GPtrArray.
 * g_ptr_array_free() the result.
 */
static GPtrArray *hash_to_array(GHashTable *hash)
{
	GPtrArray *array;

	array = g_ptr_array_new();

	g_hash_table_foreach(hash, to_array, array);

	return array;
}

/**
 * NNStreamer Subplugin Manager
 * Copyright (C) 2018 MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 */
/**
 * @file	nnstreamer_subplugin.c
 * @date	27 Nov 2018
 * @brief	Subplugin Manager for NNStreamer
 * @see		http://github.com/nnsuite/nnstreamer
 * @author	MyungJoo Ham <myungjoo.ham@samsung.com>
 * @bug		No known bugs except for NYI items
 *
 */

#include <dlfcn.h>
#include <glib.h>
#include <gmodule.h>
#include "nnstreamer_subplugin.h"
#include "nnstreamer_conf.h"

typedef struct
{
  char *name; /**< The name of subplugin */
  const void *data; /**< subplugin specific data forwarded from the subplugin */
  void *handle; /**< dlopen'ed handle */
} subpluginData;

static GHashTable *subplugins[NNS_SUBPLUGIN_END] = { 0 };

G_LOCK_DEFINE_STATIC (splock);

/** @brief Private function for g_hash_table data destructor, GDestroyNotify */
static void
_spdata_destroy (gpointer _data)
{
  subpluginData *data = _data;
  g_free (data->name);
  if (data->handle)
    dlclose (data->handle);
  g_free (data);
}

typedef enum
{
  NNS_SEARCH_FILENAME,
  NNS_SEARCH_GETALL,
  NNS_SEARCH_NO_OP,
} subpluginSearchLogic;

static subpluginSearchLogic searchAlgorithm[] = {
  [NNS_SUBPLUGIN_FILTER] = NNS_SEARCH_FILENAME,
  [NNS_SUBPLUGIN_DECODER] = NNS_SEARCH_FILENAME,
  [NNS_EASY_CUSTOM_FILTER] = NNS_SEARCH_FILENAME,
  [NNS_SUBPLUGIN_CONVERTER] = NNS_SEARCH_GETALL,
  [NNS_SUBPLUGIN_END] = NNS_SEARCH_NO_OP,
};

/** @brief Public function defined in the header */
const void *
get_subplugin (subpluginType type, const char *name)
{
  GHashTable *table;
  subpluginData *spdata = NULL;
  void *handle;

  g_return_val_if_fail (name, NULL);

  G_LOCK (splock);

  if (subplugins[type] == NULL)
    subplugins[type] =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
        _spdata_destroy);

  table = subplugins[type];

  if (searchAlgorithm[type] == NNS_SEARCH_GETALL) {
    nnsconf_type_path conf_type = (nnsconf_type_path) type;
    subplugin_info_s info;
    guint i;
    guint ret = nnsconf_get_subplugin_info (conf_type, &info);

    for (i = 0; i < ret; i++) {
      const gchar *fullpath = info.paths[i];

      dlerror ();
      G_UNLOCK (splock);
      handle = dlopen (fullpath, RTLD_NOW);
      /* If this is a correct subplugin, it will register itself */
      G_LOCK (splock);
      if (NULL == handle) {
        g_critical ("Cannot dlopen %s with error %s.", fullpath, dlerror ());
        continue;
      }
    }

    searchAlgorithm[type] = NNS_SEARCH_NO_OP;
  }

  spdata = g_hash_table_lookup (table, name);
  if (spdata == NULL && searchAlgorithm[type] == NNS_SEARCH_FILENAME) {
    /** Search and register if found with the conf */
    nnsconf_type_path conf_type = (nnsconf_type_path) type;
    const gchar *fullpath = nnsconf_get_fullpath (name, conf_type);

    if (!nnsconf_validate_file (conf_type, fullpath))
      goto error;               /* No Such Thing !!! */

    G_UNLOCK (splock);

    /** clear any existing errors */
    dlerror ();
    handle = dlopen (fullpath, RTLD_NOW);
    if (NULL == handle) {
      g_critical ("Cannot dlopen %s (%s) with error %s.", name, fullpath,
          dlerror ());
      return NULL;
    }

    G_LOCK (splock);

    /* If a subplugin's constructor has called register_subplugin, skip the rest */
    spdata = g_hash_table_lookup (table, name);
    if (spdata == NULL) {
      g_critical
          ("nnstreamer_subplugin of %s (%s) is broken. It does not call register_subplugin with its init function.",
          name, fullpath);
      dlclose (handle);
      goto error;
    }
  }

error:
  G_UNLOCK (splock);
  return (spdata != NULL) ? spdata->data : NULL;
}

/** @brief Public function defined in the header */
gboolean
register_subplugin (subpluginType type, const char *name, const void *data)
{
  /** @todo data out of scope at add */
  subpluginData *spdata;
  gboolean ret;

  g_return_val_if_fail (name, FALSE);
  g_return_val_if_fail (data, FALSE);

  switch (type) {
    case NNS_SUBPLUGIN_FILTER:
    case NNS_SUBPLUGIN_DECODER:
    case NNS_EASY_CUSTOM_FILTER:
    case NNS_SUBPLUGIN_CONVERTER:
      break;
    default:
      /* unknown sub-plugin type */
      return FALSE;
  }

  if (subplugins[type] == NULL) {
    subplugins[type] =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
        _spdata_destroy);
  } else {
    G_LOCK (splock);
    spdata = g_hash_table_lookup (subplugins[type], name);
    G_UNLOCK (splock);

    if (spdata) {
      /* already exists */
      g_critical ("Subplugin %s is already registered.", name);
      return FALSE;
    }
  }

  spdata = g_new (subpluginData, 1);
  if (spdata == NULL) {
    g_critical ("Failed to allocate memory for subplugin registration.");
    return FALSE;
  }

  spdata->name = g_strdup (name);
  spdata->data = data;
  spdata->handle = NULL;

  G_LOCK (splock);
  ret = g_hash_table_insert (subplugins[type], g_strdup (name), spdata);
  G_UNLOCK (splock);

  return ret;
}

/** @brief Public function defined in the header */
gboolean
unregister_subplugin (subpluginType type, const char *name)
{
  gboolean ret;

  g_return_val_if_fail (name, FALSE);
  g_return_val_if_fail (subplugins[type], FALSE);

  G_LOCK (splock);
  ret = g_hash_table_remove (subplugins[type], name);
  G_UNLOCK (splock);

  return ret;
}

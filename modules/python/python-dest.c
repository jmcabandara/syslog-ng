/*
 * Copyright (c) 2014 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 2014 Gergely Nagy <algernon@balabit.hu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "python-dest.h"
#include "python-globals.h"
#include "python-value-pairs.h"
#include "logthrdestdrv.h"
#include "stats/stats.h"
#include "misc.h"

#ifndef SCS_PYTHON
#define SCS_PYTHON 0
#endif

typedef struct
{
  LogThrDestDriver super;

  gchar *filename;
  gchar *init_func_name;
  gchar *queue_func_name;
  gchar *deinit_func_name;
  GList *imports;

  LogTemplateOptions template_options;
  ValuePairs *vp;

  struct
  {
    PyObject *module;
    PyObject *init;
    PyObject *queue;
    PyObject *deinit;
  } py;
} PythonDestDriver;

/** Setters & config glue **/

void
python_dd_set_init_func(LogDriver *d, gchar *init_func_name)
{
  PythonDestDriver *self = (PythonDestDriver *)d;

  g_free(self->init_func_name);
  self->init_func_name = g_strdup(init_func_name);
}

void
python_dd_set_queue_func(LogDriver *d, gchar *queue_func_name)
{
  PythonDestDriver *self = (PythonDestDriver *)d;

  g_free(self->queue_func_name);
  self->queue_func_name = g_strdup(queue_func_name);
}

void
python_dd_set_deinit_func(LogDriver *d, gchar *deinit_func_name)
{
  PythonDestDriver *self = (PythonDestDriver *)d;

  g_free(self->deinit_func_name);
  self->deinit_func_name = g_strdup(deinit_func_name);
}

void
python_dd_set_filename(LogDriver *d, gchar *filename)
{
  PythonDestDriver *self = (PythonDestDriver *)d;

  g_free(self->filename);
  self->filename = g_strdup(filename);
}

void
python_dd_set_value_pairs(LogDriver *d, ValuePairs *vp)
{
  PythonDestDriver *self = (PythonDestDriver *)d;

  if (self->vp)
    value_pairs_free(self->vp);
  self->vp = vp;
}

void
python_dd_set_imports(LogDriver *d, GList *imports)
{
  PythonDestDriver *self = (PythonDestDriver *)d;

  string_list_free(self->imports);
  self->imports = imports;
}

LogTemplateOptions *
python_dd_get_template_options(LogDriver *d)
{
  PythonDestDriver *self = (PythonDestDriver *)d;

  return &self->template_options;
}

/** Helpers for stats & persist_name formatting **/

static gchar *
python_dd_format_stats_instance(LogThrDestDriver *d)
{
  PythonDestDriver *self = (PythonDestDriver *)d;
  static gchar persist_name[1024];

  g_snprintf(persist_name, sizeof(persist_name),
             "python,%s,%s,%s,%s",
             self->filename,
             self->init_func_name,
             self->queue_func_name,
             self->deinit_func_name);
  return persist_name;
}

static gchar *
python_dd_format_persist_name(LogThrDestDriver *d)
{
  PythonDestDriver *self = (PythonDestDriver *)d;
  static gchar persist_name[1024];

  g_snprintf(persist_name, sizeof(persist_name),
             "python(%s,%s,%s,%s)",
             self->filename,
             self->init_func_name,
             self->queue_func_name,
             self->deinit_func_name);
  return persist_name;
}

/** Python calling helpers **/
static const gchar *
_py_get_callable_name(PyObject *callable, gchar *buf, gsize buf_len)
{
  PyObject *name = PyObject_GetAttrString(callable, "__name__");

  if (name)
    {
      g_strlcpy(buf, PyString_AsString(name), buf_len);
    }
  else
    {
      PyErr_Clear();
      g_strlcpy(buf, "<unknown>", buf_len);
    }
  Py_XDECREF(name);
  return buf;
}

static const gchar *
_py_format_exception_text(gchar *buf, gsize buf_len)
{
  PyObject *exc, *value, *tb, *str;

  PyErr_Fetch(&exc, &value, &tb);
  PyErr_NormalizeException(&exc, &value, &tb);

  str = PyObject_Str(value);
  if (str)
    {
      g_snprintf(buf, buf_len, "%s: %s", ((PyTypeObject *) exc)->tp_name, PyString_AsString(str));
    }
  else
    {
      g_strlcpy(buf, "<unknown>", buf_len);
    }
  Py_XDECREF(exc);
  Py_XDECREF(value);
  Py_XDECREF(tb);
  Py_XDECREF(str);
  return buf;
}

static gboolean
_py_invoke_function(PythonDestDriver *self, PyObject *func, PyObject *arg)
{
  PyObject *ret;

  ret = PyObject_CallFunctionObjArgs(func, arg, NULL);
  if (!ret)
    {
      gchar buf1[256], buf2[256];

      msg_error("Exception while calling a Python function",
                evt_tag_str("driver", self->super.super.super.id),
                evt_tag_str("script", self->filename),
                evt_tag_str("function", _py_get_callable_name(func, buf1, sizeof(buf1))),
                evt_tag_str("exception", _py_format_exception_text(buf2, sizeof(buf2))),
                NULL);
      return FALSE;
    }
  Py_XDECREF(ret);
  return ret != NULL;
}

static PyObject *
_py_do_import(PythonDestDriver *self, const gchar *modname)
{
  PyObject *module, *modobj;

  module = PyUnicode_FromString(modname);
  if (!module)
    {
      msg_error("Error allocating Python string",
                evt_tag_str("driver", self->super.super.super.id),
                evt_tag_str("string", modname),
                NULL);
      return NULL;
    }

  modobj = PyImport_Import(module);
  Py_DECREF(module);
  if (!modobj)
    {
      msg_error("Error loading Python module",
                evt_tag_str("driver", self->super.super.super.id),
                evt_tag_str("module", modname),
                NULL);
      return NULL;
    }
  return modobj;
}

static void
_foreach_import(gpointer data, gpointer user_data)
{
  PythonDestDriver *self = (PythonDestDriver *) user_data;
  gchar *modname = (gchar *) data;
  PyObject *mod;

  mod = _py_do_import(self, modname);
  Py_XDECREF(mod);
}

static void
_py_perform_imports(PythonDestDriver *self)
{
  g_list_foreach(self->imports, _foreach_import, self);
}

static gboolean
_py_invoke_queue(PythonDestDriver *self, PyObject *dict)
{
  return _py_invoke_function(self, self->py.queue, dict);
}

static gboolean
_py_invoke_init(PythonDestDriver *self)
{
  if (!self->py.init)
    return TRUE;

  return _py_invoke_function(self, self->py.init, NULL);
}

static gboolean
_py_invoke_deinit(PythonDestDriver *self)
{
  if (!self->py.deinit)
    return TRUE;

  return _py_invoke_function(self, self->py.deinit, NULL);
}

static worker_insert_result_t
python_dd_insert(LogThrDestDriver *d, LogMessage *msg)
{
  PythonDestDriver *self = (PythonDestDriver *)d;
  gboolean success;
  PyObject *dict;
  PyGILState_STATE gstate;

  gstate = PyGILState_Ensure();

  success = py_value_pairs_apply(self->vp, &self->template_options, self->super.seq_num, msg, &dict);
  if (!success && (self->template_options.on_error & ON_ERROR_DROP_MESSAGE))
    {
      goto exit;
    }

  success = _py_invoke_queue(self, dict);
 exit:

  PyGILState_Release(gstate);
  if (success)
    return WORKER_INSERT_RESULT_SUCCESS;
  else
    return WORKER_INSERT_RESULT_DROP;
}

static PyObject *
_py_get_attr_or_null(PyObject *o, const gchar *attr)
{
  PyObject *result;

  if (!attr)
    return NULL;

  result = PyObject_GetAttrString(o, attr);
  if (!result)
    {
      PyErr_Clear();
      return NULL;
    }
  return result;
}

static gboolean
_py_init_bindings(PythonDestDriver *self)
{
  self->py.module = _py_do_import(self, self->filename);
  if (!self->py.module)
    return FALSE;

  self->py.queue = _py_get_attr_or_null(self->py.module, self->queue_func_name);
  if (!self->py.queue)
    return FALSE;

  self->py.init = _py_get_attr_or_null(self->py.module, self->init_func_name);
  self->py.deinit = _py_get_attr_or_null(self->py.module, self->deinit_func_name);
  return TRUE;
}

static void
_py_free_bindings(PythonDestDriver *self)
{
  Py_CLEAR(self->py.module);
  Py_CLEAR(self->py.init);
  Py_CLEAR(self->py.queue);
  Py_CLEAR(self->py.deinit);
}

static gboolean
python_dd_init(LogPipe *d)
{
  PythonDestDriver *self = (PythonDestDriver *)d;
  GlobalConfig *cfg = log_pipe_get_config(d);
  PyGILState_STATE gstate;

  if (!self->filename)
    {
      msg_error("Error initializing Python destination: no script specified!",
                evt_tag_str("driver", self->super.super.super.id),
                NULL);
      return FALSE;
    }

  if (!log_dest_driver_init_method(d))
    return FALSE;

  log_template_options_init(&self->template_options, cfg);

  if (!self->queue_func_name)
    self->queue_func_name = g_strdup("queue");

  gstate = PyGILState_Ensure();

  _py_perform_imports(self);
  if (!_py_init_bindings(self) ||
      !_py_invoke_init(self))
    goto fail;

  PyGILState_Release(gstate);

  msg_verbose("Initializing Python destination",
              evt_tag_str("driver", self->super.super.super.id),
              evt_tag_str("script", self->filename),
              NULL);

  return log_threaded_dest_driver_start(d);

 fail:
  PyGILState_Release(gstate);
  return FALSE;
}

static gboolean
python_dd_deinit(LogPipe *d)
{
  PythonDestDriver *self = (PythonDestDriver *)d;
  PyGILState_STATE gstate;

  gstate = PyGILState_Ensure();
  _py_invoke_deinit(self);
  PyGILState_Release(gstate);

  return log_threaded_dest_driver_deinit_method(d);
}

static void
python_dd_free(LogPipe *d)
{
  PythonDestDriver *self = (PythonDestDriver *)d;
  PyGILState_STATE gstate;

  log_template_options_destroy(&self->template_options);

  gstate = PyGILState_Ensure();
  _py_free_bindings(self);
  PyGILState_Release(gstate);

  g_free(self->filename);
  g_free(self->init_func_name);
  g_free(self->queue_func_name);
  g_free(self->deinit_func_name);

  if (self->vp)
    value_pairs_free(self->vp);

  log_threaded_dest_driver_free(d);
}

LogDriver *
python_dd_new(GlobalConfig *cfg)
{
  PythonDestDriver *self = g_new0(PythonDestDriver, 1);

  log_threaded_dest_driver_init_instance(&self->super, cfg);
  log_template_options_defaults(&self->template_options);

  self->super.super.super.super.init = python_dd_init;
  self->super.super.super.super.deinit = python_dd_deinit;
  self->super.super.super.super.free_fn = python_dd_free;

  self->super.worker.disconnect = NULL;
  self->super.worker.insert = python_dd_insert;

  self->super.format.stats_instance = python_dd_format_stats_instance;
  self->super.format.persist_name = python_dd_format_persist_name;
  self->super.stats_source = SCS_PYTHON;

  python_dd_set_value_pairs(&self->super.super.super, value_pairs_new_default(cfg));

  return (LogDriver *)self;
}

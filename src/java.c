/**
 * collectd - src/java.c
 * Copyright (C) 2009  Florian octo Forster
 * Copyright (C) 2008  Justo Alonso Achaques
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 *   Justo Alonso Achaques <justo.alonso at gmail.com>
 **/

#include "collectd.h"
#include "plugin.h"
#include "common.h"

#include <pthread.h>
#include <jni.h>

#if !defined(JNI_VERSION_1_2)
# error "Need JNI 1.2 compatible interface!"
#endif

/*
 * Types
 */
struct cjni_jvm_env_s /* {{{ */
{
  JNIEnv *jvm_env;
  int reference_counter;
};
typedef struct cjni_jvm_env_s cjni_jvm_env_t;
/* }}} */

struct java_plugin_class_s /* {{{ */
{
  char     *name;
  jclass    class;
  jobject   object;
};
typedef struct java_plugin_class_s java_plugin_class_t;
/* }}} */

struct java_plugin_config_s /* {{{ */
{
  char *name;
  oconfig_item_t *ci;
};
typedef struct java_plugin_config_s java_plugin_config_t;
/* }}} */

#define CB_TYPE_CONFIG   1
#define CB_TYPE_INIT     2
#define CB_TYPE_READ     3
#define CB_TYPE_WRITE    4
#define CB_TYPE_SHUTDOWN 5
struct cjni_callback_info_s /* {{{ */
{
  char     *name;
  int       type;
  jclass    class;
  jobject   object;
  jmethodID method;
};
typedef struct cjni_callback_info_s cjni_callback_info_t;
/* }}} */

/*
 * Global variables
 */
static JavaVM *jvm = NULL;
static pthread_key_t jvm_env_key;

/* Configuration options for the JVM. */
static char **jvm_argv = NULL;
static size_t jvm_argc = 0;

/* List of class names to load */
static java_plugin_class_t  *java_classes_list = NULL;
static size_t                java_classes_list_len;

/* List of `config_item_t's for Java plugins */
static java_plugin_config_t *java_plugin_configs     = NULL;
static size_t                java_plugin_configs_num = 0;

/* List of config, init, and shutdown callbacks. */
static cjni_callback_info_t *java_callbacks      = NULL;
static size_t                java_callbacks_num  = 0;
static pthread_mutex_t       java_callbacks_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Prototypes
 *
 * Mostly functions that are needed by the Java interface (``native'')
 * functions.
 */
static void cjni_callback_info_destroy (void *arg);
static cjni_callback_info_t *cjni_callback_info_create (JNIEnv *jvm_env,
    jobject o_name, jobject o_callback, int type);
static int cjni_callback_register (JNIEnv *jvm_env, jobject o_name,
    jobject o_callback, int type);
static int cjni_read (user_data_t *user_data);
static int cjni_write (const data_set_t *ds, const value_list_t *vl,
    user_data_t *ud);

/* 
 * C to Java conversion functions
 */
static int ctoj_string (JNIEnv *jvm_env, /* {{{ */
    const char *string,
    jclass class_ptr, jobject object_ptr, const char *method_name)
{
  jmethodID m_set;
  jstring o_string;

  /* Create a java.lang.String */
  o_string = (*jvm_env)->NewStringUTF (jvm_env,
      (string != NULL) ? string : "");
  if (o_string == NULL)
  {
    ERROR ("java plugin: ctoj_string: NewStringUTF failed.");
    return (-1);
  }

  /* Search for the `void setFoo (String s)' method. */
  m_set = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      method_name, "(Ljava/lang/String;)V");
  if (m_set == NULL)
  {
    ERROR ("java plugin: ctoj_string: Cannot find method `void %s (String)'.",
        method_name);
    (*jvm_env)->DeleteLocalRef (jvm_env, o_string);
    return (-1);
  }

  /* Call the method. */
  (*jvm_env)->CallVoidMethod (jvm_env, object_ptr, m_set, o_string);

  /* Decrease reference counter on the java.lang.String object. */
  (*jvm_env)->DeleteLocalRef (jvm_env, o_string);

  return (0);
} /* }}} int ctoj_string */

static int ctoj_int (JNIEnv *jvm_env, /* {{{ */
    jint value,
    jclass class_ptr, jobject object_ptr, const char *method_name)
{
  jmethodID m_set;

  /* Search for the `void setFoo (int i)' method. */
  m_set = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      method_name, "(I)V");
  if (m_set == NULL)
  {
    ERROR ("java plugin: ctoj_int: Cannot find method `void %s (int)'.",
        method_name);
    return (-1);
  }

  (*jvm_env)->CallVoidMethod (jvm_env, object_ptr, m_set, value);

  return (0);
} /* }}} int ctoj_int */

static int ctoj_long (JNIEnv *jvm_env, /* {{{ */
    jlong value,
    jclass class_ptr, jobject object_ptr, const char *method_name)
{
  jmethodID m_set;

  /* Search for the `void setFoo (long l)' method. */
  m_set = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      method_name, "(J)V");
  if (m_set == NULL)
  {
    ERROR ("java plugin: ctoj_long: Cannot find method `void %s (long)'.",
        method_name);
    return (-1);
  }

  (*jvm_env)->CallVoidMethod (jvm_env, object_ptr, m_set, value);

  return (0);
} /* }}} int ctoj_long */

static int ctoj_double (JNIEnv *jvm_env, /* {{{ */
    jdouble value,
    jclass class_ptr, jobject object_ptr, const char *method_name)
{
  jmethodID m_set;

  /* Search for the `void setFoo (double d)' method. */
  m_set = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      method_name, "(D)V");
  if (m_set == NULL)
  {
    ERROR ("java plugin: ctoj_double: Cannot find method `void %s (double)'.",
        method_name);
    return (-1);
  }

  (*jvm_env)->CallVoidMethod (jvm_env, object_ptr, m_set, value);

  return (0);
} /* }}} int ctoj_double */

/* Convert a jlong to a java.lang.Number */
static jobject ctoj_jlong_to_number (JNIEnv *jvm_env, jlong value) /* {{{ */
{
  jclass c_long;
  jmethodID m_long_constructor;

  /* Look up the java.lang.Long class */
  c_long = (*jvm_env)->FindClass (jvm_env, "java.lang.Long");
  if (c_long == NULL)
  {
    ERROR ("java plugin: ctoj_jlong_to_number: Looking up the "
        "java.lang.Long class failed.");
    return (NULL);
  }

  m_long_constructor = (*jvm_env)->GetMethodID (jvm_env,
      c_long, "<init>", "(J)V");
  if (m_long_constructor == NULL)
  {
    ERROR ("java plugin: ctoj_jlong_to_number: Looking up the "
        "`Long (long)' constructor failed.");
    return (NULL);
  }

  return ((*jvm_env)->NewObject (jvm_env,
        c_long, m_long_constructor, value));
} /* }}} jobject ctoj_jlong_to_number */

/* Convert a jdouble to a java.lang.Number */
static jobject ctoj_jdouble_to_number (JNIEnv *jvm_env, jdouble value) /* {{{ */
{
  jclass c_double;
  jmethodID m_double_constructor;

  /* Look up the java.lang.Long class */
  c_double = (*jvm_env)->FindClass (jvm_env, "java.lang.Double");
  if (c_double == NULL)
  {
    ERROR ("java plugin: ctoj_jdouble_to_number: Looking up the "
        "java.lang.Double class failed.");
    return (NULL);
  }

  m_double_constructor = (*jvm_env)->GetMethodID (jvm_env,
      c_double, "<init>", "(D)V");
  if (m_double_constructor == NULL)
  {
    ERROR ("java plugin: ctoj_jdouble_to_number: Looking up the "
        "`Double (double)' constructor failed.");
    return (NULL);
  }

  return ((*jvm_env)->NewObject (jvm_env,
        c_double, m_double_constructor, value));
} /* }}} jobject ctoj_jdouble_to_number */

/* Convert a value_t to a java.lang.Number */
static jobject ctoj_value_to_number (JNIEnv *jvm_env, /* {{{ */
    value_t value, int ds_type)
{
  if (ds_type == DS_TYPE_COUNTER)
    return (ctoj_jlong_to_number (jvm_env, (jlong) value.counter));
  else if (ds_type == DS_TYPE_GAUGE)
    return (ctoj_jdouble_to_number (jvm_env, (jdouble) value.gauge));
  else
    return (NULL);
} /* }}} jobject ctoj_value_to_number */

/* Convert a data_source_t to a org.collectd.api.DataSource */
static jobject ctoj_data_source (JNIEnv *jvm_env, /* {{{ */
    const data_source_t *dsrc)
{
  jclass c_datasource;
  jmethodID m_datasource_constructor;
  jobject o_datasource;
  int status;

  /* Look up the DataSource class */
  c_datasource = (*jvm_env)->FindClass (jvm_env,
      "org.collectd.api.DataSource");
  if (c_datasource == NULL)
  {
    ERROR ("java plugin: ctoj_data_source: "
        "FindClass (org.collectd.api.DataSource) failed.");
    return (NULL);
  }

  /* Lookup the `ValueList ()' constructor. */
  m_datasource_constructor = (*jvm_env)->GetMethodID (jvm_env, c_datasource,
      "<init>", "()V");
  if (m_datasource_constructor == NULL)
  {
    ERROR ("java plugin: ctoj_data_source: Cannot find the "
        "`DataSource ()' constructor.");
    return (NULL);
  }

  /* Create a new instance. */
  o_datasource = (*jvm_env)->NewObject (jvm_env, c_datasource,
      m_datasource_constructor);
  if (o_datasource == NULL)
  {
    ERROR ("java plugin: ctoj_data_source: "
        "Creating a new DataSource instance failed.");
    return (NULL);
  }

  /* Set name via `void setName (String name)' */
  status = ctoj_string (jvm_env, dsrc->name,
      c_datasource, o_datasource, "setName");
  if (status != 0)
  {
    ERROR ("java plugin: ctoj_data_source: "
        "ctoj_string (setName) failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_datasource);
    return (NULL);
  }

  /* Set type via `void setType (int type)' */
  status = ctoj_int (jvm_env, dsrc->type,
      c_datasource, o_datasource, "setType");
  if (status != 0)
  {
    ERROR ("java plugin: ctoj_data_source: "
        "ctoj_int (setType) failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_datasource);
    return (NULL);
  }

  /* Set min via `void setMin (double min)' */
  status = ctoj_double (jvm_env, dsrc->min,
      c_datasource, o_datasource, "setMin");
  if (status != 0)
  {
    ERROR ("java plugin: ctoj_data_source: "
        "ctoj_double (setMin) failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_datasource);
    return (NULL);
  }

  /* Set max via `void setMax (double max)' */
  status = ctoj_double (jvm_env, dsrc->max,
      c_datasource, o_datasource, "setMax");
  if (status != 0)
  {
    ERROR ("java plugin: ctoj_data_source: "
        "ctoj_double (setMax) failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_datasource);
    return (NULL);
  }

  return (o_datasource);
} /* }}} jobject ctoj_data_source */

/* Convert a oconfig_value_t to a org.collectd.api.OConfigValue */
static jobject ctoj_oconfig_value (JNIEnv *jvm_env, /* {{{ */
    oconfig_value_t ocvalue)
{
  jclass c_ocvalue;
  jmethodID m_ocvalue_constructor;
  jobject o_argument;
  jobject o_ocvalue;

  m_ocvalue_constructor = NULL;
  o_argument = NULL;

  c_ocvalue = (*jvm_env)->FindClass (jvm_env,
      "org.collectd.api.OConfigValue");
  if (c_ocvalue == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_value: "
        "FindClass (org.collectd.api.OConfigValue) failed.");
    return (NULL);
  }

  if (ocvalue.type == OCONFIG_TYPE_BOOLEAN)
  {
    jboolean tmp_boolean;

    tmp_boolean = (ocvalue.value.boolean == 0) ? JNI_FALSE : JNI_TRUE;

    m_ocvalue_constructor = (*jvm_env)->GetMethodID (jvm_env, c_ocvalue,
        "<init>", "(Z)V");
    if (m_ocvalue_constructor == NULL)
    {
      ERROR ("java plugin: ctoj_oconfig_value: Cannot find the "
          "`OConfigValue (boolean)' constructor.");
      return (NULL);
    }

    return ((*jvm_env)->NewObject (jvm_env,
          c_ocvalue, m_ocvalue_constructor, tmp_boolean));
  } /* if (ocvalue.type == OCONFIG_TYPE_BOOLEAN) */
  else if (ocvalue.type == OCONFIG_TYPE_STRING)
  {
    m_ocvalue_constructor = (*jvm_env)->GetMethodID (jvm_env, c_ocvalue,
        "<init>", "(Ljava/lang/String;)V");
    if (m_ocvalue_constructor == NULL)
    {
      ERROR ("java plugin: ctoj_oconfig_value: Cannot find the "
          "`OConfigValue (String)' constructor.");
      return (NULL);
    }

    o_argument = (*jvm_env)->NewStringUTF (jvm_env, ocvalue.value.string);
    if (o_argument == NULL)
    {
      ERROR ("java plugin: ctoj_oconfig_value: "
          "Creating a String object failed.");
      return (NULL);
    }
  }
  else if (ocvalue.type == OCONFIG_TYPE_NUMBER)
  {
    m_ocvalue_constructor = (*jvm_env)->GetMethodID (jvm_env, c_ocvalue,
        "<init>", "(Ljava/lang/Number;)V");
    if (m_ocvalue_constructor == NULL)
    {
      ERROR ("java plugin: ctoj_oconfig_value: Cannot find the "
          "`OConfigValue (Number)' constructor.");
      return (NULL);
    }

    o_argument = ctoj_jdouble_to_number (jvm_env,
        (jdouble) ocvalue.value.number);
    if (o_argument == NULL)
    {
      ERROR ("java plugin: ctoj_oconfig_value: "
          "Creating a Number object failed.");
      return (NULL);
    }
  }
  else
  {
    return (NULL);
  }

  assert (m_ocvalue_constructor != NULL);
  assert (o_argument != NULL);

  o_ocvalue = (*jvm_env)->NewObject (jvm_env,
      c_ocvalue, m_ocvalue_constructor, o_argument);
  if (o_ocvalue == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_value: "
        "Creating an OConfigValue object failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_argument);
    return (NULL);
  }

  (*jvm_env)->DeleteLocalRef (jvm_env, o_argument);
  return (o_ocvalue);
} /* }}} jobject ctoj_oconfig_value */

/* Convert a oconfig_item_t to a org.collectd.api.OConfigItem */
static jobject ctoj_oconfig_item (JNIEnv *jvm_env, /* {{{ */
    const oconfig_item_t *ci)
{
  jclass c_ocitem;
  jmethodID m_ocitem_constructor;
  jmethodID m_addvalue;
  jmethodID m_addchild;
  jobject o_key;
  jobject o_ocitem;
  int i;

  c_ocitem = (*jvm_env)->FindClass (jvm_env, "org.collectd.api.OConfigItem");
  if (c_ocitem == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_item: "
        "FindClass (org.collectd.api.OConfigItem) failed.");
    return (NULL);
  }

  /* Get the required methods: m_ocitem_constructor, m_addvalue, and m_addchild
   * {{{ */
  m_ocitem_constructor = (*jvm_env)->GetMethodID (jvm_env, c_ocitem,
      "<init>", "(Ljava/lang/String;)V");
  if (m_ocitem_constructor == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_item: Cannot find the "
        "`OConfigItem (String)' constructor.");
    return (NULL);
  }

  m_addvalue = (*jvm_env)->GetMethodID (jvm_env, c_ocitem,
      "addValue", "(Lorg/collectd/api/OConfigValue;)V");
  if (m_addvalue == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_item: Cannot find the "
        "`addValue (OConfigValue)' method.");
    return (NULL);
  }

  m_addchild = (*jvm_env)->GetMethodID (jvm_env, c_ocitem,
      "addChild", "(Lorg/collectd/api/OConfigItem;)V");
  if (m_addchild == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_item: Cannot find the "
        "`addChild (OConfigItem)' method.");
    return (NULL);
  }
  /* }}} */

  /* Create a String object with the key.
   * Needed for calling the constructor. */
  o_key = (*jvm_env)->NewStringUTF (jvm_env, ci->key);
  if (o_key == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_item: "
        "Creating String object failed.");
    return (NULL);
  }

  /* Create an OConfigItem object */
  o_ocitem = (*jvm_env)->NewObject (jvm_env,
      c_ocitem, m_ocitem_constructor, o_key);
  if (o_ocitem == NULL)
  {
    ERROR ("java plugin: ctoj_oconfig_item: "
        "Creating an OConfigItem object failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_key);
    return (NULL);
  }

  /* We don't need the String object any longer.. */
  (*jvm_env)->DeleteLocalRef (jvm_env, o_key);

  /* Call OConfigItem.addValue for each value */
  for (i = 0; i < ci->values_num; i++) /* {{{ */
  {
    jobject o_value;

    o_value = ctoj_oconfig_value (jvm_env, ci->values[i]);
    if (o_value == NULL)
    {
      ERROR ("java plugin: ctoj_oconfig_item: "
          "Creating an OConfigValue object failed.");
      (*jvm_env)->DeleteLocalRef (jvm_env, o_ocitem);
      return (NULL);
    }

    (*jvm_env)->CallVoidMethod (jvm_env, o_ocitem, m_addvalue, o_value);
    (*jvm_env)->DeleteLocalRef (jvm_env, o_value);
  } /* }}} for (i = 0; i < ci->values_num; i++) */

  /* Call OConfigItem.addChild for each child */
  for (i = 0; i < ci->children_num; i++) /* {{{ */
  {
    jobject o_child;

    o_child = ctoj_oconfig_item (jvm_env, ci->children + i);
    if (o_child == NULL)
    {
      ERROR ("java plugin: ctoj_oconfig_item: "
          "Creating an OConfigItem object failed.");
      (*jvm_env)->DeleteLocalRef (jvm_env, o_ocitem);
      return (NULL);
    }

    (*jvm_env)->CallVoidMethod (jvm_env, o_ocitem, m_addvalue, o_child);
    (*jvm_env)->DeleteLocalRef (jvm_env, o_child);
  } /* }}} for (i = 0; i < ci->children_num; i++) */

  return (o_ocitem);
} /* }}} jobject ctoj_oconfig_item */

/* Convert a data_set_t to a org.collectd.api.DataSet */
static jobject ctoj_data_set (JNIEnv *jvm_env, const data_set_t *ds) /* {{{ */
{
  jclass c_dataset;
  jmethodID m_constructor;
  jmethodID m_add;
  jobject o_type;
  jobject o_dataset;
  int i;

  /* Look up the org.collectd.api.DataSet class */
  c_dataset = (*jvm_env)->FindClass (jvm_env, "org.collectd.api.DataSet");
  if (c_dataset == NULL)
  {
    ERROR ("java plugin: ctoj_data_set: Looking up the "
        "org.collectd.api.DataSet class failed.");
    return (NULL);
  }

  /* Search for the `DataSet (String type)' constructor. */
  m_constructor = (*jvm_env)->GetMethodID (jvm_env,
      c_dataset, "<init>", "(Ljava.lang.String;)V");
  if (m_constructor == NULL)
  {
    ERROR ("java plugin: ctoj_data_set: Looking up the "
        "`DataSet (String)' constructor failed.");
    return (NULL);
  }

  /* Search for the `void addDataSource (DataSource)' method. */
  m_add = (*jvm_env)->GetMethodID (jvm_env,
      c_dataset, "addDataSource", "(Lorg.collectd.api.DataSource;)V");
  if (m_add == NULL)
  {
    ERROR ("java plugin: ctoj_data_set: Looking up the "
        "`addDataSource (DataSource)' method failed.");
    return (NULL);
  }

  o_type = (*jvm_env)->NewStringUTF (jvm_env, ds->type);
  if (o_type == NULL)
  {
    ERROR ("java plugin: ctoj_data_set: Creating a String object failed.");
    return (NULL);
  }

  o_dataset = (*jvm_env)->NewObject (jvm_env,
      c_dataset, m_constructor, o_type);
  if (o_dataset == NULL)
  {
    ERROR ("java plugin: ctoj_data_set: Creating a DataSet object failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_type);
    return (NULL);
  }

  /* Decrease reference counter on the java.lang.String object. */
  (*jvm_env)->DeleteLocalRef (jvm_env, o_type);

  for (i = 0; i < ds->ds_num; i++)
  {
    jobject o_datasource;

    o_datasource = ctoj_data_source (jvm_env, ds->ds + i);
    if (o_datasource == NULL)
    {
      ERROR ("java plugin: ctoj_data_set: ctoj_data_source (%s.%s) failed",
          ds->type, ds->ds[i].name);
      (*jvm_env)->DeleteLocalRef (jvm_env, o_dataset);
      return (NULL);
    }

    (*jvm_env)->CallVoidMethod (jvm_env, o_dataset, m_add, o_datasource);

    (*jvm_env)->DeleteLocalRef (jvm_env, o_datasource);
  } /* for (i = 0; i < ds->ds_num; i++) */

  return (o_dataset);
} /* }}} jobject ctoj_data_set */

static int ctoj_value_list_add_value (JNIEnv *jvm_env, /* {{{ */
    value_t value, int ds_type,
    jclass class_ptr, jobject object_ptr)
{
  jmethodID m_addvalue;
  jobject o_number;

  m_addvalue = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      "addValue", "(Ljava/lang/Number;)V");
  if (m_addvalue == NULL)
  {
    ERROR ("java plugin: ctoj_value_list_add_value: "
        "Cannot find method `void addValue (Number)'.");
    return (-1);
  }

  o_number = ctoj_value_to_number (jvm_env, value, ds_type);
  if (o_number == NULL)
  {
    ERROR ("java plugin: ctoj_value_list_add_value: "
        "ctoj_value_to_number failed.");
    return (-1);
  }

  (*jvm_env)->CallVoidMethod (jvm_env, object_ptr, m_addvalue, o_number);

  (*jvm_env)->DeleteLocalRef (jvm_env, o_number);

  return (0);
} /* }}} int ctoj_value_list_add_value */

static int ctoj_value_list_add_data_set (JNIEnv *jvm_env, /* {{{ */
    jclass c_valuelist, jobject o_valuelist, const data_set_t *ds)
{
  jmethodID m_setdataset;
  jobject o_dataset;

  /* Look for the `void setDataSource (List<DataSource> ds)' method. */
  m_setdataset = (*jvm_env)->GetMethodID (jvm_env, c_valuelist,
      "setDataSet", "(Lorg.collectd.api.DataSet;)V");
  if (m_setdataset == NULL)
  {
    ERROR ("java plugin: ctoj_value_list_add_data_set: "
        "Cannot find the `void setDataSet (DataSet)' method.");
    return (-1);
  }

  /* Create a DataSet object. */
  o_dataset = ctoj_data_set (jvm_env, ds);
  if (o_dataset == NULL)
  {
    ERROR ("java plugin: ctoj_value_list_add_data_set: "
        "ctoj_data_set (%s) failed.", ds->type);
    return (-1);
  }

  /* Actually call the method. */
  (*jvm_env)->CallVoidMethod (jvm_env,
      o_valuelist, m_setdataset, o_dataset);

  /* Decrease reference counter on the List<DataSource> object. */
  (*jvm_env)->DeleteLocalRef (jvm_env, o_dataset);

  return (0);
} /* }}} int ctoj_value_list_add_data_set */

/* Convert a value_list_t (and data_set_t) to a org.collectd.api.ValueList */
static jobject ctoj_value_list (JNIEnv *jvm_env, /* {{{ */
    const data_set_t *ds, const value_list_t *vl)
{
  jclass c_valuelist;
  jmethodID m_valuelist_constructor;
  jobject o_valuelist;
  int status;
  int i;

  /* First, create a new ValueList instance..
   * Look up the class.. */
  c_valuelist = (*jvm_env)->FindClass (jvm_env,
      "org.collectd.api.ValueList");
  if (c_valuelist == NULL)
  {
    ERROR ("java plugin: ctoj_value_list: "
        "FindClass (org.collectd.api.ValueList) failed.");
    return (NULL);
  }

  /* Lookup the `ValueList ()' constructor. */
  m_valuelist_constructor = (*jvm_env)->GetMethodID (jvm_env, c_valuelist,
      "<init>", "()V");
  if (m_valuelist_constructor == NULL)
  {
    ERROR ("java plugin: ctoj_value_list: Cannot find the "
        "`ValueList ()' constructor.");
    return (NULL);
  }

  /* Create a new instance. */
  o_valuelist = (*jvm_env)->NewObject (jvm_env, c_valuelist,
      m_valuelist_constructor);
  if (o_valuelist == NULL)
  {
    ERROR ("java plugin: ctoj_value_list: Creating a new ValueList instance "
        "failed.");
    return (NULL);
  }

  status = ctoj_value_list_add_data_set (jvm_env,
      c_valuelist, o_valuelist, ds);
  if (status != 0)
  {
    ERROR ("java plugin: ctoj_value_list: "
        "ctoj_value_list_add_data_set failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_valuelist);
    return (NULL);
  }

  /* Set the strings.. */
#define SET_STRING(str,method_name) do { \
  status = ctoj_string (jvm_env, str, \
      c_valuelist, o_valuelist, method_name); \
  if (status != 0) { \
    ERROR ("java plugin: ctoj_value_list: jtoc_string (%s) failed.", \
        method_name); \
    (*jvm_env)->DeleteLocalRef (jvm_env, o_valuelist); \
    return (NULL); \
  } } while (0)

  SET_STRING (vl->host,            "setHost");
  SET_STRING (vl->plugin,          "setPlugin");
  SET_STRING (vl->plugin_instance, "setPluginInstance");
  SET_STRING (vl->type,            "setType");
  SET_STRING (vl->type_instance,   "setTypeInstance");

#undef SET_STRING

  /* Set the `time' member. Java stores time in milliseconds. */
  status = ctoj_long (jvm_env, ((jlong) vl->time) * ((jlong) 1000),
      c_valuelist, o_valuelist, "setTime");
  if (status != 0)
  {
    ERROR ("java plugin: ctoj_value_list: ctoj_long (setTime) failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_valuelist);
    return (NULL);
  }

  /* Set the `interval' member.. */
  status = ctoj_long (jvm_env, (jlong) vl->interval,
      c_valuelist, o_valuelist, "setInterval");
  if (status != 0)
  {
    ERROR ("java plugin: ctoj_value_list: ctoj_long (setInterval) failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, o_valuelist);
    return (NULL);
  }

  for (i = 0; i < vl->values_len; i++)
  {
    status = ctoj_value_list_add_value (jvm_env, vl->values[i], ds->ds[i].type,
        c_valuelist, o_valuelist);
    if (status != 0)
    {
      ERROR ("java plugin: ctoj_value_list: "
          "ctoj_value_list_add_value failed.");
      (*jvm_env)->DeleteLocalRef (jvm_env, o_valuelist);
      return (NULL);
    }
  }

  return (o_valuelist);
} /* }}} int ctoj_value_list */

/*
 * Java to C conversion functions
 */
/* Call a `String <method> ()' method. */
static int jtoc_string (JNIEnv *jvm_env, /* {{{ */
    char *buffer, size_t buffer_size,
    jclass class_ptr, jobject object_ptr, const char *method_name)
{
  jmethodID method_id;
  jobject string_obj;
  const char *c_str;

  method_id = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      method_name, "()Ljava/lang/String;");
  if (method_id == NULL)
  {
    ERROR ("java plugin: jtoc_string: Cannot find method `String %s ()'.",
        method_name);
    return (-1);
  }

  string_obj = (*jvm_env)->CallObjectMethod (jvm_env, object_ptr, method_id);
  if (string_obj == NULL)
  {
    ERROR ("java plugin: jtoc_string: CallObjectMethod (%s) failed.",
        method_name);
    return (-1);
  }

  c_str = (*jvm_env)->GetStringUTFChars (jvm_env, string_obj, 0);
  if (c_str == NULL)
  {
    ERROR ("java plugin: jtoc_string: GetStringUTFChars failed.");
    (*jvm_env)->DeleteLocalRef (jvm_env, string_obj);
    return (-1);
  }

  sstrncpy (buffer, c_str, buffer_size);

  (*jvm_env)->ReleaseStringUTFChars (jvm_env, string_obj, c_str);
  (*jvm_env)->DeleteLocalRef (jvm_env, string_obj);

  return (0);
} /* }}} int jtoc_string */

/* Call a `long <method> ()' method. */
static int jtoc_long (JNIEnv *jvm_env, /* {{{ */
    jlong *ret_value,
    jclass class_ptr, jobject object_ptr, const char *method_name)
{
  jmethodID method_id;

  method_id = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      method_name, "()J");
  if (method_id == NULL)
  {
    ERROR ("java plugin: jtoc_long: Cannot find method `long %s ()'.",
        method_name);
    return (-1);
  }

  *ret_value = (*jvm_env)->CallLongMethod (jvm_env, object_ptr, method_id);

  return (0);
} /* }}} int jtoc_long */

/* Call a `double <method> ()' method. */
static int jtoc_double (JNIEnv *jvm_env, /* {{{ */
    jdouble *ret_value,
    jclass class_ptr, jobject object_ptr, const char *method_name)
{
  jmethodID method_id;

  method_id = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      method_name, "()D");
  if (method_id == NULL)
  {
    ERROR ("java plugin: jtoc_string: Cannot find method `double %s ()'.",
        method_name);
    return (-1);
  }

  *ret_value = (*jvm_env)->CallDoubleMethod (jvm_env, object_ptr, method_id);

  return (0);
} /* }}} int jtoc_double */

static int jtoc_value (JNIEnv *jvm_env, /* {{{ */
    value_t *ret_value, int ds_type, jobject object_ptr)
{
  jclass class_ptr;
  int status;

  class_ptr = (*jvm_env)->GetObjectClass (jvm_env, object_ptr);

  if (ds_type == DS_TYPE_COUNTER)
  {
    jlong tmp_long;

    status = jtoc_long (jvm_env, &tmp_long,
        class_ptr, object_ptr, "longValue");
    if (status != 0)
    {
      ERROR ("java plugin: jtoc_value: "
          "jtoc_long failed.");
      return (-1);
    }
    (*ret_value).counter = (counter_t) tmp_long;
  }
  else
  {
    jdouble tmp_double;

    status = jtoc_double (jvm_env, &tmp_double,
        class_ptr, object_ptr, "doubleValue");
    if (status != 0)
    {
      ERROR ("java plugin: jtoc_value: "
          "jtoc_double failed.");
      return (-1);
    }
    (*ret_value).gauge = (gauge_t) tmp_double;
  }

  return (0);
} /* }}} int jtoc_value */

/* Read a List<Number>, convert it to `value_t' and add it to the given
 * `value_list_t'. */
static int jtoc_values_array (JNIEnv *jvm_env, /* {{{ */
    const data_set_t *ds, value_list_t *vl,
    jclass class_ptr, jobject object_ptr)
{
  jmethodID m_getvalues;
  jmethodID m_toarray;
  jobject o_list;
  jobjectArray o_number_array;

  value_t *values;
  int values_num;
  int i;

  values_num = ds->ds_num;

  values = NULL;
  o_number_array = NULL;
  o_list = NULL;

#define BAIL_OUT(status) \
  free (values); \
  if (o_number_array != NULL) \
    (*jvm_env)->DeleteLocalRef (jvm_env, o_number_array); \
  if (o_list != NULL) \
    (*jvm_env)->DeleteLocalRef (jvm_env, o_list); \
  return (status);

  /* Call: List<Number> ValueList.getValues () */
  m_getvalues = (*jvm_env)->GetMethodID (jvm_env, class_ptr,
      "getValues", "()Ljava/util/List;");
  if (m_getvalues == NULL)
  {
    ERROR ("java plugin: jtoc_values_array: "
        "Cannot find method `List getValues ()'.");
    BAIL_OUT (-1);
  }

  o_list = (*jvm_env)->CallObjectMethod (jvm_env, object_ptr, m_getvalues);
  if (o_list == NULL)
  {
    ERROR ("java plugin: jtoc_values_array: "
        "CallObjectMethod (getValues) failed.");
    BAIL_OUT (-1);
  }

  /* Call: Number[] List.toArray () */
  m_toarray = (*jvm_env)->GetMethodID (jvm_env,
      (*jvm_env)->GetObjectClass (jvm_env, o_list),
      "toArray", "()[Ljava/lang/Object;");
  if (m_toarray == NULL)
  {
    ERROR ("java plugin: jtoc_values_array: "
        "Cannot find method `Object[] toArray ()'.");
    BAIL_OUT (-1);
  }

  o_number_array = (*jvm_env)->CallObjectMethod (jvm_env, o_list, m_toarray);
  if (o_number_array == NULL)
  {
    ERROR ("java plugin: jtoc_values_array: "
        "CallObjectMethod (toArray) failed.");
    BAIL_OUT (-1);
  }

  values = (value_t *) calloc (values_num, sizeof (value_t));
  if (values == NULL)
  {
    ERROR ("java plugin: jtoc_values_array: calloc failed.");
    BAIL_OUT (-1);
  }

  for (i = 0; i < values_num; i++)
  {
    jobject o_number;
    int status;

    o_number = (*jvm_env)->GetObjectArrayElement (jvm_env,
        o_number_array, (jsize) i);
    if (o_number == NULL)
    {
      ERROR ("java plugin: jtoc_values_array: "
          "GetObjectArrayElement (%i) failed.", i);
      BAIL_OUT (-1);
    }

    status = jtoc_value (jvm_env, values + i, ds->ds[i].type, o_number);
    if (status != 0)
    {
      ERROR ("java plugin: jtoc_values_array: "
          "jtoc_value (%i) failed.", i);
      BAIL_OUT (-1);
    }
  } /* for (i = 0; i < values_num; i++) */

  vl->values = values;
  vl->values_len = values_num;

#undef BAIL_OUT
  (*jvm_env)->DeleteLocalRef (jvm_env, o_number_array);
  (*jvm_env)->DeleteLocalRef (jvm_env, o_list);
  return (0);
} /* }}} int jtoc_values_array */

/* Convert a org.collectd.api.ValueList to a value_list_t. */
static int jtoc_value_list (JNIEnv *jvm_env, value_list_t *vl, /* {{{ */
    jobject object_ptr)
{
  jclass class_ptr;
  int status;
  jlong tmp_long;
  const data_set_t *ds;

  class_ptr = (*jvm_env)->GetObjectClass (jvm_env, object_ptr);
  if (class_ptr == NULL)
  {
    ERROR ("java plugin: jtoc_value_list: GetObjectClass failed.");
    return (-1);
  }

#define SET_STRING(buffer,method) do { \
  status = jtoc_string (jvm_env, buffer, sizeof (buffer), \
      class_ptr, object_ptr, method); \
  if (status != 0) { \
    ERROR ("java plugin: jtoc_value_list: jtoc_string (%s) failed.", \
        method); \
    return (-1); \
  } } while (0)

  SET_STRING(vl->type, "getType");

  ds = plugin_get_ds (vl->type);
  if (ds == NULL)
  {
    ERROR ("java plugin: jtoc_value_list: Data-set `%s' is not defined. "
        "Please consult the types.db(5) manpage for mor information.",
        vl->type);
    return (-1);
  }

  SET_STRING(vl->host, "getHost");
  SET_STRING(vl->plugin, "getPlugin");
  SET_STRING(vl->plugin_instance, "getPluginInstance");
  SET_STRING(vl->type_instance, "getTypeInstance");

#undef SET_STRING

  status = jtoc_long (jvm_env, &tmp_long, class_ptr, object_ptr, "getTime");
  if (status != 0)
  {
    ERROR ("java plugin: jtoc_value_list: jtoc_long (getTime) failed.");
    return (-1);
  }
  /* Java measures time in milliseconds. */
  vl->time = (time_t) (tmp_long / ((jlong) 1000));

  status = jtoc_long (jvm_env, &tmp_long,
      class_ptr, object_ptr, "getInterval");
  if (status != 0)
  {
    ERROR ("java plugin: jtoc_value_list: jtoc_long (getInterval) failed.");
    return (-1);
  }
  vl->interval = (int) tmp_long;

  status = jtoc_values_array (jvm_env, ds, vl, class_ptr, object_ptr);
  if (status != 0)
  {
    ERROR ("java plugin: jtoc_value_list: jtoc_values_array failed.");
    return (-1);
  }

  return (0);
} /* }}} int jtoc_value_list */

/* 
 * Functions accessible from Java
 */
static jint JNICALL cjni_api_dispatch_values (JNIEnv *jvm_env, /* {{{ */
    jobject this, jobject java_vl)
{
  value_list_t vl = VALUE_LIST_INIT;
  int status;

  DEBUG ("cjni_api_dispatch_values: java_vl = %p;", (void *) java_vl);

  status = jtoc_value_list (jvm_env, &vl, java_vl);
  if (status != 0)
  {
    ERROR ("java plugin: cjni_api_dispatch_values: jtoc_value_list failed.");
    return (-1);
  }

  status = plugin_dispatch_values (&vl);

  sfree (vl.values);

  return (status);
} /* }}} jint cjni_api_dispatch_values */

static jobject JNICALL cjni_api_get_ds (JNIEnv *jvm_env, /* {{{ */
    jobject this, jobject o_string_type)
{
  const char *ds_name;
  const data_set_t *ds;
  jobject o_dataset;

  ds_name = (*jvm_env)->GetStringUTFChars (jvm_env, o_string_type, 0);
  if (ds_name == NULL)
  {
    ERROR ("java plugin: cjni_api_get_ds: GetStringUTFChars failed.");
    return (NULL);
  }

  ds = plugin_get_ds (ds_name);
  DEBUG ("java plugin: cjni_api_get_ds: "
      "plugin_get_ds (%s) = %p;", ds_name, (void *) ds);

  (*jvm_env)->ReleaseStringUTFChars (jvm_env, o_string_type, ds_name);

  if (ds == NULL)
    return (NULL);

  o_dataset = ctoj_data_set (jvm_env, ds);
  return (o_dataset);
} /* }}} jint cjni_api_get_ds */

static jint JNICALL cjni_api_register_config (JNIEnv *jvm_env, /* {{{ */
    jobject this, jobject o_name, jobject o_config)
{
  return (cjni_callback_register (jvm_env, o_name, o_config, CB_TYPE_CONFIG));
} /* }}} jint cjni_api_register_config */

static jint JNICALL cjni_api_register_init (JNIEnv *jvm_env, /* {{{ */
    jobject this, jobject o_name, jobject o_config)
{
  return (cjni_callback_register (jvm_env, o_name, o_config, CB_TYPE_INIT));
} /* }}} jint cjni_api_register_init */

static jint JNICALL cjni_api_register_read (JNIEnv *jvm_env, /* {{{ */
    jobject this, jobject o_name, jobject o_read)
{
  user_data_t ud;
  cjni_callback_info_t *cbi;

  cbi = cjni_callback_info_create (jvm_env, o_name, o_read, CB_TYPE_READ);
  if (cbi == NULL)
    return (-1);

  DEBUG ("java plugin: Registering new read callback: %s", cbi->name);

  memset (&ud, 0, sizeof (ud));
  ud.data = (void *) cbi;
  ud.free_func = cjni_callback_info_destroy;

  plugin_register_complex_read (cbi->name, cjni_read, &ud);

  (*jvm_env)->DeleteLocalRef (jvm_env, o_read);

  return (0);
} /* }}} jint cjni_api_register_read */

static jint JNICALL cjni_api_register_write (JNIEnv *jvm_env, /* {{{ */
    jobject this, jobject o_name, jobject o_write)
{
  user_data_t ud;
  cjni_callback_info_t *cbi;

  cbi = cjni_callback_info_create (jvm_env, o_name, o_write, CB_TYPE_WRITE);
  if (cbi == NULL)
    return (-1);

  DEBUG ("java plugin: Registering new write callback: %s", cbi->name);

  memset (&ud, 0, sizeof (ud));
  ud.data = (void *) cbi;
  ud.free_func = cjni_callback_info_destroy;

  plugin_register_write (cbi->name, cjni_write, &ud);

  (*jvm_env)->DeleteLocalRef (jvm_env, o_write);

  return (0);
} /* }}} jint cjni_api_register_write */

static jint JNICALL cjni_api_register_shutdown (JNIEnv *jvm_env, /* {{{ */
    jobject this, jobject o_name, jobject o_shutdown)
{
  return (cjni_callback_register (jvm_env, o_name, o_shutdown,
        CB_TYPE_SHUTDOWN));
} /* }}} jint cjni_api_register_shutdown */

static void JNICALL cjni_api_log (JNIEnv *jvm_env, /* {{{ */
    jobject this, jint severity, jobject o_message)
{
  const char *c_str;

  c_str = (*jvm_env)->GetStringUTFChars (jvm_env, o_message, 0);
  if (c_str == NULL)
  {
    ERROR ("java plugin: cjni_api_log: GetStringUTFChars failed.");
    return;
  }

  if (severity < LOG_ERR)
    severity = LOG_ERR;
  if (severity > LOG_DEBUG)
    severity = LOG_DEBUG;

  plugin_log (severity, "%s", c_str);

  (*jvm_env)->ReleaseStringUTFChars (jvm_env, o_message, c_str);
} /* }}} void cjni_api_log */

/* List of ``native'' functions, i. e. C-functions that can be called from
 * Java. */
static JNINativeMethod jni_api_functions[] = /* {{{ */
{
  { "DispatchValues",
    "(Lorg/collectd/api/ValueList;)I",
    cjni_api_dispatch_values },

  { "GetDS",
    "(Ljava/lang/String;)Lorg/collectd/api/DataSet;",
    cjni_api_get_ds },

  { "RegisterConfig",
    "(Ljava/lang/String;Lorg/collectd/api/CollectdConfigInterface;)I",
    cjni_api_register_config },

  { "RegisterInit",
    "(Ljava/lang/String;Lorg/collectd/api/CollectdInitInterface;)I",
    cjni_api_register_init },

  { "RegisterRead",
    "(Ljava/lang/String;Lorg/collectd/api/CollectdReadInterface;)I",
    cjni_api_register_read },

  { "RegisterWrite",
    "(Ljava/lang/String;Lorg/collectd/api/CollectdWriteInterface;)I",
    cjni_api_register_write },

  { "RegisterShutdown",
    "(Ljava/lang/String;Lorg/collectd/api/CollectdShutdownInterface;)I",
    cjni_api_register_shutdown },

  { "Log",
    "(ILjava/lang/String;)V",
    cjni_api_log },
};
static size_t jni_api_functions_num = sizeof (jni_api_functions)
  / sizeof (jni_api_functions[0]);
/* }}} */

/*
 * Functions
 */
/* Allocate a `cjni_callback_info_t' given the type and objects necessary for
 * all registration functions. */
static cjni_callback_info_t *cjni_callback_info_create (JNIEnv *jvm_env, /* {{{ */
    jobject o_name, jobject o_callback, int type)
{
  const char *c_name;
  cjni_callback_info_t *cbi;
  const char *method_name;
  const char *method_signature;

  switch (type)
  {
    case CB_TYPE_CONFIG:
      method_name = "Config";
      method_signature = "(Lorg/collectd/api/OConfigItem;)I";
      break;

    case CB_TYPE_INIT:
      method_name = "Init";
      method_signature = "()I";
      break;

    case CB_TYPE_READ:
      method_name = "Read";
      method_signature = "()I";
      break;

    case CB_TYPE_WRITE:
      method_name = "Write";
      method_signature = "(Lorg/collectd/api/ValueList;)I";
      break;

    case CB_TYPE_SHUTDOWN:
      method_name = "Shutdown";
      method_signature = "()I";
      break;

    default:
      ERROR ("java plugin: cjni_callback_info_create: Unknown type: %#x",
          type);
      return (NULL);
  }

  c_name = (*jvm_env)->GetStringUTFChars (jvm_env, o_name, 0);
  if (c_name == NULL)
  {
    ERROR ("java plugin: cjni_callback_info_create: "
        "GetStringUTFChars failed.");
    return (NULL);
  }

  cbi = (cjni_callback_info_t *) malloc (sizeof (*cbi));
  if (cbi == NULL)
  {
    ERROR ("java plugin: cjni_callback_info_create: malloc failed.");
    (*jvm_env)->ReleaseStringUTFChars (jvm_env, o_name, c_name);
    return (NULL);
  }
  memset (cbi, 0, sizeof (*cbi));
  cbi->type = type;

  cbi->name = strdup (c_name);
  if (cbi->name == NULL)
  {
    pthread_mutex_unlock (&java_callbacks_lock);
    ERROR ("java plugin: cjni_callback_info_create: strdup failed.");
    (*jvm_env)->ReleaseStringUTFChars (jvm_env, o_name, c_name);
    return (NULL);
  }

  (*jvm_env)->ReleaseStringUTFChars (jvm_env, o_name, c_name);

  cbi->class  = (*jvm_env)->GetObjectClass (jvm_env, o_callback);
  if (cbi->class == NULL)
  {
    ERROR ("java plugin: cjni_callback_info_create: GetObjectClass failed.");
    free (cbi);
    return (NULL);
  }

  cbi->object = o_callback;

  cbi->method = (*jvm_env)->GetMethodID (jvm_env, cbi->class,
      method_name, method_signature);
  if (cbi->method == NULL)
  {
    ERROR ("java plugin: cjni_callback_info_create: "
        "Cannot find the `%s' method with signature `%s'.",
        method_name, method_signature);
    free (cbi);
    return (NULL);
  }

  (*jvm_env)->NewGlobalRef (jvm_env, o_callback);

  return (cbi);
} /* }}} cjni_callback_info_t cjni_callback_info_create */

/* Allocate a `cjni_callback_info_t' via `cjni_callback_info_create' and add it
 * to the global `java_callbacks' variable. This is used for `config', `init',
 * and `shutdown' callbacks. */
static int cjni_callback_register (JNIEnv *jvm_env, /* {{{ */
    jobject o_name, jobject o_callback, int type)
{
  cjni_callback_info_t *cbi;
  cjni_callback_info_t *tmp;
#if COLLECT_DEBUG
  const char *type_str;
#endif

  cbi = cjni_callback_info_create (jvm_env, o_name, o_callback, type);
  if (cbi == NULL)
    return (-1);

#if COLLECT_DEBUG
  switch (type)
  {
    case CB_TYPE_CONFIG:
      type_str = "config";
      break;

    case CB_TYPE_INIT:
      type_str = "init";
      break;

    case CB_TYPE_SHUTDOWN:
      type_str = "shutdown";
      break;

    default:
      type_str = "<unknown>";
  }
  DEBUG ("java plugin: Registering new %s callback: %s",
      type_str, cbi->name);
#endif

  pthread_mutex_lock (&java_callbacks_lock);

  tmp = (cjni_callback_info_t *) realloc (java_callbacks,
      (java_callbacks_num + 1) * sizeof (*java_callbacks));
  if (tmp == NULL)
  {
    pthread_mutex_unlock (&java_callbacks_lock);
    ERROR ("java plugin: cjni_callback_register: realloc failed.");

    (*jvm_env)->DeleteGlobalRef (jvm_env, cbi->object);
    free (cbi);

    return (-1);
  }
  java_callbacks = tmp;
  java_callbacks[java_callbacks_num] = *cbi;
  java_callbacks_num++;

  pthread_mutex_unlock (&java_callbacks_lock);

  free (cbi);
  return (0);
} /* }}} int cjni_callback_register */

/* Increase the reference counter to the JVM for this thread. If it was zero,
 * attach the JVM first. */
static JNIEnv *cjni_thread_attach (void) /* {{{ */
{
  cjni_jvm_env_t *cjni_env;
  JNIEnv *jvm_env;

  cjni_env = pthread_getspecific (jvm_env_key);
  if (cjni_env == NULL)
  {
    /* This pointer is free'd in `cjni_jvm_env_destroy'. */
    cjni_env = (cjni_jvm_env_t *) malloc (sizeof (*cjni_env));
    if (cjni_env == NULL)
    {
      ERROR ("java plugin: cjni_thread_attach: malloc failed.");
      return (NULL);
    }
    memset (cjni_env, 0, sizeof (*cjni_env));
    cjni_env->reference_counter = 0;
    cjni_env->jvm_env = NULL;

    pthread_setspecific (jvm_env_key, cjni_env);
  }

  if (cjni_env->reference_counter > 0)
  {
    cjni_env->reference_counter++;
    jvm_env = cjni_env->jvm_env;
  }
  else
  {
    int status;
    JavaVMAttachArgs args;

    assert (cjni_env->jvm_env == NULL);

    memset (&args, 0, sizeof (args));
    args.version = JNI_VERSION_1_2;

    status = (*jvm)->AttachCurrentThread (jvm, (void *) &jvm_env, (void *) &args);
    if (status != 0)
    {
      ERROR ("java plugin: cjni_thread_attach: AttachCurrentThread failed "
          "with status %i.", status);
      return (NULL);
    }

    cjni_env->reference_counter = 1;
    cjni_env->jvm_env = jvm_env;
  }

  DEBUG ("java plugin: cjni_thread_attach: cjni_env->reference_counter = %i",
      cjni_env->reference_counter);
  assert (jvm_env != NULL);
  return (jvm_env);
} /* }}} JNIEnv *cjni_thread_attach */

/* Decrease the reference counter of this thread. If it reaches zero, detach
 * from the JVM. */
static int cjni_thread_detach (void) /* {{{ */
{
  cjni_jvm_env_t *cjni_env;
  int status;

  cjni_env = pthread_getspecific (jvm_env_key);
  if (cjni_env == NULL)
  {
    ERROR ("java plugin: cjni_thread_detach: pthread_getspecific failed.");
    return (-1);
  }

  assert (cjni_env->reference_counter > 0);
  assert (cjni_env->jvm_env != NULL);

  cjni_env->reference_counter--;
  DEBUG ("java plugin: cjni_thread_detach: cjni_env->reference_counter = %i",
      cjni_env->reference_counter);

  if (cjni_env->reference_counter > 0)
    return (0);

  status = (*jvm)->DetachCurrentThread (jvm);
  if (status != 0)
  {
    ERROR ("java plugin: cjni_thread_detach: DetachCurrentThread failed "
        "with status %i.", status);
  }

  cjni_env->reference_counter = 0;
  cjni_env->jvm_env = NULL;

  return (0);
} /* }}} JNIEnv *cjni_thread_attach */

/* Callback for `pthread_key_create'. It frees the data contained in
 * `jvm_env_key' and prints a warning if the reference counter is not zero. */
static void cjni_jvm_env_destroy (void *args) /* {{{ */
{
  cjni_jvm_env_t *cjni_env;

  if (args == NULL)
    return;

  cjni_env = (cjni_jvm_env_t *) args;

  if (cjni_env->reference_counter > 0)
  {
    ERROR ("java plugin: cjni_jvm_env_destroy: "
        "cjni_env->reference_counter = %i;", cjni_env->reference_counter);
  }

  if (cjni_env->jvm_env != NULL)
  {
    ERROR ("java plugin: cjni_jvm_env_destroy: cjni_env->jvm_env = %p;",
        (void *) cjni_env->jvm_env);
  }

  /* The pointer is allocated in `cjni_thread_attach' */
  free (cjni_env);
} /* }}} void cjni_jvm_env_destroy */

/* Boring configuration functions.. {{{ */
static int cjni_config_add_jvm_arg (oconfig_item_t *ci) /* {{{ */
{
  char **tmp;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("java plugin: `JVMArg' needs exactly one string argument.");
    return (-1);
  }

  tmp = (char **) realloc (jvm_argv, sizeof (char *) * (jvm_argc + 1));
  if (tmp == NULL)
  {
    ERROR ("java plugin: realloc failed.");
    return (-1);
  }
  jvm_argv = tmp;

  jvm_argv[jvm_argc] = strdup (ci->values[0].value.string);
  if (jvm_argv[jvm_argc] == NULL)
  {
    ERROR ("java plugin: strdup failed.");
    return (-1);
  }
  jvm_argc++;

  return (0);
} /* }}} int cjni_config_add_jvm_arg */

static int cjni_config_load_plugin (oconfig_item_t *ci) /* {{{ */
{
  java_plugin_class_t *tmp;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("java plugin: `LoadPlugin' needs exactly one string argument.");
    return (-1);
  }

  tmp = (java_plugin_class_t *) realloc (java_classes_list,
      (java_classes_list_len + 1) * sizeof (*java_classes_list));
  if (tmp == NULL)
  {
    ERROR ("java plugin: realloc failed.");
    return (-1);
  }
  java_classes_list = tmp;
  tmp = java_classes_list + java_classes_list_len;

  memset (tmp, 0, sizeof (*tmp));
  tmp->name = strdup (ci->values[0].value.string);
  if (tmp->name == NULL)
  {
    ERROR ("java plugin: strdup failed.");
    return (-1);
  }
  tmp->class = NULL;
  tmp->object = NULL;

  java_classes_list_len++;

  return (0);
} /* }}} int cjni_config_load_plugin */

static int cjni_config_plugin_block (oconfig_item_t *ci) /* {{{ */
{
  java_plugin_config_t *tmp;
  char *name;
  size_t i;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("java plugin: `Plugin' blocks "
        "need exactly one string argument.");
    return (-1);
  }

  name = strdup (ci->values[0].value.string);
  if (name == NULL)
  {
    ERROR ("java plugin: cjni_config_plugin_block: strdup faiiled.");
    return (-1);
  }

  for (i = 0; i < java_plugin_configs_num; i++)
  {
    if (strcmp (java_plugin_configs[i].name, name) == 0)
    {
      WARNING ("java plugin: There is more than one <Plugin \"%s\"> block. "
          "This is currently not supported - "
          "only the first block will be used!",
          name);
      free (name);
      return (0);
    }
  }

  tmp = (java_plugin_config_t *) realloc (java_plugin_configs,
      (java_plugin_configs_num + 1) * sizeof (*java_plugin_configs));
  if (tmp == NULL)
  {
    ERROR ("java plugin: cjni_config_plugin_block: realloc failed.");
    free (name);
    return (-1);
  }
  java_plugin_configs = tmp;
  tmp = java_plugin_configs + java_plugin_configs_num;

  tmp->name = name;
  tmp->ci = oconfig_clone (ci);
  if (tmp->ci == NULL)
  {
    ERROR ("java plugin: cjni_config_plugin_block: "
        "oconfig_clone failed for `%s'.",
        name);
    free (name);
    return (-1);
  }

  DEBUG ("java plugin: cjni_config_plugin_block: "
      "Successfully copied config for `%s'.",
      name);

  java_plugin_configs_num++;
  return (0);
} /* }}} int cjni_config_plugin_block */

static int cjni_config (oconfig_item_t *ci) /* {{{ */
{
  int success;
  int errors;
  int status;
  int i;

  success = 0;
  errors = 0;

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("JVMArg", child->key) == 0)
    {
      status = cjni_config_add_jvm_arg (child);
      if (status == 0)
        success++;
      else
        errors++;
    }
    else if (strcasecmp ("LoadPlugin", child->key) == 0)
    {
      status = cjni_config_load_plugin (child);
      if (status == 0)
        success++;
      else
        errors++;
    }
    else if (strcasecmp ("Plugin", child->key) == 0)
    {
      status = cjni_config_plugin_block (child);
      if (status == 0)
        success++;
      else
        errors++;
    }
    else
    {
      WARNING ("java plugin: Option `%s' not allowed here.", child->key);
      errors++;
    }
  }

  DEBUG ("java plugin: jvm_argc = %zu;", jvm_argc);
  DEBUG ("java plugin: java_classes_list_len = %zu;", java_classes_list_len);
  DEBUG ("java plugin: java_plugin_configs_num = %zu;",
      java_plugin_configs_num);

  if ((success == 0) && (errors > 0))
  {
    ERROR ("java plugin: All statements failed.");
    return (-1);
  }

  return (0);
} /* }}} int cjni_config */
/* }}} */

/* Free the data contained in the `user_data_t' pointer passed to `cjni_read'
 * and `cjni_write'. In particular, delete the global reference to the Java
 * object. */
static void cjni_callback_info_destroy (void *arg) /* {{{ */
{
  JNIEnv *jvm_env;
  cjni_callback_info_t *cbi;

  DEBUG ("java plugin: cjni_callback_info_destroy (arg = %p);", arg);

  if (arg == NULL)
    return;

  jvm_env = cjni_thread_attach ();
  if (jvm_env == NULL)
  {
    ERROR ("java plugin: cjni_callback_info_destroy: cjni_thread_attach failed.");
    return;
  }

  cbi = (cjni_callback_info_t *) arg;

  (*jvm_env)->DeleteGlobalRef (jvm_env, cbi->object);

  cbi->method = NULL;
  cbi->object = NULL;
  cbi->class  = NULL;
  free (cbi);

  cjni_thread_detach ();
} /* }}} void cjni_callback_info_destroy */

/* Call the CB_TYPE_READ callback pointed to by the `user_data_t' pointer. */
static int cjni_read (user_data_t *ud) /* {{{ */
{
  JNIEnv *jvm_env;
  cjni_callback_info_t *cbi;
  int status;

  if (jvm == NULL)
  {
    ERROR ("java plugin: cjni_read: jvm == NULL");
    return (-1);
  }

  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("java plugin: cjni_read: Invalid user data.");
    return (-1);
  }

  jvm_env = cjni_thread_attach ();
  if (jvm_env == NULL)
    return (-1);

  cbi = (cjni_callback_info_t *) ud->data;

  status = (*jvm_env)->CallIntMethod (jvm_env, cbi->object,
      cbi->method);

  status = cjni_thread_detach ();
  if (status != 0)
  {
    ERROR ("java plugin: cjni_read: cjni_thread_detach failed.");
    return (-1);
  }

  return (status);
} /* }}} int cjni_read */

/* Call the CB_TYPE_WRITE callback pointed to by the `user_data_t' pointer. */
static int cjni_write (const data_set_t *ds, const value_list_t *vl, /* {{{ */
    user_data_t *ud)
{
  JNIEnv *jvm_env;
  cjni_callback_info_t *cbi;
  jobject vl_java;
  int status;

  if (jvm == NULL)
  {
    ERROR ("java plugin: cjni_write: jvm == NULL");
    return (-1);
  }

  if ((ud == NULL) || (ud->data == NULL))
  {
    ERROR ("java plugin: cjni_write: Invalid user data.");
    return (-1);
  }

  jvm_env = cjni_thread_attach ();
  if (jvm_env == NULL)
    return (-1);

  cbi = (cjni_callback_info_t *) ud->data;

  vl_java = ctoj_value_list (jvm_env, ds, vl);
  if (vl_java == NULL)
  {
    ERROR ("java plugin: cjni_write: ctoj_value_list failed.");
    return (-1);
  }

  status = (*jvm_env)->CallIntMethod (jvm_env,
      cbi->object, cbi->method, vl_java);

  (*jvm_env)->DeleteLocalRef (jvm_env, vl_java);

  status = cjni_thread_detach ();
  if (status != 0)
  {
    ERROR ("java plugin: cjni_write: cjni_thread_detach failed.");
    return (-1);
  }

  return (status);
} /* }}} int cjni_write */

/* Iterate over `java_classes_list' and create one object of each class. This
 * will trigger the object's constructors, to the objects can register callback
 * methods. */
static int cjni_load_plugins (JNIEnv *jvm_env) /* {{{ */
{
  size_t i;

  for (i = 0; i < java_classes_list_len; i++)
  {
    java_plugin_class_t *class;
    jmethodID constructor_id;

    class = java_classes_list + i;

    DEBUG ("java plugin: Loading class %s", class->name);

    class->class = (*jvm_env)->FindClass (jvm_env, class->name);
    if (class->class == NULL)
    {
      ERROR ("java plugin: cjni_load_plugins: FindClass (%s) failed.",
          class->name);
      continue;
    }

    constructor_id = (*jvm_env)->GetMethodID (jvm_env, class->class,
        "<init>", "()V");
    if (constructor_id == NULL)
    {
      ERROR ("java plugin: cjni_load_plugins: Could not find the constructor for `%s'.",
          class->name);
      continue;
    }

    class->object = (*jvm_env)->NewObject (jvm_env, class->class,
        constructor_id);
    if (class->object == NULL)
    {
      ERROR ("java plugin: cjni_load_plugins: Could create a new `%s' object.",
          class->name);
      continue;
    }

    (*jvm_env)->NewGlobalRef (jvm_env, class->object);
  } /* for (i = 0; i < java_classes_list_len; i++) */

  return (0);
} /* }}} int cjni_load_plugins */

/* Iterate over `java_plugin_configs' and `java_callbacks' and call all
 * `config' callback methods for which a configuration is available. */
static int cjni_config_plugins (JNIEnv *jvm_env) /* {{{ */
{
  int status;
  size_t i;
  size_t j;

  for (i = 0; i < java_plugin_configs_num; i++)
  {
    jobject o_ocitem;

    if (java_plugin_configs[i].ci == NULL)
      continue;

    for (j = 0; j < java_callbacks_num; j++)
    {
      if (java_callbacks[j].type != CB_TYPE_CONFIG)
        continue;

      if (strcmp (java_plugin_configs[i].name, java_callbacks[j].name) == 0)
        break;
    }

    if (j >= java_callbacks_num)
    {
      NOTICE ("java plugin: Configuration for `%s' is present, but no such "
          "configuration callback has been registered.",
          java_plugin_configs[i].name);
      continue;
    }

    DEBUG ("java plugin: Configuring %s", java_plugin_configs[i].name);

    o_ocitem = ctoj_oconfig_item (jvm_env, java_plugin_configs[i].ci);
    if (o_ocitem == NULL)
    {
      ERROR ("java plugin: cjni_config_plugins: ctoj_oconfig_item failed.");
      continue;
    }

    status = (*jvm_env)->CallIntMethod (jvm_env,
        java_callbacks[j].object, java_callbacks[j].method, o_ocitem);
    WARNING ("java plugin: Config callback for `%s' returned status %i.",
        java_plugin_configs[i].name, status);
  } /* for (i = 0; i < java_plugin_configs; i++) */

  return (0);
} /* }}} int cjni_config_plugins */

/* Iterate over `java_callbacks' and call all CB_TYPE_INIT callbacks. */
static int cjni_init_plugins (JNIEnv *jvm_env) /* {{{ */
{
  int status;
  size_t i;

  for (i = 0; i < java_callbacks_num; i++)
  {
    if (java_callbacks[i].type != CB_TYPE_INIT)
      continue;

    DEBUG ("java plugin: Initializing %s", java_callbacks[i].name);

    status = (*jvm_env)->CallIntMethod (jvm_env,
        java_callbacks[i].object, java_callbacks[i].method);
    if (status != 0)
    {
      ERROR ("java plugin: Initializing `%s' failed with status %i. "
          "Removing read function.",
          java_callbacks[i].name, status);
      plugin_unregister_read (java_callbacks[i].name);
    }
  }

  return (0);
} /* }}} int cjni_init_plugins */

/* Iterate over `java_callbacks' and call all CB_TYPE_SHUTDOWN callbacks. */
static int cjni_shutdown_plugins (JNIEnv *jvm_env) /* {{{ */
{
  int status;
  size_t i;

  for (i = 0; i < java_callbacks_num; i++)
  {
    if (java_callbacks[i].type != CB_TYPE_SHUTDOWN)
      continue;

    DEBUG ("java plugin: Shutting down %s", java_callbacks[i].name);

    status = (*jvm_env)->CallIntMethod (jvm_env,
        java_callbacks[i].object, java_callbacks[i].method);
    if (status != 0)
    {
      ERROR ("java plugin: Shutting down `%s' failed with status %i. ",
          java_callbacks[i].name, status);
    }
  }

  return (0);
} /* }}} int cjni_shutdown_plugins */


static int cjni_shutdown (void) /* {{{ */
{
  JNIEnv *jvm_env;
  JavaVMAttachArgs args;
  int status;
  size_t i;

  if (jvm == NULL)
    return (0);

  jvm_env = NULL;
  memset (&args, 0, sizeof (args));
  args.version = JNI_VERSION_1_2;

  status = (*jvm)->AttachCurrentThread (jvm, (void **) &jvm_env, &args);
  if (status != 0)
  {
    ERROR ("java plugin: cjni_shutdown: AttachCurrentThread failed with status %i.",
        status);
    return (-1);
  }

  /* Execute all the shutdown functions registered by plugins. */
  cjni_shutdown_plugins (jvm_env);

  /* Release all the global references to callback functions */
  for (i = 0; i < java_callbacks_num; i++)
  {
    if (java_callbacks[i].object != NULL)
    {
      (*jvm_env)->DeleteGlobalRef (jvm_env, java_callbacks[i].object);
      java_callbacks[i].object = NULL;
    }
    sfree (java_callbacks[i].name);
  }
  java_callbacks_num = 0;
  sfree (java_callbacks);

  /* Release all the global references to directly loaded classes. */
  for (i = 0; i < java_classes_list_len; i++)
  {
    if (java_classes_list[i].object != NULL)
    {
      (*jvm_env)->DeleteGlobalRef (jvm_env, java_classes_list[i].object);
      java_classes_list[i].object = NULL;
    }
    sfree (java_classes_list[i].name);
  }
  java_classes_list_len = 0;
  sfree (java_classes_list);

  /* Destroy the JVM */
  (*jvm)->DestroyJavaVM (jvm);
  jvm = NULL;
  jvm_env = NULL;

  pthread_key_delete (jvm_env_key);

  /* Free the JVM argument list */
  for (i = 0; i < jvm_argc; i++)
    sfree (jvm_argv[i]);
  jvm_argc = 0;
  sfree (jvm_argv);

  /* Free the copied configuration */
  for (i = 0; i < java_plugin_configs_num; i++)
  {
    sfree (java_plugin_configs[i].name);
    oconfig_free (java_plugin_configs[i].ci);
  }
  java_plugin_configs_num = 0;
  sfree (java_plugin_configs);

  return (0);
} /* }}} int cjni_shutdown */

/* Register ``native'' functions with the JVM. Native functions are C-functions
 * that can be called by Java code. */
static int cjni_init_native (JNIEnv *jvm_env) /* {{{ */
{
  jclass api_class_ptr;
  int status;

  api_class_ptr = (*jvm_env)->FindClass (jvm_env, "org.collectd.api.CollectdAPI");
  if (api_class_ptr == NULL)
  {
    ERROR ("cjni_init_native: Cannot find API class `org.collectd.api.CollectdAPI'.");
    return (-1);
  }

  status = (*jvm_env)->RegisterNatives (jvm_env, api_class_ptr,
      jni_api_functions, (jint) jni_api_functions_num);
  if (status != 0)
  {
    ERROR ("cjni_init_native: RegisterNatives failed with status %i.", status);
    return (-1);
  }

  return (0);
} /* }}} int cjni_init_native */

/* Initialization: Create a JVM, load all configured classes and call their
 * `config' and `init' callback methods. */
static int cjni_init (void) /* {{{ */
{
  JNIEnv *jvm_env;
  JavaVMInitArgs vm_args;
  JavaVMOption vm_options[jvm_argc];

  int status;
  size_t i;

  if (jvm != NULL)
    return (0);

  status = pthread_key_create (&jvm_env_key, cjni_jvm_env_destroy);
  if (status != 0)
  {
    ERROR ("java plugin: cjni_init: pthread_key_create failed "
        "with status %i.", status);
    return (-1);
  }

  jvm_env = NULL;

  memset (&vm_args, 0, sizeof (vm_args));
  vm_args.version = JNI_VERSION_1_2;
  vm_args.options = vm_options;
  vm_args.nOptions = (jint) jvm_argc;

  for (i = 0; i < jvm_argc; i++)
  {
    DEBUG ("java plugin: cjni_init: jvm_argv[%zu] = %s", i, jvm_argv[i]);
    vm_args.options[i].optionString = jvm_argv[i];
  }
  /*
  vm_args.options[0].optionString = "-verbose:jni";
  vm_args.options[1].optionString = "-Djava.class.path=/home/octo/collectd/bindings/java";
  */

  status = JNI_CreateJavaVM (&jvm, (void **) &jvm_env, (void **) &vm_args);
  if (status != 0)
  {
    ERROR ("cjni_init: JNI_CreateJavaVM failed with status %i.",
	status);
    return (-1);
  }
  assert (jvm != NULL);
  assert (jvm_env != NULL);

  /* Call RegisterNatives */
  status = cjni_init_native (jvm_env);
  if (status != 0)
  {
    ERROR ("cjni_init: cjni_init_native failed.");
    return (-1);
  }

  cjni_load_plugins (jvm_env);
  cjni_config_plugins (jvm_env);
  cjni_init_plugins (jvm_env);

  return (0);
} /* }}} int cjni_init */

void module_register (void)
{
  plugin_register_complex_config ("java", cjni_config);
  plugin_register_init ("java", cjni_init);
  plugin_register_shutdown ("java", cjni_shutdown);
} /* void module_register (void) */

/* vim: set sw=2 sts=2 et fdm=marker : */

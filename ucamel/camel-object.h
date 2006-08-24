#ifndef _CAMEL_OBJECT_H
#define _CAMEL_OBJECT_H

#include <glib-object.h>

#define CamelType GType

#define CAMEL_INVALID_TYPE 0

#define CAMEL_CHECK_CAST(obj, type, cast) G_TYPE_CHECK_INSTANCE_CAST (obj, type, cast)
#define CAMEL_CHECK_CLASS_CAST(class, type, cast) G_TYPE_CHECK_CLASS_CAST (class, type, cast)
#define CAMEL_CHECK_TYPE(obj, type) G_TYPE_CHECK_INSTANCE_TYPE(obj, type)

#define CAMEL_OBJECT(obj)         G_OBJECT(obj)
#define CAMEL_OBJECT_GET_CLASS(obj) G_OBJECT_GET_CLASS(obj)

#define CamelObject GObject
#define CamelObjectClass GObjectClass
#define CAMEL_OBJECT_GET_TYPE(obj) G_OBJECT_TYPE(obj)
#define camel_type_get_global_classfuncs(class) g_type_class_peek_parent (klass)

#define CamelObjectClassInitFunc GClassInitFunc
#define CamelObjectInitFunc GInstanceInitFunc
#define CamelObjectFinalizeFunc void*

#define camel_object_get_type() G_TYPE_OBJECT()

#define camel_object_new(type) g_object_new(type, NULL)
#define camel_object_unref(o) g_object_unref(o)

#endif /* _CAMEL_OBJECT_H */

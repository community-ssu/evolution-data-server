#include <glib.h>

guint
craphash (gconstpointer v, guint len)
{
  /* 31 bit hash function */
  const signed char *p = v;
  guint32 h = *p++;
  
  while (--len) {
    h = (h << 5) - h + *p++;
  }
  return h;
}

#include <glib.h>

static guint
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

int main (int argc, char **argv) {
  char *buf;
  int len;
  g_file_get_contents(argv[1], &buf, &len, NULL);
  g_print("CrapHash is %u\n", craphash (buf, len));
  g_free (buf);
  return 0;
}

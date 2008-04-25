#include <gtk/gtk.h>
#include "e-categories-dialog.h"

int
main(int argc, char **argv)
{
  GtkWidget *dialog;
  gtk_init (&argc, &argv);
  dialog = e_categories_dialog_new ("");
  gtk_widget_show (dialog);
  gtk_main ();
  return 0;
}

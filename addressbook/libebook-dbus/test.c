#include "e-book.h"
#include <dbus/dbus-glib.h>

static GMainLoop *loop = NULL;

/* Convenience function to print an error and exit */
static void
die (const char *prefix, GError *error) 
{
  if (error) {
    if (error->code == DBUS_GERROR_REMOTE_EXCEPTION) {
      g_error("%s: %s (%s)", prefix, error->message, dbus_g_error_get_name (error));
    } else {
      g_error("%s: %s (%d)", prefix, error->message, error->code);
    }
    g_error_free (error);
  } else {
    g_error (prefix);
  }
  exit(1);
}

static void get_contacts_cb (EBook *book, EBookStatus status, GList *list, gpointer closure)
{
  g_print ("Got %d contacts over async\n", g_list_length (list));
  g_list_foreach (list, (GFunc)g_object_unref, NULL);
  g_list_free (list);
  g_main_loop_quit (loop);
}

static void view_status_message (EBookView *book_view, const char*message, gpointer userdata)
{
  g_print("Status: %s\n", message);
}

static void view_contacts_added (EBookView *book_view, GList *contacts, gpointer userdata)
{
  while (contacts) {
    g_print ("Got contact %s\n",  (char*)e_contact_get_const (contacts->data, E_CONTACT_FILE_AS));
    contacts = g_list_next (contacts);
  }
}

static void view_complete (EBookView *book_view, EBookViewStatus status, gpointer userdata)
{
  g_print ("Finished book view with status %d\n", status);
  e_book_view_stop (book_view);
  g_object_unref (book_view);
  g_main_loop_quit (loop);
}

int main(int argc, char **argv) {
  int count;
  GError *error = NULL;
  EBook *book;
  char *uri;

  g_type_init();

  switch (argc) {
  case 1:
    uri = g_strconcat ("file://", g_get_home_dir (), "/.evolution/addressbook/local/system", NULL);
    count = 1;
    break;
  case 2:
    uri = g_strconcat ("file://", g_get_home_dir (), "/.evolution/addressbook/local/system", NULL);
    count = atoi (argv[1]);
    break;
  case 3:
    uri = g_strdup (argv[1]);
    count = atoi (argv[2]);
    break;
  default:
    g_printerr("$ test\n$ test <count>\n$test <uri> <count>\n");
    return 1;
  }

  loop = g_main_loop_new (NULL, FALSE);

  book = e_book_new_from_uri (uri, &error);
  g_free (uri);
  
  if (!book)
    die ("Cannot get book", error);
  g_print("Got book\n");

  if (!e_book_open (book, TRUE, &error))
    die ("Cannot open book", error);
  g_print("Opened book\n");

  while (count--) {
#if 0
  {
    EContact *contact = NULL;
    if (!e_book_get_contact (book, "pas-id-4174EAA10000000C", &contact, &error))
      die ("Call to get_contact failed", error);
    g_print ("Got contact with name %s\n", (char*)e_contact_get_const (contact, E_CONTACT_FILE_AS));
    {
      GList *l;
      l = e_contact_get (contact, E_CONTACT_EMAIL);
      if (l) {
        g_print ("Contact has %d email addresses\n", g_list_length (l));
        g_list_foreach(l, (GFunc)g_free, NULL);
        g_list_free(l); 
      } else {
        g_print ("Contact has no email addresses\n");
      }
      l = NULL;
    }
    g_object_unref (contact);
  }
#endif

#if 0
  {
    GList *contacts;
    EBookQuery *query = e_book_query_field_exists (E_CONTACT_FULL_NAME);
    if (!e_book_get_contacts (book, query, &contacts, &error))
      die ("Call to get_contacts failed", error);
    g_print ("Got %d contacts\n", g_list_length (contacts));
    g_list_foreach (contacts, (GFunc)g_object_unref, NULL);
    g_list_free (contacts);
    e_book_query_unref (query);
  }
#endif

#if 0
  {
    e_book_async_get_contacts (book, e_book_query_field_exists (E_CONTACT_FULL_NAME), get_contacts_cb, NULL);
    g_main_loop_run (loop);
  }
#endif

#if 1
  {
    EBookView *view = NULL;
    EBookQuery *query = e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_CONTAINS, "Burton");
    //EBookQuery *query = e_book_query_any_field_contains ("");
    if (!e_book_get_book_view (book, query, NULL, 50, &view, &error))
      die ("Call to get_book_view failed", error);
    e_book_query_unref (query);
    g_object_connect (view,
                      "signal::contacts-added", G_CALLBACK (view_contacts_added), NULL,
                      "signal::status-message", G_CALLBACK (view_status_message), NULL,
                      "signal::sequence-complete", G_CALLBACK (view_complete), NULL,
                      NULL);
    e_book_view_start (view);
    g_main_loop_run (loop);
  }
#endif
  }
  g_object_unref (book);
  g_print("Closed book\n");

  g_main_loop_unref (loop);
  return 0;
}

#ifndef __E_BOOK_UTIL_H__
#define __E_BOOK_UTIL_H__

#include <libebook/e-contact.h>
#include <libebook/e-book.h>

void            e_book_util_remove_duplicates            (GList  *haystack,
                                                          GList **needles);

gboolean        e_book_util_remove_duplicates_using_book (EBook   *book,
                                                          GList  **contacts,
                                                          GError **error);

#endif /* __E_BOOK_UTIL_H__ */

#ifndef __E_BOOK_UTIL_H__
#define __E_BOOK_UTIL_H__

#include <libebook/e-contact.h>
#include <libebook/e-book.h>

G_BEGIN_DECLS

void            e_book_util_remove_duplicates            (GList  *haystack,
                                                          GList **needles,
                                                          GList **duplicate_ids);

gboolean        e_book_util_remove_duplicates_using_book (EBook   *book,
                                                          GList  **contacts,
                                                          GList  **duplicate_ids,
                                                          GError **error);

G_END_DECLS

#endif /* __E_BOOK_UTIL_H__ */

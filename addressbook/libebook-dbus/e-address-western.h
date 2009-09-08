#ifndef __E_ADDRESS_WESTERN_H__
#define __E_ADDRESS_WESTERN_H__

G_BEGIN_DECLS

/**
 * EAddressWestern:
 * @po_box: the post office box
 * @extended: an extra line for the address (e.g., "Apt. #101", "c/o John Doe").
 * Locale-specific.
 * @street: the street and number
 * @locality: usually "city", though this depends upon the locale
 * @region: region, province, or state (locale-specific)
 * @postal_code: postal/zip code
 * @country: country
 *
 * A structured representation of the contact's ADR vCard field.
 **/
typedef struct {
	char *po_box;
	char *extended;
	char *street;
	char *locality;
	char *region;
	char *postal_code;
	char *country;
} EAddressWestern;

EAddressWestern *e_address_western_parse (const char *in_address);
void e_address_western_free (EAddressWestern *eaw);

G_END_DECLS

#endif  /* ! __E_ADDRESS_WESTERN_H__ */



#include "config.h"
#include "e-vcard.h"

#include <nscore.h>
#include <nsUniversalDetector.h>
#include <string.h>

class ECharsetDetector :
        public nsUniversalDetector
{
public:
        ECharsetDetector (void) :
                nsUniversalDetector (NS_FILTER_ALL),
                mCharset (0)
        {
        }

        virtual ~ECharsetDetector (void)
        {
                g_free (mCharset);
        }

        const char * GetCharset (void) const
        {
                return mCharset;
        }

        char * StealCharset (void)
        {
                char *charset = mCharset;
                mCharset = NULL;
                return charset;
        }

protected:
        virtual void Report (const char *charset)
        {
                g_free (mCharset);
                mCharset = g_strdup (charset);
        }

        char *mCharset;
};

extern "C" G_GNUC_INTERNAL char *
_evc_guess_charset (EVCard *evc)
{
        ECharsetDetector  detector;
        nsresult          rv;

        /* feed the charset detector with attribute values */
        for (GList *a = e_vcard_get_attributes (evc); a; a = a->next) {
                EVCardAttribute *attr = static_cast<EVCardAttribute *> (a->data);

                for (GList *v = e_vcard_attribute_get_values (attr); v; v = v->next) {
                        char *value = static_cast<char *> (v->data);

                        rv = detector.HandleData (value, strlen (value));

                        if (detector.GetCharset ())
                                return detector.StealCharset ();
                        if (NS_FAILED (rv))
                                return NULL;
                }
        }

        /* seems we didn't have enought food yet, so continue with param values */
        for (GList *a = e_vcard_get_attributes (evc); a; a = a->next) {
                EVCardAttribute *attr = static_cast<EVCardAttribute *> (a->data);

                for (GList *p = e_vcard_attribute_get_params (attr); p; p = p->next) {
                        EVCardAttributeParam *param = static_cast<EVCardAttributeParam *> (p->data);

                        for (GList *v = e_vcard_attribute_param_get_values (param); v; v = v->next) {
                                char *value = static_cast<char *> (v->data);

                                rv = detector.HandleData (value, strlen (value));

                                if (detector.GetCharset ())
                                        return detector.StealCharset ();
                                if (NS_FAILED (rv))
                                        return NULL;
                        }
                }
        }

        /* last chance for the detector to get an opinion */
        detector.DataEnd ();

        return detector.StealCharset ();
}


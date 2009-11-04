#include "nscore.h"
#include "nsUniversalDetector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

class TestCharsetDetector :
        public nsUniversalDetector
{
public:
        TestCharsetDetector (void) :
                nsUniversalDetector (NS_FILTER_ALL),
                mCharset (NULL)
        {
        }

        virtual ~TestCharsetDetector (void)
        {
                if (mCharset)
                        free (mCharset);
        }

        const char * GetCharset (void) const
        {
                return mCharset;
        }

protected:
        virtual void Report (const char *charset)
        {
                if (mCharset)
                        free (mCharset);

                mCharset = strdup (charset);
        }

        char *mCharset;
};

int
main (int    argc,
      char **argv)
{
        TestCharsetDetector  detector;
        char                 buf[512];
        size_t               len;

        while (0 != (len = fread (buf, 1, sizeof buf, stdin))) {
                detector.HandleData (buf, len);

                if (detector.GetCharset ())
                        break;
        }

        if (!detector.GetCharset ())
                detector.DataEnd ();

        if (detector.GetCharset ()) {
                printf ("%s\n", detector.GetCharset ());
                return 0;
        }

        return 1;
}

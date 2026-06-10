#include "test/ktest.h"
#include "test/tests.h"

#include "security/cred.h"

void test_cred_run(void)
{
    ktest_suite("cred");

    /* A setuid-root program: real uid is the user (1000), effective and saved
       are 0 (elevated at exec). It can drop and later restore privilege. */
    cred_t c;
    cred_init(&c, 1000, 1000);
    c.euid = c.suid = 0;   /* simulate setuid-root exec */

    /* Drop effective privilege to the real user. */
    KTEST_ASSERT(cred_seteuid(&c, 1000) == true);
    KTEST_ASSERT(c.euid == 1000 && c.uid == 1000 && c.suid == 0);

    /* Restore: saved uid is 0, so re-elevation is permitted. */
    KTEST_ASSERT(cred_seteuid(&c, 0) == true);
    KTEST_ASSERT(c.euid == 0);

    /* An unprivileged process cannot become an arbitrary uid. */
    cred_t u;
    cred_init(&u, 1000, 1000);   /* real = eff = saved = 1000 */
    KTEST_ASSERT(cred_seteuid(&u, 0) == false);
    KTEST_ASSERT(cred_setuid(&u, 0) == false);
    KTEST_ASSERT(u.euid == 1000);

    /* setuid by root sets all three ids permanently (no way back). */
    cred_t r;
    cred_init(&r, 0, 0);
    KTEST_ASSERT(cred_setuid(&r, 1000) == true);
    KTEST_ASSERT(r.uid == 1000 && r.euid == 1000 && r.suid == 1000);
    KTEST_ASSERT(cred_seteuid(&r, 0) == false);

    /* cred_check: root bypasses; owner/group/other bits enforced. */
    cred_t o;
    cred_init(&o, 1000, 1000);
    KTEST_ASSERT(cred_check(&o, 1000, 1000, 0600, 4) == true);  /* owner read   */
    KTEST_ASSERT(cred_check(&o, 1000, 1000, 0600, 1) == false); /* no exec bit  */
    KTEST_ASSERT(cred_check(&o, 0, 0, 0600, 4) == false);       /* not owner    */
    cred_t root;
    cred_init(&root, 0, 0);
    KTEST_ASSERT(cred_check(&root, 1000, 1000, 0000, 7) == true); /* root all   */
}

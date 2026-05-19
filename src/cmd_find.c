/*
 * cmd_find.c — renatum find
 *
 * Deferred to v0.1.0+. A useful implementation needs an
 * index_iter_prefix or index_iter_all over the files sub-DB
 * (currently the only iterator walks a single exact path).
 * Stubbed out so the CLI binary still links and so users get a
 * clear "not yet" rather than a silent no-op.
 */

#include "config.h"
#include "renatum.h"

#include <stdio.h>

int cmd_find(int argc, char **argv, const rnt_config_t *cfg)
{
    (void)argc; (void)argv; (void)cfg;
    fprintf(stderr,
            "renatum find: not implemented in v0.1.0 "
            "(needs index prefix iterator)\n");
    return RNT_EXIT_USAGE;
}

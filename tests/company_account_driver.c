#include "company_account.h"
#include <string.h>
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--require") == 0) return company_account_require();
    return company_account_cli(argc, argv);
}

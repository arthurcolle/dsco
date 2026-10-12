#include "native_startup_card.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char card[512];
    assert(native_startup_card_format(card, sizeof(card), 18, 40, "trusted",
                                      "feature/pixel", 223.13, 25, 100));
    assert(strstr(card, "Features  18/40\nTrust  trusted\nBranch  feature/pixel\nBudget  $223.13 today / $25.00 session / $100.00 daily"));
    assert(!strpbrk(card, "\033\r|#"));
    assert(native_startup_card_format(card, sizeof(card), 5, 10,
                                      "trusted\nINVALID", "branch\033[31m", 0, 0, 0));
    assert(!strchr(card, '\033') && !strstr(card, "\nINVALID"));
    assert(!native_startup_card_format(card, 8, 18, 40, "trusted", "branch", 1, 2, 3));
    assert(card[0] == '\0');
    puts("native startup card: ok");
    return 0;
}

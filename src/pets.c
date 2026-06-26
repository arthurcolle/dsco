/* pets.c — companion sprites for background agents. See pets.h.
 *
 * Sprite art and the deterministic-roll idea are ported from claude-code-pure's
 * buddy/ module (sprites.ts, companion.ts). Each species has 3 idle-fidget
 * frames, 5 lines × 12 display columns, with "{E}" eye slots and an optional
 * hat on line 0. Bones derive from hash(seed) → mulberry32 PRNG so the same
 * agent always hatches the same pet without persisting anything.
 */
#include "pets.h"
#include "tui.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ── small utils ───────────────────────────────────────────────────────── */

static double pet_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Wall-clock-derived animation frame (~6fps). Lets sprites fidget across
 * successive renders without a perpetual render thread or per-call plumbing. */
static int pet_auto_frame(void) {
    return (int)(pet_now() * 6.0);
}

#include "crypto.h" /* fnv1a_32, mulberry32 — consolidated */

/* FNV-1a 32-bit — now uses shared fnv1a_32() from crypto.h */
#define pet_hash(s) fnv1a_32(s)

/* mulberry32 — now uses shared mulberry32() from crypto.h */
#define mulberry(a) mulberry32(a)

static int pick(uint32_t *rng, int n) {
    int v = (int)(mulberry(rng) * n);
    return v < 0 ? 0 : (v >= n ? n - 1 : v);
}

/* ── tables ────────────────────────────────────────────────────────────── */

static const char *const EYES[PET_EYE_COUNT] = {"·", "✦", "×", "◉", "@", "°"};

static const char *const SPECIES_NAMES[PET_SPECIES_COUNT] = {
    "duck",     "goose",    "blob",  "cat",     "dragon",   "octopus", "owl",   "penguin",
    "turtle",   "snail",    "ghost", "axolotl", "capybara", "cactus",  "robot", "rabbit",
    "mushroom", "chonk",    "fox",   "frog",    "bee",      "crab",    "whale", "fish",
    "bat",      "hedgehog", "panda", "sloth",   "narwhal",  "dino"};

static const char *const STAT_NAMES[PET_STAT_COUNT] = {"DEBUGGING", "PATIENCE", "CHAOS", "WISDOM",
                                                       "SNARK"};

static const int RARITY_WEIGHTS[PET_RARITY_COUNT] = {60, 25, 10, 4, 1};
static const int RARITY_FLOOR[PET_RARITY_COUNT] = {5, 15, 25, 35, 50};
static const char *const RARITY_NAMES[PET_RARITY_COUNT] = {"common", "uncommon", "rare", "epic",
                                                           "legendary"};
static const char *const RARITY_STARS[PET_RARITY_COUNT] = {"★", "★★", "★★★", "★★★★", "★★★★★"};

static const char *const HAT_LINES[PET_HAT_COUNT] = {
    "",              /* none */
    "   \\^^^/    ", /* crown */
    "   [___]    ",  /* tophat */
    "    -+-     ",  /* propeller */
    "   (   )    ",  /* halo */
    "    /^\\     ", /* wizard */
    "   (___)    ",  /* beanie */
    "    ,>      ",  /* tinyduck */
};

/* 5 lines × 3 frames per species. "{E}" = eye slot; line 0 = hat/effects slot. */
static const char *const BODIES[PET_SPECIES_COUNT][3][5] = {
    [PET_DUCK] =
        {
            {"            ", "    __      ", "  <({E} )___  ", "   (  ._>   ", "    `--´    "},
            {"            ", "    __      ", "  <({E} )___  ", "   (  ._>   ", "    `--´~   "},
            {"            ", "    __      ", "  <({E} )___  ", "   (  .__>  ", "    `--´    "},
        },
    [PET_GOOSE] =
        {
            {"            ", "     ({E}>    ", "     ||     ", "   _(__)_   ", "    ^^^^    "},
            {"            ", "    ({E}>     ", "     ||     ", "   _(__)_   ", "    ^^^^    "},
            {"            ", "     ({E}>>   ", "     ||     ", "   _(__)_   ", "    ^^^^    "},
        },
    [PET_BLOB] =
        {
            {"            ", "   .----.   ", "  ( {E}  {E} )  ", "  (      )  ", "   `----´   "},
            {"            ", "  .------.  ", " (  {E}  {E}  ) ", " (        ) ", "  `------´  "},
            {"            ", "    .--.    ", "   ({E}  {E})   ", "   (    )   ", "    `--´    "},
        },
    [PET_CAT] =
        {
            {"            ", "   /\\_/\\    ", "  ( {E}   {E})  ", "  (  ω  )   ",
             "  (\")_(\")   "},
            {"            ", "   /\\_/\\    ", "  ( {E}   {E})  ", "  (  ω  )   ",
             "  (\")_(\")~  "},
            {"            ", "   /\\-/\\    ", "  ( {E}   {E})  ", "  (  ω  )   ",
             "  (\")_(\")   "},
        },
    [PET_DRAGON] =
        {
            {"            ", "  /^\\  /^\\  ", " <  {E}  {E}  > ", " (   ~~   ) ", "  `-vvvv-´  "},
            {"            ", "  /^\\  /^\\  ", " <  {E}  {E}  > ", " (        ) ", "  `-vvvv-´  "},
            {"   ~    ~   ", "  /^\\  /^\\  ", " <  {E}  {E}  > ", " (   ~~   ) ", "  `-vvvv-´  "},
        },
    [PET_OCTOPUS] =
        {
            {"            ", "   .----.   ", "  ( {E}  {E} )  ", "  (______)  ",
             "  /\\/\\/\\/\\  "},
            {"            ", "   .----.   ", "  ( {E}  {E} )  ", "  (______)  ",
             "  \\/\\/\\/\\/  "},
            {"     o      ", "   .----.   ", "  ( {E}  {E} )  ", "  (______)  ",
             "  /\\/\\/\\/\\  "},
        },
    [PET_OWL] =
        {
            {"            ", "   /\\  /\\   ", "  (({E})({E}))  ", "  (  ><  )  ", "   `----´   "},
            {"            ", "   /\\  /\\   ", "  (({E})({E}))  ", "  (  ><  )  ", "   .----.   "},
            {"            ", "   /\\  /\\   ", "  (({E})(-))  ", "  (  ><  )  ", "   `----´   "},
        },
    [PET_PENGUIN] =
        {
            {"            ", "  .---.     ", "  ({E}>{E})     ", " /(   )\\    ", "  `---´     "},
            {"            ", "  .---.     ", "  ({E}>{E})     ", " |(   )|    ", "  `---´     "},
            {"  .---.     ", "  ({E}>{E})     ", " /(   )\\    ", "  `---´     ", "   ~ ~      "},
        },
    [PET_TURTLE] =
        {
            {"            ", "   _,--._   ", "  ( {E}  {E} )  ", " /[______]\\ ", "  ``    ``  "},
            {"            ", "   _,--._   ", "  ( {E}  {E} )  ", " /[______]\\ ", "   ``  ``   "},
            {"            ", "   _,--._   ", "  ( {E}  {E} )  ", " /[======]\\ ", "  ``    ``  "},
        },
    [PET_SNAIL] =
        {
            {"            ", " {E}    .--.  ", "  \\  ( @ )  ", "   \\_`--´   ", "  ~~~~~~~   "},
            {"            ", "  {E}   .--.  ", "  |  ( @ )  ", "   \\_`--´   ", "  ~~~~~~~   "},
            {"            ", " {E}    .--.  ", "  \\  ( @  ) ", "   \\_`--´   ", "   ~~~~~~   "},
        },
    [PET_GHOST] =
        {
            {"            ", "   .----.   ", "  / {E}  {E} \\  ", "  |      |  ", "  ~`~``~`~  "},
            {"            ", "   .----.   ", "  / {E}  {E} \\  ", "  |      |  ", "  `~`~~`~`  "},
            {"    ~  ~    ", "   .----.   ", "  / {E}  {E} \\  ", "  |      |  ", "  ~~`~~`~~  "},
        },
    [PET_AXOLOTL] =
        {
            {"            ", "}~(______)~{", "}~({E} .. {E})~{", "  ( .--. )  ", "  (_/  \\_)  "},
            {"            ", "~}(______){~", "~}({E} .. {E}){~", "  ( .--. )  ", "  (_/  \\_)  "},
            {"            ", "}~(______)~{", "}~({E} .. {E})~{", "  (  --  )  ", "  ~_/  \\_~  "},
        },
    [PET_CAPYBARA] =
        {
            {"            ", "  n______n  ", " ( {E}    {E} ) ", " (   oo   ) ", "  `------´  "},
            {"            ", "  n______n  ", " ( {E}    {E} ) ", " (   Oo   ) ", "  `------´  "},
            {"    ~  ~    ", "  u______n  ", " ( {E}    {E} ) ", " (   oo   ) ", "  `------´  "},
        },
    [PET_CACTUS] =
        {
            {"            ", " n  ____  n ", " | |{E}  {E}| | ", " |_|    |_| ", "   |    |   "},
            {"            ", "    ____    ", " n |{E}  {E}| n ", " |_|    |_| ", "   |    |   "},
            {" n        n ", " |  ____  | ", " | |{E}  {E}| | ", " |_|    |_| ", "   |    |   "},
        },
    [PET_ROBOT] =
        {
            {"            ", "   .[||].   ", "  [ {E}  {E} ]  ", "  [ ==== ]  ", "  `------´  "},
            {"            ", "   .[||].   ", "  [ {E}  {E} ]  ", "  [ -==- ]  ", "  `------´  "},
            {"     *      ", "   .[||].   ", "  [ {E}  {E} ]  ", "  [ ==== ]  ", "  `------´  "},
        },
    [PET_RABBIT] =
        {
            {"            ", "   (\\__/)   ", "  ( {E}  {E} )  ", " =(  ..  )= ", "  (\")__(\")  "},
            {"            ", "   (|__/)   ", "  ( {E}  {E} )  ", " =(  ..  )= ", "  (\")__(\")  "},
            {"            ", "   (\\__/)   ", "  ( {E}  {E} )  ", " =( .  . )= ", "  (\")__(\")  "},
        },
    [PET_MUSHROOM] =
        {
            {"            ", " .-o-OO-o-. ", "(__________)", "   |{E}  {E}|   ", "   |____|   "},
            {"            ", " .-O-oo-O-. ", "(__________)", "   |{E}  {E}|   ", "   |____|   "},
            {"   . o  .   ", " .-o-OO-o-. ", "(__________)", "   |{E}  {E}|   ", "   |____|   "},
        },
    [PET_CHONK] =
        {
            {"            ", "  /\\    /\\  ", " ( {E}    {E} ) ", " (   ..   ) ", "  `------´  "},
            {"            ", "  /\\    /|  ", " ( {E}    {E} ) ", " (   ..   ) ", "  `------´  "},
            {"            ", "  /\\    /\\  ", " ( {E}    {E} ) ", " (   ..   ) ", "  `------´~ "},
        },
    [PET_FOX] =
        {
            {"            ", "  /\\    /\\  ", " ( {E} ww {E}) ", "  (  ^^  )  ", "  \\_><_>>_  "},
            {"            ", "  /\\    /\\  ", " ( {E} ww {E}) ", "  (  ^^  )  ", "  \\_><_>>_~ "},
            {"   ^    ^   ", "  /\\    /\\  ", " ( {E} ww {E}) ", "  (  ^^  )  ", "  \\_><_>>_  "},
        },
    [PET_FROG] =
        {
            {"            ", " ({E})  ({E})  ", "  \\(____)/  ", "  ( ~~~~ )  ", "  `-`  `-´  "},
            {"            ", " ({E})  ({E})  ", "  \\(____)/  ", "  ( ____ )  ", "  `-`  `-´  "},
            {"   ~    ~   ", " ({E})  ({E})  ", "  \\(____)/  ", "  ( ~~~~ )  ", "  `-`  `-´  "},
        },
    [PET_BEE] =
        {
            {"            ", "  \\\\ {E}{E} //  ", "  (=-=-=)   ", "  (-=-=-)   ", "   `---´    "},
            {"            ", "  // {E}{E} \\\\  ", "  (=-=-=)   ", "  (-=-=-)   ", "   `---´    "},
            {"    z  z    ", "  \\\\ {E}{E} //  ", "  (=-=-=)   ", "  (-=-=-)   ", "   `---´    "},
        },
    [PET_CRAB] =
        {
            {"            ", " (\\/)  (\\/) ", "  ( {E}{E} )   ", " <(======)> ", "  /\\    /\\ "},
            {"            ", " (\\/)  (\\/) ", "  ( {E}{E} )   ", " <(======)> ", "  \\/    \\/ "},
            {" (\\/)  (\\/) ", "   \\    /   ", "  ( {E}{E} )   ", " <(======)> ", "  /\\    /\\ "},
        },
    [PET_WHALE] =
        {
            {"            ", "   .---.    ", "  ( {E}  {E})~~ ", "  (______)  ", "   \\~~~~/   "},
            {"            ", "   .---.    ", " ~( {E}  {E})~~ ", "  (______)  ", "   \\~~~~/   "},
            {"    ___     ", "  .'   '.   ", "  ( {E}  {E})~~ ", "  (______)  ", "   \\~~~~/   "},
        },
    [PET_FISH] =
        {
            {"            ", "   ______   ", "  /{E}     \\> ", "  \\____~~~/  ", "            "},
            {"            ", "   ______   ", "  /{E}    \\>> ", "  \\____~~~/  ", "            "},
            {"            ", "   ______   ", "  /{E}     \\> ", "  \\___~~~~/  ", "   °   °    "},
        },
    [PET_BAT] =
        {
            {"            ", " /\\_/\\_/\\  ", " ) {E}  {E} (  ", "  \\ vvvv /  ", "   ^^^^^^   "},
            {"            ", " \\/^\\_/^\\/  ", " ) {E}  {E} (  ", "  \\ vvvv /  ", "   ^^^^^^   "},
            {"  ^v^  ^v^  ", " /\\_/\\_/\\  ", " ) {E}  {E} (  ", "  \\ vvvv /  ", "   ^^^^^^   "},
        },
    [PET_HEDGEHOG] =
        {
            {"            ", "  \\|/|\\|/   ", " <({E}  {E})>  ", "  (  ·-·  ) ", "   ^^  ^^   "},
            {"            ", "  /|\\|/|\\   ", " <({E}  {E})>  ", "  (  ·-·  ) ", "   ^^  ^^   "},
            {"  '''''''   ", "  \\|/|\\|/   ", " <({E}  {E})>  ", "  (  ·-·  ) ", "   ^^  ^^   "},
        },
    [PET_PANDA] =
        {
            {"            ", "  (o)  (o)  ", " ( ({E})({E}) )", " (   ω    ) ", "  `------´  "},
            {"            ", "  (o)  (o)  ", " ( ({E})({E}) )", " (   ω    ) ", "  `------´~ "},
            {"   .    .   ", "  (o)  (o)  ", " ( ({E})({E}) )", " (   ω    ) ", "  `------´  "},
        },
    [PET_SLOTH] =
        {
            {"            ", "   .----.   ", "  ( {E}__{E} )  ", "  ( \\__/ )  ", "   |    |   "},
            {"            ", "   .----.   ", "  ( {E}__{E} )  ", "  ( \\__/ )  ", "    |  |    "},
            {"   z z z    ", "   .----.   ", "  ( {E}__{E} )  ", "  ( \\__/ )  ", "   |    |   "},
        },
    [PET_NARWHAL] =
        {
            {"            ", "    |       ", "  .-{E}{E}---.  ", " ( ______ ) ", "  \\~~~~~~/  "},
            {"            ", "    |       ", "  .-{E}{E}---.  ", " ( ______ ) ", "  \\~~~~~~/~ "},
            {"    |       ", "    |       ", "  .-{E}{E}---.  ", " ( ______ ) ", "  \\~~~~~~/  "},
        },
    [PET_DINO] =
        {
            {"            ", "    ____    ", "   /{E}  {E}\\_ ", "  <  vvv  | ", "   ^^  ^^   "},
            {"            ", "    ____    ", "   /{E}  {E}\\_ ", "  <  vvv  | ", "   ^^  ^^ ~ "},
            {"   .  .     ", "    ____    ", "   /{E}  {E}\\_ ", "  <  vvv  | ", "   ^^  ^^   "},
        },
};

/* ── bones derivation ──────────────────────────────────────────────────── */

#define PET_SALT "dsco-pets-2026"

static pet_rarity_t roll_rarity(uint32_t *rng) {
    int total = 0;
    for (int i = 0; i < PET_RARITY_COUNT; i++)
        total += RARITY_WEIGHTS[i];
    double roll = mulberry(rng) * total;
    for (int i = 0; i < PET_RARITY_COUNT; i++) {
        roll -= RARITY_WEIGHTS[i];
        if (roll < 0)
            return (pet_rarity_t)i;
    }
    return PET_RARITY_COMMON;
}

static void roll_stats(uint32_t *rng, pet_rarity_t rarity, int out[PET_STAT_COUNT]) {
    int floor = RARITY_FLOOR[rarity];
    int peak = pick(rng, PET_STAT_COUNT);
    int dump = pick(rng, PET_STAT_COUNT);
    while (dump == peak)
        dump = pick(rng, PET_STAT_COUNT);
    for (int i = 0; i < PET_STAT_COUNT; i++) {
        int v;
        if (i == peak)
            v = floor + 50 + (int)(mulberry(rng) * 30);
        else if (i == dump)
            v = floor - 10 + (int)(mulberry(rng) * 15);
        else
            v = floor + (int)(mulberry(rng) * 40);
        if (v > 100)
            v = 100;
        if (v < 1)
            v = 1;
        out[i] = v;
    }
}

void pet_roll(const char *seed, pet_bones_t *out) {
    if (!out)
        return;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s", seed ? seed : "anon", PET_SALT);
    uint32_t rng = pet_hash(buf);

    pet_rarity_t rarity = roll_rarity(&rng);
    out->rarity = rarity;
    out->species = (pet_species_t)pick(&rng, PET_SPECIES_COUNT);
    out->eye = pick(&rng, PET_EYE_COUNT);
    out->hat = (rarity == PET_RARITY_COMMON) ? PET_HAT_NONE : (pet_hat_t)pick(&rng, PET_HAT_COUNT);
    out->shiny = mulberry(&rng) < 0.01;
    roll_stats(&rng, rarity, out->stats);
}

/* ── sprite rendering ──────────────────────────────────────────────────── */

static bool line_blank(const char *s) {
    for (; *s; s++)
        if (*s != ' ')
            return false;
    return true;
}

static void subst_eye(const char *src, const char *eye, char *dst, size_t n) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 8 < n;) {
        if (src[i] == '{' && src[i + 1] == 'E' && src[i + 2] == '}') {
            for (const char *e = eye; *e && di + 1 < n; e++)
                dst[di++] = *e;
            i += 3;
        } else {
            dst[di++] = src[i++];
        }
    }
    dst[di] = '\0';
}

int pet_sprite_frame_count(pet_species_t s) {
    (void)s;
    return 3;
}

int pet_render_sprite(const pet_bones_t *b, int frame, char lines[6][96]) {
    int sp = (b->species >= 0 && b->species < PET_SPECIES_COUNT) ? b->species : 0;
    int nf = 3;
    int f = ((frame % nf) + nf) % nf;
    const char *const *body = BODIES[sp][f];
    int ei = ((b->eye % PET_EYE_COUNT) + PET_EYE_COUNT) % PET_EYE_COUNT;
    const char *eye = EYES[ei];

    char tmp[5][96];
    for (int i = 0; i < 5; i++)
        subst_eye(body[i], eye, tmp[i], sizeof(tmp[i]));

    if (b->hat != PET_HAT_NONE && line_blank(tmp[0]))
        snprintf(tmp[0], sizeof(tmp[0]), "%s", HAT_LINES[b->hat]);

    int start = 0;
    if (line_blank(tmp[0])) {
        int all_blank = 1;
        for (int ff = 0; ff < 3; ff++)
            if (!line_blank(BODIES[sp][ff][0])) {
                all_blank = 0;
                break;
            }
        if (all_blank)
            start = 1;
    }

    int k = 0;
    for (int i = start; i < 5 && k < 6; i++)
        snprintf(lines[k++], 96, "%s", tmp[i]);
    return k;
}

void pet_render_face(const pet_bones_t *b, char *buf, size_t n) {
    int ei = ((b->eye % PET_EYE_COUNT) + PET_EYE_COUNT) % PET_EYE_COUNT;
    const char *e = EYES[ei];
    switch (b->species) {
        case PET_DUCK:
        case PET_GOOSE:
            snprintf(buf, n, "(%s>", e);
            break;
        case PET_BLOB:
            snprintf(buf, n, "(%s%s)", e, e);
            break;
        case PET_CAT:
            snprintf(buf, n, "=%sω%s=", e, e);
            break;
        case PET_DRAGON:
            snprintf(buf, n, "<%s~%s>", e, e);
            break;
        case PET_OCTOPUS:
            snprintf(buf, n, "~(%s%s)~", e, e);
            break;
        case PET_OWL:
            snprintf(buf, n, "(%s)(%s)", e, e);
            break;
        case PET_PENGUIN:
            snprintf(buf, n, "(%s>)", e);
            break;
        case PET_TURTLE:
            snprintf(buf, n, "[%s_%s]", e, e);
            break;
        case PET_SNAIL:
            snprintf(buf, n, "%s(@)", e);
            break;
        case PET_GHOST:
            snprintf(buf, n, "/%s%s\\", e, e);
            break;
        case PET_AXOLOTL:
            snprintf(buf, n, "}%s.%s{", e, e);
            break;
        case PET_CAPYBARA:
            snprintf(buf, n, "(%soo%s)", e, e);
            break;
        case PET_CACTUS:
            snprintf(buf, n, "|%s  %s|", e, e);
            break;
        case PET_ROBOT:
            snprintf(buf, n, "[%s%s]", e, e);
            break;
        case PET_RABBIT:
            snprintf(buf, n, "(%s..%s)", e, e);
            break;
        case PET_MUSHROOM:
            snprintf(buf, n, "|%s  %s|", e, e);
            break;
        case PET_CHONK:
            snprintf(buf, n, "(%s.%s)", e, e);
            break;
        case PET_FOX:
            snprintf(buf, n, "=%sw%s=", e, e);
            break;
        case PET_FROG:
            snprintf(buf, n, "(%s)(%s)", e, e);
            break;
        case PET_BEE:
            snprintf(buf, n, "\\%s%s/", e, e);
            break;
        case PET_CRAB:
            snprintf(buf, n, "(%s%s)", e, e);
            break;
        case PET_WHALE:
            snprintf(buf, n, "(%s%s)~", e, e);
            break;
        case PET_FISH:
            snprintf(buf, n, "<%s><", e);
            break;
        case PET_BAT:
            snprintf(buf, n, ")%s%s(", e, e);
            break;
        case PET_HEDGEHOG:
            snprintf(buf, n, "<%s%s>", e, e);
            break;
        case PET_PANDA:
            snprintf(buf, n, "(%s)(%s)", e, e);
            break;
        case PET_SLOTH:
            snprintf(buf, n, "(%s_%s)", e, e);
            break;
        case PET_NARWHAL:
            snprintf(buf, n, "%s%s|", e, e);
            break;
        case PET_DINO:
            snprintf(buf, n, "/%s%s\\", e, e);
            break;
        default:
            snprintf(buf, n, "(%s%s)", e, e);
            break;
    }
}

/* ── naming / labels ───────────────────────────────────────────────────── */

const char *pet_species_name(pet_species_t s) {
    return (s >= 0 && s < PET_SPECIES_COUNT) ? SPECIES_NAMES[s] : "?";
}
const char *pet_rarity_name(pet_rarity_t r) {
    return (r >= 0 && r < PET_RARITY_COUNT) ? RARITY_NAMES[r] : "?";
}
const char *pet_rarity_stars(pet_rarity_t r) {
    return (r >= 0 && r < PET_RARITY_COUNT) ? RARITY_STARS[r] : "";
}
const char *pet_rarity_color(pet_rarity_t r) {
    switch (r) {
        case PET_RARITY_COMMON:
            return TUI_DIM;
        case PET_RARITY_UNCOMMON:
            return TUI_GREEN;
        case PET_RARITY_RARE:
            return TUI_BCYAN;
        case PET_RARITY_EPIC:
            return TUI_BMAGENTA;
        case PET_RARITY_LEGENDARY:
            return TUI_BYELLOW;
        default:
            return TUI_RESET;
    }
}
const char *pet_stat_name(int i) {
    return (i >= 0 && i < PET_STAT_COUNT) ? STAT_NAMES[i] : "?";
}

const char *pet_status_glyph(pet_status_t st) {
    switch (st) {
        case PET_ST_PENDING:
            return "·";
        case PET_ST_WORKING:
            return "◉";
        case PET_ST_DONE:
            return "✓";
        case PET_ST_ERROR:
            return "✗";
        case PET_ST_IDLE:
            return "-";
        default:
            return "·";
    }
}
const char *pet_status_color(pet_status_t st) {
    switch (st) {
        case PET_ST_PENDING:
            return TUI_DIM;
        case PET_ST_WORKING:
            return TUI_BCYAN;
        case PET_ST_DONE:
            return TUI_GREEN;
        case PET_ST_ERROR:
            return TUI_RED;
        case PET_ST_IDLE:
            return TUI_DIM;
        default:
            return TUI_RESET;
    }
}
const char *pet_status_word(pet_status_t st) {
    switch (st) {
        case PET_ST_PENDING:
            return "pending";
        case PET_ST_WORKING:
            return "working";
        case PET_ST_DONE:
            return "done";
        case PET_ST_ERROR:
            return "error";
        case PET_ST_IDLE:
            return "idle";
        default:
            return "?";
    }
}

/* Status tweaks the eye so the face emotes (dizzy on error, wide when done). */
static pet_bones_t bones_for_status(const pet_bones_t *src, pet_status_t st) {
    pet_bones_t b = *src;
    switch (st) {
        case PET_ST_ERROR:
            b.eye = 2;
            break; /* × */
        case PET_ST_DONE:
            b.eye = 3;
            break; /* ◉ */
        case PET_ST_PENDING:
            b.eye = 0;
            break; /* · */
        default:
            break;
    }
    return b;
}

/* ── pretty card ───────────────────────────────────────────────────────── */

static void emit_sprite_line(FILE *out, const char *line, const pet_bones_t *b, int frame,
                             int row) {
    if (b->shiny && tui_supports_truecolor()) {
        float hue = fmodf((float)(frame * 12 + row * 40), 360.0f);
        tui_rgb_t c = tui_hsv_to_rgb(hue, 0.65f, 1.0f);
        tui_fg_rgb(c);
        fprintf(out, "%s%s\n", line, TUI_RESET);
    } else {
        fprintf(out, "%s%s%s\n", pet_rarity_color(b->rarity), line, TUI_RESET);
    }
}

void pet_card_print(FILE *out, const pet_t *p, int frame) {
    if (!out || !p)
        return;
    frame += pet_auto_frame(); /* animate idle fidget over wall-clock time */
    char lines[6][96];
    int n = pet_render_sprite(&p->bones, frame, lines);

    const char *name = p->name[0] ? p->name : pet_species_name(p->bones.species);
    fprintf(out, "\n");
    for (int i = 0; i < n; i++) {
        fputs("  ", out);
        emit_sprite_line(out, lines[i], &p->bones, frame, i);
    }
    /* info block under the sprite */
    fprintf(out, "  %s%s%s  %s%s%s  %s%s%s\n", TUI_BOLD, name, TUI_RESET,
            pet_rarity_color(p->bones.rarity), pet_rarity_stars(p->bones.rarity), TUI_RESET,
            pet_status_color(p->status), pet_status_word(p->status), TUI_RESET);
    fprintf(out, "  %s%s · %s%s\n", TUI_DIM, pet_species_name(p->bones.species),
            pet_rarity_name(p->bones.rarity), TUI_RESET);
    /* stats */
    fputs("  ", out);
    for (int i = 0; i < PET_STAT_COUNT; i++)
        fprintf(out, "%s%s%s %d  ", TUI_DIM, pet_stat_name(i), TUI_RESET, p->bones.stats[i]);
    fprintf(out, "\n");
}

void pet_gallery_print(FILE *out, int frame) {
    if (!out)
        return;
    /* One card per species so every creature is on display; rarity/eye/hat
     * still vary per seed for flavor. */
    for (int s = 0; s < PET_SPECIES_COUNT; s++) {
        pet_t p;
        memset(&p, 0, sizeof(p));
        char seed[32];
        snprintf(seed, sizeof(seed), "gallery-%d", s);
        pet_roll(seed, &p.bones);
        p.bones.species = (pet_species_t)s; /* force this species */
        p.status = PET_ST_IDLE;
        snprintf(p.name, sizeof(p.name), "%s", pet_species_name((pet_species_t)s));
        pet_card_print(out, &p, frame + s);
    }
}

/* ── roster ────────────────────────────────────────────────────────────── */

void pet_roster_init(pet_roster_t *r) {
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->mu, NULL);
}

void pet_roster_free(pet_roster_t *r) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    free(r->pets);
    r->pets = NULL;
    r->count = r->cap = 0;
    pthread_mutex_unlock(&r->mu);
    pthread_mutex_destroy(&r->mu);
}

/* caller holds lock */
static pet_t *roster_find(pet_roster_t *r, int id) {
    for (int i = 0; i < r->count; i++)
        if (r->pets[i].id == id)
            return &r->pets[i];
    return NULL;
}

int pet_roster_upsert(pet_roster_t *r, int id, int project_id, const char *label, const char *seed,
                      pet_status_t status) {
    if (!r)
        return -1;
    pthread_mutex_lock(&r->mu);
    pet_t *p = roster_find(r, id);
    if (!p) {
        if (r->count >= r->cap) {
            int nc = r->cap ? r->cap * 2 : 16;
            pet_t *np = realloc(r->pets, (size_t)nc * sizeof(pet_t));
            if (!np) {
                pthread_mutex_unlock(&r->mu);
                return -1;
            }
            r->pets = np;
            r->cap = nc;
        }
        p = &r->pets[r->count++];
        memset(p, 0, sizeof(*p));
        p->id = id;
        p->project_id = project_id;
        p->start_time = pet_now();
        pet_roll(seed ? seed : (label ? label : "agent"), &p->bones);
    }
    if (label)
        snprintf(p->label, sizeof(p->label), "%s", label);
    p->status = status;
    if (status == PET_ST_DONE || status == PET_ST_ERROR)
        if (p->end_time == 0)
            p->end_time = pet_now();
    int idx = (int)(p - r->pets);
    pthread_mutex_unlock(&r->mu);
    return idx;
}

void pet_roster_set_status(pet_roster_t *r, int id, pet_status_t status, double cost_usd) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    pet_t *p = roster_find(r, id);
    if (p) {
        p->status = status;
        if (cost_usd > 0)
            p->cost_usd = cost_usd;
        if ((status == PET_ST_DONE || status == PET_ST_ERROR) && p->end_time == 0)
            p->end_time = pet_now();
    }
    pthread_mutex_unlock(&r->mu);
}

void pet_roster_activity(pet_roster_t *r, int id, double bytes) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    pet_t *p = roster_find(r, id);
    if (p) {
        p->activity_ring[p->activity_head % 16] = bytes;
        p->activity_head++;
    }
    pthread_mutex_unlock(&r->mu);
}

void pet_roster_remove(pet_roster_t *r, int id) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    for (int i = 0; i < r->count; i++) {
        if (r->pets[i].id == id) {
            r->pets[i] = r->pets[--r->count];
            break;
        }
    }
    pthread_mutex_unlock(&r->mu);
}

void pet_roster_counts(pet_roster_t *r, int *pending, int *working, int *done, int *error,
                       int *total) {
    int pe = 0, wo = 0, dn = 0, er = 0;
    pthread_mutex_lock(&r->mu);
    for (int i = 0; i < r->count; i++) {
        switch (r->pets[i].status) {
            case PET_ST_PENDING:
                pe++;
                break;
            case PET_ST_WORKING:
                wo++;
                break;
            case PET_ST_DONE:
                dn++;
                break;
            case PET_ST_ERROR:
                er++;
                break;
            default:
                break;
        }
    }
    if (total)
        *total = r->count;
    pthread_mutex_unlock(&r->mu);
    if (pending)
        *pending = pe;
    if (working)
        *working = wo;
    if (done)
        *done = dn;
    if (error)
        *error = er;
}

static int status_priority(pet_status_t s) {
    switch (s) {
        case PET_ST_WORKING:
            return 0;
        case PET_ST_PENDING:
            return 1;
        case PET_ST_IDLE:
            return 2;
        case PET_ST_ERROR:
            return 3;
        case PET_ST_DONE:
            return 4;
        default:
            return 5;
    }
}

static const char *const SPARK_BLOCKS[8] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

static void render_spark(FILE *out, const pet_t *p) {
    int n = p->activity_head < 16 ? p->activity_head : 16;
    if (n <= 0)
        return;
    double mx = 0;
    for (int i = 0; i < n; i++)
        if (p->activity_ring[i] > mx)
            mx = p->activity_ring[i];
    if (mx <= 0)
        return;
    fputs(TUI_DIM, out);
    for (int i = 0; i < n; i++) {
        int idx = (int)((p->activity_ring[i] / mx) * 7.0);
        if (idx < 0)
            idx = 0;
        if (idx > 7)
            idx = 7;
        fputs(SPARK_BLOCKS[idx], out);
    }
    fputs(TUI_RESET, out);
}

void pet_roster_render(FILE *out, pet_roster_t *r, int width, int max_rows) {
    if (!out || !r)
        return;
    if (width <= 0)
        width = 80;
    pthread_mutex_lock(&r->mu);

    int n = r->count;
    int frame = pet_auto_frame();
    if (n == 0) {
        pthread_mutex_unlock(&r->mu);
        fprintf(out, "  %sno background pets — spawn an agent to hatch one%s\n", TUI_DIM,
                TUI_RESET);
        return;
    }

    /* order index by status priority then id */
    int *order = malloc((size_t)n * sizeof(int));
    if (!order) {
        pthread_mutex_unlock(&r->mu);
        return;
    }
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int a = order[i], b = order[j];
            int pa = status_priority(r->pets[a].status);
            int pb = status_priority(r->pets[b].status);
            if (pb < pa || (pb == pa && r->pets[b].id < r->pets[a].id)) {
                order[i] = b;
                order[j] = a;
            }
        }
    }

    int pe, wo, dn, er;
    pe = wo = dn = er = 0;
    for (int i = 0; i < n; i++) {
        switch (r->pets[i].status) {
            case PET_ST_PENDING:
                pe++;
                break;
            case PET_ST_WORKING:
                wo++;
                break;
            case PET_ST_DONE:
                dn++;
                break;
            case PET_ST_ERROR:
                er++;
                break;
            default:
                break;
        }
    }

    fprintf(out, "  %s%sPETS%s  %s%d agents%s  %s◉ %d%s %s· %d%s %s✓ %d%s %s✗ %d%s\n", TUI_BOLD,
            TUI_BCYAN, TUI_RESET, TUI_DIM, n, TUI_RESET, TUI_BCYAN, wo, TUI_RESET, TUI_DIM, pe,
            TUI_RESET, TUI_GREEN, dn, TUI_RESET, TUI_RED, er, TUI_RESET);

    int shown = 0;
    int limit = (max_rows > 0 && max_rows < n) ? max_rows : n;
    for (int oi = 0; oi < limit; oi++) {
        const pet_t *p = &r->pets[order[oi]];
        pet_bones_t fb = bones_for_status(&p->bones, p->status);
        char face[32];
        pet_render_face(&fb, face, sizeof(face));

        /* working pets animate their face via a tiny bob in the leading glyph */
        const char *bob = (p->status == PET_ST_WORKING) ? ((frame / 2) % 2 ? " " : "") : "";

        char lbl[40];
        snprintf(lbl, sizeof(lbl), "%-20.20s", p->label[0] ? p->label : "agent");

        fprintf(out, "  %s%s%s%-8s%s %s#%-3d%s %s%s%s ", pet_status_color(p->status),
                pet_status_glyph(p->status), TUI_RESET, "", "", /* spacing kept simple */
                TUI_DIM, p->id, TUI_RESET, pet_rarity_color(p->bones.rarity), face, TUI_RESET);
        fprintf(out, "%s%s%s%s ", bob, TUI_BOLD, lbl, TUI_RESET);

        fprintf(out, "%s%-7s%s", pet_status_color(p->status), pet_status_word(p->status),
                TUI_RESET);
        if (p->cost_usd > 0)
            fprintf(out, " %s$%.3f%s", TUI_GREEN, p->cost_usd, TUI_RESET);
        fputc(' ', out);
        render_spark(out, p);
        fputc('\n', out);
        shown++;
    }
    if (shown < n)
        fprintf(out, "  %s… +%d more%s\n", TUI_DIM, n - shown, TUI_RESET);

    free(order);
    pthread_mutex_unlock(&r->mu);
}

void pet_roster_tick(pet_roster_t *r) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    r->frame = (r->frame + 1) % 100000;
    pthread_mutex_unlock(&r->mu);
}

int pet_roster_next_unnotified(pet_roster_t *r, pet_status_t *out_status, char *out_label,
                               size_t label_n, pet_bones_t *out_bones) {
    if (!r)
        return -1;
    int id = -1;
    pthread_mutex_lock(&r->mu);
    for (int i = 0; i < r->count; i++) {
        pet_t *p = &r->pets[i];
        if (!p->notified && (p->status == PET_ST_DONE || p->status == PET_ST_ERROR)) {
            p->notified = true;
            id = p->id;
            if (out_status)
                *out_status = p->status;
            if (out_bones)
                *out_bones = p->bones;
            if (out_label && label_n)
                snprintf(out_label, label_n, "%s", p->label[0] ? p->label : "agent");
            break;
        }
    }
    pthread_mutex_unlock(&r->mu);
    return id;
}

static pet_roster_t g_pet_roster;
static pthread_once_t g_pet_once = PTHREAD_ONCE_INIT;
static void pet_roster_global_init(void) {
    pet_roster_init(&g_pet_roster);
}

pet_roster_t *pet_roster_global(void) {
    pthread_once(&g_pet_once, pet_roster_global_init);
    return &g_pet_roster;
}

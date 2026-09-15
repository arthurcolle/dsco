#ifndef DSCO_SKILL_CANDIDATE_H
#define DSCO_SKILL_CANDIDATE_H

/* CLI also accepts verify <directory> (integrity only) and trace <journal.wal>
 * (payload-omitting evidence selection, not automatic skill extraction).
 * Local-only candidate scaffold; argv[0] is "from", followed by an explicit
 * episode JSON file and a directory that must not exist. Returns 0 on success.
 * Does not extract conversations, execute procedures, install, or promote.
 * Input: regular file <= 1 MiB, name <= 64 bytes, strings <= 64 KiB,
 * required arrays contain 1..256 nonblank strings. Unknown/duplicate fields,
 * embedded NUL, malformed JSON/UTF-8 and unsafe slugs are rejected.
 * Output: private directory with SKILL.md, original episode.json and manifest.json.
 */
int skill_candidate_cli(int argc, char **argv);

#endif

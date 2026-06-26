#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src/tools.c"
OUT="$ROOT/docs/TOOL_CATALOG.md"
TMP="$(mktemp)"
CHECK=0

if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
fi

{
  echo "# Built-in Tool Catalog"
  echo
  echo "This catalog is generated from the static \`src/tools.c\` built-in tool registry."
  echo
  echo "- Source: \`src/tools.c\`"

  perl - "$SRC" <<'PERL'
use strict;
use warnings;

my $src_path = shift @ARGV;
open my $fh, '<', $src_path or die "open $src_path: $!\n";
local $/;
my $src = <$fh>;
close $fh;

my ($registry) = $src =~
    /static\s+const\s+tool_def_t\s+s_tools\[\]\s*=\s*\{(.*?)\n\};\s*\n\s*static\s+const\s+int\s+s_tool_count/s
    or die "could not locate s_tools[] registry in $src_path\n";

my @entries;
while ($registry =~ /(\{\s*\.name\s*=\s*"(?:(?:\\.)|[^"\\])*".*?)(?=\n\s*\{\s*\.name\s*=|\z)/sg) {
    push @entries, $1;
}

sub decode_c_string {
    my ($s) = @_;
    $s =~ s/\\n/ /g;
    $s =~ s/\\r/ /g;
    $s =~ s/\\t/ /g;
    $s =~ s/\\"/"/g;
    $s =~ s/\\'/'/g;
    $s =~ s/\\\\/\\/g;
    return $s;
}

sub string_expr_value {
    my ($expr) = @_;
    my $out = '';
    while ($expr =~ /"((?:\\.|[^"\\])*)"/sg) {
        $out .= decode_c_string($1);
    }
    return $out;
}

sub field_string {
    my ($entry, $field) = @_;
    return string_expr_value($1)
        if $entry =~ /\.$field\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+)/s;
    return "";
}

sub flag {
    my ($entry, $field) = @_;
    return $entry =~ /\.$field\s*=\s*true\b/ ? 1 : 0;
}

sub md_escape {
    my ($s) = @_;
    $s =~ s/\s+/ /g;
    $s =~ s/^\s+|\s+$//g;
    $s =~ s/\|/\\|/g;
    return $s;
}

my @tools;
my %seen;
for my $entry (@entries) {
    my $name = field_string($entry, "name");
    next if $name eq "";
    die "duplicate tool registration: $name\n" if $seen{$name}++;
    push @tools, {
        name        => $name,
        description => field_string($entry, "description"),
        core        => flag($entry, "core"),
        read_only   => flag($entry, "is_read_only"),
        concurrent  => flag($entry, "is_concurrent"),
        interactive => flag($entry, "is_interactive"),
    };
}

@tools = sort {
    lc($a->{name}) cmp lc($b->{name}) || $a->{name} cmp $b->{name}
} @tools;

my $total = scalar @tools;
my $core = 0;
my $read_only = 0;
my $concurrent = 0;
my $interactive = 0;
for my $tool (@tools) {
    $core++        if $tool->{core};
    $read_only++   if $tool->{read_only};
    $concurrent++  if $tool->{concurrent};
    $interactive++ if $tool->{interactive};
}

print "- Total built-in tools: $total\n";
print "- Core tools: $core\n";
print "- Read-only tools: $read_only\n";
print "- Concurrent tools: $concurrent\n";
print "- Interactive tools: $interactive\n";
print "\n";
print "Regeneration:\n\n";
print "```bash\n";
print "./scripts/gen_tool_catalog.sh\n";
print "```\n\n";
print "Flags:\n\n";
print "- Core: always available in the active register set.\n";
print "- Read-only: marked as side-effect-free for streaming execution.\n";
print "- Concurrent: marked safe for parallel execution.\n";
print "- Interactive: owns the terminal or user turn.\n";
print "\n";
print "| Tool | Core | Read-only | Concurrent | Interactive | Description |\n";
print "|---|---:|---:|---:|---:|---|\n";

for my $tool (@tools) {
    printf "| <code>%s</code> | %s | %s | %s | %s | %s |\n",
        md_escape($tool->{name}),
        $tool->{core} ? "yes" : "",
        $tool->{read_only} ? "yes" : "",
        $tool->{concurrent} ? "yes" : "",
        $tool->{interactive} ? "yes" : "",
        md_escape($tool->{description});
}
PERL
} > "$TMP"

if [[ $CHECK -eq 1 ]]; then
  if ! cmp -s "$TMP" "$OUT"; then
    echo "docs drift: $OUT is out of date. Run ./scripts/gen_tool_catalog.sh" >&2
    rm -f "$TMP"
    exit 1
  fi
  rm -f "$TMP"
  exit 0
fi

mv "$TMP" "$OUT"
echo "wrote $OUT"

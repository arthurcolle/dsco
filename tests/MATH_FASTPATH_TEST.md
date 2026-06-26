# Math Fast-Path Corpus

`make test_math_corpus` rebuilds the production `math_fastpath.c` and `eval.c`
objects, regenerates `tests/math_corpus.tsv`, and checks both routing and
computed values.

Rows use tab-separated fields:

```text
route	expr	expected
mathf	sqrt(144)	12
llm	what is 2+2	-
```

`mathf` means the expression must be handled by the fast path. `llm` means the
expression must be declined so normal model routing can handle it.

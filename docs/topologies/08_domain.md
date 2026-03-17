# Category 8: Domain-Specific Workflows (T55–T60)

## T55 — `code_review`
**Multi-stage code review pipeline**
```
    H(lint) ──▶ S(logic) ──▶ S(security) ──▶ O(verdict)
```
- Haiku runs cheap static checks (formatting, style, obvious issues)
- Sonnet reviews business logic correctness
- Second Sonnet audits for security vulnerabilities
- Opus renders final approve/request-changes with unified feedback
- Use: PR review automation, pre-merge gates
- Est. latency: 4x | Agents: 4

## T56 — `research`
**Academic research pattern: search → analyze → synthesize**
```
    H(search₁) ──┐
    H(search₂) ──┤──▶ S(analyze₁) ──┐
    H(search₃) ──┤                   ├──▶ O(synthesize)
    H(search₄) ──┤──▶ S(analyze₂) ──┘

```
- 4 Haiku scouts search different angles in parallel
- 2 Sonnets analyze findings (each takes 2 scouts' results)
- Opus synthesizes into coherent thesis with citations
- Use: literature review, market research, technical investigation
- Est. latency: 3x | Agents: 7

## T57 — `incident`
**Incident response lifecycle**
```
    H(alert) ──▶ S(root_cause) ──▶ O(remediate) ──▶ S(implement) ──▶ H(verify)
```
- Haiku triages alert severity and gathers initial data
- Sonnet performs root cause analysis
- Opus designs remediation plan
- Sonnet implements the fix
- Haiku verifies the fix resolved the issue
- Use: on-call incident response, automated healing, SRE workflows
- Est. latency: 5x | Agents: 5

## T58 — `data_pipeline`
**ETL with progressive intelligence**
```
    H(ingest) ──▶ H(clean) ──▶ S(transform) ──▶ S(analyze) ──▶ O(insight)
```
- Cheap stages for mechanical work (ingest, clean)
- Mid-tier for domain transforms and statistical analysis
- Expensive for generating actionable insights from results
- Use: data science workflows, log analysis, metrics dashboards
- Est. latency: 5x | Agents: 5

## T59 — `security_audit`
**Full security assessment pipeline**
```
    H(scan₁) ──┐
    H(scan₂) ──├──▶ S(classify) ──▶ O(risk) ──▶ S(remediate)
    H(scan₃) ──┘
```
- 3 Haiku scanners check different attack surfaces in parallel (deps, code, config)
- Sonnet classifies and deduplicates findings by severity
- Opus performs risk assessment and prioritization
- Sonnet generates remediation plan with specific fixes
- Use: security audits, compliance checks, pen-test report analysis
- Est. latency: 4x | Agents: 6

## T60 — `creative`
**Creative production pipeline**
```
    H(idea₁) ──┐
    H(idea₂) ──┤
    H(idea₃) ──├──▶ S(curate) ──▶ O(develop) ──▶ S(refine) ──▶ H(format)
    H(idea₄) ──┤
    H(idea₅) ──┤
    H(idea₆) ──┘
```
- 6 Haiku brainstorm wildly with high temperature (cheap, diverse ideas)
- Sonnet curates top ideas by feasibility and novelty
- Opus develops the winning concept into full form
- Sonnet refines prose/code quality
- Haiku handles final formatting and packaging
- Use: creative writing, product ideation, marketing campaigns, naming
- Est. latency: 5x | Agents: 10

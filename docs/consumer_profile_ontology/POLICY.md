# Policy and Activation Rules

## Purpose-Limited Activation

A facet must never be used merely because it exists. Downstream systems must evaluate:

1. requested purpose,
2. sensitivity class,
3. consent state,
4. jurisdiction,
5. user age/safety state,
6. source class,
7. confidence/freshness,
8. proxy risk,
9. advertiser or campaign category where applicable,
10. user suppression/deletion controls.

## Default Purpose Matrix

| Sensitivity | Ranking | Recommendation | Personalized Ads | Contextual Ads | Measurement | Safety | Eligibility Decisions |
|---|---:|---:|---:|---:|---:|---:|---:|
| S0 Operational | Yes | Sometimes | Usually no | Sometimes | Yes | Yes | Sometimes |
| S1 Standard | Yes | Yes | Yes, if consented | Yes | Aggregate | No unless relevant | No |
| S2 Private | Limited | Limited | Usually no / review | Sometimes | Aggregate only | Sometimes | No |
| S3 Restricted | Strict | Strict | No | No | Privacy-preserving aggregate | Yes if necessary | No |
| S4 Prohibited | No | No | No | No | Fairness audit only | Special systems only | No |

## Non-Negotiable Boundaries

Do not infer or activate general-purpose facets for:

- race or ethnicity,
- religion,
- sexual orientation,
- gender identity beyond explicit product need,
- political persuasion,
- union membership,
- health diagnosis,
- addiction status,
- pregnancy inference,
- immigration status,
- criminal history,
- disability inference,
- precise financial distress,
- sensitive-place visits,
- proxies for the above.

## Sensitive Place Rule

Sensitive-place detection, if implemented, may be used for suppression and safety controls. It must not become an activation signal for targeting.

## User Controls

Every non-operational profile surface should support, where applicable:

- profile view,
- explanation,
- interest deletion/suppression,
- correction of explicit facts,
- ad personalization opt-out,
- partner-data opt-out,
- precise-location opt-out,
- data download,
- data deletion,
- appeal/report.

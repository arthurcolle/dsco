# Authenticated account identity in the runtime UI

The runtime UI may display an account-scoped label such as `DISPLAY_NAME / SWARM`, but the label is never an authorization key and no person's name is compiled into DSCO.

An authenticated Router/product launcher may provide the following non-secret capsule fields to the DSCO process after verifying the Router audience, issuer, principal, project, and session:

- `DSCO_ACCOUNT_DISPLAY_NAME` — cosmetic display label
- `DSCO_ACCOUNT_PRINCIPAL_ID` — verified Router principal identifier
- `DSCO_ACCOUNT_PROJECT_ID` — verified account/project boundary
- `DSCO_ACCOUNT_SESSION_ID` — current runtime session identifier

The pixel masthead displays the account label only when all four fields are present; otherwise it falls back to the ordinary DSCO signature/title. Provider credentials, email addresses, and bearer tokens are not accepted as display identity. The launcher remains responsible for rejecting expired, wrong-audience, or mismatched Router capsules before starting DSCO.

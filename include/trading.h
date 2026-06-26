#ifndef DSCO_TRADING_H
#define DSCO_TRADING_H

#include <stdbool.h>
#include "env_config.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CROSS-PLATFORM PREDICTION MARKET TRADING INFRASTRUCTURE
 *
 * Supports authenticated trading on:
 *   - Kalshi  (RSA-PSS + SHA-256 signature auth)
 *   - Polymarket CLOB (HMAC-SHA256 + EIP-712 order signing)
 *
 * Plus cross-platform arbitrage execution, risk management,
 * and unified portfolio views.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Platforms ─────────────────────────────────────────────────────────── */

typedef enum {
    PLATFORM_KALSHI     = 0,
    PLATFORM_POLYMARKET = 1,
} trading_platform_t;

/* ── Order types ──────────────────────────────────────────────────────── */

typedef enum {
    ORDER_SIDE_BUY  = 0,
    ORDER_SIDE_SELL = 1,
} order_side_t;

typedef enum {
    ORDER_TYPE_LIMIT  = 0,
    ORDER_TYPE_MARKET = 1,
} order_type_t;

typedef enum {
    OUTCOME_YES = 0,
    OUTCOME_NO  = 1,
} outcome_t;

/* ── Polymarket Order (EIP-712 signed struct) ─────────────────────────── */

typedef struct {
    char     salt[80];          /* uint256 as decimal string               */
    char     maker[44];         /* 0x-prefixed Ethereum address            */
    char     signer[44];        /* signing authority address               */
    char     taker[44];         /* 0x000...000 for open orders             */
    char     token_id[80];      /* outcome token ID (decimal string)       */
    char     maker_amount[32];  /* USDC amount (6 decimals, in wei str)    */
    char     taker_amount[32];  /* token amount (6 decimals, in wei str)   */
    char     expiration[20];    /* unix timestamp string                   */
    char     nonce[20];         /* replay prevention nonce                 */
    int      fee_rate_bps;      /* fee rate in basis points                */
    int      side;              /* 0=BUY, 1=SELL                           */
    int      signature_type;    /* 0=EOA, 1=POLY_PROXY, 2=GNOSIS_SAFE     */
} poly_order_t;

/* ── Risk limits ──────────────────────────────────────────────────────── */

typedef struct {
    double max_position_usd;       /* max USD in any single position      */
    double max_total_exposure_usd; /* max total USD across all positions   */
    double max_order_usd;          /* max single order size in USD         */
    double min_arb_spread;         /* min spread to execute arb (e.g. 0.02) */
    int    max_open_orders;        /* max concurrent open orders           */
    bool   dry_run;                /* if true, simulate but don't execute  */
} risk_limits_t;

/* Default risk limits — conservative */
#define TRADING_DEFAULT_MAX_POSITION_USD        500.0
#define TRADING_DEFAULT_MAX_TOTAL_EXPOSURE_USD 2000.0
#define TRADING_DEFAULT_MAX_ORDER_USD           100.0
#define TRADING_DEFAULT_MIN_ARB_SPREAD            0.03
#define TRADING_DEFAULT_MAX_OPEN_ORDERS          20

static inline risk_limits_t risk_limits_default(void) {
    return (risk_limits_t){
        .max_position_usd       = TRADING_DEFAULT_MAX_POSITION_USD,
        .max_total_exposure_usd = TRADING_DEFAULT_MAX_TOTAL_EXPOSURE_USD,
        .max_order_usd          = TRADING_DEFAULT_MAX_ORDER_USD,
        .min_arb_spread         = TRADING_DEFAULT_MIN_ARB_SPREAD,
        .max_open_orders        = TRADING_DEFAULT_MAX_OPEN_ORDERS,
        .dry_run                = true,  /* safe default: dry run */
    };
}

static inline risk_limits_t risk_limits_from_env(void) {
    risk_limits_t r = risk_limits_default();
    r.dry_run = dsco_env_bool("DSCO_TRADING_DRY_RUN", r.dry_run);
    r.max_order_usd = dsco_env_double("DSCO_TRADING_MAX_ORDER", r.max_order_usd, 0.0, 1000000000.0);
    r.max_total_exposure_usd = dsco_env_double("DSCO_TRADING_MAX_EXPOSURE", r.max_total_exposure_usd, 0.0, 1000000000.0);
    r.min_arb_spread = dsco_env_double("DSCO_TRADING_MIN_ARB_SPREAD", r.min_arb_spread, 0.0, 1.0);
    r.max_open_orders = dsco_env_int("DSCO_TRADING_MAX_OPEN_ORDERS", r.max_open_orders, 1, 1000000);
    r.max_position_usd = dsco_env_double("DSCO_TRADING_MAX_POSITION", r.max_position_usd, 0.0, 1000000000.0);
    return r;
}

/* ── Kalshi Auth ──────────────────────────────────────────────────────── */

/*
 * Kalshi v2 API authentication (RSA-PSS):
 *   message   = timestamp_ms (string) + method + "/trade-api/v2" + path
 *   signature = base64(RSA_PSS_SIGN(SHA256, MGF1(SHA256), saltlen=32, message, private_key))
 *
 * Headers:
 *   KALSHI-ACCESS-KEY:       api_key
 *   KALSHI-ACCESS-SIGNATURE: base64 signature
 *   KALSHI-ACCESS-TIMESTAMP: timestamp_ms
 *
 * Env vars:
 *   KALSHI_API_KEY               — API key ID
 *   KALSHI_RSA_PRIVATE_KEY_PATH  — path to PEM private key file
 */

/* ── Polymarket CLOB Auth ─────────────────────────────────────────────── */

/*
 * Polymarket CLOB API authentication:
 *
 * L2 (API requests — HMAC-SHA256):
 *   message = timestamp + method + path + body
 *   sig     = base64(hmac_sha256(base64_decode(api_secret), message))
 *   Headers: POLY-ADDRESS, POLY-SIGNATURE, POLY-TIMESTAMP,
 *            POLY-NONCE, POLY-API-KEY, POLY-PASSPHRASE
 *
 * Order signing (EIP-712 — secp256k1):
 *   Domain:  {name:"ClobAuthDomain", version:"1", chainId:137}
 *   Struct:  Order(uint256 salt, address maker, address signer, ...)
 *   Hash:    keccak256(0x1901 || domainSep || structHash)
 *   Sig:     secp256k1_sign(privkey, hash) → r || s || v (65 bytes)
 *
 * Contract addresses (Polygon mainnet):
 *   CTF Exchange:          0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E
 *   Neg Risk CTF Exchange: 0xC5d563A36AE78145C45a50134d48A1215220f80a
 *   USDC.e:                0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174
 *
 * Env vars:
 *   POLYMARKET_ADDRESS      — Ethereum wallet address (0x...)
 *   POLYMARKET_PRIVATE_KEY  — hex private key (no 0x prefix)
 *   POLYMARKET_API_KEY      — CLOB API key
 *   POLYMARKET_API_SECRET   — CLOB API secret (base64)
 *   POLYMARKET_PASSPHRASE   — CLOB passphrase
 *   POLYMARKET_FUNDER       — (optional) proxy wallet / funder address
 */

/* ══════════════════════════════════════════════════════════════════════════
 * KALSHI TRADING TOOLS
 * ══════════════════════════════════════════════════════════════════════════ */

/* Account & Portfolio */
bool tool_kalshi_balance(const char *input, char *result, size_t rlen);
bool tool_kalshi_positions(const char *input, char *result, size_t rlen);
bool tool_kalshi_portfolio(const char *input, char *result, size_t rlen);
bool tool_kalshi_fills(const char *input, char *result, size_t rlen);

/* Order Management */
bool tool_kalshi_create_order(const char *input, char *result, size_t rlen);
bool tool_kalshi_batch_create_orders(const char *input, char *result, size_t rlen);
bool tool_kalshi_cancel_order(const char *input, char *result, size_t rlen);
bool tool_kalshi_cancel_all(const char *input, char *result, size_t rlen);
bool tool_kalshi_amend_order(const char *input, char *result, size_t rlen);
bool tool_kalshi_open_orders(const char *input, char *result, size_t rlen);

/* ══════════════════════════════════════════════════════════════════════════
 * POLYMARKET TRADING TOOLS
 * ══════════════════════════════════════════════════════════════════════════ */

/* Account & Portfolio */
bool tool_polymarket_balance(const char *input, char *result, size_t rlen);
bool tool_polymarket_positions(const char *input, char *result, size_t rlen);
bool tool_polymarket_open_orders(const char *input, char *result, size_t rlen);
bool tool_polymarket_api_keys(const char *input, char *result, size_t rlen);
bool tool_polymarket_derive_api_key(const char *input, char *result, size_t rlen);

/* Order Management */
bool tool_polymarket_create_order(const char *input, char *result, size_t rlen);
bool tool_polymarket_cancel_order(const char *input, char *result, size_t rlen);
bool tool_polymarket_cancel_all(const char *input, char *result, size_t rlen);

/* Relayer (gasless transactions) */
bool tool_polymarket_relayer_deploy(const char *input, char *result, size_t rlen);
bool tool_polymarket_relayer_approve(const char *input, char *result, size_t rlen);
bool tool_polymarket_relayer_execute(const char *input, char *result, size_t rlen);
bool tool_polymarket_relayer_status(const char *input, char *result, size_t rlen);

/* ══════════════════════════════════════════════════════════════════════════
 * CROSS-PLATFORM ARBITRAGE & PORTFOLIO
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_arb_execute(const char *input, char *result, size_t rlen);
bool tool_arb_monitor(const char *input, char *result, size_t rlen);
bool tool_portfolio_cross(const char *input, char *result, size_t rlen);
bool tool_risk_check(const char *input, char *result, size_t rlen);
bool tool_risk_configure(const char *input, char *result, size_t rlen);

#endif

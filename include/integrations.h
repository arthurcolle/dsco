#ifndef DSCO_INTEGRATIONS_H
#define DSCO_INTEGRATIONS_H

#include <stdbool.h>
#include <stddef.h>

/* ── Web Search ────────────────────────────────────────────────────────── */
bool tool_tavily_search(const char *input, char *result, size_t rlen);
bool tool_brave_search(const char *input, char *result, size_t rlen);
bool tool_serpapi(const char *input, char *result, size_t rlen);
bool tool_jina_read(const char *input, char *result, size_t rlen);

/* ── GitHub ────────────────────────────────────────────────────────────── */
bool tool_github_search(const char *input, char *result, size_t rlen);
bool tool_github_issue(const char *input, char *result, size_t rlen);
bool tool_github_pr(const char *input, char *result, size_t rlen);
bool tool_github_repo(const char *input, char *result, size_t rlen);
bool tool_github_create_issue(const char *input, char *result, size_t rlen);
bool tool_github_actions(const char *input, char *result, size_t rlen);

/* ── Financial Data (Alpha Vantage) — 100+ endpoints ──────────────────── */
bool tool_alpha_vantage(const char *input, char *result, size_t rlen);
/* Core Stock Time Series */
bool tool_av_time_series_intraday(const char *input, char *result, size_t rlen);
bool tool_av_time_series_daily(const char *input, char *result, size_t rlen);
bool tool_av_time_series_daily_adj(const char *input, char *result, size_t rlen);
bool tool_av_time_series_weekly(const char *input, char *result, size_t rlen);
bool tool_av_time_series_weekly_adj(const char *input, char *result, size_t rlen);
bool tool_av_time_series_monthly(const char *input, char *result, size_t rlen);
bool tool_av_time_series_monthly_adj(const char *input, char *result, size_t rlen);
bool tool_av_quote(const char *input, char *result, size_t rlen);
bool tool_av_bulk_quotes(const char *input, char *result, size_t rlen);
/* Search & Market Status */
bool tool_av_search(const char *input, char *result, size_t rlen);
bool tool_av_market_status(const char *input, char *result, size_t rlen);
/* Options */
bool tool_av_realtime_options(const char *input, char *result, size_t rlen);
bool tool_av_historical_options(const char *input, char *result, size_t rlen);
/* News & Sentiment */
bool tool_av_news(const char *input, char *result, size_t rlen);
/* Company Fundamentals */
bool tool_av_overview(const char *input, char *result, size_t rlen);
bool tool_av_etf(const char *input, char *result, size_t rlen);
bool tool_av_income(const char *input, char *result, size_t rlen);
bool tool_av_balance(const char *input, char *result, size_t rlen);
bool tool_av_cashflow(const char *input, char *result, size_t rlen);
bool tool_av_earnings(const char *input, char *result, size_t rlen);
bool tool_av_earnings_estimates(const char *input, char *result, size_t rlen);
bool tool_av_dividends(const char *input, char *result, size_t rlen);
bool tool_av_splits(const char *input, char *result, size_t rlen);
bool tool_av_insider(const char *input, char *result, size_t rlen);
bool tool_av_institutional(const char *input, char *result, size_t rlen);
/* Earnings Call Transcript */
bool tool_av_transcript(const char *input, char *result, size_t rlen);
/* Corporate Events & Calendar */
bool tool_av_movers(const char *input, char *result, size_t rlen);
bool tool_av_listing_status(const char *input, char *result, size_t rlen);
bool tool_av_earnings_calendar(const char *input, char *result, size_t rlen);
bool tool_av_ipo_calendar(const char *input, char *result, size_t rlen);
/* Advanced Analytics */
bool tool_av_analytics_fixed(const char *input, char *result, size_t rlen);
bool tool_av_analytics_sliding(const char *input, char *result, size_t rlen);
/* Forex */
bool tool_av_forex(const char *input, char *result, size_t rlen);
bool tool_av_fx_intraday(const char *input, char *result, size_t rlen);
bool tool_av_fx_daily(const char *input, char *result, size_t rlen);
bool tool_av_fx_weekly(const char *input, char *result, size_t rlen);
bool tool_av_fx_monthly(const char *input, char *result, size_t rlen);
/* Crypto */
bool tool_av_crypto(const char *input, char *result, size_t rlen);
bool tool_av_crypto_intraday(const char *input, char *result, size_t rlen);
bool tool_av_crypto_weekly(const char *input, char *result, size_t rlen);
bool tool_av_crypto_monthly(const char *input, char *result, size_t rlen);
/* Commodities */
bool tool_av_wti(const char *input, char *result, size_t rlen);
bool tool_av_brent(const char *input, char *result, size_t rlen);
bool tool_av_natural_gas(const char *input, char *result, size_t rlen);
bool tool_av_copper(const char *input, char *result, size_t rlen);
bool tool_av_aluminum(const char *input, char *result, size_t rlen);
bool tool_av_wheat(const char *input, char *result, size_t rlen);
bool tool_av_corn(const char *input, char *result, size_t rlen);
bool tool_av_cotton(const char *input, char *result, size_t rlen);
bool tool_av_sugar(const char *input, char *result, size_t rlen);
bool tool_av_coffee(const char *input, char *result, size_t rlen);
bool tool_av_all_commodities(const char *input, char *result, size_t rlen);
/* Precious Metals */
bool tool_av_gold_spot(const char *input, char *result, size_t rlen);
bool tool_av_gold_history(const char *input, char *result, size_t rlen);
/* Economic Indicators */
bool tool_av_real_gdp(const char *input, char *result, size_t rlen);
bool tool_av_real_gdp_per_capita(const char *input, char *result, size_t rlen);
bool tool_av_treasury_yield(const char *input, char *result, size_t rlen);
bool tool_av_federal_funds_rate(const char *input, char *result, size_t rlen);
bool tool_av_cpi(const char *input, char *result, size_t rlen);
bool tool_av_inflation(const char *input, char *result, size_t rlen);
bool tool_av_retail_sales(const char *input, char *result, size_t rlen);
bool tool_av_durables(const char *input, char *result, size_t rlen);
bool tool_av_unemployment(const char *input, char *result, size_t rlen);
bool tool_av_nonfarm_payroll(const char *input, char *result, size_t rlen);
/* Technical Indicators (54) */
bool tool_av_sma(const char *input, char *result, size_t rlen);
bool tool_av_ema(const char *input, char *result, size_t rlen);
bool tool_av_wma(const char *input, char *result, size_t rlen);
bool tool_av_dema(const char *input, char *result, size_t rlen);
bool tool_av_tema(const char *input, char *result, size_t rlen);
bool tool_av_trima(const char *input, char *result, size_t rlen);
bool tool_av_kama(const char *input, char *result, size_t rlen);
bool tool_av_mama(const char *input, char *result, size_t rlen);
bool tool_av_vwap(const char *input, char *result, size_t rlen);
bool tool_av_t3(const char *input, char *result, size_t rlen);
bool tool_av_macd(const char *input, char *result, size_t rlen);
bool tool_av_macdext(const char *input, char *result, size_t rlen);
bool tool_av_stoch(const char *input, char *result, size_t rlen);
bool tool_av_stochf(const char *input, char *result, size_t rlen);
bool tool_av_rsi(const char *input, char *result, size_t rlen);
bool tool_av_stochrsi(const char *input, char *result, size_t rlen);
bool tool_av_willr(const char *input, char *result, size_t rlen);
bool tool_av_adx(const char *input, char *result, size_t rlen);
bool tool_av_adxr(const char *input, char *result, size_t rlen);
bool tool_av_apo(const char *input, char *result, size_t rlen);
bool tool_av_ppo(const char *input, char *result, size_t rlen);
bool tool_av_mom(const char *input, char *result, size_t rlen);
bool tool_av_bop(const char *input, char *result, size_t rlen);
bool tool_av_cci(const char *input, char *result, size_t rlen);
bool tool_av_cmo(const char *input, char *result, size_t rlen);
bool tool_av_roc(const char *input, char *result, size_t rlen);
bool tool_av_rocr(const char *input, char *result, size_t rlen);
bool tool_av_aroon(const char *input, char *result, size_t rlen);
bool tool_av_aroonosc(const char *input, char *result, size_t rlen);
bool tool_av_mfi(const char *input, char *result, size_t rlen);
bool tool_av_trix_ind(const char *input, char *result, size_t rlen);
bool tool_av_ultosc(const char *input, char *result, size_t rlen);
bool tool_av_dx(const char *input, char *result, size_t rlen);
bool tool_av_minus_di(const char *input, char *result, size_t rlen);
bool tool_av_plus_di(const char *input, char *result, size_t rlen);
bool tool_av_minus_dm(const char *input, char *result, size_t rlen);
bool tool_av_plus_dm(const char *input, char *result, size_t rlen);
bool tool_av_bbands(const char *input, char *result, size_t rlen);
bool tool_av_midpoint(const char *input, char *result, size_t rlen);
bool tool_av_midprice(const char *input, char *result, size_t rlen);
bool tool_av_sar(const char *input, char *result, size_t rlen);
bool tool_av_trange(const char *input, char *result, size_t rlen);
bool tool_av_atr(const char *input, char *result, size_t rlen);
bool tool_av_natr(const char *input, char *result, size_t rlen);
bool tool_av_ad_line(const char *input, char *result, size_t rlen);
bool tool_av_adosc(const char *input, char *result, size_t rlen);
bool tool_av_obv(const char *input, char *result, size_t rlen);
bool tool_av_ht_trendline(const char *input, char *result, size_t rlen);
bool tool_av_ht_sine(const char *input, char *result, size_t rlen);
bool tool_av_ht_trendmode(const char *input, char *result, size_t rlen);
bool tool_av_ht_dcperiod(const char *input, char *result, size_t rlen);
bool tool_av_ht_dcphase(const char *input, char *result, size_t rlen);
bool tool_av_ht_phasor(const char *input, char *result, size_t rlen);
/* FRED */
bool tool_fred_series(const char *input, char *result, size_t rlen);

/* ── Communication ─────────────────────────────────────────────────────── */
bool tool_slack_post(const char *input, char *result, size_t rlen);
bool tool_discord_post(const char *input, char *result, size_t rlen);
bool tool_twilio_sms(const char *input, char *result, size_t rlen);

/* ── Knowledge & Productivity ──────────────────────────────────────────── */
bool tool_notion_search(const char *input, char *result, size_t rlen);
bool tool_notion_page(const char *input, char *result, size_t rlen);

/* ── Weather & Geo ─────────────────────────────────────────────────────── */
bool tool_weather(const char *input, char *result, size_t rlen);
bool tool_mapbox_geocode(const char *input, char *result, size_t rlen);

/* ── Web Scraping ──────────────────────────────────────────────────────── */
bool tool_firecrawl(const char *input, char *result, size_t rlen);

/* ── Audio/Speech ──────────────────────────────────────────────────────── */
bool tool_elevenlabs_tts(const char *input, char *result, size_t rlen);

/* ── Vector DB ─────────────────────────────────────────────────────────── */
bool tool_pinecone_query(const char *input, char *result, size_t rlen);

/* ── Payments ──────────────────────────────────────────────────────────── */
bool tool_stripe(const char *input, char *result, size_t rlen);

/* ── Cloud Databases ───────────────────────────────────────────────────── */
bool tool_supabase_query(const char *input, char *result, size_t rlen);

/* ── ML Inference ──────────────────────────────────────────────────────── */
bool tool_huggingface(const char *input, char *result, size_t rlen);

#endif
